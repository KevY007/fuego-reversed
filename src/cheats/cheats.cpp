#include "cheats.h"

#include "../core/settings.h"
#include "../core/logger.h"
#include "../core/trace.h"
#include "../net/net.h"
#include "../net/BitStream.h"
#include "../net/RakClient.h"   // g_RakClient (the saved real interface)
#include "../render/render.h"
#include "../samp/structs.h"
#include "../gta/gtahost.h"     // game:: accessors (via gta-reversed)
#include "../samp/samp.h"       // samp::chat
#include "../samp/pools.h"      // is_connected / player_name / SAMP_MAX_PLAYERS
#include "../net/rpc.h"         // rpc::give_damage (trigger bot)
#include "../menu/menu.h"       // menu::is_open (killer menu guard)
#include "friends.h"            // friends::is_friend (killer/slapper filter)
#include "trolls.h"             // trolls::itroll (menu iTroll hotkey)
#include "server.h"             // server::tick (LSRP automations)
#include "chams.h"              // chams::install (DrawIndexedPrimitive hook)
#include "status.h"             // status::set (1:1 active-cheat feedback)
#include "../core/input.h"      // Fuego keyhook semantics (gated: not while typing/alt-tabbed)
#include "../core/memory.h"     // mem::write (DisableFlyingBirds RET-patch)
#include "veh.h"                // veh::is_picking (cursor-up gating, like Fuego's g_iCursorEnabled)

#include <windows.h>
#include <cmath>
#include <deque>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {

using namespace samp;

constexpr double kPi = 3.14159265358979323846;

// --- Fake lag (Fuego 1:1: store-and-forward, NOT a freeze). Outgoing player /
//     vehicle sync is captured and held in a queue; each held packet is actually
//     RELEASED to the server `delay` ms later. So you really do move for others --
//     just late -- which is why it rubber-bands like genuine lag instead of just
//     standing still. `g_lagLastSentPos` is where the last *released* packet put
//     us (= where others currently see you); the overlay draws to it. Holding
//     BACKSPACE snaps your local ped back there (the "backtrack"). ---
struct HeldSync {
	uint8_t           id;             // ID_PLAYER_SYNC / ID_VEHICLE_SYNC
	uint8_t           bytes[512];     // raw sync payload (after the id byte)
	int               len;            // payload length in bytes
	unsigned long     executeTick;    // GetTickCount() at/after which to release
	float             pos[3];         // world position this packet reports (payload)
	PacketPriority    priority;       // replayed with the exact original send params
	PacketReliability reliability;
	char              channel;
	// Snapshot of the live ped/vehicle transform at enqueue (for the BACKSPACE backtrack).
	float             mat[12];        // [right.xyz, forward.xyz, up.xyz, pos.xyz]
	float             gameSpeed[3];   // live movespeed
	bool              inVehicle;      // captured from a vehicle vs on foot
};

std::mutex           g_lagMx;
std::deque<HeldSync> g_lagQueue;
float                g_lagLastSentPos[3]  = { 0, 0, 0 };
unsigned long        g_lagLastSentTick    = 0;   // g_lagLastSentPos valid until here
unsigned long        g_lagBackHoldTick    = 0;   // BACKSPACE debounce
float                g_lagBTLeft          = 0.5f; // backtrack countdown (seconds)
HeldSync             g_lagLastSent{};        // last released sync (backtrack restore)
unsigned long        g_lagBtReapplyAt     = 0;   // backTrackSetVehicle (+200ms) deadline

// --- Dual shot (Fuego 1:1): stash the bullet we just fired and re-fire it once
//     `dual_shot_delay` ms later, so one trigger pull registers as two hits. ---
std::mutex    g_dShotMx;
stBulletData  g_dShotData{};
bool          g_dShotReady = false;
unsigned long g_dShotTick  = 0;

// --- Zombie infect (Fuego 1:1): for 100ms after /zominfect, warp our on-foot
//     sync onto the target with attack keys held (infect them on zombie servers). ---
int           g_zomInfectID   = -1;
unsigned long g_zomInfectTick  = 0;

// --- Desync (Fuego forceDesync): pin the local SA:MP ped's drunk level to 0. ---
bool g_desync = false;

// --- Slap kill (Fuego 1:1): continuously warp our on-foot sync onto a target with
//     a knockback impulse until they die or stream out. ---
int g_slapkillTarget = -1;

// iTroll (I-key) target, re-resolved each frame in on_frame while I is held (Fuego's
// iTroll_Target / targetfinder). Used by the continuous in-vehicle rewrite below.
int g_itrollTarget = -1;

// Slapper ESP: last slapped position + freshness (Fuego slapperPos/slapperLastUpdate).
float         g_slapperPos[3] = { 0, 0, 0 };
unsigned long g_slapperTick   = 0;

// --- Get here (Fuego 1:1): drop our vehicle sync onto a target, then FREEZE our
//     outgoing vehicle sync there until the release tick (dragging them to us). ---
unsigned long g_getHereRelease = 0; // 0 = inactive

// --- Trigger checkpoint (Fuego 1:1): replay our on-foot sync at the last checkpoint
//     position (captured from the incoming RPC_SetCheckpoint) for a short window. ---
float         g_lastCheckpoint[3] = { 0, 0, 0 };
unsigned long g_checkpointTick    = 0;

// --- Car control (Fuego /cc 1:1): warp our ped INTO the target's car so the
//     game itself emits that car's in-car sync (we drive their vehicle), then
//     warp back out + restore on release. The server's kick-from-car RPCs are
//     swallowed in compat during the possession + a 1s grace. State mirrors
//     Fuego's carControl_* globals exactly. ---
float         g_ccLastPos[3]  = { 0, 0, 0 }; // where we were before possessing (restore point)
int           g_ccLastVehSeat = 0;           // our seat in our OLD car (0 driver, 1 passenger)
int           g_ccLastVeh     = -1;          // SA:MP id of our OLD car; -1 = inactive (Fuego NULL)
unsigned long g_ccLastUse     = 0;           // GetTickCount()+1000 grace (gates the incoming block)
bool          g_ccUsedOnfoot  = false;       // started on foot (entered a nearby empty car first)
// Delayed steps (Fuego runs these on detached threads; we tick them on the main thread):
unsigned long g_ccReapplyAt   = 0; int g_ccReapplyVid = -1; // carControlSetVehicle (Sleep 100)
unsigned long g_ccRetryAt     = 0; int g_ccRetryId    = -1; // ccAgainWait re-call (Sleep 500)
unsigned long g_ccResetAt     = 0;                          // carControlResetOnfootMode (Sleep 500)
unsigned long g_ccNocolsAt    = 0; int g_ccNocolsVid = -1;  // DisableNoCols (Sleep 1500) -> un-proof

// --- Freeze troll (Fuego 1:1): while you're in a vehicle, keep re-sending the
//     target driver's car sync at a fixed position with zero velocity (freezes it). ---
int   g_freezeTarget = -1;
float g_freezePos[3] = { 0, 0, 0 };

// --- Sync freeze (the prerequisite the held-state cheats need): while a freeze is
//     live, the GAME's outgoing sync is DROPPED so it can't overwrite the spoofed
//     position a cheat just sent (njkill's jack pose, an itroll driver sync, ...).
//     mask bits: 1=player sync, 2=vehicle sync, 4=spectator sync. njkill also opens
//     an incoming block window (below) so the server can't yank us out mid-move. ---
unsigned long g_freezeTick = 0;
int           g_freezeMask = 0;
unsigned long g_njkillTick = 0; // njkill's 3s window (also gates the incoming block)

// No-spread state (Fuego): go = spread killed this frame; triggers = bullet counter.
bool g_nospreadGo = false;
int  g_nospreadTriggers = 0;

// Silent-aim letGo: true when the game itself just scored a player hit -> keep the
// game's own GiveDamage instead of dropping it.
bool g_silentLetGo = false;

// --- Streamed-out player positions (captured from the incoming ID_MARKERS_SYNC)
//     so ESP can show DISTANT / off-screen players, not just streamed-in ones. ---
float         g_markerPos[SAMP_MAX_PLAYERS][3] = {};
unsigned long g_markerTick[SAMP_MAX_PLAYERS]   = {};

// Read the on-foot sync out of the packet, hand it to fn() to mutate, write it
// back. Matches Fuego's ResetReadPointer/Read/ResetWritePointer/Write dance
// exactly (the sync payload is byte-aligned right after the 1-byte packet id).
template <class Fn>
void rewrite_onfoot( BitStream *p, Fn fn ) {
	stOnFootData d{};
	uint8_t id;
	p->ResetReadPointer();
	p->Read( id );
	p->Read( (char *)&d, sizeof d );
	fn( d );
	p->ResetWritePointer();
	p->Write( id );
	p->Write( (char *)&d, sizeof d );
}

template <class Fn>
void rewrite_incar( BitStream *p, Fn fn ) {
	stInCarData d{};
	uint8_t id;
	p->ResetReadPointer();
	p->Read( id );
	p->Read( (char *)&d, sizeof d );
	fn( d );
	p->ResetWritePointer();
	p->Write( id );
	p->Write( (char *)&d, sizeof d );
}

// Invert the yaw quaternion's w term -> ped faces backward while walking forward
// (the "moonwalk" look). Ported from Fuego cheat_samp.cpp.
float moonwalk_quat_w( float w ) {
	double angle       = 2.0 * acos( (double)w );
	double invertedRad = angle - kPi;
	return (float)cos( invertedRad / 2.0 );
}

// A player the slappers should skip: dead/downed (Fuego sampFindNearestPlayer checkdead --
// GTA hp < 0.9, SA:MP synced health < 0.9, an injured/KO/dead anim, or a NONE/WASTED state).
bool player_dead_or_downed( int id ) {
	if ( game::player_gta_health( id ) < 0.9f ) return true;
	float sh = samp::player_synced_health( id );
	if ( sh >= 0.0f && sh < 0.9f ) return true;
	if ( game::player_playing_anim( id, "SWEET_INJUREDLOOP" ) ||
	     game::player_playing_anim( id, "CS_DEAD_GUY" ) ||
	     game::player_playing_anim( id, "KO_SKID_FRONT" ) ||
	     game::player_playing_anim( id, "KO_SKID_BACK" ) )
		return true;
	samp::stRemotePlayer *rp = samp::remote_player( id );
	int st = ( rp && rp->pPlayerData ) ? rp->pPlayerData->bytePlayerState : 0;
	return st == PLAYER_STATE_NONE || st == PLAYER_STATE_WASTED;
}

// Nearest streamed player to us. friendFilter skips friends; skipDead skips dead/downed
// players (Fuego's near-slapper passes checkdead=true so it doesn't keep hitting corpses).
int find_nearest_player( bool friendFilter, bool skipDead = false ) {
	int best = -1; float bestDist = 1e18f;
	for ( int id = 0; id < SAMP_MAX_PLAYERS; id++ ) {
		if ( !samp::is_connected( id ) || !game::player_streamed( id ) ) continue;
		if ( friendFilter && friends::is_friend( samp::player_name( id ) ) ) continue;
		if ( skipDead && player_dead_or_downed( id ) ) continue;
		float p[3];
		if ( !game::player_world_pos( id, p ) ) continue;
		float d = game::distance_to( p );
		if ( d >= 0 && d < bestDist ) { bestDist = d; best = id; }
	}
	return best;
}

