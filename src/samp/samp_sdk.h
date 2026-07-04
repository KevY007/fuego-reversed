#pragma once
/*
	samp/samp_sdk.h  --  the complete SA:MP 0.3.DL type SDK.

	Include THIS one header to get every SA:MP datatype the client exposes:
	  * structs.h  -> CNetGame (stSAMP), the sync payloads, key bits
	  * pools.h    -> player / vehicle pools, entity wrappers, local & remote
	                  player state, and the null-safe accessor helpers
	  * RakClient  -> the RakNet client interface + full RPC / packet id enums
	  * BitStream  -> RakNet BitStream reader/writer

	and, defined below, the remaining pools + UI structures that pools.h leaves
	as raw void* (pickups, objects, gangzones, 3D text labels, textdraws, actors)
	plus every SA:MP enum and typed accessors for those pools.

	Offsets are byte-exact and #pragma pack(1), ported from Project Fuego's samp.h
	(the authoritative 0.3.DL layout) and cross-checked against the SA-MP packet
	list. GTA game entities (CPed / CVehicle / CObject) are referenced only through
	the forward-declared actor_info / vehicle_info / object_info pointers, so this
	SDK stays usable from every translation unit (it never pulls in gta-reversed).

	For the GTA side, see gta/gtahost.h (the C++20-safe accessor layer) and
	gta/gta_reversed.h (the umbrella for C++23 TUs that want the raw game classes).
*/

#include "samp.h"        // core API + ChatMessageType
#include "structs.h"
#include "pools.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"

