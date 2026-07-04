#pragma once
/*
	samp/structs.h  --  SA:MP 0.3.DL structures (byte-exact, ported from Project Fuego).

	Only what's needed so far. SA:MP structs are #pragma pack(1) -- reproduce that
	exactly or every offset shifts. static_asserts pin the critical offsets so a
	layout mistake fails the build instead of crashing in-game.
*/

#include <cstdint>
#include <cstddef>

namespace samp {

// CNetGame::iGameState values (the connect state machine). VERIFIED across
// SAMP-API / mod_sa / the reconstructed client.
enum Gamestate : int {
	GAMESTATE_WAIT_CONNECT = 9,
	GAMESTATE_CONNECTING   = 13,
	GAMESTATE_CONNECTED    = 14,
	GAMESTATE_AWAIT_JOIN   = 15,
	GAMESTATE_RESTARTING   = 18,
};

#pragma pack( push, 1 )
struct stSAMP // == CNetGame
{
	uint8_t   _pad0[20];
	void     *pUnk0;
	void     *pServerInfo;
	uint8_t   _pad1[16];
	void     *pRakClientInterface;         // +0x2C
	char      szIP[256 + 1];
	char      szHostname[256 + 1];
	uint8_t   _pad2;
	bool      m_bUpdateCameraTarget;
	bool      m_bNoNameTagStatus;
	uint32_t  ulPort;
	int       m_bLanMode;
	uint32_t  ulMapIcons[100];
	int       iGameState;                  // +0x3CD
	uint32_t  ulConnectTick;
	void     *pSettings;
	uint8_t   _pad3[5];
	void     *pPools;
};
#pragma pack( pop )

static_assert( offsetof( stSAMP, pRakClientInterface ) == 0x2C,  "stSAMP.pRakClientInterface must be +0x2C" );
static_assert( offsetof( stSAMP, iGameState )          == 0x3CD, "stSAMP.iGameState must be +0x3CD (0.3.DL)" );

// --- SA:MP sync structures (the payloads of the outgoing sync packets that the
//     packet-rewrite cheats manipulate). Ported verbatim from Fuego samp.h. ---
#pragma pack( push, 1 )

struct stSAMPKeys {
	uint8_t keys_primaryFire : 1, keys_horn__crouch : 1, keys_secondaryFire__shoot : 1,
	        keys_accel__zoomOut : 1, keys_enterExitCar : 1, keys_decel__jump : 1,
	        keys_circleRight : 1, keys_aim : 1, keys_circleLeft : 1, keys_landingGear__lookback : 1,
	        keys_unknown__walkSlow : 1, keys_specialCtrlUp : 1, keys_specialCtrlDown : 1,
	        keys_specialCtrlLeft : 1, keys_specialCtrlRight : 1, keys__unused : 1;
};

struct stOnFootData {
	uint16_t sLeftRightKeys, sUpDownKeys;
	union { uint16_t sKeys; stSAMPKeys stSampKeys; };
	float    fPosition[3];
	float    fQuaternion[4];
	uint8_t  byteHealth, byteArmor, byteCurrentWeapon, byteSpecialAction;
	float    fMoveSpeed[3];
	float    fSurfingOffsets[3];
	uint16_t sSurfingVehicleID;
	short    sCurrentAnimationID, sAnimFlags;
};

struct stInCarData {
	uint16_t sVehicleID, sLeftRightKeys, sUpDownKeys;
	union { uint16_t sKeys; stSAMPKeys stSampKeys; };
	float    fQuaternion[4];
	float    fPosition[3];
	float    fMoveSpeed[3];
	float    fVehicleHealth;
	uint8_t  bytePlayerHealth, byteArmor, byteCurrentWeapon, byteSiren, byteLandingGearState;
	uint16_t sTrailerID;
	union { uint16_t HydraThrustAngle[2]; float fTrainSpeed; float fBikeSideAngle; };
};

struct stAimData {
	uint8_t byteCamMode;
	float   vecAimf1[3];
	float   vecAimPos[3];
	float   fAimZ;
	uint8_t byteCamExtZoom : 6, byteWeaponState : 2;
	uint8_t byteAspectRatio;
};

struct stBulletData {
	uint8_t  byteType;
	uint16_t sTargetID;
	float    fOrigin[3];
	float    fTarget[3];
	float    fCenter[3];
	uint8_t  byteWeaponID;
};

// Spectator sync payload (used by Smart Invisibility to masquerade as a spectator).
struct stSpectatorData {
	uint16_t lrAnalog;
	uint16_t udAnalog;
	uint16_t wKeys;
	float    fPosition[3];
};

struct stTrailerData {
	uint16_t sTrailerID;
	float    fPosition[3];
	float    fQuaternion[4];
	float    fSpeed[3];
	float    fSpin[3];
};

struct stPassengerData {
	uint16_t sVehicleID;
	uint8_t  byteSeatID, byteCurrentWeapon, byteHealth, byteArmor;
	uint16_t sLeftRightKeys, sUpDownKeys;
	union { uint16_t sKeys; stSAMPKeys stSampKeys; };
	float    fPosition[3];
};

struct stUnoccupiedData {
	int16_t  sVehicleID;
	uint8_t  byteSeatID;
	float    fRoll[3];
	float    fDirection[3];
	float    fPosition[3];
	float    fMoveSpeed[3];
	float    fTurnSpeed[3];
	float    fVehicleHealth;
};

#pragma pack( pop )

// NOTE: the sync/control packet IDs (ID_PLAYER_SYNC = 207, ID_VEHICLE_SYNC =
// 200, ...) are already defined by RakNet's PacketEnumeration in RakClient.h.
// Don't redeclare them here or every use becomes ambiguous.

} // namespace samp