// --- outgoing packet interception (net seam). Return false to DROP. ----------
// Faithful port of Fuego OnSendPacket(): fake-lag drop gate first, then the
// sync-rewrite cheats (each self-contained, re-reading the packet id like Fuego).
bool on_send_packet( BitStream *params, PacketPriority priority, PacketReliability reliability, char orderingChannel ) {
	auto &set = fx::g_set;
	if ( set.cheat_panic )
		return true; // master "hide everything": never touch traffic

	uint8_t packetId;
	params->ResetReadPointer();
	params->Read( packetId );

	// Teleportish / onceperms (Fuego): while fake-lag "Teleportish Movement" is on, let
	// only ONE movement sync through per delay window and drop the rest -- you appear to
	// teleport between points. (Gated to movement syncs so RPCs/other packets are safe.)
	if ( set.fake_lag && set.fake_lag_teleportish &&
	     ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		static unsigned long tpTick = 0;
		unsigned long now = GetTickCount();
		unsigned long delay = (unsigned long) ( set.fake_lag_delay > 0 ? set.fake_lag_delay : 0 );
		if ( now - tpTick > delay ) tpTick = now; // this one passes; reset the window
		else return false;                        // too soon -> drop
	}

	// Sync freeze (held-state prerequisite): drop the GAME's outgoing sync while a
	// cheat is holding a spoofed pose (njkill jack / itroll / gun-steal). Without
	// this the real sync overwrites the spoof a frame later and the method breaks.
	if ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC || packetId == ID_SPECTATOR_SYNC ) {
		unsigned long now = GetTickCount();
		bool frozen = false;
		if ( g_njkillTick > now )           frozen = true; // njkill: all 3 sync types, 3s
		if ( server::stealguns_active() )   frozen = true; // gun-steal: all 3 while working
		if ( now < g_freezeTick ) {                        // itroll/fuckup masked freeze
			if ( ( g_freezeMask & 1 ) && packetId == ID_PLAYER_SYNC )    frozen = true;
			if ( ( g_freezeMask & 2 ) && packetId == ID_VEHICLE_SYNC )   frozen = true;
			if ( ( g_freezeMask & 4 ) && packetId == ID_SPECTATOR_SYNC ) frozen = true;
		}
		if ( frozen )
			return false;
	}

	// iTroll (I-key, in-vehicle continuous -- Fuego 1:1): while I is held and you're
	// driving, rewrite EVERY outgoing vehicle sync into the target's car + mode mutation,
	// send it HIGH/RELIABLE and drop your real sync -- so the spoof isn't overwritten by the
	// target's own sync a frame later. (On foot it's the one-shot command, fired in on_frame.)
	if ( set.itroll_mode >= 1 && g_itrollTarget >= 0 && !set.no_big_hacks && packetId == ID_VEHICLE_SYNC &&
	     input::down( 'I' ) && !menu::is_open() && g_RakClient ) {
		stInCarData d{}; uint8_t tmp;
		params->ResetReadPointer(); params->Read( tmp ); params->Read( (char *)&d, sizeof d );
		if ( trolls::itroll_incar_rewrite( g_itrollTarget, set.itroll_mode, d ) ) {
			BitStream bs;
			bs.Write( (uint8_t) ID_VEHICLE_SYNC );
			bs.Write( (char *)&d, sizeof d );
			g_RakClient->Send( &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0 );
			return false; // drop our real sync so only the spoofed target-car sync goes out
		}
	}

	// (Fake lag is applied LAST -- see the end of this function -- so held packets
	// carry every rewrite below, exactly like Fuego's OnSendPacket order.)

	// Get here: while the release timer is live, freeze our vehicle sync at the
	// drop position (drop the outgoing ID_VEHICLE_SYNC). At release, resume normal
	// syncs so our real position snaps back, dragging the stuck target with us.
	if ( g_getHereRelease != 0 && packetId == ID_VEHICLE_SYNC ) {
		if ( GetTickCount() >= g_getHereRelease ) {
			g_getHereRelease = 0;
			status::set( "Get Here: operation completed" );
		} else {
			return false; // freeze at the drop position
		}
	}

	// Car control drives THEIR car by warping our ped into it (cheats::car_control),
	// so the game emits the target's in-car sync natively -- no packet rewrite here.

	// Invisibility: sink the outgoing position underground so remote players
	// render/aim at empty space. On foot -15z, in a vehicle -20z (Fuego values).
	if ( set.invisibility ) {
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, []( stOnFootData &d ) {
				d.fPosition[2]       -= 15.0f;
				d.sSurfingVehicleID   = 0;
			} );
		else if ( packetId == ID_VEHICLE_SYNC )
			rewrite_incar( params, []( stInCarData &d ) { d.fPosition[2] -= 20.0f; } );
		if ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC )
			status::set( "Invisibility  is ACTIVE" );
	}

	// Smart Invisibility (Fuego 1:1): while you're NOT clicking (LMB/RMB up), send
	// your sync as an ID_SPECTATOR_SYNC carrying just your position -- you vanish to
	// others. The moment you click (to shoot/aim) it stops, so your bullets land.
	if ( set.smart_invis && ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		bool clicking = ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) || ( GetAsyncKeyState( VK_RBUTTON ) & 0x8000 );
		if ( clicking ) {
			status::set( "Smart Invisibility NOT ACTIVE" );
		} else if ( packetId == ID_VEHICLE_SYNC ) {
			// In a vehicle (Fuego 1:1): report the car as UNOCCUPIED at (0,0,0) with its
			// real health + movespeed, so you vanish. Fuego sends it TWICE -- an explicit
			// LOW_PRIORITY send (original reliability/channel) AND the passthrough (at the
			// vehicle-sync's own priority) -- so we match by rewriting the packet + return true.
			stInCarData in{}; uint8_t tmp;
			params->ResetReadPointer(); params->Read( tmp ); params->Read( (char *)&in, sizeof in );
			stUnoccupiedData u{}; // zero-init -> fPosition/fDirection/fTurnSpeed/fRoll = 0
			u.sVehicleID     = (int16_t) in.sVehicleID;
			u.byteSeatID     = 0;
			u.fVehicleHealth = in.fVehicleHealth;
			u.fMoveSpeed[0]  = in.fMoveSpeed[0]; u.fMoveSpeed[1] = in.fMoveSpeed[1]; u.fMoveSpeed[2] = in.fMoveSpeed[2];
			if ( g_RakClient ) {
				BitStream ub;
				ub.Write( (uint8_t) ID_UNOCCUPIED_SYNC );
				ub.Write( (char *)&u, sizeof u );
				g_RakClient->Send( &ub, LOW_PRIORITY, reliability, orderingChannel );
			}
			params->ResetWritePointer(); // passthrough re-send (Fuego's double send)
			params->Write( (uint8_t) ID_UNOCCUPIED_SYNC );
			params->Write( (char *)&u, sizeof u );
			status::set( "Smart Invisibility ACTIVE" );
			return true;
		} else {
			// On foot (Fuego 1:1): rewrite the sync to a spectator sync at your position.
			float me[3];
			stSpectatorData sd{};
			if ( game::local_player_pos( me ) ) { sd.fPosition[0] = me[0]; sd.fPosition[1] = me[1]; sd.fPosition[2] = me[2]; }
			params->ResetWritePointer();
			params->Write( (uint8_t) ID_SPECTATOR_SYNC );
			params->Write( (char *)&sd, sizeof sd );
			status::set( "Smart Invisibility ACTIVE" );
		}
	}

	// Moonwalk: invert the facing quaternion's w term.
	if ( set.moonwalk ) {
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, []( stOnFootData &d ) { d.fQuaternion[3] = moonwalk_quat_w( d.fQuaternion[3] ); } );
		else if ( packetId == ID_VEHICLE_SYNC )
			rewrite_incar( params, []( stInCarData &d ) { d.fQuaternion[3] = moonwalk_quat_w( d.fQuaternion[3] ); } );
	}

	// Upside down: fixed quaternion (0,1,0,0).
	if ( set.upside_down ) {
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, []( stOnFootData &d ) {
				d.fQuaternion[0] = 0; d.fQuaternion[1] = 1; d.fQuaternion[2] = 0; d.fQuaternion[3] = 0;
			} );
		else if ( packetId == ID_VEHICLE_SYNC )
			rewrite_incar( params, []( stInCarData &d ) {
				d.fQuaternion[0] = 0; d.fQuaternion[1] = 1; d.fQuaternion[2] = 0; d.fQuaternion[3] = 0;
			} );
	}

	// Twister: spin the facing quaternion + force the dance anim so the ped whirls on
	// the spot for everyone (Fuego 1:1: rotation steps every 95ms, weapon 46, and the
	// dance keys/anim every 250ms). Applies to on-foot and in-vehicle sync.
	if ( set.twister_mode && ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		static float         rot = 0.0f;
		static unsigned long rotTick = 0, animTick = 0;
		unsigned long now = GetTickCount();
		if ( now - rotTick > 95 ) { rot += 1.0f; if ( rot >= 360.0f ) rot = 0.0f; rotTick = now; }
		bool anim = ( now - animTick > 250 );
		if ( anim ) animTick = now;
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, [&]( stOnFootData &d ) {
				d.fQuaternion[0] = cosf( rot ); d.fQuaternion[3] = sinf( rot ); // leave [1]/[2] (Fuego)
				d.byteCurrentWeapon = 46;
				if ( anim ) { d.sUpDownKeys = 128; d.sCurrentAnimationID = 0x3C5; d.sAnimFlags = 0x1901; }
			} );
		else
			rewrite_incar( params, [&]( stInCarData &d ) {
				d.fQuaternion[0] = cosf( rot ); d.fQuaternion[3] = sinf( rot );
				d.byteCurrentWeapon = 46;
				if ( anim ) d.sUpDownKeys = 128;
			} );
	}

	// Ghost mode: force special-action 3 in the on-foot sync (Fuego 1:1). Makes
	// the ped read as an unusual state to other clients.
	if ( set.ghost_mode && packetId == ID_PLAYER_SYNC )
		rewrite_onfoot( params, []( stOnFootData &d ) { d.byteSpecialAction = 3; } );

	// Zombie infect: for the 100ms window after /zominfect, keep our on-foot sync
	// pinned to the target with the attack keys down (Fuego 1:1).
	if ( g_zomInfectID >= 0 && GetTickCount() < g_zomInfectTick && packetId == ID_PLAYER_SYNC ) {
		float tp[3];
		if ( samp::is_connected( g_zomInfectID ) && game::player_world_pos( g_zomInfectID, tp ) ) {
			rewrite_onfoot( params, [&]( stOnFootData &d ) {
				d.fPosition[0] = tp[0]; d.fPosition[1] = tp[1]; d.fPosition[2] = tp[2];
				d.stSampKeys.keys_aim                 = 1;
				d.stSampKeys.keys_decel__jump         = 1;
				d.stSampKeys.keys_primaryFire         = 1;
				d.stSampKeys.keys_unknown__walkSlow   = 1;
				d.stSampKeys.keys_secondaryFire__shoot = 1;
				d.stSampKeys.keys_horn__crouch        = 1;
			} );
		}
	}

	// Trigger checkpoint: replay our position at the last checkpoint for a short
	// window (Fuego 1:1) -- on foot, in a vehicle, or as a passenger.
	if ( g_checkpointTick != 0 ) {
		if ( GetTickCount() >= g_checkpointTick ) {
			g_checkpointTick = 0;
		} else if ( packetId == ID_PLAYER_SYNC ) {
			rewrite_onfoot( params, []( stOnFootData &d ) {
				d.fPosition[0] = g_lastCheckpoint[0]; d.fPosition[1] = g_lastCheckpoint[1]; d.fPosition[2] = g_lastCheckpoint[2];
			} );
		} else if ( packetId == ID_VEHICLE_SYNC ) {
			rewrite_incar( params, []( stInCarData &d ) {
				d.fPosition[0] = g_lastCheckpoint[0]; d.fPosition[1] = g_lastCheckpoint[1]; d.fPosition[2] = g_lastCheckpoint[2];
			} );
		} else if ( packetId == ID_PASSENGER_SYNC ) {
			stPassengerData d{}; uint8_t id;
			params->ResetReadPointer(); params->Read( id ); params->Read( (char *)&d, sizeof d );
			d.fPosition[0] = g_lastCheckpoint[0]; d.fPosition[1] = g_lastCheckpoint[1]; d.fPosition[2] = g_lastCheckpoint[2];
			params->ResetWritePointer(); params->Write( id ); params->Write( (char *)&d, sizeof d );
		}
	}

	// Slap kill (Fuego 1:1): warp our on-foot sync onto the locked target with rotating
	// knockback -- vehicle target => anim 301 + x15 offset; on-foot => random anim +
	// random quaternion + x5. Uses x20 move-speed (slapkill), not x10 (near-slap).
	// Fuego calls this UNGUARDED (rewrites ANY outgoing sync to a PLAYER_SYNC), so it
	// fires from a vehicle too -- we catch both movement syncs and emit a player sync.
	if ( g_slapkillTarget >= 0 && !set.no_big_hacks && ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		float tp[3];
		if ( samp::is_connected( g_slapkillTarget ) && game::player_world_pos( g_slapkillTarget, tp ) ) {
			static int           sk = 0;
			static unsigned long skTick = 0;
			int angleDeg = sk * 90;
			if ( GetTickCount() - skTick > 250 ) { sk = ( sk + 1 ) & 3; skTick = GetTickCount(); }
			float rad = angleDeg * 0.0174532925f;
			samp::stRemotePlayer *rp = samp::remote_player( g_slapkillTarget );
			int  st    = ( rp && rp->pPlayerData ) ? rp->pPlayerData->bytePlayerState : 17;
			bool inVeh = ( st == 18 || st == 19 );
			float off  = inVeh ? 15.0f : 5.0f;
			g_slapperPos[0] = tp[0]; g_slapperPos[1] = tp[1]; g_slapperPos[2] = tp[2]; g_slapperTick = GetTickCount() + 1000;
			// Fuego builds a FRESH zeroed on-foot packet: weapon 0, keys 0, local hp/armor.
			float shp = 0, sar = 0; game::local_health_armor( shp, sar );
			stOnFootData d{};
			d.byteArmor         = (uint8_t) sar;
			d.byteCurrentWeapon = 0;
			d.byteHealth        = (uint8_t) shp;
			d.fPosition[0] = tp[0] + sinf( -rad ) * off;
			d.fPosition[1] = tp[1] + sinf( -rad ) * off;
			d.fPosition[2] = tp[2];
			if ( inVeh ) {
				d.sCurrentAnimationID = 301;
			} else {
				d.sCurrentAnimationID = (short) ( rand() % 1000 );
				d.fQuaternion[0] = (float) ( rand() % 200 ) / 100.0f;
				d.fQuaternion[1] = (float) ( rand() % 200 ) / 100.0f;
				d.fQuaternion[2] = (float) ( rand() % 200 ) / 100.0f;
				d.fQuaternion[3] = (float) ( rand() % 200 ) / 100.0f;
			}
			d.fMoveSpeed[0] = sinf( rad ) * 20.0f;
			d.fMoveSpeed[1] = cosf( rad ) * 20.0f;
			params->ResetWritePointer();
			params->Write( (uint8_t) ID_PLAYER_SYNC );
			params->Write( (char *)&d, sizeof d );
		}
	}

	// Aim Slapper: rocket your ped in the camera direction (Fuego 1:1) so you slam
	// into whatever you point at. 0xB6F178 is the GTA camera-heading global.
	if ( set.aim_slapper && !set.no_big_hacks && ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		float camRot = *reinterpret_cast<float *>( 0xB6F178 );
		float spd    = (float) set.aim_slapper_power; // raw speed (0 = no push, Fuego)
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, [&]( stOnFootData &d ) {
				d.fMoveSpeed[0] = sinf( camRot ) * spd;
				d.fMoveSpeed[1] = cosf( camRot ) * spd;
				d.fPosition[2] += 1.13f;
			} );
		else
			rewrite_incar( params, [&]( stInCarData &d ) {
				d.fMoveSpeed[0] = sinf( camRot ) * spd;
				d.fMoveSpeed[1] = cosf( camRot ) * spd;
				d.fPosition[2] += 1.13f;
			} );
	}

	// Near Slapper (Fuego 1:1): warp your on-foot sync onto the nearest player with a
	// rotating knockback. A target in a VEHICLE gets anim 301 + a x15 offset and NO
	// random quaternion; an on-foot target gets a random anim + random quaternion + x5.
	if ( set.near_slapper && !set.no_big_hacks && packetId == ID_PLAYER_SYNC ) {
		int   tgt = find_nearest_player( set.near_slapper_friend_filter, /*skipDead=*/true );
		float tp[3];
		if ( tgt >= 0 && game::player_world_pos( tgt, tp ) ) {
			static int           slapState = 0;
			static unsigned long slapTick  = 0;
			int angleDeg = slapState * 90;
			if ( GetTickCount() - slapTick > 250 ) { slapState = ( slapState + 1 ) & 3; slapTick = GetTickCount(); }
			float rad = angleDeg * 0.0174532925f;
			samp::stRemotePlayer *rp = samp::remote_player( tgt );
			int  st    = ( rp && rp->pPlayerData ) ? rp->pPlayerData->bytePlayerState : 17;
			bool inVeh = ( st == 18 || st == 19 );
			float off  = inVeh ? 15.0f : 5.0f;
			g_slapperPos[0] = tp[0]; g_slapperPos[1] = tp[1]; g_slapperPos[2] = tp[2]; g_slapperTick = GetTickCount() + 1000;
			rewrite_onfoot( params, [&]( stOnFootData &d ) {
				d.fPosition[0] = tp[0] + sinf( -rad ) * off;
				d.fPosition[1] = tp[1] + sinf( -rad ) * off;
				d.fPosition[2] = tp[2];
				if ( inVeh ) {
					d.sCurrentAnimationID = 301;
				} else {
					d.sCurrentAnimationID = (short) ( rand() % 1000 );
					d.fQuaternion[0] = (float) ( rand() % 200 ) / 100.0f;
					d.fQuaternion[1] = (float) ( rand() % 200 ) / 100.0f;
					d.fQuaternion[2] = (float) ( rand() % 200 ) / 100.0f;
					d.fQuaternion[3] = (float) ( rand() % 200 ) / 100.0f;
				}
				d.fMoveSpeed[0]  = sinf( rad ) * 10.0f;
				d.fMoveSpeed[1]  = cosf( rad ) * 10.0f;
			} );
			status::set( "Near Slapper -> %s [%d]", samp::player_name( tgt ).c_str(), tgt );
		}
	}

	// Silent aim: redirect the outgoing bullet to the locked target (chosen each
	// frame in visuals::update_aim_target). Rewrites the bullet-sync to register a
	// hit on the target's bone (+ optional lead prediction), and optionally fires
	// the GiveDamage RPC so damage-packet servers also register it.
	if ( set.silent_aim && set.silent_aim_target >= 0 && packetId == ID_BULLET_SYNC ) {
		// letGo (Fuego): if the game already scored a player hit, pass it through
		// unchanged and flag it so on_send_rpc keeps the game's own GiveDamage.
		{
			stBulletData probe{}; uint8_t pid;
			params->ResetReadPointer(); params->Read( pid ); params->Read( (char *)&probe, sizeof probe );
			if ( probe.byteType == 1 ) { g_silentLetGo = true; return true; }
			g_silentLetGo = false;
		}
		const int tgt = set.silent_aim_target;
		float mine[3], theirs[3];
		// Bone target: random bone/place each shot so hit registration looks legit;
		// forced to the head (bone 5) when fake-headshot or always-headshot is on.
		int bone;
		if ( set.fake_headshot || set.always_headshot ) {
			bone = 5; // BONE_HEAD
		} else {
			static const int kBones[] = { 5, 4, 3, 2, 1, 32, 22, 34, 24, 42, 52, 43, 53 };
			bone = kBones[ rand() % (int) ( sizeof kBones / sizeof kBones[0] ) ];
		}
		bool haveTgt = game::player_bone_pos( tgt, bone, theirs )
		            || game::player_world_pos( tgt, theirs );
		if ( game::player_streamed( tgt ) && game::local_player_pos( mine ) && haveTgt ) {
			if ( set.silent_aim_prediction ) {
				float v[3];
				if ( game::player_velocity( tgt, v ) ) {
					theirs[0] += v[0] * 6.0f; theirs[1] += v[1] * 6.0f; theirs[2] += v[2] * 6.0f;
				}
			}
			stBulletData d{};
			uint8_t id;
			params->ResetReadPointer();
			params->Read( id );
			params->Read( (char *)&d, sizeof d );
			// Target state -> fCenter[2] (0 on-foot else 0.56-rand%5/100); decide hit/miss
			// BEFORE writing so a false shot becomes a genuine byteType=0 (Fuego).
			samp::stRemotePlayer *rpSA = samp::remote_player( tgt );
			int  stSA   = ( rpSA && rpSA->pPlayerData ) ? rpSA->pPlayerData->bytePlayerState : 17;
			bool onFoot = ( stSA != 18 && stSA != 19 );
			int  wep    = game::local_weapon_type(); if ( wep < 0 ) wep = 0;
			bool go = true;
			if ( set.silent_aim_random_shots ) {
				int r = rand();
				if ( wep >= 22 && wep <= 25 )      go = ( r % 4 ) != 0;
				else if ( wep >= 26 && wep <= 27 ) go = ( r % 6 ) <= 2;
				else if ( wep >= 28 && wep <= 32 ) go = ( r % 11 ) <= 3;
				else if ( wep >= 33 && wep <= 34 ) go = ( r % 6 ) <= 2;
			}
			d.byteType  = go ? 1 : 0; // 1 = hit, 0 = genuine miss
			d.sTargetID = (uint16_t) tgt;
			d.fOrigin[0] = mine[0];   d.fOrigin[1] = mine[1];   d.fOrigin[2] = mine[2];
			d.fTarget[0] = theirs[0]; d.fTarget[1] = theirs[1]; d.fTarget[2] = theirs[2];
			d.fCenter[0] = d.fCenter[1] = 0.0f;
			d.fCenter[2] = onFoot ? 0.0f : ( 0.56f - ( rand() % 5 ) / 100.0f );
			params->ResetWritePointer();
			params->Write( id );
			params->Write( (char *)&d, sizeof d );

			// Fire GiveDamage only on a hit + only if the target is alive (Fuego).
			if ( set.silent_aim_give_damage && go && game::player_gta_health( tgt ) != 0.0f ) {
				int part = set.fake_headshot ? 9 : ( 3 + rand() % 7 );
				if ( part == 4 ) part = 3;
				rpc::give_damage( tgt, wep, part );
			}
			status::set( "Attacking: %d", tgt );
		}
	}

	// No spread (Fuego 1:1): the spread itself is killed by zeroing the game global
	// 0xB7CDC8 in on_frame. Here we advance the trigger state machine -- with
	// no_spread_trigger = N, accuracy switches on for one shot every N bullets.
	if ( set.no_spread_trigger > 0 && packetId == ID_BULLET_SYNC ) {
		if ( g_nospreadGo ) { g_nospreadGo = false; g_nospreadTriggers = 0; }
		else if ( ++g_nospreadTriggers >= set.no_spread_trigger ) g_nospreadGo = true;
	}

	// Dual shot: remember the just-fired bullet; on_frame re-fires it once after
	// dual_shot_delay ms so a single trigger pull registers as two hits.
	if ( set.dual_shot && packetId == ID_BULLET_SYNC ) {
		stBulletData d{};
		uint8_t id;
		params->ResetReadPointer();
		params->Read( id );
		params->Read( (char *)&d, sizeof d );
		std::lock_guard<std::mutex> lk( g_dShotMx );
		g_dShotData = d;
		if ( !g_dShotReady ) { g_dShotTick = GetTickCount(); g_dShotReady = true; }
	}

	// Shot repeater: resend the bullet packet N extra times (spam one shot).
	if ( set.shot_repeater >= 1 && packetId == ID_BULLET_SYNC && g_RakClient ) {
		for ( int i = 0; i < set.shot_repeater; i++ )
			g_RakClient->Send( params, priority, reliability, orderingChannel );
	}

	// Menu/cursor input freeze (Fuego 1:1, its block #11): while the menu is open,
	// zero the sync input fields so clicking widgets / navigating doesn't fire your
	// gun or walk you around in-game. Runs after the sync rewrites so it always wins.
	if ( menu::is_open() ) {
		if ( packetId == ID_PLAYER_SYNC )
			rewrite_onfoot( params, []( stOnFootData &d ) { d.sKeys = 0; d.sLeftRightKeys = 0; d.sUpDownKeys = 0; } );
		else if ( packetId == ID_VEHICLE_SYNC )
			rewrite_incar( params, []( stInCarData &d ) { d.sKeys = 0; d.sLeftRightKeys = 0; d.sUpDownKeys = 0; } );
		else if ( packetId == ID_PASSENGER_SYNC ) {
			stPassengerData d{}; uint8_t id;
			params->ResetReadPointer(); params->Read( id ); params->Read( (char *)&d, sizeof d );
			d.sKeys = 0; d.sLeftRightKeys = 0; d.sUpDownKeys = 0;
			params->ResetWritePointer(); params->Write( id ); params->Write( (char *)&d, sizeof d );
		}
	}

	// Fake lag (Fuego's LAST send-path handler): capture the now fully-rewritten
	// sync, hold it, release it later from the drain (store-and-forward). Placed here
	// so held packets carry invis/moonwalk/slapper/etc. rewrites, not the raw sync.
	if ( set.fake_lag && ( packetId == ID_PLAYER_SYNC || packetId == ID_VEHICLE_SYNC ) ) {
		HeldSync h{};
		h.id          = packetId;
		h.priority    = priority;
		h.reliability = reliability;
		h.channel     = orderingChannel;
		if ( packetId == ID_PLAYER_SYNC ) {
			stOnFootData d{};
			params->ResetReadPointer();
			uint8_t tmp; params->Read( tmp );
			params->Read( (char *)&d, sizeof d );
			std::memcpy( h.bytes, &d, sizeof d );
			h.len    = (int) sizeof d;
			h.pos[0] = d.fPosition[0]; h.pos[1] = d.fPosition[1]; h.pos[2] = d.fPosition[2];
		} else {
			stInCarData d{};
			params->ResetReadPointer();
			uint8_t tmp; params->Read( tmp );
			params->Read( (char *)&d, sizeof d );
			std::memcpy( h.bytes, &d, sizeof d );
			h.len    = (int) sizeof d;
			h.pos[0] = d.fPosition[0]; h.pos[1] = d.fPosition[1]; h.pos[2] = d.fPosition[2];
		}
		// Fuego 1:1: only hold the sync if we have a live ped/vehicle handle for the backtrack
		// transform; on an invalid handle send it normally instead of holding a stale-matrix sync.
		if ( !game::fakelag_capture( h.mat, h.gameSpeed, h.inVehicle ) )
			return true;
		int delay = set.fake_lag_delay; // Fuego uses the raw value (no floor); 0 => release next drain
		if ( delay < 0 ) delay = 0;
		int add   = set.fake_lag_random_spikes ? ( rand() % ( delay + 1 ) ) : delay;
		h.executeTick = GetTickCount() + (unsigned long) add;
		{
			std::lock_guard<std::mutex> lk( g_lagMx );
			if ( g_lagQueue.size() < 256 ) g_lagQueue.push_back( h );
		}
		status::set( "Fake Lag: holding sync (%dms)", add );
		return false; // don't send now -- the drain releases it on time
	}

	return true;
}

