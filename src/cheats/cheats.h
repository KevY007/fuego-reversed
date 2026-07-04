#pragma once
/*
	cheats/cheats.h  --  cheat processing.

	Registers a per-frame driver (render hook, main thread) and outgoing-packet /
	RPC handlers (net seam). All read fx::g_set (the menu's state).
*/

namespace cheats {
void install();

// Fake-lag appear-point (where others currently see you). false if none live.
bool fakelag_held( float out[3] );

// Seconds left on the BACKSPACE backtrack countdown (overlay text).
float fakelag_backtrack_left();

// Zombie infect: warp onto a player with attack keys for 100ms. false if invalid.
bool zombie_infect( int id );

// Toggle /desync (pins local drunk level to 0). Returns new state.
bool toggle_desync();

// Toggle continuous slap-kill on a target (-1 to stop). Returns true if now active.
bool slapkill( int id );

// Get here: drop your vehicle onto an on-foot target and drag them to you.
bool get_here( int id );

// Ninja kill: jack an enemy driver out of their car (EnterVehicle seat 0).
bool ninja_kill( int id );
bool njkill_active(); // true during the 3s hold (compat swallows kick RPCs)

// Freeze the game's outgoing sync for `ms` (drop player/vehicle/spectator sync as
// selected) so a spoofed pose a cheat just sent isn't overwritten. Used by trolls.
void freeze_sync_ms( int ms, bool player, bool vehicle, bool spectator );

// Checkpoint: capture the last checkpoint pos (from RPC_SetCheckpoint) + replay it.
void set_checkpoint( float x, float y, float z );
bool trigger_checkpoint( int delayMs );

// Car control: toggle remote-driving a target's vehicle. Returns 1=on, 0=off, -1=fail.
int  car_control( int id );
bool car_control_active(); // true while controlling (compat swallows kick RPCs)

// Streamed-out player position (from ID_MARKERS_SYNC), for distant-player ESP.
bool streamed_out_pos( int id, float out[3] );

// Last slapped position, for the slapper ESP (line + circle + "Currently Slapping").
bool slapper_pos( float out[3] );

// Freeze troll: pin a target driver's car in place (toggle). 1=on 0=off -1/-2=fail.
int freeze_troll( int id );
}
