#pragma once
/*
	samp/samp.h  --  SA:MP 0.3.DL detection & core integration

	Foundation surface: find samp.dll, verify it's 0.3.DL, and talk to the chat.
	Grows as the networking/RPC layer is migrated.
*/

#include <cstdint>

namespace samp {

enum ChatMessageType : int {
	CHAT_TYPE_NONE  = 0,
	CHAT_TYPE_CHAT  = 2,
	CHAT_TYPE_INFO  = 4,
	CHAT_TYPE_DEBUG = 8, // local, no player prefix
};

// samp.dll base (0 if not loaded).
uint32_t base();

// True once samp.dll is present AND its 0.3.DL signature matches.
bool detected();

// Run detection (reads the version signature). Returns detected().
bool detect();

// The chat interface pointer (null until SA:MP's chat exists == you're in-game).
void *chat_ptr();

// The input/command interface pointer (SA:MP's CInput; null until UI is up).
void *input_ptr();

// The CNetGame object (stSAMP*), or null. Full type in structs.h.
struct stSAMP;
stSAMP *netgame();

// Reset the netgame for a fresh (re)connect (CNetGame::ShutDownForGameRestart).
void restart_game();

// Force SA:MP to reconnect to the CURRENT server (sets iGameState=WAIT_CONNECT;
// its per-frame Process() then prints "Connecting to..." and calls Connect()).
void reconnect_current_server();

// True once the chat interface is live == the local player has loaded into a server.
bool is_in_game();

// Show/hide SA:MP's cursor (the one it uses for dialogs). Showing it frees the OS
// cursor + suspends the game camera, so the overlay's mouse works; hiding it
// restores camera control. Mirrors Fuego's toggleSAMPCursor.
void toggle_cursor( bool show );

// Print a local (debug) chat line. Supports SA:MP "{RRGGBB}" colour codes.
// No-op until is_in_game(). Colour is D3DCOLOR ARGB.
void chat( const char *text, uint32_t argb = 0xFFFFFFFF );

// RET-patch SA:MP's own nametag draw (so the S0biet custom overlay doesn't double
// up). true = suppress native nametags; false = restore.
void disable_native_nametags( bool disable );

} // namespace samp