// --- outgoing RPC interception (net seam). Return false to DROP. -------------
// Godmode anti-detection (Fuego 1:1): when the server's anti-cheat sends a type-2
// CLIENT_CHECK flag query, our client replies with its actor flags. If health-hack
// is on, strip the INVULNERABLE bits from the reported flags so godmode is hidden.
bool on_send_rpc( int uniqueID, BitStream *params, PacketPriority, PacketReliability, char, bool ) {
	// Panic: Fuego gates every one of these on !isCheatPanicEnabled() -- pass untouched.
	// (The flag strip below is moot under panic: engaging it clears the invuln flags.)
	if ( fx::g_set.cheat_panic )
		return true;

	// Smart Health Hack (Fuego 1:1): drop a proc% of our OUTGOING take-damage reports
	// so the server ignores that damage -- but sometimes it lands, reading as legit.
	// (Our own give-damage RPCs use g_RakClient directly and bypass this hook, so
	// only the GAME's "I took damage" reports (bTake==true) are affected.)
	if ( uniqueID == RPC_GiveTakeDamage && fx::g_set.smart_health_hack && params ) {
		params->ResetReadPointer();
		bool bTake = false;
		params->Read( bTake );
		if ( bTake ) {
			int proc = fx::g_set.smart_hh_proc; // 0..100
			if ( ( rand() % 100 ) <= proc )
				return false; // drop the report
		}
	}

	// Silent aim (Fuego 1:1): while aiming, drop the game's OWN outgoing GiveTakeDamage --
	// BOTH give and take (the latter is Fuego's side-godmode), we send our own to the
	// locked target. A genuine hit (letGo) passes through once, cleared here like Fuego.
	if ( uniqueID == RPC_GiveTakeDamage && fx::g_set.silent_aim && fx::g_set.silent_aim_target >= 0 && params ) {
		if ( g_silentLetGo ) { g_silentLetGo = false; return true; }
		return false;
	}

	// Fake headshot (Fuego): rewrite the game's own outgoing give-damage bodypart to 9.
	if ( uniqueID == RPC_GiveTakeDamage && fx::g_set.fake_headshot && params ) {
		params->ResetReadPointer();
		bool bTake = false; uint16_t pid = 0; float amt = 0; uint32_t wep = 0, part = 0;
		params->Read( bTake ); params->Read( pid ); params->Read( amt ); params->Read( wep ); params->Read( part );
		if ( !bTake ) {
			params->SetWriteOffset( 0 );
			params->Write( bTake ); params->Write( pid ); params->Write( amt ); params->Write( wep ); params->Write( (uint32_t) 9 );
		}
	}

	if ( uniqueID == RPC_ClientCheck && fx::g_set.health_hack && params ) {
		params->ResetReadPointer();
		uint8_t type = 0;
		params->Read( type );
		if ( type == 2 ) {
			uint32_t arg = 0; uint8_t response = 0;
			params->Read( arg );
			params->Read( response );
			constexpr uint32_t ACTOR_FLAGS_INVULNERABLE = ( 4u | 8u | 64u | 128u );
			uint32_t fakeFlags = arg & ( 0xFF00FFFFu | ( ( ~ACTOR_FLAGS_INVULNERABLE ) << 16 ) );
			params->SetWriteOffset( 0 );
			params->Write( type );
			params->Write( fakeFlags );
			params->Write( response );
		}
	}
	return true; // never drop the check reply (that itself would flag us)
}

