#pragma once
/*
	core/settings.h  --  cheat toggle state (the modern replacement for Fuego's
	`struct settings set`). Fields + grouping mirror Project Fuego's AntTweakBar
	menu (atb_menu.cpp) so the reimplementation is 1:1. The menu binds to these;
	the cheat handlers read them.
*/

#include <cstdint>

namespace fx {

struct Settings {
	// ================= MAIN =================
	bool  no_big_hacks = false;      // "Disable Unsafe/Big Hacks" (noBigHacks)
	bool  cheat_panic = false;       // master hide-everything (our own)

	// ============ WEAPON FUNCTIONS ============
	int   shot_repeater = 0;         // 0..100
	bool  fake_headshot = false;
	bool  always_headshot = false;   // silent aim: force the head bone (else random bone/place)
	bool  trigger_bot = false;
	bool  no_spread = false;
	int   no_spread_trigger = 0;     // 0..10
	bool  no_reload = false;
	bool  dual_shot = false;         // dShotStatus
	int   dual_shot_delay = 375;     // 0..3000 (Fuego default)
	// --- Silent Aim group ---
	bool  silent_aim = false;
	bool  silent_aim_random_shots = false; // "Random False Shots" (antiAimBypass)
	bool  silent_aim_range_hack = false;   // "Range Hack" (outOfRangeFilter)
	bool  silent_aim_wall_shot = false;    // "Shoot Through Walls"
	int   silent_aim_tracer = 0;     // Draw Tracer: 0=Off 1=ESP 2=ESP&FOV
	float silent_aim_fov = 75.0f;    // "Search FOV" raw px (Fuego default)
	bool  silent_aim_friend_filter = true;
	bool  silent_aim_color_filter = false;
	bool  silent_aim_skin_filter = false;
	bool  silent_aim_team_filter = false;
	bool  silent_aim_afk_filter = true;
	bool  silent_aim_alive_filter = true;
	int   silent_aim_target = -1;    // resolved each frame (transient)
	// our extras
	int   silent_aim_bone = 5;       // 5=head 3=spine1 2=spine
	bool  silent_aim_prediction = false;
	bool  silent_aim_give_damage = true;
	// --- Killer group ---
	bool  killer_enabled = false;
	int   killer_delay = 250;        // 1..10000 (Fuego default)
	int   killer_bullets = 1;        // 1..92
	int   killer_stats_interval = 1; // 1..10 (Fuego default)
	bool  killer_friend_filter = true;

	// ============ MISC FUNCTIONS ============
	// --- Invisibility ---
	bool  invisibility = false;
	bool  smart_invis = false;
	// --- Slappers ---
	bool  aim_slapper = false;
	int   aim_slapper_power = 20;    // 0..100
	bool  near_slapper = false;
	bool  near_slapper_friend_filter = true;
	// --- Visuals ---
	bool  chams = false;
	bool  chams_wireframe = false;   // chams as wireframe vs solid
	bool  nametags = false;          // "Nametag" (render_player_tags)
	bool  nametags_samp = false;     // "Nametag Type": true=SA-MP false=S0biet(ours)
	bool  nametags_masked = false;   // our extra: censor names
	bool  locked_vehicle_esp = false;
	bool  weapon_finder = false;
	bool  show_players_guns = false; // "Show Players Guns"
	bool  player_esp = false;        // our extra: full box ESP
	bool  esp_box = true, esp_name = true, esp_health = true, esp_distance = true;
	// --- other misc ---
	bool  break_dance = false;       // "Onfoot Break Dance"
	bool  auto_unscramble = false;   // "Auto Unscrambler LSRP"
	int   auto_unscramble_delay = 1250; // ms before submitting /uns (Fuego default)
	// --- Health Hack ---
	bool  health_hack = false;       // "Health Hack" (hp_cheat) -- packet based
	bool  enable_extra_godmode = false; // also swallow server vehicle-health drops (Fuego)
	bool  smart_health_hack = false;
	int   smart_hh_proc = 50;        // 0..100
	// --- Fake Lag group ---
	bool  fake_lag = false;          // "Fake Movement Lag" (fakesynclag) = freeze+backtrack
	bool  fake_lag_random_spikes = false;
	int   fake_lag_delay = 250;      // 0..10000 (Fuego default)
	bool  fake_lag_teleportish = false; // "Teleportish Movement" (onceperms)
	// --- Skin Trolls group ---
	bool  moonwalk = false;
	bool  upside_down = false;
	bool  ghost_mode = false;
	bool  twister_mode = false;

	// ============ VEHICLE FUNCTIONS ============
	int   itroll_mode = 0;           // 0..7 (hold I while driving)
	bool  car_shooter = false;
	bool  remove_cars = false;
	bool  explode_cars = false;
	bool  floating_cars = false;
	bool  explode_cars_silent = false;
	bool  unoccupied_remover = false;

	// ============ MOVEMENT (our extras) ============
	bool  fly = false;
	float fly_speed = 0.6f;
	bool  air_brake = false;
	float air_brake_speed = 1.0f;

	// ============ DISPLAY ============
	bool  uncap_fps = false;         // force GTA's frame limiter (0xBA6794) off every frame
	bool  borderless = false;        // borderless-window the game window
};

extern Settings g_set;

} // namespace fx
