#define _CRT_SECURE_NO_WARNINGS 1
#include "commands.h"

#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../core/memory.h"
#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/config.h"
#include "../core/trace.h"
#include "../compat/compat.h"
#include "../gta/gtahost.h"
#include "../samp/pools.h"
#include "../net/rpc.h"
#include "../cheats/friends.h"
#include "../cheats/trolls.h"
#include "../cheats/veh.h"
#include "../cheats/cheats.h"
#include "../cheats/server.h"
#include "../recorder/recorder.h"
#include "../fishing/autofish.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
std::vector<const char *> g_visible; // names shown by /fuego_cmds
}

namespace cmd {

void add( const char *name, Handler handler, bool hidden ) {
	void *input = samp::input_ptr();
	const uint32_t b = samp::base();
	if ( !input || !b || !name || !handler )
		return;
	if ( !hidden )
		g_visible.push_back( name );
	// CInput::AddClientCommand(this=input, char* name, CMDPROC handler) __thiscall
	mem::thiscall<void>( b + samp::addr::FUNC_ADD_CLIENT_CMD, input, name, handler );
}

} // namespace cmd

// --- built-in commands ------------------------------------------------------

static void __cdecl cmd_compat( char * ) {
	compat::toggle_mode();
	cfg::save(); // persist the /compat preference so it survives restarts
	samp::chat( compat::in_037_mode()
		? "{FF0000}[COMPAT] {FFFFFF}Now joining as {FF0000}0.3.7{FFFFFF} -- reconnect to apply."
		: "{FF0000}[COMPAT] {FFFFFF}Now joining as {FF0000}0.3.DL{FFFFFF}." );
}

// /shotrepeater <n> -- sets the shot-repeater count (the bullet-sync resend
// cheat wired in cheats.cpp). Faithful port of Fuego cmd_shotrepeater.
static void __cdecl cmd_shotrepeater( char *params ) {
	if ( !params || params[0] == '\0' ) {
		samp::chat( "{FFFF00}USAGE: /shotrepeater <value> {FFFFFF}(0 = off, max 100)" );
		return;
	}
	int v = atoi( params );
	if ( v < 0 || v > 100 ) {
		samp::chat( "{FF6D70}Value invalid -- min 0, max 100." );
		return;
	}
	fx::g_set.shot_repeater = v;
	char buf[96];
	_snprintf( buf, sizeof( buf ) - 1, "{FF6D70}Shot Repeater -> {FFFFFF}%d", v );
	samp::chat( buf );
}

static void toggle_msg( const char *name, bool on ) {
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF6D70}%s: %s", name, on ? "{33FF33}ON" : "{FF3333}OFF" );
	samp::chat( b );
}

// --- cheat toggles (also on the F2 menu) ---
static void __cdecl cmd_godmode( char * ) { traceLastFunc( "cmd_godmode" ); fx::g_set.health_hack = !fx::g_set.health_hack; toggle_msg( "Health Hack", fx::g_set.health_hack ); }
static void __cdecl cmd_esp    ( char * ) { traceLastFunc( "cmd_esp" );     fx::g_set.player_esp   = !fx::g_set.player_esp;   toggle_msg( "Player ESP", fx::g_set.player_esp ); }
static void __cdecl cmd_aimbot ( char * ) { traceLastFunc( "cmd_aimbot" );  fx::g_set.silent_aim   = !fx::g_set.silent_aim;   toggle_msg( "Silent Aim", fx::g_set.silent_aim ); }
static void __cdecl cmd_invis  ( char * ) { traceLastFunc( "cmd_invis" );   fx::g_set.invisibility = !fx::g_set.invisibility; toggle_msg( "Invisibility", fx::g_set.invisibility ); }
static void __cdecl cmd_moonwalk(char * ) { traceLastFunc( "cmd_moonwalk" );fx::g_set.moonwalk     = !fx::g_set.moonwalk;     toggle_msg( "Moonwalk", fx::g_set.moonwalk ); }

static void __cdecl cmd_fakelag( char *params ) {
	traceLastFunc( "cmd_fakelag" );
	if ( params && params[0] ) fx::g_set.fake_lag_delay = atoi( params );
	fx::g_set.fake_lag = !fx::g_set.fake_lag;
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF6D70}Fake Lag: %s{FFFFFF} (%dms)", fx::g_set.fake_lag ? "{33FF33}ON" : "{FF3333}OFF", fx::g_set.fake_lag_delay );
	samp::chat( b );
}

// /heal -- restore local health via the game ped.
static void __cdecl cmd_heal( char * ) {
	traceLastFunc( "cmd_heal" );
	game::set_local_player_health( 100.0f );
	samp::chat( "{33FF33}[heal] {FFFFFF}Health restored." );
}