namespace samp {

// --- limits (Fuego "enum Limits"; PLAYERS/VEHICLES already live in pools.h) --
constexpr int SAMP_MAX_ACTORS             = 1000;
constexpr int SAMP_MAX_PICKUPS            = 4096;
constexpr int SAMP_MAX_OBJECTS            = 2100;
constexpr int SAMP_MAX_GANGZONES          = 1024;
constexpr int SAMP_MAX_3DTEXTS            = 2048;
constexpr int SAMP_MAX_TEXTDRAWS          = 2048;
constexpr int SAMP_MAX_PLAYERTEXTDRAWS    = 256;
constexpr int SAMP_MAX_CLIENTCMDS         = 144;
constexpr int SAMP_MAX_MENUS              = 128;
constexpr int SAMP_MAX_PLAYER_NAME        = 24;
constexpr int SAMP_ALLOWED_NAME_LENGTH    = 20;
constexpr int SAMP_MAX_MAPICONS           = 100;

// --- enums (the ones not already in structs.h / pools.h) --------------------
enum DialogStyle : int {
	DIALOG_STYLE_MSGBOX,
	DIALOG_STYLE_INPUT,
	DIALOG_STYLE_LIST,
	DIALOG_STYLE_PASSWORD,
	DIALOG_STYLE_TABLIST,
	DIALOG_STYLE_TABLIST_HEADERS,
};

// (ChatMessageType lives in samp.h, included above.)

enum ChatDisplayMode : int {
	CHAT_WINDOW_MODE_OFF   = 0,
	CHAT_WINDOW_MODE_LIGHT = 1,
	CHAT_WINDOW_MODE_FULL  = 2,
};

enum MarkersMode : int {
	PLAYER_MARKERS_MODE_OFF,
	PLAYER_MARKERS_MODE_GLOBAL,
	PLAYER_MARKERS_MODE_STREAMED,
};

enum SpecialAction : int {
	SPECIAL_ACTION_NONE,
	SPECIAL_ACTION_DUCK,
	SPECIAL_ACTION_USEJETPACK,
	SPECIAL_ACTION_ENTER_VEHICLE,
	SPECIAL_ACTION_EXIT_VEHICLE,
	SPECIAL_ACTION_DANCE1,
	SPECIAL_ACTION_DANCE2,
	SPECIAL_ACTION_DANCE3,
	SPECIAL_ACTION_DANCE4,
	SPECIAL_ACTION_HANDSUP,
	SPECIAL_ACTION_USECELLPHONE,
	SPECIAL_ACTION_SITTING,
	SPECIAL_ACTION_STOPUSECELLPHONE,
	SPECIAL_ACTION_DRINK_BEER = 20,
	SPECIAL_ACTION_SMOKE_CIGGY,
	SPECIAL_ACTION_DRINK_WINE,
	SPECIAL_ACTION_DRINK_SPRUNK,
	SPECIAL_ACTION_CUFFED,
	SPECIAL_ACTION_CARRY,
};

#pragma pack( push, 1 )

// --- server / connection metadata -------------------------------------------
struct stServerInfo {
	uint32_t uiIP;
	uint16_t usPort;
	// remainder unmapped
};

struct stServerPresets {
	uint8_t  byteCJWalk;
	int      m_iDeathDropMoney;
	float    fWorldBoundaries[4];
	bool     m_bAllowWeapons;
	float    fGravity;
	uint8_t  byteDisableInteriorEnterExits;
	uint32_t ulVehicleFriendlyFire;
	bool     m_byteHoldTime;
	bool     m_bInstagib;
	bool     m_bZoneNames;
	bool     m_byteFriendlyFire;
	int      iClassesAvailable;
	float    fNameTagsDistance;
	bool     m_bManualVehicleEngineAndLight;
	uint8_t  byteWorldTime_Hour;
	uint8_t  byteWorldTime_Minute;
	uint8_t  byteWeather;
	uint8_t  byteNoNametagsBehindWalls;
	int      iPlayerMarkersMode;
	float    fGlobalChatRadiusLimit;
	uint8_t  byteShowNameTags;
	bool     m_bLimitGlobalChatRadius;
};

// --- dialogs ----------------------------------------------------------------
struct stDialogInfo {
	void    *m_pD3DDevice;                    // IDirect3DDevice9*
	int      iTextPosX;
	int      iTextPosY;
	uint32_t uiDialogSizeX;
	uint32_t uiDialogSizeY;
	int      iBtnOffsetX;
	int      iBtnOffsetY;
	void    *pDialog;                         // _CDXUTDialog*
	void    *pList;                           // _CDXUTListBox*
	void    *pEditBox;                        // _CDXUTIMEEditBox*
	int      iIsActive;
	int      iType;
	uint32_t DialogID;
	char    *pText;
	uint32_t uiTextWidth;
	uint32_t uiTextHeight;
	char     szCaption[65];
	int      bServerside;
};

// --- textdraws --------------------------------------------------------------
struct stTextDrawTransmit {
	union {
		uint8_t byteFlags;
		struct {
			uint8_t byteBox : 1, byteLeft : 1, byteRight : 1, byteCenter : 1,
			        byteProportional : 1, bytePadding : 3;
		};
	};
	float    fLetterWidth;
	float    fLetterHeight;
	uint32_t dwLetterColor;
	float    fBoxWidth;
	float    fBoxHeight;
	uint32_t dwBoxColor;
	uint8_t  byteShadow;
	uint8_t  byteOutline;
	uint32_t dwBackgroundColor;
	uint8_t  byteStyle;
	uint8_t  byteUNK;
	float    fX;
	float    fY;
	uint16_t sModel;
	float    fRot[3];
	float    fZoom;
	uint16_t sColor[2];
};

struct stTextdraw {
	char     szText[800 + 1];
	char     szString[1600 + 2];
	float    fLetterWidth;
	float    fLetterHeight;
	uint32_t dwLetterColor;
	uint8_t  byte_unk;
	uint8_t  byteCenter;
	uint8_t  byteBox;
	float    fBoxSizeX;
	float    fBoxSizeY;
	uint32_t dwBoxColor;
	uint8_t  byteProportional;
	uint32_t dwShadowColor;
	uint8_t  byteShadowSize;
	uint8_t  byteOutline;
	uint8_t  byteLeft;
	uint8_t  byteRight;
	int      iStyle;
	float    fX;
	float    fY;
	uint8_t  unk[8];
	uint32_t dword99B;
	uint32_t dword99F;
	uint32_t index;
	uint8_t  byte9A7;
	uint16_t sModel;
	float    fRot[3];
	float    fZoom;
	uint16_t sColor[2];
	uint8_t  f9BE;
	uint8_t  byte9BF;
	uint8_t  byte9C0;
	uint32_t dword9C1;
	uint32_t dword9C5;
	uint32_t dword9C9;
	uint32_t dword9CD;
	uint8_t  byte9D1;
	uint32_t dword9D2;
};

struct stTextdrawPool {
	int         iIsListed[SAMP_MAX_TEXTDRAWS];
	int         iPlayerTextDraw[SAMP_MAX_PLAYERTEXTDRAWS];
	stTextdraw *textdraw[SAMP_MAX_TEXTDRAWS];
	stTextdraw *playerTextdraw[SAMP_MAX_PLAYERTEXTDRAWS];
};

// --- pickups ----------------------------------------------------------------
struct stPickup {
	int   iModelID;
	int   iType;
	float fPosition[3];
};

struct stPickupPool {
	int      iPickupsCount;
	uint32_t ul_GTA_PickupID[SAMP_MAX_PICKUPS];
	int      iPickupID[SAMP_MAX_PICKUPS];
	int      iTimePickup[SAMP_MAX_PICKUPS];
	uint8_t  unk[SAMP_MAX_PICKUPS * 3];
	stPickup pickup[SAMP_MAX_PICKUPS];
};

// --- objects (world objects; the entity wrapper is stObject) -----------------
struct stObject : public stSAMPEntity<object_info> {
	uint8_t  byteUnk0[2];
	uint32_t ulUnk1;
	int      iModel;
	uint16_t byteUnk2;
	float    fDrawDistance;
	float    fUnk;
	float    fPos[3];
	uint8_t  byteUnk3[68];
	uint8_t  byteUnk4;
	float    fRot[3];
};

struct stObjectPool {
	int       iObjectCount;
	int       iIsListed[SAMP_MAX_OBJECTS];
	stObject *object[SAMP_MAX_OBJECTS];
};

// --- gangzones --------------------------------------------------------------
struct stGangzone {
	float    fPosition[4];
	uint32_t dwColor;
	uint32_t dwAltColor;
};

struct stGangzonePool {
	stGangzone *pGangzone[SAMP_MAX_GANGZONES];
	int         iIsListed[SAMP_MAX_GANGZONES];
};

// --- 3D text labels ---------------------------------------------------------
struct stTextLabel {
	char    *pText;
	uint32_t color;
	float    fPosition[3];
	float    fMaxViewDistance;
	uint8_t  byteShowBehindWalls;
	uint16_t sAttachedToPlayerID;
	uint16_t sAttachedToVehicleID;
};

struct stTextLabelPool {
	stTextLabel textLabel[SAMP_MAX_3DTEXTS];
	int         iIsListed[SAMP_MAX_3DTEXTS];
};

// --- actors (0.3.DL server-spawned peds), exact Fuego layout ----------------
struct stActorPool {
	int               iLastActorID;
	stSAMPEntity<void> *pActor[SAMP_MAX_ACTORS];
	int               iIsListed[SAMP_MAX_ACTORS];
	actor_info       *pGTAPed[SAMP_MAX_ACTORS];
	uint32_t          ulUnk0[SAMP_MAX_ACTORS];
	uint32_t          ulUnk1[SAMP_MAX_ACTORS];
};

// --- small misc payloads ----------------------------------------------------
struct stStatsData {
	int iMoney;
	int iAmmo;
};

// (stUnoccupiedData lives in structs.h with the other sync payloads.)

// --- UI layer (chat window / kill feed / scoreboard / command input). D3D
//     interface pointers are void* and D3DCOLOR is uint32_t so this header needs
//     no d3d9/d3dx9 include; the byte layout is identical either way. ---------
typedef void ( __cdecl *CMDPROC )( char * );   // SA:MP client-command handler

struct stChatEntry {
	uint32_t SystemTime;
	char     szPrefix[28];
	char     szText[144];
	uint8_t  unknown[64];
	int      iType;              // 2 text+prefix, 4 server msg, 8 debug
	uint32_t clTextColor;        // D3DCOLOR
	uint32_t clPrefixColor;
};

struct stFontRenderer {
	void *m_pChatFont;           // ID3DXFont
	void *m_pLittleFont;
	void *m_pChatShadowFont;
	void *m_pLittleShadowFont;
	void *m_pCarNumberFont;
	void *m_pTempSprite;         // ID3DXSprite
	void *m_pD3DDevice;          // IDirect3DDevice9
	char *m_pszTextBuffer;
};

struct stChatInfo {
	int             pagesize;
	char           *pLastMsgText;
	int             iChatWindowMode;
	uint8_t         bTimestamps;
	uint32_t        m_iLogFileExist;
	char            logFilePathChatLog[260 + 1];   // MAX_PATH+1
	void           *pGameUI;
	void           *pEditBackground;
	void           *pDXUTScrollBar;
	uint32_t        clTextColor;
	uint32_t        clInfoColor;
	uint32_t        clDebugColor;
	uint32_t        m_lChatWindowBottom;
	stChatEntry     chatEntry[100];
	stFontRenderer *m_pFontRenderer;
	void           *m_pChatTextSprite;
	void           *m_pSprite;
	void           *m_pD3DDevice;
	int             m_iRenderMode;
	void           *pID3DXRenderToSurface;
	void           *m_pTexture;
	void           *pSurface;
	void           *pD3DDisplayMode;
	int             iUnk1[3];
	int             iUnk2;
	int             m_iRedraw;
	int             m_nPrevScrollBarPosition;
	int             m_iFontSizeY;
	int             m_iTimestampWidth;
};

struct stInputBox {
	void    *pUnknown;
	uint8_t  bIsChatboxOpen;
	uint8_t  bIsMouseInChatbox;
	uint8_t  bMouseClick_related;
	uint8_t  unk;
	uint32_t dwPosChatInput[2];
	uint8_t  unk2[263];
	int      iCursorPosition;
	uint8_t  unk3;
	int      iMarkedText_startPos;
	uint8_t  unk4[20];
	int      iMouseLeftButton;
};

struct stInputInfo {
	void       *pD3DDevice;
	void       *pDXUTDialog;
	stInputBox *pDXUTEditBox;
	CMDPROC     pCMDs[SAMP_MAX_CLIENTCMDS];
	char        szCMDNames[SAMP_MAX_CLIENTCMDS][33];
	int         iCMDCount;
	int         iInputEnabled;
	char        szInputBuffer[129];
	char        szRecallBufffer[10][129];
	char        szCurrentBuffer[129];
	int         iCurrentRecall;
	int         iTotalRecalls;
	CMDPROC     pszDefaultCMD;
};

struct stKillEntry {
	char     szKiller[25];
	char     szVictim[25];
	uint32_t clKillerColor;
	uint32_t clVictimColor;
	uint8_t  byteType;
};

struct stKillInfo {
	int         iEnabled;
	stKillEntry killEntry[5];
	int         iLongestNickLength;
	int         iOffsetX;
	int         iOffsetY;
	void       *pD3DFont;
	void       *pWeaponFont1;
	void       *pWeaponFont2;
	void       *pSprite;
	void       *pD3DDevice;
	int         iAuxFontInited;
	void       *pAuxFont1;
	void       *pAuxFont2;
};

struct stScoreboardInfo {
	int    iIsEnabled;
	int    iPlayersCount;
	float  fTextOffset[2];
	float  fScalar;
	float  fSize[2];
	float  fUnk0[5];
	void  *pDirectDevice;
	void  *pDialog;
	void  *pList;
	int    iOffset;
	int    iIsSorted;
};

#pragma pack( pop )

// --- typed pool accessors (cast the void* pool pointers pools.h exposes). All
//     null-safe: return nullptr if the client isn't loaded / in-game. --------
inline stActorPool    *actor_pool()    { stSAMPPools *p = pools(); return p ? (stActorPool *)    p->pActor    : nullptr; }
inline stPickupPool   *pickup_pool()   { stSAMPPools *p = pools(); return p ? (stPickupPool *)   p->pPickup   : nullptr; }
inline stObjectPool   *object_pool()   { stSAMPPools *p = pools(); return p ? (stObjectPool *)   p->pObject   : nullptr; }
inline stGangzonePool *gangzone_pool() { stSAMPPools *p = pools(); return p ? (stGangzonePool *) p->pGangzone : nullptr; }
inline stTextLabelPool*textlabel_pool(){ stSAMPPools *p = pools(); return p ? (stTextLabelPool *)p->pText3D   : nullptr; }
inline stTextdrawPool *textdraw_pool() { stSAMPPools *p = pools(); return p ? (stTextdrawPool *) p->pTextdraw : nullptr; }

// --- UI-layer accessors (fixed RVAs into samp.dll that hold the singletons).
//     Each is the address OF the pointer, so we deref once. Null-safe. --------
// The server preset block (nametag distance / wall / show flags live here).
inline stServerPresets  *server_presets()  { stSAMP *ng = netgame(); return ng ? (stServerPresets *) ng->pSettings : nullptr; }

inline stChatInfo       *chat_info()       { uint32_t b = base(); return b ? *(stChatInfo **)       ( b + 0x2ACA10 ) : nullptr; }
inline stInputInfo      *input_info_st()   { uint32_t b = base(); return b ? *(stInputInfo **)      ( b + 0x2ACA14 ) : nullptr; }
inline stKillInfo       *kill_info()       { uint32_t b = base(); return b ? *(stKillInfo **)       ( b + 0x2ACA18 ) : nullptr; }
inline stScoreboardInfo *scoreboard_info() { uint32_t b = base(); return b ? *(stScoreboardInfo **) ( b + 0x2AC9DC ) : nullptr; }

} // namespace samp