// --- incoming packet capture: streamed-out player positions (Fuego 1:1). --------
// The server's ID_MARKERS_SYNC carries the (short-precision) positions of players
// who are NOT streamed in. Cache them so ESP can draw distant players too.
bool on_recv_packet( Packet *p ) {
	if ( !p || !p->data || p->length < 1 || p->data[0] != ID_MARKERS_SYNC )
		return true;
	BitStream bs( p->data, p->length, false );
	bs.IgnoreBits( 8 );          // skip the packet id
	int num = 0;
	bs.Read( num );
	if ( num < 0 || num > SAMP_MAX_PLAYERS )
		return true;
	unsigned long now = GetTickCount();
	for ( int i = 0; i < num; i++ ) {
		uint16_t pid = 0xFFFF;
		bool     active = false;
		bs.Read( pid );
		bs.ReadCompressed( active );
		if ( !active || pid >= SAMP_MAX_PLAYERS )
			continue;
		short sx = 0, sy = 0, sz = 0;
		bs.Read( sx ); bs.Read( sy ); bs.Read( sz );
		g_markerPos[pid][0] = (float) sx;
		g_markerPos[pid][1] = (float) sy;
		g_markerPos[pid][2] = (float) sz;
		g_markerTick[pid]   = now;
	}
	return true; // never drop -- capture only
}

// --- per-frame cheat driver (render hook, MAIN thread). ----------------------
// Seam for cheats that run on the game loop rather than in the packet path
// (local memory writes: air brake, fly, god-mode top-up, ESP overlays, ...).
// Wired as those cheats are migrated; harmless no-op until then.
// WASD + Space/Ctrl as a camera-relative movement vector, scaled by speed.
void read_move( float speed, float &fwd, float &right, float &up ) {
	fwd = right = up = 0;
	if ( input::down( 'W' ) ) fwd   += speed;
	if ( input::down( 'S' ) ) fwd   -= speed;
	if ( input::down( 'D' ) ) right += speed;
	if ( input::down( 'A' ) ) right -= speed;
	if ( input::down( VK_SPACE ) )    up += speed;
	if ( input::down( VK_LCONTROL ) ) up -= speed;
}

// Release every held sync whose delay has elapsed (called each frame). Sends via
// the real RakClient with the exact priority/reliability the game originally used.
void drain_fakelag() {
	if ( !g_RakClient ) return;
	unsigned long now = GetTickCount();
	std::vector<HeldSync> due;
	{
		std::lock_guard<std::mutex> lk( g_lagMx );
		for ( auto it = g_lagQueue.begin(); it != g_lagQueue.end(); ) {
			if ( now >= it->executeTick ) { due.push_back( *it ); it = g_lagQueue.erase( it ); }
			else ++it;
		}
	}
	for ( auto &h : due ) {
		BitStream bs;
		bs.Write( h.id );
		bs.Write( (char *)h.bytes, h.len );
		g_RakClient->Send( &bs, h.priority, h.reliability, h.channel );
		g_lagLastSentPos[0] = h.pos[0];
		g_lagLastSentPos[1] = h.pos[1];
		g_lagLastSentPos[2] = h.pos[2];
		g_lagLastSentTick   = now + 1000; // overlay shows the appear-point for 1s
		g_lagLastSent       = h;
	}
}

