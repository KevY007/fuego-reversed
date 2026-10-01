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
#include "settings.h"
#include "input.h"
#include "../samp/samp.h"
#include "../net/net.h"
#include "../compat/compat.h"
#include "../commands/commands.h"
#include "../render/render.h"
#include "../menu/menu.h"
#include "../cheats/cheats.h"
#include "../cheats/friends.h"
#include "../i18n/config.h"
#include "../i18n/cache.h"
#include "../i18n/provider.h"
#include "../i18n/engine.h"
#include "../i18n/hooks.h"
#include "../i18n/outbound.h"
#include "../i18n/surfaces.h"
#include "../recorder/recorder.h"
#include "../fishing/autofish.h"

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

	// --- register every handler BEFORE the hooks go live ---
	// The game's main thread starts walking the net/render callback lists the moment
	// net::install() / render::install() swap our hooks in; appending to those lists
	// afterwards (from this thread) would race with that iteration.
	compat::set_037_mode( fx::g_set.compat_037 ); // restore the saved /compat preference
	compat::install();  // 0.3.7 layer (inert until /compat) + the incoming RPC-dispatch hook
	input::install();   // first frame/WndProc user: hotkey snapshot for everyone after it
	menu::install();    // in-house ImGui menu
	cheats::install();  // net-seam packet handlers + per-frame driver (reads menu state)

	// --- i18n: Phase 0 inbound chat translation (inert unless enabled) ---
	// Registered here, alongside compat/input/menu/cheats above (NOT after
	// render::install() below), for the same reason those are: hooks::install()
	// calls render::on_frame(pump_tick), which push_back()s onto render's
	// frameCbs vector. That must happen BEFORE render::install() makes
	// hkEndScene live and starts iterating that same vector every frame on
	// the game thread -- otherwise the push_back races a concurrent
	// iteration (vector realloc -> use-after-free/torn read -> crash), which
	// is exactly what the comment above compat::install() warns about.
	// samp::base()/is_in_game() are already guaranteed by this point (the
	// polling loops earlier in this function), and neither engine::start()
	// nor hooks::install() needs net:: or render:: to be installed first.
	// i18n settings are loaded as part of the UNIFIED cfg::load() (init_thread,
	// below) -- one config file, one Save button. Here we just load the on-disk
	// translation-memory cache, wire the chat seam ONCE (race-free, before
	// render::install()), then start the worker engine iff translation is on.
	// The translation memory is ONE file next to fuego_reversed.cfg/.log. The first
	// start after the rename moves (or merges) a good old fuego_i18n_tm.txt over, or
	// removes a damaged pre-v2 one and its .v1.bak, so no old file is left behind.
	{
		const i18n::cache::Migration m = i18n::cache::migrate_legacy( "fuego_i18n_tm.txt", "fuego_reversed_memory.txt" );
		if ( m.moved )
			Log( "[i18n] translation memory moved: fuego_i18n_tm.txt -> fuego_reversed_memory.txt." );
		if ( m.merged )
			Log( "[i18n] translation memory: merged the old fuego_i18n_tm.txt into fuego_reversed_memory.txt." );
		if ( m.removedOld || m.removedBak )
			Log( "[i18n] removed the old translation-memory file(s) from an earlier version "
			     "(translations are re-learned automatically)." );
	}
	i18n::cache::save_path( "fuego_reversed_memory.txt" );
	i18n::cache::load( "fuego_reversed_memory.txt" );
	i18n::hooks::install();
	i18n::outbound::install(); // register the outgoing-chat seam BEFORE net::install() below
	i18n::surfaces::install(); // register the DialogResponse handler (list remap + input translation; same, pre net::install)
	i18n::hooks::apply_config();

	// Packet recorder (debug, idle until /.rec): its wire taps + per-frame summary,
	// registered before net::install() / render::install() like everything above.
	recorder::install();
	// Mike's CnR auto fish (idle until turned on): its sync seam + per-frame driver.
	autofish::install();
	// Guards for known GTA crashes: patched on the first frame, on the render thread
	// (they patch code that thread runs -- never from this one).
	render::on_frame( []( IDirect3DDevice9 * ) { crashlog::install_guards(); } );

	// --- networking / RakNet hook (accurate) ---
	// Poll until SA:MP's RakClient interface is live, then swap in our hook.
	int ni = 0;
	while ( !net::install() && ni++ < 200 )
		Sleep( 100 );
	Log( "[init] networking hook: %s", net::installed() ? "ACTIVE" : "NOT INSTALLED (samp iface not ready)" );

	// --- client chat commands (/compat, /fuego_cmds, ...) ---
	cmd::install();

	// --- overlay: D3D9 render hook (menu + per-frame cheats ride on it) ---
	// Poll until GTA's D3D9 device is live (0xC97C28), then hook it.
	int ri = 0;
	while ( !render::install() && ri++ < 100 )
		Sleep( 100 );
	Log( render::installed() ? "[init] overlay online (F2 for menu)."
	                         : "[init] WARNING: render hook failed; menu unavailable." );
	if ( render::installed() )
		render::set_max_fullscreen_refresh( fx::g_set.fullscreen_max_hz ); // restore saved preference

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