// /warp <id> -- teleport to a connected player (registered as "warp", Fuego 1:1).
static void __cdecl cmd_goto( char *params ) {
	traceLastFunc( "cmd_goto" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /warp <player id>" ); return; }
	int id = atoi( params );
	if ( !samp::is_connected( id ) ) { samp::chat( "{FF6347}[warp] {FFFFFF}That player isn't connected." ); return; }
	float pos[3];
	if ( !game::player_world_pos( id, pos ) ) { samp::chat( "{FF6347}[warp] {FFFFFF}No position for that player." ); return; }
	game::teleport_local( pos );
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}[warp] {FFFFFF}Warped to %s.", samp::player_name( id ).c_str() );
	samp::chat( b );
}

// /friend add|remove|list <name> -- manage the aim friend-filter list.
static void friend_list_cb( const char *n ) { char b[96]; _snprintf( b, sizeof( b ) - 1, "{FFFF00}  %s", n ); samp::chat( b ); }
static void __cdecl cmd_friend( char *params ) {
	traceLastFunc( "cmd_friend" );
	char sub[32] = { 0 }, name[96] = { 0 };
	int n = sscanf( params ? params : "", "%31s %95[^\t\n]", sub, name );
	if ( n < 1 ) { samp::chat( "{FFFF00}USAGE: /friend add|remove|list <name>" ); return; }
	if ( !strcmp( sub, "list" ) ) {
		char b[96];
		_snprintf( b, sizeof( b ) - 1, "{33FF33}Friends (%d):", friends::count() );
		samp::chat( b );
		friends::each( friend_list_cb );
		return;
	}
	if ( n < 2 ) { samp::chat( "{FFFF00}USAGE: /friend add|remove <name>" ); return; }
	char b[128];
	if ( !strcmp( sub, "add" ) ) {
		friends::add( name );
		_snprintf( b, sizeof( b ) - 1, "{33FF33}[friend] {FFFFFF}Added %s.", name );
	} else if ( !strcmp( sub, "remove" ) ) {
		friends::remove( name );
		_snprintf( b, sizeof( b ) - 1, "{FF6D70}[friend] {FFFFFF}Removed %s.", name );
	} else {
		_snprintf( b, sizeof( b ) - 1, "{FFFF00}USAGE: /friend add|remove|list <name>" );
	}
	samp::chat( b );
}

// /fuckup <id> -- fling the target's vehicle (spoofed driver sync).
static void __cdecl cmd_fuckup( char *params ) {
	traceLastFunc( "cmd_fuckup" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /fuckup <player id>" ); return; }
	trolls::fuckup( atoi( params ) );
}

// /itroll <id> <mode> -- vehicle troll (1=ground 2=speed 3=down 4=up 5=spin).
static void __cdecl cmd_itroll( char *params ) {
	traceLastFunc( "cmd_itroll" );
	int id = -1, mode = 0;
	if ( !params || sscanf( params, "%d %d", &id, &mode ) < 2 ) {
		samp::chat( "{FFFF00}USAGE: /itroll <player id> <mode 1-5>" );
		return;
	}
	trolls::itroll( id, mode );
}

// /kill <id> -- send lethal damage (explosion x3) via the GiveDamage RPC.
static void __cdecl cmd_kill( char *params ) {
	traceLastFunc( "cmd_kill" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /kill <player id>" ); return; }
	int id = atoi( params );
	if ( !samp::is_connected( id ) ) { samp::chat( "{FF6347}[kill] {FFFFFF}Not connected." ); return; }
	for ( int i = 0; i < 3; i++ )
		rpc::give_damage( id, 51 /*explosion, 82.5 dmg*/, 3 );
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF3333}[kill] {FFFFFF}Lethal damage sent to %s.", samp::player_name( id ).c_str() );
	samp::chat( b );
}

// /slap <id> -- a light hit via the GiveDamage RPC.
static void __cdecl cmd_slap( char *params ) {
	traceLastFunc( "cmd_slap" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /slap <player id>" ); return; }
	int id = atoi( params );
	if ( !samp::is_connected( id ) ) { samp::chat( "{FF6347}[slap] {FFFFFF}Not connected." ); return; }
	rpc::give_damage( id, 5 /*bat*/, 3 );
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF6D70}[slap] {FFFFFF}Slapped %s.", samp::player_name( id ).c_str() );
	samp::chat( b );
}

// /players -- list connected players (id: name), capped so it isn't spammy.
static void __cdecl cmd_players( char * ) {
	traceLastFunc( "cmd_players" );
	int shown = 0, total = 0;
	for ( int i = 0; i < 1004; i++ ) {
		if ( !samp::is_connected( i ) ) continue;
		total++;
		if ( shown < 24 ) {
			char b[128];
			_snprintf( b, sizeof( b ) - 1, "{FFFF00}%d{FFFFFF}: %s", i, samp::player_name( i ).c_str() );
			samp::chat( b );
			shown++;
		}
	}
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}[players] {FFFFFF}%d connected%s.", total, total > shown ? " (showing first 24)" : "" );
	samp::chat( b );
}

