#pragma once
/*
	net/rpc.h  --  cheat-facing RPC helpers (SA:MP 0.3.DL wire format).

	Thin wrappers over g_RakClient->RPC(...) for the RPCs cheats/trolls send:
	damage, death, spawn, class, animation, chat. All no-op if not connected.
*/

#include <cstdint>

namespace rpc {

// SA:MP per-weapon damage (index = weapon id 0..54). Used by silent aim / kill.
extern const float fWeaponDamage[55];

// Damage a player: RPC_GiveTakeDamage with bTakeDamage=false. bodypart 3..9
// (9 = head). weapon indexes fWeaponDamage. Sent as the local player hitting them.
void give_damage( int targetId, int weapon, int bodypart, bool ordered = false ); // ordered=true: killer (RELIABLE_ORDERED), else silent-aim (RELIABLE_SEQUENCED)

// Damage a player by an EXPLICIT amount (used by /meleedmg).
void give_damage_amount( int targetId, float dmg, int weapon, int bodypart );

// Take damage from a player (troll: make the server think THEY hit YOU).
void take_damage( int issuerId, int weapon, int bodypart );

// Kill self / respawn helpers.
void death( uint16_t killerId = 0xFFFF, uint8_t reason = 200 );
void request_spawn();
void request_class( int classId );
void spawn();

// Send a public chat line (as if typed). text is the raw message.
void say( const char *text );

// Break a vehicle's doors (RPC_DamageVehicle) -- driver + co-driver removed.
void damage_vehicle_doors( int vid );

// Destroy a vehicle (RPC_VehicleDestroyed).
void destroy_vehicle( int vid );

} // namespace rpc
