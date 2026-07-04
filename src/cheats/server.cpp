#define _CRT_SECURE_NO_WARNINGS 1
#include "server.h"

#include "../samp/samp_sdk.h"    // textdraw_pool, is_connected, chat
#include "../samp/pools.h"       // vehicle_pool, local_player
#include "../samp/structs.h"     // stPassengerData
#include "../net/rpc.h"          // rpc::say / request_spawn / spawn
#include "../net/RakClient.h"    // g_RakClient (Send / RPC / GetLastPing)
#include "../net/BitStream.h"
#include "../gta/gtahost.h"      // set_local_player_health, vehicle_empty_seat, vehicle_live_data
#include "../core/settings.h"
#include "../core/trace.h"
#include "status.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

namespace {

int ping() {
	if ( g_RakClient && g_RakClient->GetInterface() )
		return g_RakClient->GetInterface()->GetLastPing();
	return 100;
}

// printf-style /command sender (Fuego's say(...)).
void sayf( const char *fmt, ... ) {
	char b[144];
	va_list ap; va_start( ap, fmt );
	_vsnprintf( b, sizeof( b ) - 1, fmt, ap );
	va_end( ap );
	b[sizeof( b ) - 1] = 0;
	rpc::say( b );
}

// ---------------------------------------------------------------- LSRP gun stealer
bool          sg_working      = false;
int           sg_vid          = -1;
int           sg_stage        = 0;
bool          sg_forceAdjust  = false;
bool          sg_forceWaiting = false;
bool          sg_respawn      = false;
unsigned long sg_lastAction   = 0;
// Weapon objects attached to each vehicle (tracked from RPC_CreateObject/Destroy), so
// force-adjust can re-emit them onto the ped -- Fuego's lsrpStealGuns_VehicleObjects.
std::vector<int> sg_vehObjects[samp::SAMP_MAX_VEHICLES];

void handle_stealguns() {
	if ( !sg_working || !g_RakClient ) return;
	unsigned long now = GetTickCount();

	if ( sg_forceAdjust ) {
		switch ( sg_stage ) {
			case 0: case 1: case 2: case 3: {
				if ( !sg_forceWaiting && ( sg_lastAction + ping() ) < now ) {
					if ( sg_stage == 0 ) { sayf( "/taw" ); sayf( "/tad" ); }
					sayf( "/adjustgun %d", sg_stage + 1 );
					sg_forceWaiting = true;
					sg_lastAction   = now;
				} else if ( sg_forceWaiting && ( now - ( ping() + 100 ) > sg_lastAction ) ) {
					// no gun to adjust in this slot -> move on
					sg_forceWaiting = false;
					sg_stage++;
					sg_lastAction = now;
					status::set( "Gun Stealer: slot %d timeout, next...", sg_stage );
				}
				break;
			}
			case 4: {
				sayf( "/takegun 1" ); sayf( "/takegun 2" ); sayf( "/takegun 3" ); sayf( "/takegun 4" );
				sg_lastAction   = now + 25;
				sg_forceWaiting = false;
				sg_forceAdjust  = false;
				sg_stage        = 1;
				status::set( "Gun Stealer: force-adjust takegun..." );
				break;
			}
		}
		return;
	}

	switch ( sg_stage ) {
		case 0:
			if ( ( sg_lastAction + ping() + 25 ) < now ) {
				sayf( "/takegun 1" ); sayf( "/takegun 2" ); sayf( "/takegun 3" ); sayf( "/takegun 4" );
				sayf( "/tad" ); sayf( "/taw" );
				sg_lastAction = now + 25;
				sg_stage++;
			}
			break;
		case 1:
			if ( ( sg_lastAction + ping() + 25 ) < now ) {
				sg_working    = false;
				sg_lastAction = now;
				samp::chat( "{33FF33}Gun Stealer -> {FFFFFF}Steal attempt completed." );
				if ( sg_respawn ) { // Fuego: SetHealth(0) + SendSpawn -- rpc::spawn() already
					game::set_local_player_health( 0.0f ); // sends RPC_RequestSpawn + RPC_Spawn.
					rpc::spawn();
				}
			}
			break;
	}
}

// -------------------------------------------------------------- LSRP auto-unscrambler
bool          uns_loaded  = false;
bool          uns_work    = false;
unsigned long uns_newTick = 0;
std::string   uns_solved;
std::string   uns_lastWord;
std::vector<std::string> wordList;

std::string extract_between( const char *s, const char *a, const char *b ) {
	const char *p = strstr( s, a ); if ( !p ) return "";
	p += strlen( a );
	const char *q = strstr( p, b ); if ( !q ) return "";
	return std::string( p, (size_t) ( q - p ) );
}

// sorted-letter key for anagram comparison
std::string sortedKey( std::string w ) { std::sort( w.begin(), w.end() ); return w; }

void handle_unscrambler() {
	if ( !uns_loaded ) {
		uns_loaded = true;
		wordList.clear();
		std::ifstream in( "fuego_wordlist.txt" );
		if ( in.is_open() ) {
			std::string w;
			while ( in >> w ) wordList.push_back( w );
		} else {
			std::ofstream mk( "fuego_wordlist.txt" );
			if ( mk.is_open() ) mk << "[dont_remove_this]\n";
		}
	}

	unsigned long now = GetTickCount();

	// queued solution -> submit it after the delay
	if ( uns_work && now > uns_newTick ) {
		sayf( "/uns %s", uns_solved.c_str() );
		uns_work   = false;
		uns_solved = "";
	}

	if ( !fx::g_set.auto_unscramble || uns_work ) return;

	samp::stTextdrawPool *tp = samp::textdraw_pool();
	if ( !tp ) return;

	for ( int i = 0; i < 255; i++ ) {
		samp::stTextdraw *td = tp->playerTextdraw[i];
		if ( !td || IsBadReadPtr( td, sizeof( void * ) ) ) continue;
		const char *txt = td->szText;
		if ( !txt || IsBadReadPtr( (void *) txt, 3 ) || strnlen( txt, 800 ) < 3 ) continue;
		if ( !strstr( txt, " to unscramble the word.~n~\"~w~" ) ) continue;

		std::string scrambled = extract_between( txt, "\"~w~", "~r~\"" );
		if ( scrambled.length() <= 2 || uns_lastWord == scrambled ) continue;
		uns_lastWord = scrambled;

		std::string key = sortedKey( scrambled );
		for ( const std::string &w : wordList ) {
			if ( w.length() == scrambled.length() && sortedKey( w ) == key ) {
				uns_solved  = w;
				uns_newTick = now + ( fx::g_set.auto_unscramble_delay > 0 ? fx::g_set.auto_unscramble_delay : 1250 );
				uns_work    = true;
				status::set( "Unscrambled: %s", w.c_str() );
				break;
			}
		}
	}
}

} // namespace

