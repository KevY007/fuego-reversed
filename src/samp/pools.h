#pragma once
/*
	samp/pools.h  --  SA:MP 0.3.DL player / vehicle pools (byte-exact, ported from Project Fuego).

	Reuses the sync payload structs (stOnFootData / stInCarData / stAimData /
	stTrailerData / stPassengerData) and stSAMP from structs.h -- do NOT redefine
	them here. Everything below is #pragma pack(1); reproduce that exactly or every
	offset shifts and the game crashes when injected.

	The GTA game-entity structs (CPed / CVehicle / CObject) are referenced only
	through pointers, so they are forward-declared -- no need to port them.

	static_asserts pin the POD-only offsets that the accessors depend on (none of
	them sit behind a std::string, so they hold regardless of the string's ABI size).
*/

#include <cstdint>
#include <cstddef>
#include <string>
#include "structs.h"

namespace samp {

// --- limits (from Fuego "enum Limits") --------------------------------------
constexpr int SAMP_MAX_PLAYERS  = 1004;
constexpr int SAMP_MAX_VEHICLES = 2000;

// stRemotePlayerData::bytePlayerState values (subset the accessors need).
enum PlayerState : int {
	PLAYER_STATE_NONE      = 0,
	PLAYER_STATE_ONFOOT    = 17,
	PLAYER_STATE_PASSENGER = 18,
	PLAYER_STATE_DRIVER    = 19,
	PLAYER_STATE_WASTED    = 32,
	PLAYER_STATE_SPAWNED   = 33,
};

// GTA game-entity interfaces -- pointer-only, forward declarations suffice.
struct actor_info;
struct vehicle_info;
struct object_info;

#pragma pack( push, 1 )

struct VECTOR { float X, Y, Z; };

// SA:MP wrapper around a GTA entity (CPed / CVehicle / CObject interface).
template <typename T>
struct stSAMPEntity {
	void     *pVTBL;
	uint8_t   byteUnk0[60];        // game CEntity object; usually empty
	T        *pGTAEntity;
	uint32_t  ulGTAEntityHandle;
};

struct stSAMPPed : public stSAMPEntity<actor_info> {
	int         usingCellPhone;
	uint8_t     byteUnk0[600];     // attached-objects info: (state + info + ptr) * 10
	actor_info *pGTA_Ped;
	uint8_t     byteUnk1[22];
	uint8_t     byteKeysId;
	uint16_t    ulGTA_UrinateParticle_ID;
	int         DrinkingOrSmoking;
	int         object_in_hand;
	int         drunkLevel;
	uint8_t     byteUnk2[5];
	int         isDancing;
	int         danceStyle;
	int         danceMove;
	uint8_t     byteUnk3[20];
	int         isUrinating;
};

struct stSAMPVehicle : public stSAMPEntity<vehicle_info> {
	uint32_t      bUnk0;
	vehicle_info *pGTA_Vehicle;
	uint8_t       byteUnk1[8];
	int           bIsMotorOn;
	int           iIsLightsOn;
	int           iIsLocked;
	uint8_t       byteIsObjective;
	int           iObjectiveBlipCreated;
	uint8_t       byteUnk2[16];
	uint8_t       byteColor[2];
	int           iColorSync;
	int           iColor_something;
};

// --- local-player sub-structures (not present in structs.h) -----------------
struct stHeadSync {
	float fHeadSync[3];
	int   iHeadSyncUpdateTick;
	int   iHeadSyncLookTick;
};

struct CAMERA_TARGET_AIM {
	uint16_t ObjectID;
	uint16_t VehicleID;
	uint16_t PlayerID;
	uint16_t ActorID;
};

struct PLAYER_SPAWN_INFO {
	uint8_t byteTeam;
	int     iSkin;
	int     iCustomModel;
	uint8_t unk;
	VECTOR  vecPos;
	float   fRotation;
	int     iSpawnWeapons[3];
	int     iSpawnWeaponsAmmo[3];
};

struct stSurfData {
	int            iIsSurfing;
	float          fSurfPosition[3];
	int            iUnk0;
	uint16_t       sSurfingVehicleID;
	uint32_t       ulSurfTick;
	stSAMPVehicle *pSurfingVehicle;
	int            iUnk1;
	int            iSurfMode;       // 0 = not surfing, 1 = moving, 2 = fixed on vehicle
};

struct stDamageData {
	uint16_t sVehicleID_lastDamageProcessed;
	int      iBumperDamage;
	int      iDoorDamage;
	uint8_t  byteLightDamage;
	uint8_t  byteWheelDamage;
};

struct stLocalPlayer {
	stSAMPPed        *pSAMP_Actor;
	stTrailerData     trailerData;
	stOnFootData      onFootData;
	stPassengerData   passengerData;
	stInCarData       inCarData;
	stAimData         aimData;
	int               iIsActive;
	int               iIsWasted;
	uint16_t          sCurrentVehicleID;
	uint16_t          sLastVehicleID;
	uint16_t          sCurrentAnimID;
	uint16_t          sAnimFlags;
	uint32_t          ulUnk0;
	CAMERA_TARGET_AIM cameraTarget;
	uint32_t          ulCameraTargetTick;
	stHeadSync        headSyncData;
	uint32_t          ulHeadSyncTick;
	int               iIsSpectating;
	uint8_t           byteTeamID2;
	uint16_t          usUnk2;
	uint32_t          ulSendTick;
	uint32_t          ulSpectateTick;
	uint32_t          ulAimTick;
	uint32_t          ulStatsUpdateTick;
	int               iSpawnClassLoaded;
	uint32_t          ulSpawnSelectionTick;
	uint32_t          ulSpawnSelectionStart;
	PLAYER_SPAWN_INFO spawnInfo;
	int               iIsActorAlive;
	uint32_t          ulWeapUpdateTick;
	uint16_t          sAimingAtPlayerID;
	uint16_t          sAimingAtActorID;
	uint8_t           byteCurrentWeapon;
	uint8_t           byteWeaponInventory[13];
	int               iWeaponAmmo[13];
	int               iPassengerDriveBy;
	uint8_t           byteCurrentInterior;
	int               iIsInRCVehicle;
	char              szPlayerName[256];
	stSurfData        surfData;
	int               iClassSelectionOnDeath;
	int               iSpawnClassID;
	int               iRequestToSpawn;
	int               iIsInSpawnScreen;
	uint32_t          ulDisplayZoneTick;
	uint8_t           byteSpectateMode;       // 3 = vehicle, 4 = player, side = 14, fixed = 15
	uint8_t           byteSpectateType;       // 0 = none, 1 = player, 2 = vehicle
	int               iSpectateID;
	int               iInitiatedSpectating;
	stDamageData      vehicleDamageData;
};

struct stRemotePlayerData {
	uint16_t        sPlayerID;
	uint16_t        sVehicleID;
	stSAMPPed      *pSAMP_Actor;
	stSAMPVehicle  *pSAMP_Vehicle;
	int             iHasJetPack;
	int             iShowNameTag;
	int32_t         bUsingJetPack;            // BOOL
	uint8_t         byteSpecialAction;
	uint8_t         byteTeamID;
	uint8_t         bytePlayerState;
	uint8_t         byteSeatID;
	int32_t         bIsNpc;                   // BOOL
	int             iPassengerDriveBy;
	stPassengerData passengerData;
	stOnFootData    onFootData;
	stInCarData     inCarData;
	stTrailerData   trailerData;
	stAimData       aimData;
	uint32_t        ulUnk0[3];
	float           fOnFootPos[3];
	float           fOnFootMoveSpeed[3];
	float           fVehiclePosition[3];
	float           fVehicleMoveSpeed[3];
	void           *pUnk0;
	uint8_t         byteUnk1[60];
	VECTOR          vecVehiclePosOffset;
	float           fVehicleRoll[4];
	float           fActorArmor;
	float           fActorHealth;
	uint32_t        ulUnk1[3];
	int             iLastAnimationID;
	uint8_t         byteUpdateFromNetwork;
	uint32_t        dwTick;
	uint32_t        dwLastStreamedInTick;     // 0 while currently streamed in
	uint32_t        ulUnk2;
	int             iAFKState;
	stHeadSync      headSyncData;
	int             iGlobalMarkerLoaded;
	int             iGlobalMarkerLocation[3];
	uint32_t        ulGlobalMarker_GTAID;
};

struct stRemotePlayer {
	int                 iScore;
	int                 iIsNPC;
	stRemotePlayerData *pPlayerData;
	int                 iPing;
	void               *pVTBL_txtHandler;
	std::string         strPlayerName;
};

struct stPlayerPool {
	uint16_t        sLocalPlayerID;
	void           *pVTBL_txtHandler;
	std::string     strLocalPlayerName;
	stLocalPlayer  *pLocalPlayer;
	uint32_t        ulMaxPlayerID;
	stRemotePlayer *pRemotePlayer[SAMP_MAX_PLAYERS];
	int             iIsListed[SAMP_MAX_PLAYERS];
	int32_t         bSavedCheckCollision[SAMP_MAX_PLAYERS];   // BOOL
	int             iLocalPlayerPing;
	int             iLocalPlayerScore;
};

struct stVehiclePool {
	int            iVehicleCount;
	void          *pUnk0;
	uint8_t        byteSpace1[0x112C];
	stSAMPVehicle *pSAMP_Vehicle[SAMP_MAX_VEHICLES];
	int            iIsListed[SAMP_MAX_VEHICLES];
	vehicle_info  *pGTA_Vehicle[SAMP_MAX_VEHICLES];
	uint8_t        byteSpace2[SAMP_MAX_VEHICLES * 6];
	uint32_t       ulShit[SAMP_MAX_VEHICLES];
	int            iIsListed2[SAMP_MAX_VEHICLES];
	uint32_t       byteSpace3[SAMP_MAX_VEHICLES * 2];
	float          fSpawnPos[SAMP_MAX_VEHICLES][3];
	int            iInitiated;
};

struct stSAMPPools {
	void          *pMenu;
	void          *pActor;
	stPlayerPool  *pPlayer;        // +0x08
	stVehiclePool *pVehicle;
	void          *pPickup;
	void          *pObject;
	void          *pGangzone;
	void          *pText3D;
	void          *pTextdraw;
};

#pragma pack( pop )

// --- offset guards (POD-only; independent of the std::string ABI size) ------
static_assert( sizeof( VECTOR ) == 12,                                       "VECTOR must be 12 bytes" );
static_assert( offsetof( stSAMPPools, pPlayer )                == 0x08,       "stSAMPPools.pPlayer must be +0x08" );
static_assert( offsetof( stLocalPlayer, onFootData )           == 0x3A,       "stLocalPlayer.onFootData must be +0x3A" );
static_assert( offsetof( stLocalPlayer, inCarData )            == 0x96,       "stLocalPlayer.inCarData must be +0x96" );
static_assert( offsetof( stRemotePlayerData, bytePlayerState ) == 0x1A,       "stRemotePlayerData.bytePlayerState must be +0x1A" );
static_assert( offsetof( stRemotePlayerData, fOnFootPos )      == 0x120,      "stRemotePlayerData.fOnFootPos must be +0x120" );
static_assert( offsetof( stRemotePlayerData, fVehiclePosition )== 0x138,      "stRemotePlayerData.fVehiclePosition must be +0x138" );
static_assert( offsetof( stRemotePlayerData, fActorHealth )    == 0x1B0,      "stRemotePlayerData.fActorHealth must be +0x1B0" );

// --- accessor helpers (implemented in pools.cpp) ----------------------------
// Every one is null-safe: returns null / -1 / "" / false rather than crashing
// when the client is not loaded, not in-game, or a pointer is bad.
stSAMPPools    *pools();                         // netgame()->pPools
stPlayerPool   *player_pool();                   // pools()->pPlayer
stVehiclePool  *vehicle_pool();                  // pools()->pVehicle
bool            vehicle_streamed( int vid );     // listed + live GTA handle (ValidVehicle)
int             local_player_id();               // player_pool()->sLocalPlayerID, or -1
stLocalPlayer  *local_player();                  // player_pool()->pLocalPlayer
stRemotePlayer *remote_player( int id );         // player_pool()->pRemotePlayer[id]
bool            is_connected( int id );          // iIsListed + remote_player + pPlayerData
std::string     player_name( int id );           // remote (or local) player name, "" if none
bool            player_pos( int id, float out[3] );  // net pos: in-vehicle -> fVehiclePosition else fOnFootPos
bool            local_pos( float out[3] );       // local player's inCarData / onFootData position
bool            player_afk( int id );            // remote player's AFK state (iAFKState == 2)
void            set_local_drunk( int level );    // local SA:MP ped drunkLevel (desync uses 0)
bool            player_shows_nametag( int id );  // remote player's iShowNameTag (server-hidden = false)
uint32_t        player_color( int id );          // SA:MP marker/name colour (0xRRGGBBAA), 0 if none
float           player_synced_health( int id );  // SA:MP synced actor health (-1 unknown)
float           player_synced_armor( int id );   // SA:MP synced actor armor

} // namespace samp
