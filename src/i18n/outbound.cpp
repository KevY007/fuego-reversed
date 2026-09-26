#include "outbound.h"

#include "engine.h"
#include "config.h"
#include "hooks.h"

#include "../net/net.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"
#include "../samp/samp.h"
#include "../core/logger.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace i18n::outbound {

namespace {

constexpr int kRpcChat = 101; // RPC_Chat: client->server chat (uint8 len + bytes)

std::atomic<uint64_t> g_seq{ 1 };
// handle -> the player's original bytes, for the "translation failed -> send what
// they typed" fallback. Touched only on the main thread (the net send seam and
// engine::pump()/apply_result both run there), so no lock is needed.
std::unordered_map<uint64_t, std::string> g_pending;

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
bool on_send_chat( int id, BitStream* params, PacketPriority, PacketReliability, char, bool ) {
	if ( id != kRpcChat || !params ) return true;             // not chat -> pass
	if ( !i18n::config::get().outbound_enabled ) return true; // feature off -> pass

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

	g_pending[job.handle] = text; // fallback source if the translation fails
	i18n::engine::submit( job );
	return false; // swallow the original; on_result() sends the translation (or original)
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
		? "{33FF33}[translate] {FFFFFF}Outgoing translation ON -- your chat is sent in the server's language."
		: "{FF6D70}[translate] {FFFFFF}Outgoing translation OFF." );
	Log( "[i18n] outbound = %s (target_lang=%d).", s.outbound_enabled ? "ON" : "OFF", s.outbound_target_lang );
}

void on_result( const i18n::engine::Result& r ) {
	std::string original;
	auto it = g_pending.find( r.handle );
	if ( it != g_pending.end() ) { original = it->second; g_pending.erase( it ); }

	if ( r.ok && !r.displayBytes.empty() )
		send_chat( r.displayBytes );          // send the translation
	else if ( !original.empty() )
		send_chat( original );                // translation failed -> send what the player typed
}

} // namespace i18n::outbound