// /pos -- reports the local player's world position + health, read through the
// gta-reversed game layer (CPlayerPed::GetPosition / m_fHealth). Doubles as the
// proof that the gta-reversed integration is live.
static void __cdecl cmd_pos( char * ) {
	float p[3];
	if ( game::local_player_pos( p ) ) {
		char buf[128];
		_snprintf( buf, sizeof( buf ) - 1,
			"{33FF33}[pos] {FFFFFF}%.2f, %.2f, %.2f   {AAAAAA}hp %.0f/%.0f",
			p[0], p[1], p[2], game::local_player_health(), game::local_player_max_health() );
		samp::chat( buf );
	} else {
		samp::chat( "{FF6347}[pos] {FFFFFF}player ped not available yet." );
	}
}

// --- ported Fuego commands --------------------------------------------------

// /dualshot -- toggle the dual-shot cheat (1:1 cmd_dualshot).
static void __cdecl cmd_dualshot( char * ) {
	traceLastFunc( "cmd_dualshot" );
	fx::g_set.dual_shot = !fx::g_set.dual_shot;
	toggle_msg( "Dual Shot", fx::g_set.dual_shot );
}

// /dsdelay <ms> -- dual-shot resend delay (1:1 cmd_dualshot_delay).
static void __cdecl cmd_dsdelay( char *params ) {
	traceLastFunc( "cmd_dsdelay" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /dsdelay <ms>" ); return; }
	fx::g_set.dual_shot_delay = atoi( params );
	char b[80]; _snprintf( b, sizeof( b ) - 1, "{FF6D70}Dual Shot Delay -> {FFFFFF}%dms", fx::g_set.dual_shot_delay );
	samp::chat( b );
}

// /damagedoors <vehicle id> -- break a vehicle's doors (1:1 cmd_damagedoors).
static void __cdecl cmd_damagedoors( char *params ) {
	traceLastFunc( "cmd_damagedoors" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /damagedoors <vehicle id>" ); return; }
	rpc::damage_vehicle_doors( atoi( params ) );
	samp::chat( "{33FF33}[veh] {FFFFFF}Door-damage sent." );
}

// /.destroycar <vehicle id> -- destroy a vehicle (1:1 cmd_destroycar).
static void __cdecl cmd_destroycar( char *params ) {
	traceLastFunc( "cmd_destroycar" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /.destroycar <vehicle id>" ); return; }
	rpc::destroy_vehicle( atoi( params ) );
	samp::chat( "{33FF33}[veh] {FFFFFF}Vehicle-destroy sent." );
}

// /.destroyallcars -- server-side destroy every streamed vehicle (RPC_VehicleDestroyed).
static void __cdecl cmd_destroyallcars( char * ) {
	traceLastFunc( "cmd_destroyallcars" );
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp ) { samp::chat( "{FF6347}[veh] {FFFFFF}Vehicle pool not ready." ); return; }
	int myVid = game::local_vehicle_samp_id(), n = 0;
	for ( int v = 0; v < samp::SAMP_MAX_VEHICLES; v++ )
		if ( vp->iIsListed[v] == 1 && v != myVid ) { rpc::destroy_vehicle( v ); n++; }
	char b[64]; _snprintf( b, sizeof( b ) - 1, "{FF6347}[veh] {FFFFFF}Destroyed %d vehicles.", n );
	samp::chat( b );
}

// /.takedmgrpc <id> <amount> <weapon> -- make the server think that player damaged you
// for an explicit amount with an arbitrary weapon id (Fuego 1:1: illegal ids -> ban bait).
static void __cdecl cmd_takedmgrpc( char *params ) {
	traceLastFunc( "cmd_takedmgrpc" );
	int id = -1, amount = 0, weapon = 0;
	if ( !params || sscanf( params, "%d %d %d", &id, &amount, &weapon ) < 3 ) {
		samp::chat( "{FFFF00}/.takedmgrpc [PlayerID] [Amount] [WeaponID]" );
		samp::chat( "{FFFFFF}Try illegal weapon ids such as 38 to get a player banned." );
		return;
	}
	rpc::take_damage_amount( id, amount, weapon );
	samp::chat( "{33FF33}[dmg] {FFFFFF}TakeDamage RPC sent." );
}

// /unlockveh -- unlock the car you're in (1:1 cmd_unlockveh).
static void __cdecl cmd_unlockveh( char * ) { traceLastFunc( "cmd_unlockveh" ); veh::unlock_vehicle(); }

// /attveh <vehicle id> -- tow that streamed vehicle to the one you're driving (1:1 cmd_attachveh).
static void __cdecl cmd_attveh( char *params ) {
	traceLastFunc( "cmd_attveh" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /attveh <vehicle id>" ); return; }
	veh::attach_vehicle_id( atoi( params ) );
}

// /.sethp <hp> -- set the local player's health (1:1 cmd_sethpar, health only).
static void __cdecl cmd_sethp( char *params ) {
	traceLastFunc( "cmd_sethp" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /.sethp <health>" ); return; }
	game::set_local_player_health( (float) atoi( params ) );
	samp::chat( "{33FF33}[hp] {FFFFFF}Health set." );
}

// /switchwep <weapon id> -- change the local player's weapon (1:1 cmd_switchwep).
static void __cdecl cmd_switchwep( char *params ) {
	traceLastFunc( "cmd_switchwep" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /switchwep <weapon id 0-46>" ); return; }
	int w = atoi( params );
	if ( w < 0 || w > 46 ) { samp::chat( "{FF6347}Invalid weapon id!" ); return; }
	game::switch_weapon( w );
	samp::chat( "{33FF33}[wep] {FFFFFF}Weapon switched." );
}

// /toggle_big_hacks -- flip the "disable unsafe/big hacks" master safety.
static void __cdecl cmd_toggle_big_hacks( char * ) {
	traceLastFunc( "cmd_toggle_big_hacks" );
	fx::g_set.no_big_hacks = !fx::g_set.no_big_hacks;
	toggle_msg( "Disable Big Hacks", fx::g_set.no_big_hacks );
}

// /fr <name> -- quick friend-add (1:1 cmd_friendlist_add shorthand).
static void __cdecl cmd_fr( char *params ) {
	traceLastFunc( "cmd_fr" ); // bound to /friendlist_add (Fuego's /fr is freeze-troll)
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /friendlist_add <name>" ); return; }
	friends::add( params );
	char b[128]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[friend] {FFFFFF}Added %s.", params );
	samp::chat( b );
}

// /friendlist_reload -- reload the friends file from disk (1:1 cmd_friendlist_reload).
static void __cdecl cmd_friendlist_reload( char * ) {
	traceLastFunc( "cmd_friendlist_reload" );
	friends::load();
	samp::chat( "{33FF33}[friend] {FFFFFF}Friends list reloaded." );
}

// /friendlist_remove <name> -- remove a friend (1:1 cmd_friendlist_remove).
static void __cdecl cmd_friendlist_remove( char *params ) {
	traceLastFunc( "cmd_friendlist_remove" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /friendlist_remove <name>" ); return; }
	friends::remove( params );
	char b[128]; _snprintf( b, sizeof( b ) - 1, "{FF6D70}[friend] {FFFFFF}Removed %s.", params );
	samp::chat( b );
}

// /autouns -- toggle the LSRP auto-unscrambler (1:1 cmd_autounscrambler).
static void __cdecl cmd_autouns( char * ) {
	traceLastFunc( "cmd_autouns" );
	fx::g_set.auto_unscramble = !fx::g_set.auto_unscramble;
	samp::chat( fx::g_set.auto_unscramble ? "{33FF33}[autouns] {FFFFFF}Auto-unscrambler ON."
	                                      : "{FF6D70}[autouns] {FFFFFF}Auto-unscrambler OFF." );
}

// /autouns_reload -- reload the wordlist from disk (1:1 cmd_autounscrambler_reload).
static void __cdecl cmd_autouns_reload( char * ) {
	traceLastFunc( "cmd_autouns_reload" );
	server::reload_wordlist();
	samp::chat( "{33FF33}[autouns] {FFFFFF}Wordlist reloaded." );
}

// /autouns_delay <ms> -- set the submit delay (1:1 cmd_autounscrambler_delay).
static void __cdecl cmd_autouns_delay( char *params ) {
	traceLastFunc( "cmd_autouns_delay" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /autouns_delay <ms>" ); return; }
	fx::g_set.auto_unscramble_delay = atoi( params );
	char b[96]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[autouns] {FFFFFF}Submit delay = %d ms.", fx::g_set.auto_unscramble_delay );
	samp::chat( b );
}

// /pweps -- print the weapons you're currently carrying.
static void __cdecl cmd_pweps( char * ) {
	traceLastFunc( "cmd_pweps" );
	samp::stLocalPlayer *lp = samp::local_player();
	if ( !lp ) { samp::chat( "{FF6347}[weps] {FFFFFF}Not in-game." ); return; }
	samp::chat( "{33FF33}[weps] {FFFFFF}Your inventory:" );
	for ( int s = 0; s < 13; s++ ) {
		int w = lp->byteWeaponInventory[s];
		if ( w <= 0 ) continue;
		char b[96]; _snprintf( b, sizeof( b ) - 1, "{FFFF00}  slot %d{FFFFFF}: weapon %d (ammo %d)", s, w, lp->iWeaponAmmo[s] );
		samp::chat( b );
	}
}

// /.pmall <cmd> <message> -- send "<cmd> <id> <message>" to every connected player.
static void __cdecl cmd_pmall( char *params ) {
	traceLastFunc( "cmd_pmall" );
	char cmd[32] = { 0 }, msg[160] = { 0 };
	if ( !params || sscanf( params, "%31s %159[^\t\n]", cmd, msg ) < 2 ) {
		samp::chat( "{FFFF00}USAGE: /.pmall <cmd e.g. /pm> <message>" );
		return;
	}
	int sent = 0;
	for ( int i = 0; i < samp::SAMP_MAX_PLAYERS; i++ ) {
		if ( !samp::is_connected( i ) ) continue;
		char line[256];
		_snprintf( line, sizeof( line ) - 1, "%s %d %s", cmd, i, msg );
		rpc::say( line );
		sent++;
	}
	char b[64]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[pmall] {FFFFFF}Sent to %d players.", sent );
	samp::chat( b );
}

// /gmammo <amount> -- add ammo to the current weapon (1:1 cmd_gmammo).
static void __cdecl cmd_gmammo( char *params ) {
	traceLastFunc( "cmd_gmammo" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /gmammo <amount>" ); return; }
	game::give_ammo( atoi( params ) );
	samp::chat( "{33FF33}[ammo] {FFFFFF}Ammo added to current weapon." );
}

// /wordlist_add <word> -- append a word to the auto-unscrambler dictionary.
static void __cdecl cmd_wordlist_add( char *params ) {
	traceLastFunc( "cmd_wordlist_add" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /wordlist_add <word>" ); return; }
	FILE *f = fopen( "fuego_wordlist.txt", "a" );
	if ( f ) {
		fprintf( f, "%s\n", params );
		fclose( f );
		samp::chat( "{33FF33}[wordlist] {FFFFFF}Word added -- use /friendlist_reload style restart or rejoin to reload." );
	} else {
		samp::chat( "{FF6347}[wordlist] {FFFFFF}Could not open fuego_wordlist.txt." );
	}
}

// /fr <id> -- freeze a target driver's vehicle in place (you must be driving too).
static void __cdecl cmd_freeze( char *params ) {
	traceLastFunc( "cmd_freeze" );
	int id = ( params && params[0] ) ? atoi( params ) : -1;
	int r = cheats::freeze_troll( id );
	if ( r == 1 )       { char b[96]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[freeze] {FFFFFF}Freezing %s -- /fr to stop.", samp::player_name( id ).c_str() ); samp::chat( b ); }
	else if ( r == 0 )  { samp::chat( "{FF6D70}[freeze] {FFFFFF}Stopped." ); }
	else if ( r == -2 ) { samp::chat( "{FF6347}[freeze] {FFFFFF}You must be in a vehicle." ); }
	else                { samp::chat( "{FF6347}[freeze] {FFFFFF}Target must be driving a vehicle." ); }
}

// /.carcontrol (/.cc) <id> -- remote-drive a target's vehicle; run again to stop.
static void __cdecl cmd_carcontrol( char *params ) {
	traceLastFunc( "cmd_carcontrol" );
	int id = ( params && params[0] ) ? atoi( params ) : -1;
	int r = cheats::car_control( id );
	switch ( r ) {
	case 1: { char b[96]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[carcontrol] {FFFFFF}Controlling %s's vehicle -- /.cc to stop.", samp::player_name( id ).c_str() ); samp::chat( b ); break; }
	case 2:  samp::chat( "{33FF33}[carcontrol] {FFFFFF}Entering a nearby vehicle to start..." ); break;
	case 0:  samp::chat( "{FF6D70}[carcontrol] {FFFFFF}Stopped." ); break;
	case -2: samp::chat( "{FF6347}[carcontrol] {FFFFFF}Player not streamed / doesn't exist." ); break;
	case -4: samp::chat( "{FF6347}[carcontrol] {FFFFFF}Cannot find an empty vehicle to use carcontrol on foot." ); break;
	case -5: samp::chat( "{FF6347}[carcontrol] {FFFFFF}Target's vehicle handle is invalid." ); break;
	case -6: samp::chat( "{FF6347}[carcontrol] {FFFFFF}Target is not in any vehicle." ); break;
	case -3: break; // on-foot reset (handled silently)
	default: samp::chat( "{FFFF00}USAGE: /.cc <player id>" ); break; // -1 (not connected / no target)
	}
}

// /njkill <id> -- ninja-kill: jack an enemy driver out of their vehicle (1:1).
static void __cdecl cmd_njkill( char *params ) {
	traceLastFunc( "cmd_njkill" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /njkill <player id>" ); return; }
	int id = atoi( params );
	if ( !cheats::ninja_kill( id ) ) {
		samp::chat( "{FF6347}[njkill] {FFFFFF}Target isn't a driver / not streamed." );
		return;
	}
	samp::chat( "{FFFFFF}-> Trying to ninja-kill... {FFFF00}(hold position)" );
}

// /triggercp [ms] -- enter the active checkpoint or race checkpoint from where you are:
// one sync from its position at once, then your syncs stay there for [ms] (default 50,
// max 10 s). SA:MP has no "entered checkpoint" RPC -- the server fires the callback
// from your synced position -- so that's all it takes (Fuego 1:1).
static void __cdecl cmd_triggercp( char *params ) {
	traceLastFunc( "cmd_triggercp" );
	int delay = 50;
	if ( params && params[0] ) delay = atoi( params );
	if ( delay <= 0 ) delay = 50;
	if ( delay > 10000 ) delay = 10000;
	float pos[3] = { 0, 0, 0 };
	bool passenger = false;
	const int r = cheats::trigger_checkpoint( delay, pos, passenger );
	if ( r == 0 ) {
		samp::chat( "{FF6347}[triggercp] {FFFFFF}No active checkpoint right now." );
		return;
	}
	if ( r < 0 ) {
		samp::chat( "{FF6347}[triggercp] {FFFFFF}Not in game yet." );
		return;
	}
	char b[160] = {};
	_snprintf( b, sizeof( b ) - 1, "{FF6347}-> {FFFFFF}%s: sending packets for %.2fs at X: %.0f Y: %.0f Z: %.0f",
	           r == 2 ? "Race checkpoint" : "Checkpoint", delay / 1000.0f, pos[0], pos[1], pos[2] );
	samp::chat( b );
	if ( passenger )
		samp::chat( "{BBBBBB}As a passenger this might not work, try on foot or driving." );
}

// /.rec -- start/stop the packet recorder: everything in and out, decoded and
// time-stamped, to fuego_packets.log in the GTA folder (to work out a minigame).
static void __cdecl cmd_rec( char * ) {
	traceLastFunc( "cmd_rec" );
	char b[200] = {};
	if ( recorder::active() ) {
		const unsigned long long n = recorder::lines();
		const unsigned secs = recorder::seconds();
		recorder::stop();
		_snprintf( b, sizeof( b ) - 1, "{FF4040}[rec] {FFFFFF}Stopped after %u:%02u -- %llu lines in %s (GTA folder).",
		           secs / 60, secs % 60, n, recorder::file_name() );
		samp::chat( b );
	} else if ( recorder::start() ) {
		_snprintf( b, sizeof( b ) - 1, "{FF4040}[rec] {FFFFFF}Recording to %s. {AAAAAA}/.mark <note> marks a moment, /.rec stops.",
		           recorder::file_name() );
		samp::chat( b );
	} else {
		_snprintf( b, sizeof( b ) - 1, "{FF4040}[rec] {FFFFFF}Couldn't open %s -- is the GTA folder read-only?",
		           recorder::file_name() );
		samp::chat( b );
	}
}

// /.autofish -- Mike's CnR auto fish on/off (the same as the menu toggle).
static void __cdecl cmd_autofish( char * ) {
	traceLastFunc( "cmd_autofish" );
	autofish::set_enabled( !autofish::enabled() );
}

// /.mark [note] -- a numbered note in the recording at this moment ("line broke").
static void __cdecl cmd_mark( char *params ) {
	traceLastFunc( "cmd_mark" );
	if ( !recorder::active() ) {
		samp::chat( "{FF4040}[rec] {FFFFFF}Not recording -- /.rec starts it." );
		return;
	}
	recorder::mark( params ? params : "" );
	char b[96] = {};
	_snprintf( b, sizeof( b ) - 1, "{FF4040}[rec] {FFFFFF}Mark %d saved.", recorder::marks() );
	samp::chat( b );
}

// /.gethere <id> -- drag an on-foot player to you (drop your car on them + release).
static void __cdecl cmd_gethere( char *params ) {
	traceLastFunc( "cmd_gethere" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /.gethere <player id>" ); return; }
	int id = atoi( params );
	if ( !cheats::get_here( id ) ) {
		samp::chat( "{FF6347}[gethere] {FFFFFF}Need to be driving a vehicle + target streamed." );
		return;
	}
	char b[96]; _snprintf( b, sizeof( b ) - 1, "{FF6347}[gethere] {FFFFFF}Pulling %s...", samp::player_name( id ).c_str() );
	samp::chat( b );
}

// /.carpark <id> -- drop the car you're driving onto an on-foot player (1:1).
static void __cdecl cmd_carpark( char *params ) {
	traceLastFunc( "cmd_carpark" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /.carpark <player id>" ); return; }
	int id = atoi( params );
	if ( !samp::is_connected( id ) ) { samp::chat( "{FF6347}[carpark] {FFFFFF}Target not connected." ); return; }
	if ( !game::in_vehicle() )       { samp::chat( "{FF6347}[carpark] {FFFFFF}You aren't driving a vehicle." ); return; }
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( rp && rp->pPlayerData && rp->pPlayerData->bytePlayerState != 17 ) { // must be on foot
		samp::chat( "{FF6347}[carpark] {FFFFFF}Target is not on foot." ); return;
	}
	float tp[3];
	if ( !game::player_world_pos( id, tp ) ) { samp::chat( "{FF6347}[carpark] {FFFFFF}No target position." ); return; }
	// Drop from your car's real height + 3.0 (Fuego), so it lands on the victim.
	float dims[3]; int model = 0;
	int myVid = game::local_vehicle_samp_id();
	if ( myVid >= 0 && game::vehicle_dims_model( myVid, dims, model ) ) {
		tp[2] += ( dims[2] <= -0.1f ? -dims[2] : dims[2] ) + 3.0f;
		if ( model == 406 ) tp[2] += 1.35f; // Dumper
	} else {
		tp[2] += 4.0f;
	}
	game::teleport_local( tp );
	samp::chat( "{FF6347}[carpark] {FFFFFF}Car parked onto victim." );
}

// /fpslimiter -- toggle GTA's frame limiter (byte @0xBA6794). 1:1 cmd_fpslimiter.
static void __cdecl cmd_fpslimiter( char * ) {
	traceLastFunc( "cmd_fpslimiter" );
	BYTE *p = reinterpret_cast<BYTE *>( 0xBA6794 );
	*p = !*p;
	samp::chat( *p ? "{FFFFFF}Frame Limiter {00FF33}ON" : "{FFFFFF}Frame Limiter {FF0000}OFF" );
}

// /forcecrash -- deliberately crash your own client (Fuego cmd_forcecrash).
static void __cdecl cmd_forcecrash( char * ) {
	traceLastFunc( "cmd_forcecrash" );
	uint32_t b = samp::base();
	if ( b ) *reinterpret_cast<int *>( b ) = 65535; // corrupt samp.dll base -> crash
}

// /meleedmg <id> [1-4 preset | custom] -- send a specific melee damage (1:1).
static void __cdecl cmd_meleedmg( char *params ) {
	traceLastFunc( "cmd_meleedmg" );
	int id = -1, amount = 1;
	if ( !params || sscanf( params, "%d %d", &id, &amount ) < 1 ) {
		samp::chat( "{FFFF00}/meleedmg <id> [1-4 preset (1.32/1.98/5.28/6.6) | custom amount]" );
		return;
	}
	if ( !samp::is_connected( id ) ) { samp::chat( "{FF6347}[meleedmg] {FFFFFF}Player not connected." ); return; }
	float dmg;
	switch ( amount ) {
		case 1: dmg = 1.32f; break;
		case 2: dmg = 1.98f; break;
		case 3: dmg = 5.28f; break;
		case 4: dmg = 6.60f; break;
		default: dmg = (float) amount; break;
	}
	rpc::give_damage_amount( id, dmg, 0, 3 );
	samp::chat( "{33FF33}[meleedmg] {FFFFFF}Damage sent." );
}

// /.unfreezeme -- restore player control (Fuego cmd_unfreezeme).
static void __cdecl cmd_unfreezeme( char * ) {
	traceLastFunc( "cmd_unfreezeme" );
	game::make_player_controllable();
	samp::chat( "{33FF33}[unfreeze] {FFFFFF}Control restored." );
}

// /.desync -- toggle forced desync (pins the local drunk level to 0).
static void __cdecl cmd_desync( char * ) {
	traceLastFunc( "cmd_desync" );
	toggle_msg( "Desync", cheats::toggle_desync() );
}

// /slapkill <id> -- toggle continuously slap-killing a player (Fuego 1:1).
static void __cdecl cmd_slapkill( char *params ) {
	traceLastFunc( "cmd_slapkill" );
	if ( !params || !params[0] ) { cheats::slapkill( -1 ); samp::chat( "{FF6D70}[slapkill] {FFFFFF}Stopped." ); return; }
	int id = atoi( params );
	if ( cheats::slapkill( id ) ) {
		char b[96]; _snprintf( b, sizeof( b ) - 1, "{33FF33}[slapkill] {FFFFFF}Slap-killing %s -- /slapkill to stop.", samp::player_name( id ).c_str() );
		samp::chat( b );
	} else {
		samp::chat( "{FF6D70}[slapkill] {FFFFFF}Stopped (or invalid target)." );
	}
}

// /pveh <id> -- print a player's vehicle id + health (from their in-car sync).
static void __cdecl cmd_pveh( char *params ) {
	traceLastFunc( "cmd_pveh" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /pveh <player id>" ); return; }
	int id = atoi( params );
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( !rp || !rp->pPlayerData ) { samp::chat( "{FF6347}[pveh] {FFFFFF}Player not streamed." ); return; }
	int st = rp->pPlayerData->bytePlayerState;
	if ( st != 18 && st != 19 ) { samp::chat( "{FF6347}[pveh] {FFFFFF}Player is not in a vehicle." ); return; }
	char b[128];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}[pveh] {FFFFFF}%s -> vehicle %d, health %.0f",
		samp::player_name( id ).c_str(), rp->pPlayerData->inCarData.sVehicleID, rp->pPlayerData->inCarData.fVehicleHealth );
	samp::chat( b );
}

// /zominfect <id> -- warp onto a player attacking for 100ms (zombie servers).
static void __cdecl cmd_zominfect( char *params ) {
	traceLastFunc( "cmd_zominfect" );
	if ( !params || !params[0] ) { samp::chat( "{FFFF00}USAGE: /zominfect <player id>" ); return; }
	int id = atoi( params );
	if ( !cheats::zombie_infect( id ) ) { samp::chat( "{FF6347}[zombie] {FFFFFF}Invalid/unstreamed player." ); return; }
	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}[zombie] {FFFFFF}Infecting %s.", samp::player_name( id ).c_str() );
	samp::chat( b );
}

// /lsrp_stealguns <vid> [adjust true/false] [respawn true/false] -- LSRP gun stealer.
static void __cdecl cmd_lsrp_stealguns( char *params ) {
	traceLastFunc( "cmd_lsrp_stealguns" );
	int vid = -1; char adj[16] = { 0 }, resp[16] = { 0 };
	int n = sscanf( params ? params : "", "%d %15s %15s", &vid, adj, resp );
	if ( n < 1 ) {
		samp::chat( "{FFFF00}USAGE: /lsrp_stealguns <vehicle id> [adjust true/false] [respawn true/false]" );
		return;
	}
	bool forceAdjust = ( n >= 2 && !_stricmp( adj, "true" ) );
	bool respawn     = ( n >= 3 && !_stricmp( resp, "true" ) );
	server::lsrp_stealguns( vid, forceAdjust, respawn );
}

static void __cdecl cmd_fuego_cmds( char * ) {
	samp::chat( "{33FF33}Fuego Reversed {FFFFFF}commands:" );
	char line[256];
	line[0] = '\0';
	int n = 0;
	for ( const char *name : g_visible ) {
		char tmp[64];
		_snprintf( tmp, sizeof( tmp ) - 1, "{FFFF00}/%s  ", name );
		strncat( line, tmp, sizeof( line ) - strlen( line ) - 1 );
		if ( ++n % 4 == 0 ) { samp::chat( line ); line[0] = '\0'; }
	}
	if ( line[0] )
		samp::chat( line );
}

namespace cmd {

void install() {
	if ( !samp::input_ptr() ) {
		Log( "[cmd] input interface not ready; commands not registered." );
		return;
	}
	add( "compat",       cmd_compat,       false );
	add( "fuego_cmds",   cmd_fuego_cmds,   false );
	// Command names + triggers match Project Fuego EXACTLY (incl. dot prefixes) so they
	// don't shadow the server commands players rely on -- notably /fr = freeze-troll.
	add( "warp",             cmd_goto,             false );
	add( "fuckup",           cmd_fuckup,           false );
	add( "fr",               cmd_freeze,           false ); // Fuego: /fr = freeze-troll
	add( "shotrepeater",     cmd_shotrepeater,     false );
	add( "dualshot",         cmd_dualshot,         false );
	add( "dsdelay",          cmd_dsdelay,          false );
	add( "autouns",          cmd_autouns,          false );
	add( "autouns_reload",   cmd_autouns_reload,   false );
	add( "autouns_delay",    cmd_autouns_delay,    false );
	add( "wordlist_add",     cmd_wordlist_add,     false );
	add( "switchwep",        cmd_switchwep,        false );
	add( "friendlist_reload",cmd_friendlist_reload,false );
	add( "friendlist_add",   cmd_fr,               false );
	add( "friendlist_remove",cmd_friendlist_remove,false );
	add( "gmammo",           cmd_gmammo,           false );
	add( "toggle_big_hacks", cmd_toggle_big_hacks, false );
	add( "fpslimiter",       cmd_fpslimiter,       false );
	add( ".destroyallcars",  cmd_destroyallcars,   false );
	add( ".destroycar",      cmd_destroycar,       false );
	add( "pveh",             cmd_pveh,             false );
	add( ".sethp",           cmd_sethp,            false );
	add( "itroll",           cmd_itroll,           false );
	add( ".takedmgrpc",      cmd_takedmgrpc,       false );
	add( "forcecrash",       cmd_forcecrash,       false );
	add( "damagedoors",      cmd_damagedoors,      false );
	add( "slapkill",         cmd_slapkill,         false );
	add( ".gethere",         cmd_gethere,          false );
	add( ".pmall",           cmd_pmall,            false );
	add( ".desync",          cmd_desync,           false );
	add( ".unfreezeme",      cmd_unfreezeme,       false );
	add( ".cc",              cmd_carcontrol,       false );
	add( ".carcontrol",      cmd_carcontrol,       false );
	add( ".carpark",         cmd_carpark,          false );
	add( "zominfect",        cmd_zominfect,        true ); // hidden (Fuego)
	add( "attveh",           cmd_attveh,           false );
	add( "unlockveh",        cmd_unlockveh,        false );
	add( "lsrp_stealguns",   cmd_lsrp_stealguns,   false );
	add( "pweps",            cmd_pweps,            false );
	add( "njkill",           cmd_njkill,           false );
	add( "triggercp",        cmd_triggercp,        false );
	add( "meleedmg",         cmd_meleedmg,         false );
	add( ".rec",             cmd_rec,              false ); // packet recorder (debug)
	add( ".mark",            cmd_mark,             false );
	add( ".autofish",        cmd_autofish,         false ); // Mike's CnR auto fish (never /fish: that's the server's)
	Log( "[cmd] %zu commands registered.", g_visible.size() );
}

} // namespace cmd