// Hold BACKSPACE to snap the local ped back to where others last saw us (the
// last released packet). Mirrors Fuego's 0.5s countdown backtrack.
void handle_fakelag_backtrack() {
	unsigned long now = GetTickCount();

	// backTrackSetVehicle: 200ms after a vehicle backtrack, re-apply matrix + speed.
	if ( g_lagBtReapplyAt && now >= g_lagBtReapplyAt ) {
		game::fakelag_reapply_vehicle();
		g_lagBtReapplyAt = 0;
	}

	bool havePoint = ( g_lagLastSentTick > now );
	if ( !havePoint ) { g_lagBTLeft = 0.5f; return; }

	// Hold BACKSPACE (but not while the menu is up) -> 0.5s countdown, then restore the
	// last-sent snapshot (Fuego 1:1): on foot -> armor/health/weapon/matrix/pos/speed;
	// in a vehicle -> same for the ped + the car's speed(0)/matrix/pos(+0.25z)/health and
	// a +200ms matrix/speed re-apply. Latch at -1 so it fires ONCE per press.
	// Fuego: only while no cursor is up (menu / picker) and not typing -- input::down
	// already ignores keys while SA:MP's chat box is open.
	if ( input::down( VK_BACK ) && !menu::is_open() && !veh::is_picking() ) {
		if ( now >= g_lagBackHoldTick + 50 ) { g_lagBTLeft -= 0.05f; g_lagBackHoldTick = now; }
		if ( g_lagBTLeft <= 0.0f && g_lagBTLeft > -1.0f ) {
			{ std::lock_guard<std::mutex> lk( g_lagMx ); g_lagQueue.clear(); }
			g_lagBTLeft = -1.0f; // latch
			const HeldSync &h = g_lagLastSent;
			if ( h.id == ID_PLAYER_SYNC && h.len >= (int) sizeof( stOnFootData ) ) {
				const stOnFootData *o = reinterpret_cast<const stOnFootData *>( h.bytes );
				game::fakelag_restore_foot( (float) o->byteArmor, (float) o->byteHealth,
				                            o->byteCurrentWeapon, h.mat, h.gameSpeed );
				status::set( "Fake Lag: backtracked!" );
			} else if ( h.id == ID_VEHICLE_SYNC && h.len >= (int) sizeof( stInCarData ) ) {
				const stInCarData *c = reinterpret_cast<const stInCarData *>( h.bytes );
				game::fakelag_restore_vehicle( (float) c->byteArmor, (float) c->bytePlayerHealth,
				                               c->byteCurrentWeapon, h.mat, h.gameSpeed, c->fVehicleHealth );
				g_lagBtReapplyAt = now + 200; // backTrackSetVehicle
				status::set( "Fake Lag: backtracked!" );
			}
		}
	} else {
		g_lagBackHoldTick = now;
		g_lagBTLeft       = 0.5f;
	}
}

// Dual shot: re-fire the stashed bullet once the delay has elapsed.
void handle_dualshot() {
	auto &set = fx::g_set;
	if ( !set.dual_shot || !g_RakClient ) return;
	std::lock_guard<std::mutex> lk( g_dShotMx );
	if ( !g_dShotReady ) return;
	int delay = set.dual_shot_delay > 0 ? set.dual_shot_delay : 50;
	if ( (long) ( GetTickCount() - g_dShotTick ) < delay ) return;
	BitStream bs;
	bs.Write( (uint8_t) ID_BULLET_SYNC );
	bs.Write( (char *)&g_dShotData, sizeof g_dShotData );
	g_RakClient->Send( &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0 );
	g_dShotReady = false;
}

// Auto Killer (Fuego 1:1): sweep every streamed player and pump killer_bullets
// bullet-syncs + GiveDamage RPCs into each, rate-limited by killer_delay. Emits a
// running attack tally every killer_stats_interval seconds. Honours the friend
// filter and the menu-open guard (won't fire while you're clicking the menu).
void handle_killer() {
	auto &set = fx::g_set;
	if ( !set.killer_enabled || !g_RakClient ) return;
	if ( menu::is_open() ) { status::set( "Killer: close the menu to fire" ); return; }

	static int           cur         = 0;
	static unsigned long lastTick    = 0;
	static unsigned long statsTick   = 0;
	static int           statPlayers = 0;
	static int           statBullets = 0;

	unsigned long now = GetTickCount();
	int interval = set.killer_stats_interval > 0 ? set.killer_stats_interval : 10;
	if ( now >= statsTick && statPlayers > 0 ) {
		char b[96];
		samp::chat( " " );
		samp::chat( "{FF6347}[{FFFFFF}Killer Statistics{FF6347}]" );
		std::snprintf( b, sizeof b, "{FF6347}[Players Attacked] {FFFFFF}[%d]", statPlayers ); samp::chat( b );
		std::snprintf( b, sizeof b, "{FF6347}[Times Attacked] {FFFFFF}[%d]", statBullets );  samp::chat( b );
		std::snprintf( b, sizeof b, "{FF6347}[Statistics Time] {FFFFFF}[%ds]", interval );    samp::chat( b );
		samp::chat( " " );
		statPlayers = 0; statBullets = 0;
		statsTick = now + 1000u * (unsigned) interval;
	}

	int guard = 0;
	while ( cur >= SAMP_MAX_PLAYERS || !samp::is_connected( cur ) || !game::player_streamed( cur ) ||
	        ( set.killer_friend_filter && friends::is_friend( samp::player_name( cur ) ) ) ) {
		cur++;
		if ( cur >= SAMP_MAX_PLAYERS ) { cur = 0; lastTick = now + 10; return; }
		if ( ++guard > SAMP_MAX_PLAYERS ) return; // nobody valid this frame
	}

	if ( now < lastTick ) return;

	float tpos[3];
	if ( !game::player_world_pos( cur, tpos ) ) { cur++; return; }

	int wep = game::local_weapon_type(); if ( wep < 0 ) wep = 0;
	int bullets = set.killer_bullets > 0 ? set.killer_bullets : 1;
	samp::stRemotePlayer *rpK = samp::remote_player( cur );
	int  stK    = ( rpK && rpK->pPlayerData ) ? rpK->pPlayerData->bytePlayerState : 17;
	bool onFoot = ( stK != 18 && stK != 19 );
	int  part   = set.fake_headshot ? 9 : ( 3 + rand() % 7 ); // once per burst (Fuego)
	if ( part == 4 ) part = 3;
	for ( int i = 0; i < bullets; i++ ) {
		stBulletData bd{};
		bd.byteType     = 1;
		bd.byteWeaponID = (uint8_t) wep;
		bd.sTargetID    = (uint16_t) cur;
		bd.fOrigin[0] = tpos[0]; bd.fOrigin[1] = tpos[1]; bd.fOrigin[2] = tpos[2];
		bd.fTarget[0] = tpos[0]; bd.fTarget[1] = tpos[1]; bd.fTarget[2] = tpos[2];
		bd.fCenter[0] = bd.fCenter[1] = 0.0f;
		bd.fCenter[2] = onFoot ? 0.0f : ( 0.56f - ( rand() % 5 ) / 100.0f );
		BitStream bs;
		bs.Write( (uint8_t) ID_BULLET_SYNC );
		bs.Write( (char *)&bd, sizeof bd );
		g_RakClient->Send( &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0 );
		rpc::give_damage( cur, wep, part, /*ordered=*/true ); // killer: RELIABLE_ORDERED (Fuego 5583)
		statBullets++;
	}
	status::set( "Killer: attacking %s [%d]", samp::player_name( cur ).c_str(), cur );
	lastTick = now + (unsigned long) ( set.killer_delay > 0 ? set.killer_delay : 50 );
	cur++;
	statPlayers++;
}

// Send a fake-driver vehicle sync (Fuego SendFakeDriverFullSyncData): become the
// car's driver first if we're on foot, then broadcast the in-car sync in its name.
void send_fake_driver_full( stInCarData *d ) {
	if ( !g_RakClient || !d ) return;
	if ( !game::in_vehicle() ) {
		BitStream ev;
		ev.Write( d->sVehicleID );
		ev.Write<uint8_t>( 0 ); // seat 0 = driver
		g_RakClient->RPC( RPC_EnterVehicle, &ev );
	}
	BitStream bs;
	bs.Write( (uint8_t) ID_VEHICLE_SYNC );
	bs.Write( (char *)d, sizeof *d );
	g_RakClient->Send( &bs );
}

// Advance `v` to the next listed vehicle that isn't ours; returns its id, or -1 when
// the cursor wraps this frame (Fuego's find_another_car walk -- one car per frame).
int next_listed_vehicle( int &v, int myVid ) {
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp ) return -1;
	for ( ; v < samp::SAMP_MAX_VEHICLES; v++ ) {
		if ( vp->iIsListed[v] == 1 && v != myVid ) { int id = v; v++; return id; }
	}
	v = 0;
	return -1;
}

// The car trolls -- SERVER-SIDE, exactly like Fuego: one car per frame, a fake-driver
// vehicle sync (so everyone sees it) plus the client-side teleport (so we see it too).
void handle_car_trolls() {
	auto &set = fx::g_set;
	if ( set.no_big_hacks || !g_RakClient ) return;
	if ( !( set.explode_cars || set.floating_cars || set.remove_cars ||
	        set.explode_cars_silent || set.unoccupied_remover ) )
		return;

	int   myVid = game::local_vehicle_samp_id();
	float shp = 0, sarmor = 0; game::local_health_armor( shp, sarmor );
	float me[3]; bool haveMe = game::local_player_pos( me );

	auto base = [&]( int vid, stInCarData &d ) {
		std::memset( &d, 0, sizeof d );
		d.sVehicleID       = (uint16_t) vid;
		d.sKeys            = 0;
		d.bytePlayerHealth = (uint8_t) shp;
		d.byteArmor        = (uint8_t) sarmor;
	};

	// Explode: sync 1.0 health at the car's live position -> it blows for everyone.
	if ( set.explode_cars ) {
		static int v = 0;
		int vid = next_listed_vehicle( v, myVid );
		float pos[3], spd[3], hp;
		if ( vid >= 0 && game::vehicle_live_data( vid, pos, spd, hp ) ) {
			stInCarData d; base( vid, d );
			d.fPosition[0]=pos[0]; d.fPosition[1]=pos[1]; d.fPosition[2]=pos[2];
			d.fMoveSpeed[0]=spd[0]; d.fMoveSpeed[1]=spd[1]; d.fMoveSpeed[2]=spd[2];
			d.fVehicleHealth = 1.0f;
			send_fake_driver_full( &d );
			game::teleport_vehicle( vid, pos, spd, 1.0f );
		}
	}
	// Floating: scatter cars around us and drift them up.
	if ( set.floating_cars && haveMe ) {
		static int v = 0;
		int vid = next_listed_vehicle( v, myVid );
		float pos[3], spd[3], hp;
		if ( vid >= 0 && game::vehicle_live_data( vid, pos, spd, hp ) ) {
			float np[3] = { me[0] + ( -15.0f + rand() % 30 ), me[1] + ( -15.0f + rand() % 30 ), me[2] + 20.0f };
			float ns[3] = { 0.0f, 0.0f, 0.3f };
			stInCarData d; base( vid, d );
			d.fPosition[0]=np[0]; d.fPosition[1]=np[1]; d.fPosition[2]=np[2];
			d.fMoveSpeed[0]=ns[0]; d.fMoveSpeed[1]=ns[1]; d.fMoveSpeed[2]=ns[2];
			d.fVehicleHealth = hp;
			send_fake_driver_full( &d );
			game::teleport_vehicle( vid, np, ns, hp );
		}
	}
	// Remove: banish to the buried under-map spot.
	if ( set.remove_cars ) {
		static int v = 0;
		int vid = next_listed_vehicle( v, myVid );
		float pos[3], spd[3], hp;
		if ( vid >= 0 && game::vehicle_live_data( vid, pos, spd, hp ) ) {
			float np[3] = { 1088.5f, -2852.38f, -7.0f };
			float ns[3] = { 0.0f, 0.0f, 0.3f };
			stInCarData d; base( vid, d );
			d.fPosition[0]=np[0]; d.fPosition[1]=np[1]; d.fPosition[2]=np[2];
			d.fMoveSpeed[0]=ns[0]; d.fMoveSpeed[1]=ns[1]; d.fMoveSpeed[2]=ns[2];
			d.fVehicleHealth = hp;
			send_fake_driver_full( &d );
			game::teleport_vehicle( vid, np, ns, hp );
		}
	}
	// Explode silent: server-side RPC_VehicleDestroyed + client health 230.
	if ( set.explode_cars_silent ) {
		static int v = 0;
		int vid = next_listed_vehicle( v, myVid );
		if ( vid >= 0 ) {
			BitStream bs; bs.Write( vid );
			g_RakClient->RPC( RPC_VehicleDestroyed, &bs );
			float pos[3], spd[3], hp;
			if ( game::vehicle_live_data( vid, pos, spd, hp ) )
				game::teleport_vehicle( vid, pos, spd, 230.0f );
		}
	}
	// Unoccupied remover: bury cars via ID_UNOCCUPIED_SYNC (170hp while LMB = destroy).
	if ( set.unoccupied_remover ) {
		static int v = 0;
		int vid = next_listed_vehicle( v, myVid );
		float pos[3], spd[3], hp;
		if ( vid >= 0 && game::vehicle_live_data( vid, pos, spd, hp ) ) {
			float np[3] = { 1088.5f, -2852.38f, -7.0f };
			float ns[3] = { 0.0f, 0.0f, 0.3f };
			float health = ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) ? 170.0f : hp;
			stUnoccupiedData u; std::memset( &u, 0, sizeof u );
			u.sVehicleID     = (int16_t) vid;
			u.fPosition[0]=np[0]; u.fPosition[1]=np[1]; u.fPosition[2]=np[2];
			u.fMoveSpeed[0]=ns[0]; u.fMoveSpeed[1]=ns[1]; u.fMoveSpeed[2]=ns[2];
			u.fRoll[0]=-1.0f; u.fRoll[1]=0.0f; u.fRoll[2]=1.0f;
			u.fTurnSpeed[0]=-1.0f; u.fTurnSpeed[1]=0.0f; u.fTurnSpeed[2]=1.0f;
			u.fVehicleHealth = health;
			BitStream bs;
			bs.Write( (uint8_t) ID_UNOCCUPIED_SYNC );
			bs.Write( (char *)&u, sizeof u );
			g_RakClient->Send( &bs );
			game::teleport_vehicle( vid, np, ns, health );
		}
	}
}

