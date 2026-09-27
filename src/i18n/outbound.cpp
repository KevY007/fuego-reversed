#include "outbound.h"

#include "engine.h"
#include "config.h"
#include "hooks.h"

#include "../net/net.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"
#include "../samp/samp.h"
#include "../core/logger.h"

#include <windows.h> // GetTickCount
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace i18n::outbound {

namespace {

constexpr int kRpcChat = 101; // RPC_Chat: client->server chat (uint8 len + bytes)

// Guaranteed delivery: a swallowed line whose translation hasn't landed within
// this long is sent as typed (mirrors surfaces' dialog INPUT-reply deadline) --
// the player's chat must never be lost or held indefinitely.
constexpr uint32_t kDeadlineMs = 5000;

std::atomic<uint64_t> g_seq{ 1 };

struct Pending {
	std::string original; // what the player typed: sent on failure / deadline / flush
	uint32_t    t0 = 0;   // GetTickCount() when swallowed -- the deadline clock
};
// handle -> a swallowed line awaiting its translation. Ordered by handle (= the
// order the lines were typed), so a flush sends several in the order typed.
// Touched only on the main thread (the net send seam and engine::pump()/
// apply_result/pump_tick all run there), so no lock is needed.
std::map<uint64_t, Pending> g_pending;

// Wrap-safe "at least ms have elapsed since t0" for GetTickCount()-style clocks
// (same arithmetic as surfaces' reply deadline and i18n::dlgq).
bool elapsed_ms( uint32_t nowMs, uint32_t t0, uint32_t ms ) { return (int32_t) ( nowMs - t0 - ms ) >= 0; }

// Sends a client chat line the way SA:MP does (RPC_Chat = uint8 length + bytes).
// Goes through g_RakClient, which wraps the REAL RakClient interface, so it
// BYPASSES the net send seam (OnSendRPC) and can never re-enter on_send_chat()
// -- the same property compat relies on to send its spoofed ClientJoin.
void send_chat( const std::string& bytes ) {
	if ( !g_RakClient ) return;
	std::string s = bytes;
	if ( s.size() > 127 ) s.resize( 127 ); // SA:MP chat input cap; single-byte codepage -> safe truncate
	if ( s.empty() ) return;
	BitStream bs;
	bs.Write( (uint8_t) s.size() );
	bs.Write( (char*) s.data(), (int) s.size() );
	g_RakClient->RPC( kRpcChat, &bs );
}

// Net send-seam handler: return false to DROP the outgoing RPC, true to let it go.
// A C-ABI seam: no exception may escape. Anything that throws BEFORE the swallow
// decision lets the chat go out untouched.
bool on_send_chat( int id, BitStream* params, PacketPriority, PacketReliability, char, bool ) {
	if ( id != kRpcChat || !params ) return true; // not chat -> pass
	try {
		if ( !i18n::config::get().outbound_enabled ) return true; // feature off -> pass
		// Swallowing is only safe when the per-frame pump will deliver the result;
		// without it nothing would ever send the line -- let it go as typed.
		if ( !i18n::hooks::pump_ready() ) return true;

		// Parse the outgoing chat (uint8 length + bytes), fully bounds-checked so a
		// malformed stream is passed through untouched rather than crashing.
		params->ResetReadPointer();
		uint8_t len = 0;
		if ( !params->Read( len ) || len == 0 || len > 200 ) { params->ResetReadPointer(); return true; }
		char buf[256] = {};
		if ( !params->Read( buf, len ) ) { params->ResetReadPointer(); return true; }
		params->ResetReadPointer();
		buf[len] = '\0';
		std::string text( buf, len );

		// Commands go via RPC_ServerCommand, not RPC_Chat, but guard anyway: never
		// translate a "/command" line.
		if ( text[0] == '/' ) return true;

		auto& s = i18n::config::get();
		i18n::engine::Job job;
		job.handle             = g_seq.fetch_add( 1, std::memory_order_relaxed );
		job.surface            = 1; // outbound (routed by hooks::apply_result -> on_result)
		job.sourceBytes        = text;
		job.srcCodepage        = s.source_cp; // 0 = auto-detect the client's typed codepage
		job.skipContext        = true;        // don't mix with the inbound context ring
		job.noDedup            = true;        // never drop the player's message as a "duplicate"
		job.promptOverride     = i18n::config::build_system_prompt( s.outbound_target_lang, /*roman=*/false );
		job.targetLangOverride = s.outbound_target_lang;
		job.displayCpOverride  = i18n::config::display_codepage( s.outbound_target_lang, false );

		// Submit FIRST, then remember the line: if either throws, the line goes out
		// as typed right now (catch below) and a result that still comes back finds
		// no entry and is ignored -- never lost, never sent twice. (No result can
		// land in between: pump() runs on this same main thread.)
		const uint64_t handle = job.handle;
		i18n::engine::submit( job );
		g_pending.emplace( handle, Pending{ std::move( text ), GetTickCount() } );
		return false; // swallow; on_result(), the tick() deadline or flush_pending() sends it
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
		? "{33FF33}[translate] {FFFFFF}Outgoing translation ON -- your chat and dialog input go out in the server's language."
		: "{FF6D70}[translate] {FFFFFF}Outgoing translation OFF." );
	Log( "[i18n] outbound = %s (target_lang=%d).", s.outbound_enabled ? "ON" : "OFF", s.outbound_target_lang );
}

void on_result( const i18n::engine::Result& r ) {
	auto it = g_pending.find( r.handle );
	// Already sent as typed (deadline / flush) -- a late result is ignored, so the
	// line never goes out twice.
	if ( it == g_pending.end() ) return;
	const bool ok = r.ok && !r.displayBytes.empty();
	// Send, THEN forget: if sending throws (before the RPC), the entry survives and
	// the deadline still delivers the original.
	send_chat( ok ? r.displayBytes : it->second.original ); // translation, or what the player typed
	g_pending.erase( it );
}

void tick( uint32_t nowMs ) {
	for ( auto it = g_pending.begin(); it != g_pending.end(); ) {
		if ( elapsed_ms( nowMs, it->second.t0, kDeadlineMs ) ) {
			send_chat( it->second.original );
			Log( "[i18n] outgoing chat sent as typed (translation took too long)." );
			it = g_pending.erase( it );
		} else {
			++it;
		}
	}
}

void flush_pending() {
	// One at a time, forgetting each line only once it has gone out, in the order typed.
	while ( !g_pending.empty() ) {
		auto it = g_pending.begin();
		send_chat( it->second.original );
		g_pending.erase( it );
	}
}

size_t discard_pending() {
	const size_t n = g_pending.size();
	g_pending.clear(); // typed to the previous session: never sent to the new one
	return n;
}

} // namespace i18n::outbound
