#include "outbound.h"

#include "engine.h"
#include "config.h"
#include "dlgq.h"    // the ordered, timing-faithful release queue (shared with held dialogs)
#include "hooks.h"
#include "protect.h" // split_message_command

#include "../net/net.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"
#include "../samp/samp.h"
#include "../core/logger.h"

#include <windows.h> // GetTickCount
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace i18n::outbound {

namespace {

constexpr int    kRpcChat    = 101; // RPC_Chat: client->server chat (uint8 len + bytes)
constexpr int    kRpcCommand = 50;  // RPC_ServerCommand: a typed "/command" (uint32 len + bytes)
constexpr size_t kInputCap   = 127; // SA:MP's chat input takes 127 bytes -- chat and commands alike

// Guaranteed delivery: a swallowed line whose translation hasn't landed within
// this long is sent as typed (mirrors surfaces' dialog INPUT-reply deadline) --
// the player's chat must never be lost or held indefinitely.
constexpr uint32_t kDeadlineMs = 5000;
constexpr size_t   kMaxQueued  = 32;

std::atomic<uint64_t> g_seq{ 1 };

// The swallowed lines, released STRICTLY in the order typed -- a short "/do ..."
// translated fast never overtakes the "/me ..." typed before it -- and each no
// sooner than the previous one + the gap the player left between them (a server's
// anti-spam sees the player's own pace). A line whose translation hasn't landed
// within kDeadlineMs goes out as typed. While any line waits, even one that isn't
// translated ("/gps bank") queues behind it rather than overtake it.
i18n::dlgq::Queue g_queue( kDeadlineMs, kMaxQueued );

// How each queued line goes out. Touched only on the main thread (the net send
// seam and engine::pump()/apply_result/pump_tick all run there): no lock needed.
struct Line {
	bool        command   = false; // a "/command" (RPC_ServerCommand), else chat
	std::string head;               // a message command's "/pm 35 ": kept verbatim before the translated message
	bool        translate = false;  // a translation was asked for
	bool        answered  = false;  // ... and came back (translated or not)
};
std::unordered_map<uint64_t, Line> g_lines; // queue seq -> its line

// Sends a client chat line the way SA:MP does (RPC_Chat = uint8 length + bytes).
// Goes through g_RakClient, which wraps the REAL RakClient interface, so it
// BYPASSES the net send seam (OnSendRPC) and can never re-enter on_send_chat()
// -- the same property compat relies on to send its spoofed ClientJoin.
void send_chat( const std::string& bytes ) {
	if ( !g_RakClient ) return;
	std::string s = bytes;
	if ( s.size() > kInputCap ) s.resize( kInputCap ); // single-byte codepage -> safe truncate
	if ( s.empty() ) return;
	BitStream bs;
	bs.Write( (uint8_t) s.size() );
	bs.Write( (char*) s.data(), (int) s.size() );
	g_RakClient->RPC( kRpcChat, &bs );
}

// The same for a "/command" (RPC_ServerCommand = uint32 length + bytes): the server
// runs it exactly as if it had been typed.
void send_command( const std::string& bytes ) {
	if ( !g_RakClient ) return;
	std::string s = bytes;
	if ( s.size() > kInputCap ) s.resize( kInputCap );
	if ( s.empty() ) return;
	BitStream bs;
	bs.Write( (uint32_t) s.size() );
	bs.Write( (char*) s.data(), (int) s.size() );
	g_RakClient->RPC( kRpcCommand, &bs );
}

// A finished translation as it goes out: line breaks as spaces, and -- after the
// command's head -- within the input's 127 bytes, cut at the last space before
// that rather than mid-word (single-byte codepage: any byte boundary is safe).
std::string fit_input( const std::string& head, std::string t ) {
	for ( char& c : t )
		if ( c == '\n' || c == '\r' || c == '\t' ) c = ' ';
	const size_t room = head.size() < kInputCap ? kInputCap - head.size() : 0;
	if ( t.size() > room ) {
		size_t cut = t.rfind( ' ', room );
		if ( cut == std::string::npos || cut < room / 2 ) cut = room; // no space late enough: a plain cut
		t.resize( cut );
		while ( !t.empty() && t.back() == ' ' ) t.pop_back();
	}
	return head + t;
}

// Send everything released, each the way it was typed. `timed`: released by the
// clock (tick), where a translated line without an answer ran out of time.
void send_released( std::vector<i18n::dlgq::Release> released, bool timed ) {
	for ( const auto& r : released ) {
		auto it = g_lines.find( r.seq );
		const bool command = it != g_lines.end() && it->second.command;
		if ( timed && it != g_lines.end() && it->second.translate && !it->second.answered )
			Log( "[i18n] outgoing %s sent as typed (translation took too long).", command ? "command" : "chat" );
		const std::string text( r.payload.begin(), r.payload.end() );
		if ( command ) send_command( text );
		else           send_chat( text );
		if ( it != g_lines.end() ) g_lines.erase( it );
	}
}

// Queue a line as typed. False if the queue is full (the caller sends it at once).
bool enqueue( uint64_t seq, const std::string& text, Line line ) {
	if ( !g_queue.push( seq, GetTickCount(), std::vector<uint8_t>( text.begin(), text.end() ) ) ) return false;
	g_lines[seq] = std::move( line );
	return true;
}

// Net send-seam handler: return false to DROP the outgoing RPC, true to let it go.
// A C-ABI seam: no exception may escape. Anything that throws BEFORE the swallow
// decision lets the chat go out untouched.
bool on_send_chat( int id, BitStream* params, PacketPriority, PacketReliability, char, bool ) {
	if ( ( id != kRpcChat && id != kRpcCommand ) || !params ) return true; // not chat / a command -> pass
	try {
		// Swallowing is only safe when the per-frame pump will deliver the result;
		// without it nothing would ever send the line -- let it go as typed.
		if ( !i18n::hooks::pump_ready() ) return true;
		const bool on = i18n::config::get().outbound_enabled;
		if ( !on && g_queue.empty() ) return true; // feature off, nothing waiting -> pass

		// Parse the outgoing line (chat: uint8 length + bytes; a command: uint32
		// length + bytes), fully bounds-checked so a malformed stream is passed
		// through untouched rather than crashing.
		params->ResetReadPointer();
		uint32_t len = 0;
		if ( id == kRpcChat ) {
			uint8_t len8 = 0;
			if ( !params->Read( len8 ) ) { params->ResetReadPointer(); return true; }
			len = len8;
		} else if ( !params->Read( len ) ) {
			params->ResetReadPointer();
			return true;
		}
		if ( len == 0 || len > 200 ) { params->ResetReadPointer(); return true; }
		char buf[256] = {};
		if ( !params->Read( buf, (int) len ) ) { params->ResetReadPointer(); return true; }
		params->ResetReadPointer();
		std::string text( buf, len );

		// Chat is translated whole (a "/..." chat line is never a command -- guard
		// anyway). A command only when its words are a MESSAGE (/pm, /me, /b, ...):
		// "/pm 35 hey come here" keeps "/pm 35 " and translates "hey come here". Any
		// other command's words are arguments ("/gps atm"): it is sent as typed.
		Line line;
		line.command = ( id == kRpcCommand );
		std::string message;
		bool translate = false;
		if ( on ) {
			if ( !line.command ) {
				translate = text[0] != '/';
				message   = text;
			} else {
				translate = i18n::protect::split_message_command( text, line.head, message );
			}
		}

		const uint64_t seq = g_seq.fetch_add( 1, std::memory_order_relaxed );
		if ( !translate ) {
			// Not translated -- but typed after a line still waiting: it queues behind
			// it (ready as typed), so it can't overtake it.
			if ( g_queue.empty() ) return true;
			if ( !enqueue( seq, text, line ) ) return true;
			g_queue.resolve( seq, std::vector<uint8_t>() );
			return false;
		}

		auto& s = i18n::config::get();
		i18n::engine::Job job;
		job.handle             = seq;
		job.surface            = 1; // outbound (routed by hooks::apply_result -> on_result)
		job.sourceBytes        = message;
		job.srcCodepage        = s.source_cp; // 0 = auto-detect the client's typed codepage
		job.skipContext        = true;        // don't mix with the inbound context ring
		job.noDedup            = true;        // never drop the player's message as a "duplicate"
		job.promptOverride     = i18n::config::build_system_prompt( s.outbound_target_lang, /*roman=*/false );
		job.targetLangOverride = s.outbound_target_lang;
		job.displayCpOverride  = i18n::config::display_codepage( s.outbound_target_lang, false );
		job.capBytes           = 4095; // the whole translation: fit_input cuts it at a word

		// Queue FIRST, then submit: a full queue lets the line go out as typed right
		// now; if submit throws, the queued line still goes out as typed at its
		// deadline and a result that comes back anyway finds it already sent (or
		// resolves it) -- never lost, never sent twice. (No result can land in
		// between: pump() runs on this same main thread.)
		line.translate = true;
		if ( !enqueue( seq, text, line ) ) return true;
		i18n::engine::submit( job );
		return false; // swallow; on_result() + tick(), the deadline or flush_pending() sends it
	} catch ( ... ) {
		params->ResetReadPointer();
		return true; // before the swallow decision -> the chat goes out untouched
	}
}

} // namespace

void install() {
	net::on_send_rpc( on_send_chat );
	Log( "[i18n] outbound chat seam installed (idle until enabled)." );
}

bool enabled() { return i18n::config::get().outbound_enabled; }

void toggle() {
	auto& s = i18n::config::get();
	s.outbound_enabled = !s.outbound_enabled;
	i18n::hooks::apply_config(); // start the worker engine if it isn't already running
	samp::chat( s.outbound_enabled
		? "{33FF33}[translate] {FFFFFF}Outgoing translation ON -- your chat, /pm /me-style messages and dialog input go out in the server's language."
		: "{FF6D70}[translate] {FFFFFF}Outgoing translation OFF." );
	Log( "[i18n] outbound = %s (target_lang=%d).", s.outbound_enabled ? "ON" : "OFF", s.outbound_target_lang );
}

void on_result( const i18n::engine::Result& r ) {
	auto it = g_lines.find( r.handle );
	// Already sent as typed (deadline / flush / a new session) -- a late result is
	// ignored, so the line never goes out twice.
	if ( it == g_lines.end() ) return;
	it->second.answered = true;
	const bool ok = r.ok && !r.displayBytes.empty() && it->second.head.size() + 16 <= kInputCap;
	// Empty = the original goes out (the translation failed or wasn't needed).
	const std::string out = ok ? fit_input( it->second.head, r.displayBytes ) : std::string();
	g_queue.resolve( r.handle, std::vector<uint8_t>( out.begin(), out.end() ) );
	send_released( g_queue.tick( GetTickCount() ), true ); // its turn may be now
}

void tick( uint32_t nowMs ) {
	if ( !g_queue.empty() ) send_released( g_queue.tick( nowMs ), true );
}

void flush_pending() {
	// Everything still waiting goes out as typed, in the order typed.
	send_released( g_queue.flush(), false );
}

size_t discard_pending() {
	const size_t n = g_queue.size();
	g_queue = i18n::dlgq::Queue( kDeadlineMs, kMaxQueued ); // typed to the previous session: never sent
	g_lines.clear();
	return n;
}

} // namespace i18n::outbound