// Freeze troll: keep the target driver's car pinned at g_freezePos with zero velocity
// (Fuego cheat_handle_freezetroll). Requires us to be in a vehicle.
void handle_freezetroll() {
	if ( g_freezeTarget < 0 || !g_RakClient ) return;
	if ( !game::in_vehicle() ) {
		samp::chat( "{FF6D70}[freeze] {FFFFFF}Stopped -- you left your vehicle." );
		g_freezeTarget = -1; return;
	}
	samp::stRemotePlayer *rp = samp::remote_player( g_freezeTarget );
	if ( !rp || !rp->pPlayerData || rp->pPlayerData->bytePlayerState != 19 /*DRIVER*/ ) {
		samp::chat( "{FF6D70}[freeze] {FFFFFF}Stopped -- target left / stopped driving." );
		g_freezeTarget = -1; return;
	}
	samp::stInCarData &tc = rp->pPlayerData->inCarData;
	if ( tc.sVehicleID == 0 ) return;

	float shp = 0, sarmor = 0; game::local_health_armor( shp, sarmor );
	stInCarData d; std::memset( &d, 0, sizeof d );
	d.sVehicleID     = tc.sVehicleID;
	d.fPosition[0]   = g_freezePos[0]; d.fPosition[1] = g_freezePos[1]; d.fPosition[2] = g_freezePos[2];
	d.fQuaternion[0] = tc.fQuaternion[0]; d.fQuaternion[1] = tc.fQuaternion[1];
	d.fQuaternion[2] = tc.fQuaternion[2]; d.fQuaternion[3] = tc.fQuaternion[3];
	d.fMoveSpeed[0] = d.fMoveSpeed[1] = d.fMoveSpeed[2] = 0.0f; // zero velocity = frozen
	d.fVehicleHealth   = tc.fVehicleHealth;
	d.bytePlayerHealth = (uint8_t) shp;
	d.byteArmor        = (uint8_t) sarmor;
	if ( samp::stLocalPlayer *lp = samp::local_player() ) // Fuego: our in-car weapon
		d.byteCurrentWeapon = lp->inCarData.byteCurrentWeapon;
	BitStream bs;
	bs.Write( (uint8_t) ID_VEHICLE_SYNC );
	bs.Write( (char *)&d, sizeof d );
	g_RakClient->Send( &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0 );
	status::set( "Freezing %s...", samp::player_name( g_freezeTarget ).c_str() );
}

// Car-control delayed steps (Fuego runs these on detached threads with Sleep;
// we fire them from the frame tick so every game call stays on the main thread).
void handle_carcontrol() {
	unsigned long now = GetTickCount();
	// carControlSetVehicle: 100ms after the warp, re-apply the car's transform so
	// the warp-in didn't jolt it.
	if ( g_ccReapplyAt && now >= g_ccReapplyAt ) {
		game::cc_reapply_vehicle( g_ccReapplyVid );
		g_ccReapplyAt = 0; g_ccReapplyVid = -1;
	}
	// DisableNoCols(1500): drop the possessed car's collision-proofing.
	if ( g_ccNocolsAt && now >= g_ccNocolsAt ) {
		game::cc_vehicle_collision_proof( g_ccNocolsVid, false );
		g_ccNocolsAt = 0; g_ccNocolsVid = -1;
	}
	// ccAgainWait: 500ms after the on-foot enter, re-run /cc (now we're in a car,
	// so it falls through to the possession path).
	if ( g_ccRetryAt && now >= g_ccRetryAt ) {
		int id = g_ccRetryId;
		g_ccRetryAt = 0; g_ccRetryId = -1;
		cheats::car_control( id ); // public API lives in namespace cheats (defined below)
	}
	// carControlResetOnfootMode(500): finish the on-foot restore (no failure text).
	if ( g_ccResetAt && now >= g_ccResetAt ) {
		g_ccResetAt = 0;
		game::cc_release_to( g_ccLastPos );
		game::teleport_local( g_ccLastPos );
		game::cc_camera_restore();
		g_ccUsedOnfoot = false;
	}
}

// Panic transitions (Fuego cheat_panic, cheat_generic.cpp): engaging strips the health-
// hack invulnerability and closes the menu (restored on release). Runs whether panic
// came from F12 or the menu checkbox.
void handle_panic_transition() {
	auto &set = fx::g_set;
	static bool prev = false, menuWasOpen = false;
	if ( set.cheat_panic == prev ) return;
	prev = set.cheat_panic;
	if ( set.cheat_panic ) {
		game::set_local_invulnerable( false );
		game::set_local_vehicle_invulnerable( false );
		menuWasOpen = menu::is_open();
		if ( menuWasOpen ) menu::set_open( false );
	} else if ( menuWasOpen ) {
		menu::set_open( true );
		menuWasOpen = false;
	}
}

// "Disable Unsafe/Big Hacks" (Fuego sampMainCheat): while it's on, the big toggles are
// forced off every frame, so they can't be switched on at all.
void enforce_no_big_hacks() {
	auto &set = fx::g_set;
	if ( !set.no_big_hacks ) return;
	set.itroll_mode         = 0;
	g_itrollTarget          = -1;
	set.break_dance         = false;
	set.aim_slapper         = false;
	set.floating_cars       = false;
	set.explode_cars_silent = false;
	set.unoccupied_remover  = false;
	set.car_shooter         = false;
	set.explode_cars        = false;
	set.near_slapper        = false;
	set.remove_cars         = false;
	set.killer_enabled      = false;
	g_freezeTarget          = -1;
	g_slapkillTarget        = -1;
}

