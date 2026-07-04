#pragma once
/*
	gta/gtahost.h  --  bridge to the gta-reversed game layer.

	This is the ONLY gta-reversed surface the rest of fuego-reversed sees: a lean,
	C++20 interface. The implementation (gtahost.cpp) is the single translation
	unit that pulls in gta-reversed's heavy StdInc.h umbrella (compiled at C++23
	with its RenderWare/pool ordering + vendored json/spdlog/Tracy-stub deps).

	Design: we COEXIST with the running game. gtahost uses gta-reversed's headers
	for accurate struct layouts + inline accessors (CEntity::GetPosition, etc.) and
	its StaticRef address maps (ped pool @ 0xB74490), and CALLS the real game's
	functions at their addresses via mem:: helpers -- it never installs
	gta-reversed's reimplementation hooks, so the actual GTA:SA keeps running.

	Grows toward full gta-reversed coverage: any gta-reversed class/function/mapping
	can be surfaced here as the cheats need it.
*/

namespace game {

// True once GTA's local player ped exists (in a game session).
bool available();

// Local player world position -> out[3] (x,y,z). false if no ped yet.
bool local_player_pos( float out[3] );

// Local player health / max health (-1 if no ped).
float local_player_health();
float local_player_max_health();

// Set local player health (god mode / heal). No-op if no ped.
void set_local_player_health( float hp );

// Teleport the local player to a world position (CPed::Teleport). On-foot.
void teleport_local( const float pos[3] );

// --- world / players (ESP, aim, distance) ---------------------------------
// Project a world point to screen pixels. true if it's on-screen (in front).
bool world_to_screen( const float world[3], float &sx, float &sy );

// A remote SA:MP player's world position: the live game ped if streamed in,
// else the last network-synced position. false if not connected.
bool player_world_pos( int sampId, float out[3] );

// A remote player's live health from the game ped (-1 if not streamed in).
float player_gta_health( int sampId );
int   player_skin( int sampId );                    // target ped model/skin id, -1 if none
int   local_skin();                                 // local ped model/skin id, -1 if none

// True if the SA:MP player currently has a streamed-in game ped (targetable).
bool player_streamed( int sampId );

// True if a player is on the local player's SA:MP team (for the aim team filter).
bool same_team( int sampId );

// Straight-line distance from the local player to a world point (-1 if no ped).
float distance_to( const float world[3] );

// A player's bone world position (bone: 5=head, 3=spine1, 2=spine). false if not
// streamed. Used for bone ESP + bone-accurate silent aim.
bool player_bone_pos( int sampId, int bone, float out[3] );

// A player's velocity (units/frame) -- for silent-aim lead/prediction.
bool player_velocity( int sampId, float out[3] );

// --- helpers for the vehicle-sync trolls (itroll/fuckup) -------------------
float ground_z( float x, float y );                 // world ground height at (x,y)
bool  ped_ground_z( int sampId, float &outZ );       // ground below a target ped (3D query, iTroll m1)
float player_heading( int sampId );                 // target ped facing angle (radians)
bool  player_vehicle_axes( int sampId, float right[3], float up[3] ); // target vehicle right/up (spin)

// --- world pickups (weapon finder) ----------------------------------------
int  pickup_count();                                    // slots to scan
bool pickup_pos( int idx, float out[3], int &modelOut ); // active pickup -> pos+model

// --- local-player weapon / movement cheats --------------------------------
int  local_weapon_type();      // active weapon id (0..54), -1 if no ped
void top_up_clip();            // no-reload: refill the active weapon's clip
int  player_weapon( int sampId ); // a remote player's active weapon id, -1 if none
int  player_weapon_slots( int sampId, int out[13] ); // all carried weapons (Show Players Guns)
void apply_rotory_pulse( float x, float y, float z ); // break-dance angular impulse
void switch_weapon( int weaponId ); // set the local player's active weapon (0..46)
void give_ammo( int amount );       // add ammo to the local player's active weapon
void make_player_controllable();    // unfreeze self (MakePlayerSafe(false), /unfreezeme)
void set_local_invulnerable( bool on ); // ped damage-immunity flags (health hack)
void set_local_vehicle_invulnerable( bool on ); // pin the car's damage flags (in-vehicle god)
bool in_vehicle();            // is the local player in a vehicle

// Fly (on foot): move the local ped along the camera axes + cancel gravity.
void fly_move( float fwd, float right, float up );
// Toggle local ped collision (fly noclip -> pass through walls).
void set_local_collision( bool on );
// Air brake: zero the local vehicle/ped velocity (freeze in place).
void freeze_local();
// Air-brake move: shift the local vehicle/ped along the camera axes.
void airbrake_move( float fwd, float right, float up );

// Car shooter: camera forward vector + teleport a streamed SA:MP vehicle to pos
// with a move speed (client-side), so held-LMB launches cars at your aim.
bool camera_forward( float out[3] );
bool launch_vehicle( int sampVehId, const float pos[3], const float speed[3] );

// Car-troll support: read a streamed vehicle's live pos/speed/health, client-side
// teleport it (+ velocity + health), and read local health/armour for the syncs.
bool vehicle_live_data( int sampVid, float pos[3], float speed[3], float &health );
void teleport_vehicle( int sampVid, const float pos[3], const float speed[3], float health );
void local_health_armor( float &hp, float &armor );
float local_vehicle_health();                    // local vehicle health, -1 on foot (extra-godmode gate)

// Geometry refinements: model bounding-box dims + index (gethere/carpark), forward
// vector + width + pos (njkill), and the spin quaternion (iTroll mode 5).
bool vehicle_dims_model( int sampVid, float out[3], int &modelIdx );
bool vehicle_forward( int sampVid, float fwd[3], float &width, float pos[3] );
bool vehicle_spin_quat( int sampVid, float quat[4] );
void set_local_armor( float armor );             // fake-lag backtrack restore
void set_local_movespeed( const float v[3] );    // fake-lag backtrack restore

// Fake-lag backtrack (Fuego 1:1): snapshot the live ped/vehicle transform at enqueue
// (mat = [right.xyz, forward.xyz, up.xyz, pos.xyz]), and restore the last-sent snapshot
// on BACKSPACE -- foot or vehicle path, with the vehicle's +200ms matrix/speed re-apply.
bool fakelag_capture( float mat[12], float speed[3], bool &inVehicle );
void fakelag_restore_foot( float armor, float health, int weaponId, const float mat[12], const float speed[3] );
void fakelag_restore_vehicle( float armor, float health, int weaponId, const float mat[12], const float speed[3], float vehHealth );
void fakelag_reapply_vehicle(); // backTrackSetVehicle (+200ms re-apply of matrix + speed)

// Line-of-sight from the camera to a world point (buildings/objects block it).
// Used to keep Locked-Vehicle ESP from showing through walls.
bool position_visible( const float pos[3] );

// Fill out[][3] with the world positions of locked streamed vehicles (Locked
// Vehicle ESP). Returns how many were written (<= maxN).
int  locked_vehicle_positions( float out[][3], int maxN );

// LSRP gun-stealer force-adjust: snap-target for the tracked gun objects (occupant knee /
// vehicle pos+halfheight / our own knee). false if nothing resolvable.
bool stealguns_target_pos( int sampVid, float out[3] );

// Unlock the local player's current vehicle (door lock -> unlocked).
void unlock_local_vehicle();
void unlock_vehicle_id( int sampVid );              // unlock a specific streamed vehicle
int  vehicle_under_cursor( float sx, float sy );    // click-to-pick: SA:MP veh under the cursor (raycast)
// Local player's SA:MP vehicle id (from the in-car sync), -1 if on foot.
int  local_vehicle_samp_id();

// --- carcontrol (Fuego GTAfuncs: local ped possession) --------------------
// Warp the local ped INTO the target vehicle so the game emits that car's
// in-car sync (you drive it), then warp back out on release. These call the
// SAME real game tasks the SA:MP GTA interface uses (0x647D10 / 0x64B950).
void cc_possess_driver( int sampVid );              // PutActorInCar (warp self in as driver)
void cc_possess_passenger( int sampVid, int seat ); // PutActorInCarAsPassenger
void cc_release_to( const float pos[3] );           // RemoveActorFromCarAndPutAt (warp out+teleport)
void cc_camera_on_vehicle( int sampVid );           // CameraOnVehicle behind-car (style 2/jumpcut)
void cc_camera_restore();                           // Camera Restore + RestoreWithJumpCut
void cc_capture_vehicle( int sampVid );             // stash live matrix/pos/speed (pre-warp)
void cc_reapply_vehicle( int sampVid );             // carControlSetVehicle: re-apply the stash
int  cc_local_seat();                               // -1 on foot, 0 driver, else passenger
int  cc_local_vehicle_id();                         // SA:MP id of our car (by GTA ptr; any seat)
int  cc_vehicle_empty_seat( int sampVid );          // first empty passenger seat, -1 if full
int  vehicle_empty_seat( int sampVid, bool passengerOnly ); // getVehicleEmptySeat (trolls/gun-stealer)
int  cc_nearest_empty_vehicle( float radius );      // nearest empty streamed vehicle id, -1 none
void cc_vehicle_collision_proof( int sampVid, bool on ); // (nocols stand-in) collision-proof a vehicle

} // namespace game
