#define _CRT_SECURE_NO_WARNINGS 1
#include "veh.h"
#include "../samp/samp.h"
#include "../gta/gtahost.h"
#include "../net/rpc.h"
#include "../core/trace.h"
#include "status.h"

#include <cstdio>

namespace veh {

namespace {
// Click-to-pick (Fuego's clickUnlock/clickDamageDoors: a picker mode + a click applies
// it). mode: 0 none, 1 unlock, 2 damage doors, 3 destroy.
int  g_pickMode  = 0;
bool g_pickArmed = false; // don't let the click that opened the picker count as the pick
const char *pick_verb( int m ) { return m == 1 ? "unlock" : m == 2 ? "damage the doors on" : "destroy"; }
} // namespace

// Begin a vehicle pick (called from the menu buttons). The menu closes itself so the
// world is clickable; the cursor stays up via menu's is_picking() check.
void start_pick( int action ) {
	traceLastFunc( "veh::start_pick" );
	g_pickMode  = action;
	g_pickArmed = false;
	char b[128];
	_snprintf( b, sizeof( b ) - 1, "{FFFF00}[veh] {FFFFFF}Click a vehicle to %s it. (Esc / right-click cancels.)", pick_verb( action ) );
	samp::chat( b );
}

bool is_picking() { return g_pickMode != 0; }

// Driven each frame by the menu with the ImGui cursor + input. Highlights the vehicle
// under the cursor and, on click, applies the pending action to it.
void run_picker( float curX, float curY, bool clicked, bool cancel ) {
	if ( g_pickMode == 0 ) return;
	if ( cancel ) { g_pickMode = 0; samp::chat( "{FF6347}[veh] {FFFFFF}Selection cancelled." ); return; }
	if ( !g_pickArmed ) { g_pickArmed = true; return; } // skip the opening-click frame

	int vid = game::vehicle_under_cursor( curX, curY );
	if ( vid < 0 ) { status::set( "Hover a vehicle to %s it...", pick_verb( g_pickMode ) ); return; }
	const char *vname = game::vehicle_model_name( vid ); // GTA model name (e.g. "Infernus"), null if none
	if ( vname ) status::set( "%s (id %d) -- click to %s it", vname, vid, pick_verb( g_pickMode ) );
	else         status::set( "Vehicle %d -- click to %s it", vid, pick_verb( g_pickMode ) );
	if ( !clicked ) return;

	int mode = g_pickMode;
	g_pickMode = 0; // consume
	const char *nm = vname ? vname : "vehicle";
	char b[160];
	if ( mode == 1 ) {
		game::unlock_vehicle_id( vid );
		_snprintf( b, sizeof( b ) - 1, "{33FF33}[veh] {FFFFFF}Unlocked %s (id %d).", nm, vid );
	} else if ( mode == 2 ) {
		rpc::damage_vehicle_doors( vid );
		_snprintf( b, sizeof( b ) - 1, "{33FF33}[veh] {FFFFFF}Broke the doors on %s (id %d) -- restream to sync.", nm, vid );
	} else {
		rpc::destroy_vehicle( vid );
		_snprintf( b, sizeof( b ) - 1, "{33FF33}[veh] {FFFFFF}Destroyed %s (id %d).", nm, vid );
	}
	samp::chat( b );
}

void unlock_vehicle() {
	traceLastFunc( "veh::unlock_vehicle" );
	if ( game::local_vehicle_samp_id() < 0 ) {
		samp::chat( "{FF6347}[veh] {FFFFFF}Get in a vehicle first." );
		return;
	}
	game::unlock_local_vehicle();
	samp::chat( "{33FF33}[veh] {FFFFFF}Vehicle unlocked." );
}

void destroy_doors() {
	traceLastFunc( "veh::destroy_doors" );
	int vid = game::local_vehicle_samp_id();
	if ( vid < 0 ) {
		samp::chat( "{FFFF00}[veh] {FFFFFF}Get in a car (or use /damagedoors <id>)." );
		return;
	}
	rpc::damage_vehicle_doors( vid );
	samp::chat( "{33FF33}[veh] {FFFFFF}Broke the doors -- restream (enter/exit) to sync." );
}

void attach_trailer() {
	traceLastFunc( "veh::attach_trailer" );
	samp::chat( "{FFFF00}[veh] {FFFFFF}Use /attveh <vehicle id> to tow a specific vehicle." );
}

} // namespace veh