void on_frame( IDirect3DDevice9 * ) {
	traceLastFunc( "cheats::on_frame" );
	auto &set = fx::g_set;

	if ( input::pressed( VK_F12 ) ) // Fuego key_panic
		set.cheat_panic = !set.cheat_panic;
	handle_panic_transition();
	enforce_no_big_hacks();

	if ( set.cheat_panic )
		return;

	if ( set.fake_lag ) {
		drain_fakelag();            // release held syncs on schedule
		handle_fakelag_backtrack(); // BACKSPACE -> snap back to the appear-point
	} else {
		std::lock_guard<std::mutex> lk( g_lagMx );
		g_lagQueue.clear();         // dropping fake lag discards anything still held
	}

	handle_dualshot();
	handle_killer();
	trolls::tick();  // itroll/fuckup passenger-hold -> driver-sync release
	server::tick(); // LSRP gun-stealer + auto-unscrambler state machines

	// Desync: keep the SA:MP ped's drunk level pinned to 0 (Fuego forceDesync).
	if ( g_desync )
		samp::set_local_drunk( 0 );

	// Slap kill: stop automatically when the target dies or streams out.
	if ( g_slapkillTarget >= 0 ) {
		if ( !samp::is_connected( g_slapkillTarget ) || !game::player_streamed( g_slapkillTarget ) ||
		     game::player_gta_health( g_slapkillTarget ) <= 0.7f ) {
			samp::chat( "{FF6D70}[slapkill] {FFFFFF}Stopped -- target died or streamed out." );
			g_slapkillTarget = -1;
		}
	}

	// iTroll (I-key, Fuego 1:1): re-resolve the target EVERY frame while I is held (Fuego's
	// targetfinder). In a vehicle it's the continuous per-sync rewrite (on_send_packet); on
	// foot it's the one-shot command on the key edge (the passenger-enter -> driver flow).
	if ( set.itroll_mode >= 1 && !menu::is_open() ) {
		static bool prevI = false;
		bool iDown = input::down( 'I' );
		// Fuego's targetfinder: the driver of the nearest OTHER occupied vehicle (not just
		// the nearest player of any state). Resolved while I is held.
		g_itrollTarget = iDown ? game::itroll_target_driver() : -1;
		if ( iDown && !prevI && !game::in_vehicle() && g_itrollTarget >= 0 )
			trolls::itroll( g_itrollTarget, set.itroll_mode );
		prevI = iDown;
	}


	if ( !game::available() )
		return;

	// Health hack: the server-side half (swallowing RPC_SetPlayerHealth) lives in
	// compat::HandleRPCPacketFunc; the client half here mirrors Fuego's cheat_handle_hp
	// -- set the ped's damage-immunity flags. HP itself is never raised (Fuego shipped
	// hp_regen = 0). Flags are cleared when the toggle goes off.
	static bool prevHealth = false;
	if ( set.health_hack ) {
		game::set_local_invulnerable( true );
		if ( game::in_vehicle() ) game::set_local_vehicle_invulnerable( true ); // Fuego pins the car too
		prevHealth = true;
	} else if ( prevHealth ) {
		game::set_local_invulnerable( false );
		game::set_local_vehicle_invulnerable( false );
		prevHealth = false;
	}

	// No reload (Fuego): cancel the reload state as soon as it starts.
	if ( set.no_reload )
		game::no_reload_tick();

	// Fly (on foot): camera-relative WASD + Space/Ctrl, gravity cancelled, noclip.
	static bool prevFly = false;
	if ( set.fly && !game::in_vehicle() ) {
		float f, r, u;
		read_move( set.fly_speed, f, r, u );
		game::fly_move( f, r, u );
		game::set_local_collision( false ); // pass through walls while flying
		prevFly = true;
	} else if ( prevFly ) {
		game::set_local_collision( true ); // restore collision when fly stops
		prevFly = false;
	}

	// Air brake: freeze the vehicle/ped; camera-relative WASD moves it.
	if ( set.air_brake ) {
		float f, r, u;
		read_move( set.air_brake_speed, f, r, u );
		if ( f != 0 || r != 0 || u != 0 )
			game::airbrake_move( f, r, u );
		else
			game::freeze_local();
	}

	// Vehicle trolls (explode / float / remove / silent-explode / unoccupied-remove):
	// server-side fake-driver sync + client teleport, one car/frame, exactly like Fuego.
	handle_car_trolls();
	handle_freezetroll();
	handle_carcontrol(); // /cc possession: delayed re-apply / on-foot retry / restore

	// Car shooter: hold LMB to launch streamed vehicles at your aim (Fuego 1:1) --
	// one car per frame, teleported in front of the camera moving fast + a spoofed
	// driver-sync so others see it fly too.
	if ( set.car_shooter && !menu::is_open() && ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) ) {
		float fwd[3], me[3];
		samp::stVehiclePool *vp = samp::vehicle_pool();
		if ( vp && game::camera_forward( fwd ) && game::local_player_pos( me ) ) {
			static int v = 0, x = 0;
			int myVeh = game::local_vehicle_samp_id();
			for ( int scan = 0; scan < samp::SAMP_MAX_VEHICLES; scan++ ) {
				v = ( v + 1 ) % samp::SAMP_MAX_VEHICLES;
				if ( !vp->iIsListed[v] || v == myVeh ) continue;
				float lp[3], ls[3], lh;
				if ( !game::vehicle_live_data( v, lp, ls, lh ) ) continue;
				float pos[3]   = { me[0] + fwd[0] * ( 6.0f + x ), me[1] + fwd[1] * ( 6.0f + x ), me[2] + fwd[2] * ( 2.0f + x ) };
				float speed[3] = { fwd[0] * 7.5f, fwd[1] * 7.5f, fwd[2] * 7.5f };
				float shp = 0, sar = 0; game::local_health_armor( shp, sar );
				stInCarData d{};
				d.sVehicleID       = (uint16_t) v;
				d.fPosition[0] = pos[0]; d.fPosition[1] = pos[1]; d.fPosition[2] = pos[2];
				d.fMoveSpeed[0] = speed[0]; d.fMoveSpeed[1] = speed[1]; d.fMoveSpeed[2] = speed[2];
				d.fVehicleHealth   = lh;
				d.bytePlayerHealth = (uint8_t) shp;
				d.byteArmor        = (uint8_t) sar;
				send_fake_driver_full( &d );          // enter seat 0 + vehicle sync (server-side)
				game::launch_vehicle( v, pos, speed ); // client-side fling of the real car
				x--; if ( x < -1 ) x = 0;
				status::set( "Car Shooter: launching vehicles" );
				break;
			}
		}
	}

	// Break dance: IJKL/UO apply a rotary pulse to the ped (Fuego 1:1) -- tumble on
	// the spot. 0xB7CB5C is CTimer's per-frame time step. Fuego applied this every
	// frame at its capped frame rate; fuego-reversed can run uncapped (Uncap FPS /
	// high-refresh monitors), which fired the pulse many more times per second and
	// span ~10x too fast. Throttle to every 10th qualifying frame so the spin speed
	// feels like Fuego's regardless of the current FPS (user-requested).
	if ( set.break_dance && !game::in_vehicle() && !menu::is_open() ) {
		static unsigned int bdTick = 0;
		if ( ++bdTick % 10 == 0 ) {
			float ts = *reinterpret_cast<float *>( 0xB7CB5C );
			float v  = 2.0f * ts;
			if      ( input::down( 'I' ) ) game::apply_rotory_pulse( -v, 0, 0 );
			else if ( input::down( 'K' ) ) game::apply_rotory_pulse(  v, 0, 0 );
			else if ( input::down( 'L' ) ) game::apply_rotory_pulse( 0,  v, 0 );
			else if ( input::down( 'J' ) ) game::apply_rotory_pulse( 0, -v, 0 );
			else if ( input::down( 'O' ) ) game::apply_rotory_pulse( 0, 0, -v );
			else if ( input::down( 'U' ) ) game::apply_rotory_pulse( 0, 0,  v );
		}
	}

	// Trigger bot (Fuego 1:1): auto-click when the GAME's aim is on a target. Reads
	// the aim-target pointer at *(0xB6F3B8)+0x79C; on target -> LMB down once + clear
	// the pointer; off target -> LMB up. 50ms cadence, not tied to silent aim.
	if ( set.trigger_bot ) {
		static unsigned long tbTick = 0;
		static bool tbAiming = false;
		// Fuego gate: never trigger while holding fists (SA:MP byteCurrentWeapon == 0).
		samp::stLocalPlayer *tbLp = samp::local_player();
		if ( tbLp && tbLp->byteCurrentWeapon == 0 ) { /* skip */ } else
		if ( GetTickCount() - tbTick >= 50 ) {
			tbTick = GetTickCount();
			int a1 = *reinterpret_cast<int *>( 0xB6F3B8 );
			if ( a1 ) {
				a1 += 0x79C;
				int a2 = *reinterpret_cast<int *>( a1 );
				if ( a2 ) {
					if ( !tbAiming ) {
						INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
						SendInput( 1, &in, sizeof( INPUT ) ); tbAiming = true;
					}
					*reinterpret_cast<int *>( a1 ) = 0;
				} else if ( tbAiming ) {
					INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
					SendInput( 1, &in, sizeof( INPUT ) ); tbAiming = false;
				}
			}
		}
	}

	// No spread (Fuego cheat_handle_nospread): zero the game's bullet-spread global.
	if ( set.no_spread_trigger == 0 && !g_nospreadGo ) g_nospreadGo = true;
	if ( set.no_spread && g_nospreadGo ) *reinterpret_cast<float *>( 0xB7CDC8 ) = 0.0f;

	// Fuego runs DisableFlyingBirds() unconditionally every frame: RET-patch the bird
	// spawn/update entry (0x712330 -> 0xC3) so no birds ever appear.
	mem::write<uint8_t>( 0x712330, 0xC3 );
}

} // namespace

