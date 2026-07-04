#define _CRT_SECURE_NO_WARNINGS 1
#include "trolls.h"

#include "../samp/samp.h"
#include "../samp/pools.h"
#include "../samp/structs.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"
#include "../gta/gtahost.h"
#include "../core/trace.h"
#include "cheats.h"   // cheats::freeze_sync_ms (hold onfoot so the troll sticks)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

constexpr int PLAYER_STATE_PASSENGER = 18;
constexpr int PLAYER_STATE_DRIVER    = 19;

float rnd_float( float lo, float hi ) {
	return lo + ( hi - lo ) * ( (float) rand() / (float) RAND_MAX );
}

// The base spoofed driver sync for a target (false if they aren't in a vehicle).
// Fuego 1:1: memset(0) then copy ONLY id/health/quaternion/position/movespeed, so the
// packet carries ZEROED keys/weapon/player-health/armor -- not the target's live bytes.
bool target_incar( int id, samp::stInCarData &out ) {
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( !rp || !rp->pPlayerData )
		return false;
	int state = rp->pPlayerData->bytePlayerState;
	if ( state != PLAYER_STATE_DRIVER && state != PLAYER_STATE_PASSENGER )
		return false;
	const samp::stInCarData &src = rp->pPlayerData->inCarData;
	memset( &out, 0, sizeof out );
	out.sVehicleID     = src.sVehicleID;
	out.fVehicleHealth = src.fVehicleHealth;
	out.fQuaternion[0] = src.fQuaternion[0]; out.fQuaternion[1] = src.fQuaternion[1];
	out.fQuaternion[2] = src.fQuaternion[2]; out.fQuaternion[3] = src.fQuaternion[3];
	out.fPosition[0]   = src.fPosition[0];  out.fPosition[1]  = src.fPosition[1];  out.fPosition[2]  = src.fPosition[2];
	out.fMoveSpeed[0]  = src.fMoveSpeed[0]; out.fMoveSpeed[1] = src.fMoveSpeed[1]; out.fMoveSpeed[2] = src.fMoveSpeed[2];
	return out.sVehicleID != 0;
}

// Pending driver-sync release state (Fuego's iTrollWorking/fuckupWorking hold).
samp::stInCarData g_pendingData{};
unsigned long     g_pendingTick   = 0;
bool              g_pendingActive = false;

int troll_ping() {
	return ( g_RakClient && g_RakClient->GetInterface() ) ? g_RakClient->GetInterface()->GetLastPing() : 100;
}

// SendFakeDriverFullSyncData (Fuego 1:1). If on foot: enter as PASSENGER (seat 1),
// send a passenger sync, and HOLD our on-foot/spectator sync for ping+20ms; on
// release (trolls::tick) we upgrade to DRIVER (seat 0) and send the mutated vehicle
// sync. If already in a vehicle: send the driver sync immediately (Fuego's fast path).
void send_fake_driver( samp::stInCarData *data, int targetId ) {
	if ( !data || !g_RakClient )
		return;
	if ( game::in_vehicle() ) {
		BitStream bs;
		bs.Write( (uint8_t) ID_VEHICLE_SYNC );
		bs.Write( (char *) data, sizeof( samp::stInCarData ) );
		g_RakClient->Send( &bs );
		return;
	}

	// On foot (Fuego 1:1): enter as PASSENGER (seat 1) + a passenger sync built from the
	// TARGET's real vehicle/position and OUR on-foot health bytes, then HOLD our player/
	// spectator sync until the ping-comp release upgrades us to driver.
	BitStream ev;
	ev.Write( data->sVehicleID );
	ev.Write<uint8_t>( 1 ); // seat 1 = passenger
	g_RakClient->RPC( RPC_EnterVehicle, &ev );

	samp::stLocalPlayer  *lp = samp::local_player();
	samp::stRemotePlayer *rp = samp::remote_player( targetId );
	samp::stPassengerData pd{};
	pd.byteArmor  = lp ? lp->onFootData.byteArmor  : 0;
	pd.byteHealth = lp ? lp->onFootData.byteHealth : 0;
	pd.byteSeatID = (uint8_t) game::vehicle_empty_seat( data->sVehicleID, false ); // Fuego one-arg default
	pd.sVehicleID = data->sVehicleID;
	if ( rp && rp->pPlayerData ) { // the target vehicle's real position (pre-mutation)
		pd.fPosition[0] = rp->pPlayerData->inCarData.fPosition[0];
		pd.fPosition[1] = rp->pPlayerData->inCarData.fPosition[1];
		pd.fPosition[2] = rp->pPlayerData->inCarData.fPosition[2];
	}
	BitStream ps;
	ps.Write( (uint8_t) ID_PASSENGER_SYNC );
	ps.Write( (char *) &pd, sizeof pd );
	g_RakClient->Send( &ps, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0 );

	g_pendingData   = *data;
	g_pendingTick   = GetTickCount() + (unsigned long) troll_ping() + 20;
	g_pendingActive = true;
	// Fuego drops player + spectator sync while working, i.e. until the ping+20 release.
	cheats::freeze_sync_ms( troll_ping() + 20, true, false, true );
}

