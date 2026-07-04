#include "rpc.h"
#include "RakClient.h"
#include "BitStream.h"
#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../core/memory.h"
#include "../core/trace.h"

namespace rpc {

// SA:MP per-weapon damage table (ported from Project Fuego, 0.3.DL-correct).
const float fWeaponDamage[55] = {
	1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   // 0-9
	1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 82.5f, 0.0f, 1.0f, 9.9f,  // 10-19
	46.2f, 0.0f, 8.25f, 13.2f, 46.2f, 49.5f, 49.5f, 39.6f, 6.6f, 8.25f, // 20-29
	9.900001f, 9.900001f, 6.6f, 24.750001f, 41.25f, 82.5f, 82.5f, 1.0f, 46.2f, 82.5f, // 30-39
	0.0f, 0.33f, 0.33f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.64f, 9.9f, // 40-49
	330.0f, 82.5f, 1.0f, 1.0f, 165.0f                             // 50-54
};

void give_damage( int targetId, int weapon, int bodypart ) {
	traceLastFunc( "rpc::give_damage" );
	if ( !g_RakClient ) return;
	if ( weapon < 0 || weapon >= 55 ) weapon = 0;
	BitStream bs;
	bs.Write( false );                                 // bTakeDamage=false -> we give it
	bs.Write<uint16_t>( (uint16_t) targetId );
	bs.Write( fWeaponDamage[weapon] );
	bs.Write<uint32_t>( (uint32_t) weapon );
	bs.Write<uint32_t>( (uint32_t) bodypart );
	g_RakClient->RPC( RPC_GiveTakeDamage, &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false );
}

void give_damage_amount( int targetId, float dmg, int weapon, int bodypart ) {
	traceLastFunc( "rpc::give_damage_amount" );
	if ( !g_RakClient ) return;
	BitStream bs;
	bs.Write( false );                          // bTakeDamage=false -> we give it
	bs.Write<uint16_t>( (uint16_t) targetId );
	bs.Write( dmg );                            // explicit damage amount
	bs.Write<uint32_t>( (uint32_t) weapon );
	bs.Write<uint32_t>( (uint32_t) bodypart );
	g_RakClient->RPC( RPC_GiveTakeDamage, &bs );
}

void take_damage( int issuerId, int weapon, int bodypart ) {
	traceLastFunc( "rpc::take_damage" );
	if ( !g_RakClient ) return;
	if ( weapon < 0 || weapon >= 55 ) weapon = 0;
	BitStream bs;
	bs.Write( true );                                  // bTakeDamage=true -> we take it
	bs.Write<uint16_t>( (uint16_t) issuerId );
	bs.Write( fWeaponDamage[weapon] );
	bs.Write<uint32_t>( (uint32_t) weapon );
	bs.Write<uint32_t>( (uint32_t) bodypart );
	g_RakClient->RPC( RPC_GiveTakeDamage, &bs, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false );
}

void death( uint16_t killerId, uint8_t reason ) {
	traceLastFunc( "rpc::death" );
	if ( !g_RakClient ) return;
	BitStream bs;
	bs.Write( reason );
	bs.Write( killerId );
	g_RakClient->RPC( RPC_Death, &bs );
}

void request_spawn() {
	traceLastFunc( "rpc::request_spawn" );
	if ( !g_RakClient ) return;
	BitStream bs;
	g_RakClient->RPC( RPC_RequestSpawn, &bs );
}

void request_class( int classId ) {
	traceLastFunc( "rpc::request_class" );
	if ( !g_RakClient ) return;
	BitStream bs;
	bs.Write( classId );
	g_RakClient->RPC( RPC_RequestClass, &bs );
}

void spawn() {
	traceLastFunc( "rpc::spawn" );
	if ( !g_RakClient ) return;
	BitStream bs;
	g_RakClient->RPC( RPC_RequestSpawn, &bs );
	g_RakClient->RPC( RPC_Spawn, &bs );
}

void damage_vehicle_doors( int vid ) {
	traceLastFunc( "rpc::damage_vehicle_doors" );
	if ( !g_RakClient ) return;
	BitStream bs;
	bs.Write<uint16_t>( (uint16_t) vid );
	bs.Write<uint32_t>( 0u );                                 // panels
	bs.Write<uint32_t>( ( 0x04u << 24 ) | ( 0x04u << 16 ) );  // doors: driver+co-driver removed
	bs.Write<uint8_t>( 0 );                                    // lights
	bs.Write<uint8_t>( 0 );                                    // tyres
	g_RakClient->RPC( RPC_DamageVehicle, &bs );
}

void destroy_vehicle( int vid ) {
	traceLastFunc( "rpc::destroy_vehicle" );
	if ( !g_RakClient ) return;
	BitStream bs;
	bs.Write( vid );
	g_RakClient->RPC( RPC_VehicleDestroyed, &bs );
}

void say( const char *text ) {
	traceLastFunc( "rpc::say" );
	const uint32_t b = samp::base();
	if ( !b || !text || !text[0] ) return;
	// SA:MP CInput::Say(this, text) sends the line through the normal chat path.
	void *input = samp::input_ptr();
	if ( !input ) return;
	mem::thiscall<void>( b + samp::addr::FUNC_SAY, input, text );
}

} // namespace rpc