namespace cheats {

bool fakelag_held( float out[3] ) {
	// The point where the last released sync placed us (= where others see you
	// right now). Valid for ~1s after each release; that's what the overlay draws.
	if ( g_lagLastSentTick <= GetTickCount() ) return false;
	out[0] = g_lagLastSentPos[0]; out[1] = g_lagLastSentPos[1]; out[2] = g_lagLastSentPos[2];
	return true;
}

// Seconds left on the BACKSPACE backtrack countdown (for the overlay text).
float fakelag_backtrack_left() { return g_lagBTLeft; }

// Zombie infect (Fuego 1:1): fire an immediate on-foot sync warping us onto the
// target with attack keys, then open a 100ms window (on_send_packet keeps pinning
// us there). Returns false if the target isn't a valid streamed player.
bool zombie_infect( int id ) {
	traceLastFunc( "cheats::zombie_infect" );
	if ( !samp::is_connected( id ) || !g_RakClient ) return false;
	samp::stLocalPlayer *lp = samp::local_player();
	float tp[3];
	if ( !lp || !game::player_world_pos( id, tp ) ) return false;

	stOnFootData d = lp->onFootData;              // start from our real on-foot sync
	d.fPosition[0] = tp[0]; d.fPosition[1] = tp[1]; d.fPosition[2] = tp[2];
	d.stSampKeys.keys_aim                 = 1;
	d.stSampKeys.keys_decel__jump         = 1;
	d.stSampKeys.keys_primaryFire         = 1;
	d.stSampKeys.keys_unknown__walkSlow   = 1;
	d.stSampKeys.keys_secondaryFire__shoot = 1;
	d.stSampKeys.keys_horn__crouch        = 1;

	BitStream bs;
	bs.Write( (uint8_t) ID_PLAYER_SYNC );
	bs.Write( (char *)&d, sizeof d );
	g_RakClient->Send( &bs );

	g_zomInfectID   = id;
	g_zomInfectTick = GetTickCount() + 100;
	return true;
}

bool toggle_desync() { g_desync = !g_desync; return g_desync; }

// Get here (Fuego 1:1): drop our vehicle onto an on-foot target + freeze the sync
// there until a ping-compensated release. Requires us to be in a vehicle.
bool get_here( int id ) {
	traceLastFunc( "cheats::get_here" );
	if ( !samp::is_connected( id ) || !g_RakClient || !game::in_vehicle() ) return false;
	samp::stLocalPlayer  *lp = samp::local_player();
	samp::stRemotePlayer *rp = samp::remote_player( id );
	float tp[3];
	if ( !lp || !game::player_world_pos( id, tp ) ) return false;
	if ( rp && rp->pPlayerData && rp->pPlayerData->bytePlayerState != 17 ) { // must be on foot
		samp::chat( "{FF6347}[gethere] {FFFFFF}Target is not on foot." );
		return false;
	}

	stInCarData d = lp->inCarData;          // our real in-car sync
	d.fPosition[0] = tp[0];
	d.fPosition[1] = tp[1];
	d.fPosition[2] = tp[2];
	// Sink by our car's real height (from its model bounding box) + the model fixups
	// Fuego uses, so the target ends up ON the car instead of clipping through it.
	float dims[3]; int model = 0;
	int myVid = game::local_vehicle_samp_id();
	if ( myVid >= 0 && game::vehicle_dims_model( myVid, dims, model ) ) {
		d.fPosition[2] -= ( dims[2] <= -0.1f ? -dims[2] : dims[2] );
		if ( model == 406 ) d.fPosition[2] += 1.35f; // Dumper
		if ( model == 732 ) d.fPosition[2] += 0.85f; // Berkley RC Van
	} else {
		d.fPosition[2] -= 1.0f;
	}
	BitStream bs;
	bs.Write( (uint8_t) ID_VEHICLE_SYNC );
	bs.Write( (char *)&d, sizeof d );
	g_RakClient->Send( &bs );

	int selfPing = ( g_RakClient->GetInterface() ? g_RakClient->GetInterface()->GetLastPing() : 100 );
	int tgtPing  = ( rp ? rp->iPing : 0 );
	g_getHereRelease = GetTickCount() + (unsigned long) ( lroundf( tgtPing / 1.25f ) + selfPing + 50 );
	status::set( "Get Here: dropped on target, holding..." );
	return true;
}

// Ninja kill (Fuego 1:1): appear beside a driver's car pressing enter + fire the
// EnterVehicle(seat 0) RPC to jack them out. Target must be a driver.
bool ninja_kill( int id ) {
	traceLastFunc( "cheats::ninja_kill" );
	if ( !samp::is_connected( id ) || !g_RakClient ) return false;
	samp::stRemotePlayer *rp = samp::remote_player( id );
	samp::stLocalPlayer  *lp = samp::local_player();
	if ( !rp || !rp->pPlayerData || !lp ) return false;
	if ( rp->pPlayerData->bytePlayerState != 19 /*DRIVER*/ ) return false;
	uint16_t vid = (uint16_t) rp->pPlayerData->inCarData.sVehicleID;
	float tp[3];
	if ( !game::player_world_pos( id, tp ) ) return false;

	// Fuego 1:1: a ZEROED on-foot packet -- only armor/health/weapon(0)/position + the
	// jump & enter keys. Copying our live onFootData (its keys, quaternion, velocity,
	// animation) makes the jack conflict and fail, then the 3s freeze just holds us
	// desynced while it does nothing.
	stOnFootData d; std::memset( &d, 0, sizeof d );
	float shp = 0, sar = 0; game::local_health_armor( shp, sar );
	d.byteArmor         = (uint8_t) sar;
	d.byteCurrentWeapon = 0;
	d.byteHealth        = (uint8_t) shp;
	// Stand beside the driver door: a right-vector derived from the car's forward
	// (fwd.y, -fwd.x), normalised, times (width+0.15)/2 off the car centre (Fuego 1:1).
	float fwd[3], vpos[3], width = 2.0f;
	if ( game::vehicle_forward( vid, fwd, width, vpos ) ) {
		float rx = fwd[1], ry = -fwd[0];
		float len = sqrtf( rx * rx + ry * ry ); if ( len < 0.001f ) len = 1.0f;
		float off = ( width + 0.15f ) / 2.0f;
		d.fPosition[0] = vpos[0] + ( rx / len ) * off;
		d.fPosition[1] = vpos[1] + ( ry / len ) * off;
		d.fPosition[2] = vpos[2];
	} else {
		d.fPosition[0] = tp[0] + 1.5f; d.fPosition[1] = tp[1]; d.fPosition[2] = tp[2];
	}
	d.stSampKeys.keys_decel__jump  = 1;
	d.stSampKeys.keys_enterExitCar = 1; // the "enter vehicle" jack key
	samp::set_local_drunk( 0 );          // Fuego resets drunkLevel = 0 before the jack
	BitStream onfoot;
	onfoot.Write( (uint8_t) ID_PLAYER_SYNC );
	onfoot.Write( (char *)&d, sizeof d );
	g_RakClient->Send( &onfoot );

	BitStream ev;
	ev.Write( vid );
	ev.Write<uint8_t>( 0 ); // seat 0 = driver (jack them out)
	g_RakClient->RPC( RPC_EnterVehicle, &ev );

	g_njkillTick = GetTickCount() + 3000; // freeze our real sync for 3s so the jack holds
	return true;
}

bool njkill_active() { return g_njkillTick > GetTickCount(); }

void freeze_sync_ms( int ms, bool player, bool vehicle, bool spectator ) {
	g_freezeTick = GetTickCount() + (unsigned long) ( ms > 0 ? ms : 0 );
	g_freezeMask = ( player ? 1 : 0 ) | ( vehicle ? 2 : 0 ) | ( spectator ? 4 : 0 );
}

void set_checkpoint( float x, float y, float z ) {
	g_lastCheckpoint[0] = x; g_lastCheckpoint[1] = y; g_lastCheckpoint[2] = z;
}

bool trigger_checkpoint( int delayMs ) {
	traceLastFunc( "cheats::trigger_checkpoint" );
	if ( g_lastCheckpoint[0] == 0 && g_lastCheckpoint[1] == 0 && g_lastCheckpoint[2] == 0 )
		return false; // no checkpoint seen yet
	g_checkpointTick = GetTickCount() + (unsigned long) ( delayMs > 0 ? delayMs : 50 );
	return true;
}

// Active during possession AND for 1s after release (Fuego: carControl_LastVeh
// != NULL || carControl_LastUse >= now) -- gates compat's incoming-RPC block so
// the server can't yank us out of the car we just warped into.
bool car_control_active() { return g_ccLastVeh >= 0 || g_ccLastUse > GetTickCount(); }

// Freeze troll: pin a target driver's car in place (toggle). Returns 1=on, 0=off,
// -1=target-not-a-driver, -2=you're-not-in-a-vehicle.
int freeze_troll( int id ) {
	traceLastFunc( "cheats::freeze_troll" );
	if ( g_freezeTarget >= 0 ) { g_freezeTarget = -1; return 0; }
	if ( !game::in_vehicle() ) return -2;
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( !rp || !rp->pPlayerData || rp->pPlayerData->bytePlayerState != 19 /*DRIVER*/ ) return -1;
	samp::stInCarData &tc = rp->pPlayerData->inCarData;
	g_freezePos[0] = tc.fPosition[0]; g_freezePos[1] = tc.fPosition[1]; g_freezePos[2] = tc.fPosition[2];
	g_freezeTarget = id;
	return 1;
}

// Last slapped position (for the slapper ESP), valid ~1s after a slap.
bool slapper_pos( float out[3] ) {
	if ( g_slapperTick <= GetTickCount() ) return false;
	out[0] = g_slapperPos[0]; out[1] = g_slapperPos[1]; out[2] = g_slapperPos[2];
	return true;
}

// Last known position of a streamed-OUT player (from ID_MARKERS_SYNC), if recent.
bool streamed_out_pos( int id, float out[3] ) {
	if ( id < 0 || id >= SAMP_MAX_PLAYERS ) return false;
	if ( g_markerTick[id] == 0 || GetTickCount() - g_markerTick[id] > 3000 ) return false;
	out[0] = g_markerPos[id][0]; out[1] = g_markerPos[id][1]; out[2] = g_markerPos[id][2];
	return true;
}

// carControlResetOnfootMode(0): warp out of the borrowed car + teleport back to
// where we started + restore the camera (Fuego, immediate/error path, wait=0).
static void cc_reset_onfoot_now() {
	game::cc_release_to( g_ccLastPos );
	game::teleport_local( g_ccLastPos );
	game::cc_camera_restore();
	g_ccUsedOnfoot = false;
	samp::chat( "{FFFFFF}Failed to enter with onfoot carcontrol mode or some error occured. Resetting." );
}

// The target's SA:MP vehicle id from their sync state (driver -> inCarData,
// passenger -> passengerData), or -1 if they're not in a car.
static int cc_target_vehicle( samp::stRemotePlayer *rp ) {
	int st = rp->pPlayerData->bytePlayerState;
	if ( st == 19 /*DRIVER*/ )    return (int) rp->pPlayerData->inCarData.sVehicleID;
	if ( st == 18 /*PASSENGER*/ ) return (int) rp->pPlayerData->passengerData.sVehicleID;
	return -1;
}

// Toggle car control (Fuego /cc 1:1). Warps our ped into the target's car so the
// game emits its in-car sync (we drive it); toggles off by warping back + restore.
// Returns: 0 off, 1 possessing, 2 on-foot-entering (retry pending); negatives are
// errors (-1 not connected, -2 not streamed, -4 no empty car, -5 bad target veh,
// -6 target not in a vehicle).
int car_control( int id ) {
	traceLastFunc( "cheats::car_control" );
	if ( !g_RakClient ) return -1;
	g_ccLastUse = GetTickCount() + 1000; // open/extend the incoming-block grace

	// ---- toggle OFF (we're already possessing) ----------------------------------
	if ( g_ccLastVeh >= 0 ) {
		game::cc_release_to( g_ccLastPos );          // warp out of the possessed car
		bool restored = false;
		if ( samp::vehicle_streamed( g_ccLastVeh ) && !g_ccUsedOnfoot ) {
			if ( g_ccLastVehSeat == 0 )              // put us back in our OLD car ...
				game::cc_possess_driver( g_ccLastVeh );
			else
				game::cc_possess_passenger( g_ccLastVeh, game::cc_vehicle_empty_seat( g_ccLastVeh ) );
			restored = true;
		}
		if ( !restored )
			game::teleport_local( g_ccLastPos );     // ... else just drop us at the restore point
		game::cc_camera_restore();
		g_ccLastVeh = -1;
		if ( g_ccUsedOnfoot )
			g_ccResetAt = GetTickCount() + 500;      // carControlResetOnfootMode(500)
		g_ccUsedOnfoot = false;
		return 0;
	}

	// ---- validation -------------------------------------------------------------
	if ( !samp::is_connected( id ) ) {
		if ( g_ccUsedOnfoot ) cc_reset_onfoot_now();
		return -1;
	}
	samp::stRemotePlayer *rp = samp::remote_player( id );
	if ( !rp || !rp->pPlayerData || !game::player_streamed( id ) ) {
		if ( g_ccUsedOnfoot ) cc_reset_onfoot_now();
		return -2; // player not streamed / exists
	}

	// ---- on-foot path: enter a nearby empty car first, then retry ---------------
	if ( !game::in_vehicle() ) {
		if ( g_ccUsedOnfoot ) { cc_reset_onfoot_now(); return -3; }
		int vid = game::cc_nearest_empty_vehicle( 10.0f );
		if ( vid < 0 ) return -4; // no empty vehicle to borrow
		game::local_player_pos( g_ccLastPos );

		int seat = game::cc_vehicle_empty_seat( vid );
		if ( seat < 0 ) seat = 1;

		// Tell the server we entered (bypasses on_send_packet via g_RakClient). Fuego
		// writes the vid as a 4-byte int here (unlike the u16 fields elsewhere), so the
		// server reads the seat byte as 0 -- match that exactly for wire parity.
		BitStream ev; ev.Write( vid ); ev.Write<uint8_t>( 1 );
		g_RakClient->RPC( RPC_EnterVehicle, &ev );

		samp::stPassengerData data; memset( &data, 0, sizeof data );
		float hp = 0, armor = 0; game::local_health_armor( hp, armor );
		data.byteArmor         = (uint8_t) armor;
		data.byteCurrentWeapon = 0;
		data.byteHealth        = (uint8_t) hp;
		data.byteSeatID        = (uint8_t) seat;
		data.sVehicleID        = (uint16_t) vid;
		float vpos[3], vspd[3], vhp; if ( game::vehicle_live_data( vid, vpos, vspd, vhp ) ) {
			data.fPosition[0] = vpos[0]; data.fPosition[1] = vpos[1]; data.fPosition[2] = vpos[2];
		}
		BitStream ps;
		ps.Write( (uint8_t) ID_PASSENGER_SYNC );
		ps.Write( (char *)&data, sizeof data );
		g_RakClient->Send( &ps, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0 );

		game::cc_possess_passenger( vid, seat ); // warp us into that car locally
		g_ccUsedOnfoot = true;
		g_ccRetryAt = GetTickCount() + 500; g_ccRetryId = id; // ccAgainWait -> re-call /cc
		return 2;
	}

	// ---- possession path: target must be a driver/passenger ---------------------
	if ( cc_target_vehicle( rp ) < 0 ) {
		if ( g_ccUsedOnfoot ) cc_reset_onfoot_now();
		return -6; // target not in any vehicle
	}
	g_ccLastVeh = game::cc_local_vehicle_id();   // our current car (to restore into later)
	int targetVid = cc_target_vehicle( rp );
	if ( !samp::vehicle_streamed( targetVid ) ) {
		if ( g_ccUsedOnfoot ) cc_reset_onfoot_now();
		g_ccLastVeh = -1;
		return -5; // target's vehicle handle is invalid
	}
	g_ccNocolsAt = GetTickCount() + 1500; g_ccNocolsVid = targetVid; // DisableNoCols(1500)
	game::cc_vehicle_collision_proof( targetVid, true );
	g_ccLastVehSeat = game::cc_local_seat() == 0 ? 0 : 1;
	if ( !g_ccUsedOnfoot )
		game::local_player_pos( g_ccLastPos );
	game::cc_capture_vehicle( targetVid );       // stash the car's transform (pre-warp)
	game::cc_possess_driver( targetVid );        // GTAfunc_PutActorInCar
	game::cc_camera_on_vehicle( targetVid );     // GTAfunc_CameraOnVehicle(veh, 2)
	g_ccReapplyAt = GetTickCount() + 100; g_ccReapplyVid = targetVid; // carControlSetVehicle
	return 1;
}

// Toggle continuous slap-kill on a target. Returns the new state (true = slapping).
// Pass -1 to just stop. false result means it stopped (or the id was invalid).
bool slapkill( int id ) {
	traceLastFunc( "cheats::slapkill" );
	if ( g_slapkillTarget >= 0 || id < 0 ) { g_slapkillTarget = -1; return false; }
	if ( !samp::is_connected( id ) || !game::player_streamed( id ) ) return false;
	g_slapkillTarget = id;
	return true;
}

bool slapkill_active() { return g_slapkillTarget >= 0; }

void install() {
	net::on_send_packet( on_send_packet );
	net::on_send_rpc( on_send_rpc );   // godmode flag-strip on RPC_ClientCheck
	net::on_recv_packet( on_recv_packet ); // capture ID_MARKERS_SYNC (distant-player ESP)
	render::on_frame( on_frame );
	chams::install(); // DrawIndexedPrimitive hook (armed on first frame)
	Log( "[cheats] handlers registered (net seam + per-frame driver)." );
}

} // namespace cheats