namespace server {

void lsrp_stealguns( int vid, bool forceAdjust, bool respawn ) {
	traceLastFunc( "server::lsrp_stealguns" );
	if ( !g_RakClient ) return;
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp || vid < 0 || vid >= samp::SAMP_MAX_VEHICLES || !vp->iIsListed[vid] ) {
		samp::chat( "{FF6347}[stealguns] {FFFFFF}Vehicle does not exist / not streamed." );
		return;
	}
	sg_forceAdjust  = forceAdjust;
	sg_respawn      = respawn;
	sg_forceWaiting = false;
	sg_stage        = 0;
	sg_vid          = vid;
	sg_lastAction   = GetTickCount();
	sg_working      = true;

	BitStream bs;
	bs.Write( vid );
	bs.Write<uint8_t>( 1 ); // enter as passenger
	g_RakClient->RPC( RPC_EnterVehicle, &bs );

	// Passenger sync to establish occupancy (Fuego 1:1: local hp/armor, weapon 0,
	// empty passenger seat w/ fallback 1, target vehicle position, UNRELIABLE_SEQUENCED).
	float shp = 0, sar = 0; game::local_health_armor( shp, sar );
	int seat = game::vehicle_empty_seat( vid, true );
	samp::stPassengerData pd; memset( &pd, 0, sizeof pd );
	pd.byteArmor         = (uint8_t) sar;
	pd.byteCurrentWeapon = 0;
	pd.byteHealth        = (uint8_t) shp;
	pd.byteSeatID        = (uint8_t) ( seat == -1 ? 1 : seat );
	pd.sVehicleID        = (uint16_t) vid;
	float vpos[3], vspd[3], vhp; if ( game::vehicle_live_data( vid, vpos, vspd, vhp ) ) {
		pd.fPosition[0] = vpos[0]; pd.fPosition[1] = vpos[1]; pd.fPosition[2] = vpos[2];
	}
	BitStream ps;
	ps.Write( (uint8_t) ID_PASSENGER_SYNC );
	ps.Write( (char *) &pd, sizeof pd );
	g_RakClient->Send( &ps, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0 );

