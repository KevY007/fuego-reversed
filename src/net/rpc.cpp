#include "rpc.h"
#include "RakClient.h"
#include "BitStream.h"
#include "../samp/samp.h"
#include "../samp/pools.h"      // samp::local_player() -- the 'this' for CInput::Say (chat)
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

void give_damage( int targetId, int weapon, int bodypart, bool ordered ) {
	traceLastFunc( "rpc::give_damage" );
	if ( !g_RakClient ) return;
	if ( weapon < 0 || weapon >= 55 ) weapon = 0;
	BitStream bs;
	bs.Write( false );                                 // bTakeDamage=false -> we give it
	bs.Write<uint16_t>( (uint16_t) targetId );
	bs.Write( fWeaponDamage[weapon] );
	bs.Write<uint32_t>( (uint32_t) weapon );
	bs.Write<uint32_t>( (uint32_t) bodypart );
	// Fuego 1:1: silent-aim sends RELIABLE_SEQUENCED (cheat_samp.cpp:1900) so only the
	// freshest shot survives reorder; the auto-killer sends default RELIABLE_ORDERED
	// (cheat_samp.cpp:5583) so every rapid multi-hit RPC lands. ordered=true picks the
	// killer path -- SEQUENCED would let RakNet DROP the killer's out-of-order hits.
	if ( ordered )
		g_RakClient->RPC( RPC_GiveTakeDamage, &bs );   // default HIGH/RELIABLE_ORDERED/0
	else
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
	// Fuego 1:1 (addSayToChatWindow): a leading '/' is a SERVER COMMAND -- it must go
	// through CInput::SendCommand (this = the input object) so the server RUNS it
	// (RPC_ServerCommand id 50). Anything else is chat -- CInput::Say (this = the LOCAL
	// PLAYER object). Routing "/uns ..." through the chat path only posts it as text and
	// the command never fires; the leading-slash branch is required.
	if ( text[0] == '/' ) {
		void *input = samp::input_ptr();
		if ( !input ) return;
		mem::thiscall<void>( b + samp::addr::FUNC_SEND_CMD, input, text );
	} else {
		samp::stLocalPlayer *lp = samp::local_player();
		if ( !lp ) return;
		mem::thiscall<void>( b + samp::addr::FUNC_SAY, lp, text );
	}
}

} // namespace rpc