void err( const char *m ) { samp::chat( m ); }

// Apply an iTroll mode's mutation to a spoofed in-car sync `d` (which already carries the
// target's vehicle id/health/quaternion/position/movespeed). Shared by the one-shot command
// and the continuous I-key rewrite. Returns false only on an unknown mode.
bool apply_itroll_mode( samp::stInCarData &d, int id, int mode ) {
	switch ( mode ) {
		case 1: { // stuck in the ground (Fuego: FindGroundZFor3DPosition of the target ped)
			float gz;
			if ( game::ped_ground_z( id, gz ) ) {
				d.fPosition[2]  = gz - 0.25f;
				d.fQuaternion[0] = 0.707107f; d.fQuaternion[1] = -0.707107f; d.fQuaternion[2] = 0; d.fQuaternion[3] = 0;
			}
			break;
		}
		case 2: { // speed up (fling along their facing)
			float rot = game::player_heading( id );
			d.fMoveSpeed[0] = sinf( -rot ) * 3.5f;
			d.fMoveSpeed[1] = cosf( -rot ) * 3.5f;
			break;
		}
		case 3: { // burn + throw down near me
			float me[3];
			if ( game::local_player_pos( me ) ) {
				d.fVehicleHealth = 0.0f;
				d.fPosition[0] = me[0] + rnd_float( 20, 25 );
				d.fPosition[1] = me[1] + rnd_float( 20, 25 );
				d.fPosition[2] = me[2] + rnd_float( 10, 15 );
				d.fMoveSpeed[2] = -rnd_float( 4, 10 );
			}
			break;
		}
		case 4: { // burn + throw up
			d.fVehicleHealth = 0.0f;
			d.fMoveSpeed[0] = rnd_float( 0, 2 );
			d.fMoveSpeed[1] = rnd_float( 0, 2 );
			d.fMoveSpeed[2] = rnd_float( 4, 10 );
			break;
		}
		case 5: { // spin: quaternion from the inverted vehicle matrix + inverted velocity
			float q[4];
			if ( game::vehicle_spin_quat( d.sVehicleID, q ) ) {
				d.fQuaternion[0] = q[0]; d.fQuaternion[1] = q[1];
				d.fQuaternion[2] = q[2]; d.fQuaternion[3] = q[3];
			}
			d.fMoveSpeed[0] = -d.fMoveSpeed[0];
			d.fMoveSpeed[1] = -d.fMoveSpeed[1];
			d.fMoveSpeed[2] = -d.fMoveSpeed[2];
			break;
		}
		case 6: { // send to ocean (Bermuda coords)
			d.fPosition[0] = 1088.58f;
			d.fPosition[1] = -2852.38f;
			d.fPosition[2] = -7.00f;
			break;
		}
		case 7: { // light speed up
			float rot = game::player_heading( id );
			d.fMoveSpeed[0] = sinf( -rot ) * 1.75f;
			d.fMoveSpeed[1] = cosf( -rot ) * 1.75f;
			break;
		}
		default:
			return false;
	}
	return true;
}

} // namespace

