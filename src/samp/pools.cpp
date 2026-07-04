#include "pools.h"
#include "samp_sdk.h"   // full SA:MP type SDK (validates its layouts/static_asserts here)
#include "samp.h"
#include "addresses.h"
#include "../core/memory.h"

#include <windows.h>

/*
	samp/pools.cpp  --  accessors that walk samp.dll -> CNetGame -> pools.

	This runs inside the injected game process: a single bad dereference takes the
	whole game down. So EVERY pointer is null-checked and IsBadReadPtr-probed before
	it is followed. The walk is base -> INFO -> stSAMP -> pPools -> pPlayer/pVehicle,
	mirroring the original's g_Players / g_Vehicles globals.
*/

namespace samp {

stSAMPPools *pools() {
	stSAMP *ng = netgame();                       // *(stSAMP**)(base+INFO), guarded on base
	if ( !ng || IsBadReadPtr( ng, sizeof( stSAMP ) ) )
		return nullptr;

	auto *p = reinterpret_cast<stSAMPPools *>( ng->pPools );
	if ( !p || IsBadReadPtr( p, sizeof( stSAMPPools ) ) )
		return nullptr;
	return p;
}

stPlayerPool *player_pool() {
	stSAMPPools *p = pools();
	if ( !p )
		return nullptr;

	stPlayerPool *pp = p->pPlayer;
	if ( !pp || IsBadReadPtr( pp, sizeof( stPlayerPool ) ) )
		return nullptr;
	return pp;
}

stVehiclePool *vehicle_pool() {
	stSAMPPools *p = pools();
	if ( !p )
		return nullptr;

	stVehiclePool *vp = p->pVehicle;
	if ( !vp || IsBadReadPtr( vp, sizeof( stVehiclePool ) ) )
		return nullptr;
	return vp;
}

// A SA:MP vehicle id is streamed if it's listed and has a live GTA vehicle
// (Fuego's ValidVehicle + a non-null GTA handle). Used by /cc's restore logic.
bool vehicle_streamed( int vid ) {
	stVehiclePool *vp = vehicle_pool();
	if ( !vp || vid < 0 || vid >= SAMP_MAX_VEHICLES )
		return false;
	if ( !vp->iIsListed[vid] )
		return false;
	void *gv = vp->pGTA_Vehicle[vid];
	return gv && !IsBadReadPtr( gv, 4 );
}

int local_player_id() {
	stPlayerPool *pp = player_pool();
	if ( !pp )
		return -1;
	return static_cast<int>( pp->sLocalPlayerID );
}

stLocalPlayer *local_player() {
	stPlayerPool *pp = player_pool();
	if ( !pp )
		return nullptr;

	stLocalPlayer *lp = pp->pLocalPlayer;
	if ( !lp || IsBadReadPtr( lp, sizeof( stLocalPlayer ) ) )
		return nullptr;
	return lp;
}

stRemotePlayer *remote_player( int id ) {
	if ( id < 0 || id >= SAMP_MAX_PLAYERS )
		return nullptr;

	stPlayerPool *pp = player_pool();
	if ( !pp )
		return nullptr;

	stRemotePlayer *rp = pp->pRemotePlayer[id];
	if ( !rp || IsBadReadPtr( rp, sizeof( stRemotePlayer ) ) )
		return nullptr;
	return rp;
}

bool is_connected( int id ) {
	if ( id < 0 || id >= SAMP_MAX_PLAYERS )
		return false;

	stPlayerPool *pp = player_pool();
	if ( !pp )
		return false;

	// SA:MP marks a slot live with iIsListed == 1.
	if ( pp->iIsListed[id] != 1 )
		return false;

	stRemotePlayer *rp = pp->pRemotePlayer[id];
	if ( !rp || IsBadReadPtr( rp, sizeof( stRemotePlayer ) ) )
		return false;

	// pPlayerData is only populated for a genuinely connected remote player.
	stRemotePlayerData *pd = rp->pPlayerData;
	if ( !pd || IsBadReadPtr( pd, sizeof( stRemotePlayerData ) ) )
		return false;

	return true;
}

std::string player_name( int id ) {
	stPlayerPool *pp = player_pool();
	if ( !pp )
		return std::string();

	// The local player's name lives on the pool itself, not in pRemotePlayer[].
	if ( id == static_cast<int>( pp->sLocalPlayerID ) )
		return pp->strLocalPlayerName;

	stRemotePlayer *rp = remote_player( id );
	if ( !rp )
		return std::string();
	return rp->strPlayerName;
}

bool player_pos( int id, float out[3] ) {
	if ( !out )
		return false;
	if ( !is_connected( id ) )
		return false;

	stPlayerPool *pp = player_pool();
	if ( !pp )
		return false;

	stRemotePlayer *rp = pp->pRemotePlayer[id];
	if ( !rp )
		return false;

	stRemotePlayerData *pd = rp->pPlayerData;
	if ( !pd || IsBadReadPtr( pd, sizeof( stRemotePlayerData ) ) )
		return false;

	// Driver / passenger -> synced vehicle position, otherwise the on-foot position.
	const bool in_vehicle =
		( pd->bytePlayerState == PLAYER_STATE_PASSENGER ||
		  pd->bytePlayerState == PLAYER_STATE_DRIVER );

	const float *src = in_vehicle ? pd->fVehiclePosition : pd->fOnFootPos;
	out[0] = src[0];
	out[1] = src[1];
	out[2] = src[2];
	return true;
}

bool local_pos( float out[3] ) {
	if ( !out )
		return false;

	stLocalPlayer *lp = local_player();
	if ( !lp )
		return false;

	// sCurrentVehicleID == 0xFFFF (== (uint16_t)-1) means "not in a vehicle".
	const bool in_vehicle =
		( lp->sCurrentVehicleID != 0xFFFF && lp->sCurrentVehicleID != 0 );

	const float *src = in_vehicle ? lp->inCarData.fPosition : lp->onFootData.fPosition;
	out[0] = src[0];
	out[1] = src[1];
	out[2] = src[2];
	return true;
}

bool player_afk( int id ) {
	stRemotePlayer *rp = remote_player( id );
	return rp && rp->pPlayerData && rp->pPlayerData->iAFKState == 2;
}

// SA:MP's SYNCED actor health/armor (what Fuego's nametag reads -- the GTA ped's own
// health can diverge from this on servers that maintain their own values). -1 if unknown.
float player_synced_health( int id ) {
	stRemotePlayer *rp = remote_player( id );
	return ( rp && rp->pPlayerData ) ? rp->pPlayerData->fActorHealth : -1.0f;
}
float player_synced_armor( int id ) {
	stRemotePlayer *rp = remote_player( id );
	return ( rp && rp->pPlayerData ) ? rp->pPlayerData->fActorArmor : 0.0f;
}

void set_local_drunk( int level ) {
	stLocalPlayer *lp = local_player();
	if ( lp && lp->pSAMP_Actor && !IsBadWritePtr( lp->pSAMP_Actor, sizeof( void * ) ) )
		lp->pSAMP_Actor->drunkLevel = level;
}

bool player_shows_nametag( int id ) {
	stRemotePlayer *rp = remote_player( id );
	// iShowNameTag == 0 => the server has hidden this player's nametag.
	return rp && rp->pPlayerData && rp->pPlayerData->iShowNameTag != 0;
}

uint32_t player_color( int id ) {
	// SA:MP keeps the per-id name/marker colours as 0xRRGGBBAA (RGBA) at samp.dll+0x18F6C0.
	uint32_t b = base();
	if ( !b || id < 0 || id >= SAMP_MAX_PLAYERS )
		return 0;
	uint32_t *colors = reinterpret_cast<uint32_t *>( b + 0x18F6C0 );
	if ( IsBadReadPtr( colors + id, sizeof( uint32_t ) ) )
		return 0;
	return colors[id];
}

} // namespace samp
