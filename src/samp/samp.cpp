#include "samp.h"
#include "addresses.h"
#include "structs.h"
#include "samp_sdk.h"   // stInputInfo (iInputEnabled) for the cursor guard
#include "../core/memory.h"
#include "../core/logger.h"

#include <windows.h>

namespace {

bool g_detected = false;

// "528D44..." -> bytes. Returns count written.
size_t hex_to_bytes( const char *hex, uint8_t *out, size_t max ) {
	size_t n = 0;
	auto nib = []( char c ) -> int {
		if ( c >= '0' && c <= '9' ) return c - '0';
		if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
		if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
		return -1;
	};
	for ( ; hex[0] && hex[1] && n < max; hex += 2 ) {
		int hi = nib( hex[0] ), lo = nib( hex[1] );
		if ( hi < 0 || lo < 0 ) break;
		out[n++] = (uint8_t)( ( hi << 4 ) | lo );
	}
	return n;
}

} // namespace

namespace samp {

uint32_t base() {
	return reinterpret_cast<uint32_t>( GetModuleHandleA( "samp.dll" ) );
}

bool detect() {
	const uint32_t b = base();
	if ( !b ) { g_detected = false; return false; }

	uint8_t sig[16];
	size_t len = hex_to_bytes( addr::SIG_HEX, sig, sizeof( sig ) );

	// guard the read (module memory is committed, but be safe)
	const uint8_t *at = mem::ptr<const uint8_t>( b + addr::SIG_OFFSET );
	if ( IsBadReadPtr( at, len ) ) { g_detected = false; return false; }

	g_detected = ( memcmp( at, sig, len ) == 0 );
	Log( "[samp] base=0x%08X  0.3.DL signature %s", b, g_detected ? "OK" : "MISMATCH" );
	return g_detected;
}

bool detected() { return g_detected; }

void *chat_ptr() {
	const uint32_t b = base();
	if ( !b ) return nullptr;
	return mem::read<void *>( b + addr::CHAT_INFO );
}

void *input_ptr() {
	const uint32_t b = base();
	if ( !b ) return nullptr;
	return mem::read<void *>( b + addr::CHAT_INPUT );
}

stSAMP *netgame() {
	const uint32_t b = base();
	if ( !b ) return nullptr;
	return mem::read<stSAMP *>( b + addr::INFO );
}

void restart_game() {
	const uint32_t b = base();
	stSAMP *ng = netgame();
	if ( !b || !ng ) return;
	mem::thiscall<void>( b + addr::FUNC_RESTART_GAME, ng );
}

void reconnect_current_server() {
	stSAMP *ng = netgame();
	if ( !ng ) return;
	Log( "[samp] reconnect: iGameState=%d szIP=%s port=%u -> WAIT_CONNECT",
		ng->iGameState, ng->szIP, ng->ulPort );
	ng->ulConnectTick = 0;                       // skip the 3s connect throttle
	ng->iGameState    = GAMESTATE_WAIT_CONNECT;   // Process() reconnects next frame
}

bool is_in_game() { return chat_ptr() != nullptr; }

void toggle_cursor( bool show ) {
	const uint32_t b = base();
	if ( !b ) return;
	// Fuego 1:1 (toggleSAMPCursor): never toggle while SA:MP's own chat input box is
	// open, or closing our overlay would fire UnlockActorCam and stomp the game's
	// camera/cursor mid-chat. input_ptr() is the same CInput we register commands on.
	stInputInfo *ii = static_cast<stInputInfo *>( input_ptr() );
	if ( ii && ii->iInputEnabled ) return;
	void *misc = mem::read<void *>( b + addr::MISC_INFO );
	if ( !misc ) return;
	// CGame::SetCursorMode(this, mode, unlockCam): mode 3 shows the dialog cursor
	// (frees the OS cursor + suspends camera), 0 hides it. Mirrors Fuego.
	mem::thiscall<void>( b + addr::FUNC_TOGGLE_CURSOR, misc, show ? 3 : 0, !show );
	if ( !show )
		mem::thiscall<void>( b + addr::FUNC_UNLOCK_ACTOR_CAM, misc ); // restore camera
}

void chat( const char *text, uint32_t argb ) {
	const uint32_t b = base();
	void *c = chat_ptr();
	if ( !b || !c || !text ) return;

	// CChat::AddEntry(this, type, msg, prefix, textColor, prefixColor) __thiscall
	mem::thiscall<void>( b + addr::FUNC_ADD_TO_CHAT_WND, c,
		CHAT_TYPE_DEBUG, text, (const char *)nullptr, argb, (uint32_t)0 );
}

void disable_native_nametags( bool disable ) {
	// RET-patch (0xC3) SA:MP's two nametag-draw entry points so its own nametags +
	// HP bars don't draw under the custom overlay. Restores the original byte on off.
	static bool    patched = false;
	static uint8_t orig1 = 0, orig2 = 0;
	uint32_t b = base();
	if ( !b ) return;
	uint8_t *p1 = reinterpret_cast<uint8_t *>( b + addr::PATCH_DISABLE_NAMETAGS );
	uint8_t *p2 = reinterpret_cast<uint8_t *>( b + addr::PATCH_DISABLE_NAMETAGS_HP );
	DWORD o;
	if ( disable && !patched ) {
		VirtualProtect( p1, 1, PAGE_EXECUTE_READWRITE, &o ); orig1 = *p1; *p1 = 0xC3; VirtualProtect( p1, 1, o, &o );
		VirtualProtect( p2, 1, PAGE_EXECUTE_READWRITE, &o ); orig2 = *p2; *p2 = 0xC3; VirtualProtect( p2, 1, o, &o );
		patched = true;
	} else if ( !disable && patched ) {
		VirtualProtect( p1, 1, PAGE_EXECUTE_READWRITE, &o ); *p1 = orig1; VirtualProtect( p1, 1, o, &o );
		VirtualProtect( p2, 1, PAGE_EXECUTE_READWRITE, &o ); *p2 = orig2; VirtualProtect( p2, 1, o, &o );
		patched = false;
	}
}

} // namespace samp