	char b[80];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}Gun Stealer -> {FFFFFF}Started on vehicle %d.", vid );
	samp::chat( b );
}

void tick() {
	handle_stealguns();
	handle_unscrambler();
}

bool stealguns_active() { return sg_working; }

void reload_wordlist() {
	uns_loaded = false; // handle_unscrambler re-reads the file on its next tick
	uns_lastWord = "";
}

// --- Force-adjust object tracking (Fuego 1:1) --------------------------------------
void stealguns_track_object( int objId, int vid ) {
	if ( vid < 0 || vid >= samp::SAMP_MAX_VEHICLES ) return;
	std::vector<int> &v = sg_vehObjects[vid];
	if ( std::find( v.begin(), v.end(), objId ) == v.end() )
		v.push_back( objId );
}

void stealguns_clear_vehicle( int vid ) {
	if ( vid < 0 || vid >= samp::SAMP_MAX_VEHICLES ) return;
	sg_vehObjects[vid].clear();
}

void stealguns_untrack_object( int objId ) {
	if ( sg_working ) return; // Fuego only prunes when not working
	for ( int i = 0; i < samp::SAMP_MAX_VEHICLES; i++ ) {
		std::vector<int> &v = sg_vehObjects[i];
		auto it = std::remove( v.begin(), v.end(), objId );
		if ( it != v.end() ) { v.erase( it, v.end() ); break; }
	}
}

// On an incoming RPC_EditObject while force-adjusting: re-emit EditObject for every gun
// object tracked on the target vehicle, snapping them onto the ped, then advance the stage.
// Returns true to SWALLOW the server's EditObject (Fuego returns after re-emitting).
bool stealguns_on_editobject( bool playerObject ) {
	if ( !( sg_working && sg_forceAdjust ) || !g_RakClient ) return false; // pass through
	if ( sg_vid < 0 || sg_vid >= samp::SAMP_MAX_VEHICLES )   return true;   // active -> still swallow
	float tp[3];
	if ( !game::stealguns_target_pos( sg_vid, tp ) )         return true;
	for ( int objId : sg_vehObjects[sg_vid] ) {
		BitStream bs;
		bs.Write( playerObject );
		bs.Write( (uint16_t) objId );
		bs.Write( (uint32_t) 1 );
		bs.Write( tp[0] ); bs.Write( tp[1] ); bs.Write( tp[2] );
		bs.Write( 0.0f );  bs.Write( 0.0f );  bs.Write( 0.0f );
		g_RakClient->RPC( RPC_EditObject, &bs );
	}
	sg_stage++;
	sg_forceWaiting = false;
	char b[80];
	_snprintf( b, sizeof( b ) - 1, "{33FF33}Gun Stealer -> {FFFFFF}Weapon in slot [%d] adjusted.", sg_stage );
	samp::chat( b );
	return true;
}

} // namespace server