namespace trolls {

void fuckup( int id ) {
	traceLastFunc( "trolls::fuckup" );
	if ( !samp::is_connected( id ) ) { err( "{FF6347}[fuckup] {FFFFFF}Player not connected." ); return; }
	samp::stInCarData d;
	if ( !target_incar( id, d ) ) { err( "{FF6347}[fuckup] {FFFFFF}Target isn't in a vehicle." ); return; }

	d.fVehicleHealth = 0.0f;
	d.fQuaternion[0] = 0.807107f; d.fQuaternion[1] = -0.807107f; d.fQuaternion[2] = 0; d.fQuaternion[3] = 0;
	d.fMoveSpeed[0] = rnd_float( 0, 50 );
	d.fMoveSpeed[1] = rnd_float( 0, 50 );
	d.fMoveSpeed[2] = 50;
	send_fake_driver( &d, id );

	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF3333}[fuckup] {FFFFFF}Fucked %s up.", samp::player_name( id ).c_str() );
	samp::chat( b );
}

void itroll( int id, int mode ) {
	traceLastFunc( "trolls::itroll" );
	if ( !samp::is_connected( id ) ) { err( "{FF6347}[itroll] {FFFFFF}Player not connected." ); return; }
	samp::stInCarData d;
	if ( !target_incar( id, d ) ) { err( "{FF6347}[itroll] {FFFFFF}Target isn't in a vehicle." ); return; }
	if ( !apply_itroll_mode( d, id, mode ) ) {
		err( "{FFFF00}itroll modes: 1=ground 2=speed 3=down 4=up 5=spin 6=ocean 7=lightspeed" );
		return;
	}
	send_fake_driver( &d, id );

	char b[96];
	_snprintf( b, sizeof( b ) - 1, "{FF6D70}[itroll] {FFFFFF}Trolled %s (mode %d).", samp::player_name( id ).c_str(), mode );
	samp::chat( b );
}

// Continuous in-vehicle iTroll (Fuego I-key path): rewrite our live outgoing vehicle sync
// `d` in place into the target's car + the mode mutation. Our keys/weapon/player-health
// stay live (as Fuego does). false if the target isn't a driver/passenger or mode invalid.
bool itroll_incar_rewrite( int id, int mode, samp::stInCarData &d ) {
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( !rp || !rp->pPlayerData ) return false;
	int st = rp->pPlayerData->bytePlayerState;
	if ( st != PLAYER_STATE_DRIVER && st != PLAYER_STATE_PASSENGER ) return false;
	const samp::stInCarData &tc = rp->pPlayerData->inCarData;
	if ( tc.sVehicleID == 0 ) return false;
	d.sVehicleID     = tc.sVehicleID;
	d.fVehicleHealth = tc.fVehicleHealth;
	d.fQuaternion[0] = tc.fQuaternion[0]; d.fQuaternion[1] = tc.fQuaternion[1];
	d.fQuaternion[2] = tc.fQuaternion[2]; d.fQuaternion[3] = tc.fQuaternion[3];
	d.fPosition[0]   = tc.fPosition[0];   d.fPosition[1] = tc.fPosition[1];   d.fPosition[2] = tc.fPosition[2];
	d.fMoveSpeed[0]  = tc.fMoveSpeed[0];  d.fMoveSpeed[1] = tc.fMoveSpeed[1];  d.fMoveSpeed[2] = tc.fMoveSpeed[2];
	return apply_itroll_mode( d, id, mode );
}

// Per-frame release: once the ping-comp hold elapses, upgrade to driver (seat 0) and
// send the mutated vehicle sync (Fuego's OnSendPacket iTrollWorking/fuckupWorking).
void tick() {
	if ( !g_pendingActive || !g_RakClient )
		return;
	if ( GetTickCount() < g_pendingTick )
		return;
	g_pendingActive = false;
	BitStream ev;
	ev.Write( g_pendingData.sVehicleID );
	ev.Write<uint8_t>( 0 ); // upgrade to driver
	g_RakClient->RPC( RPC_EnterVehicle, &ev );
	BitStream bs;
	bs.Write( (uint8_t) ID_VEHICLE_SYNC );
	bs.Write( (char *) &g_pendingData, sizeof g_pendingData );
	g_RakClient->Send( &bs );
}

} // namespace trolls
