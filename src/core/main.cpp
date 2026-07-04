/*
	Fuego Reversed  --  a modern-C++ rebuild of Project Fuego for SA:MP 0.3.DL.

	Loads as an ASI (late load, main-thread-safe timing) rather than a d3d9 proxy.
	This file is the entry point + init orchestration. Subsystems are brought up
	in order as they are migrated: SA:MP core -> networking/RakNet -> compat ->f
	menu -> cheats/commands.
*/

#include "logger.h"
#include "crashlog.h"
#include "config.h"
#include "../samp/samp.h"
#include "../net/net.h"
#include "../compat/compat.h"
#include "../commands/commands.h"
#include "../render/render.h"
#include "../menu/menu.h"
#include "../cheats/cheats.h"
#include "../cheats/friends.h"

#include <windows.h>

namespace {

// "loaded into a server" = SA:MP chat interface exists, then a short settle.
// 250ms after the chat handle appears is a reliable in-game indicator.
constexpr DWORD kLoadSettleMs = 250;

void bring_up_subsystems() {
	// --- SA:MP core (detect + wait for in-game) ---
	int guard = 0;
	while ( samp::base() == 0 && guard++ < 240 )
		Sleep( 250 );

	if ( !samp::detect() ) {
		Log( "[init] samp.dll not present or not 0.3.DL -- staying idle." );
		return;
	}

	guard = 0;
	while ( !samp::is_in_game() && guard++ < 2400 ) // up to ~10 min
		Sleep( 250 );

	if ( !samp::is_in_game() ) {
		Log( "[init] never reached in-game state." );
		return;
	}

	Sleep( kLoadSettleMs ); // 250ms settle after chat handle == loaded in
	Log( "[init] in-game. Bringing up subsystems." );

	// --- networking / RakNet hook (accurate) ---
	// Poll until SA:MP's RakClient interface is live, then swap in our hook.
	int ni = 0;
	while ( !net::install() && ni++ < 200 )
		Sleep( 100 );
	Log( "[init] networking hook: %s", net::installed() ? "ACTIVE" : "NOT INSTALLED (samp iface not ready)" );

	// --- 0.3.7 compatibility layer (inert until /compat switches to 0.3.7) ---
	if ( net::installed() )
		compat::install();

	// --- client chat commands (/compat, /fuego_cmds, ...) ---
	cmd::install();

	// --- overlay: D3D9 render hook + in-house ImGui menu ---
	// Poll until GTA's D3D9 device is live (0xC97C28), then hook it.
	int ri = 0;
	while ( !render::install() && ri++ < 100 )
		Sleep( 100 );
	if ( render::installed() ) {
		menu::install();
		Log( "[init] overlay online (F2 for menu)." );
	} else {
		Log( "[init] WARNING: render hook failed; menu unavailable." );
	}

	// --- cheats: net-seam packet handlers + per-frame driver (reads menu state) ---
	cheats::install();

	samp::chat( "{33FF33}[Fuego Reversed] {FFFFFF}online -- {FFFF00}F2{FFFFFF} for menu, {FFFF00}/fuego_cmds{FFFFFF} for commands." );
	Log( "[init] core online." );
}

DWORD WINAPI init_thread( LPVOID ) {
	LogInit();
	crashlog::install(); // locate any crash in the log ASAP
	cfg::load();          // restore saved cheat toggles
	friends::load();      // restore friends list
	Log( "[init] Fuego Reversed loading (ASI)." );
	bring_up_subsystems();
	return 0;
}

} // namespace

BOOL APIENTRY DllMain( HMODULE hModule, DWORD reason, LPVOID ) {
	if ( reason == DLL_PROCESS_ATTACH ) {
		DisableThreadLibraryCalls( hModule );
		CreateThread( nullptr, 0, init_thread, nullptr, 0, nullptr );
	}
	return TRUE;
}
