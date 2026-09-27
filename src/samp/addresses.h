#pragma once
/*
	samp/addresses.h  --  SA:MP 0.3.DL offsets (from samp.dll base)

	Verified against Project Fuego (proven on v140) and the samp-packet-list wiki.
	These are offsets from the samp.dll module base, NOT absolute addresses.
	This file will grow as more subsystems are migrated; for now it carries what
	the core (detection + chat) needs.
*/

#include <cstdint>

namespace samp::addr {

// --- structure pointers (a T* is stored at base+offset) ---
constexpr uint32_t INFO           = 0x2ACA24; // stSAMP*
constexpr uint32_t CHAT_INFO      = 0x2ACA10; // stChatInfo*
constexpr uint32_t CHAT_INPUT     = 0x2ACA14; // stInputInfo*
constexpr uint32_t KILL_INFO      = 0x2ACA18; // stKillInfo*
constexpr uint32_t MISC_INFO      = 0x2ACA3C;
constexpr uint32_t SCOREBOARD     = 0x2AC9DC;
constexpr uint32_t DIALOG_INFO    = 0x2AC9E0; // CDialog*

// --- functions (offset from samp.dll base) ---
constexpr uint32_t FUNC_ADD_TO_CHAT_WND = 0x67650; // CChat::AddEntry (__thiscall)
constexpr uint32_t FUNC_SAY             = 0x5860;
constexpr uint32_t FUNC_SEND_CMD        = 0x69340;
constexpr uint32_t FUNC_ADD_CLIENT_CMD  = 0x691B0;
constexpr uint32_t FUNC_CNETGAME_DTOR   = 0x9570;  // CNetGame::~CNetGame
constexpr uint32_t FUNC_RESTART_GAME    = 0xA230;  // CNetGame::ShutDownForGameRestart (reset for reconnect)
constexpr uint32_t FUNC_TOGGLE_CURSOR   = 0xA0530; // CGame::SetCursorMode(this, mode, unlock) (__thiscall)
constexpr uint32_t FUNC_UNLOCK_ACTOR_CAM= 0xA0410; // CGame::UnlockActorCam(this) (__thiscall)
constexpr uint32_t FUNC_DIALOG_SHOW     = 0x6FA50; // CDialog::Show (__thiscall, ret 0x1c)

// --- SA:MP code hook points (for CDetour) ---
constexpr uint32_t HOOKENTER_HANDLE_RPC    = 0x3A9ED;
constexpr uint32_t HOOKENTER_HANDLE_RPC2   = 0x3A979;

// Nametag-draw entry points RET-patched (0xC3) to suppress SA:MP's own nametags
// while the S0biet custom overlay draws (else they double up).
constexpr uint32_t PATCH_DISABLE_NAMETAGS    = 0x74DC0;
constexpr uint32_t PATCH_DISABLE_NAMETAGS_HP = 0x73CB0;
constexpr uint32_t HOOKEXIT_HANDLE_RPC     = 0x3A9F3;
constexpr uint32_t HOOKEXIT_HANDLE_RPC2    = 0x3AA01;
constexpr uint32_t HOOKENTER_CNETGAME_DTOR  = 0xC5533;
constexpr uint32_t HOOKENTER_CNETGAME_DTOR2 = 0xC6736;

// --- offsets WITHIN structs (not from samp.dll base) ---
constexpr uint32_t NETGAME_RAKCLIENT_IFACE = 0x2C; // stSAMP::pRakClientInterface
constexpr uint32_t DIALOG_ACTIVE = 0x28; // CDialog::iIsActive (int)
constexpr uint32_t DIALOG_TYPE = 0x2C; // CDialog::iType (int)
constexpr uint32_t DIALOG_ID   = 0x30; // CDialog::DialogID (int16)

// --- version signature (10 bytes at base+SIG_OFFSET identify 0.3.DL) ---
constexpr uint32_t SIG_OFFSET = 0xBABE;
constexpr char     SIG_HEX[]  = "528D44240C508D7E09E8"; // 0.3.DL-R1

} // namespace samp::addr
