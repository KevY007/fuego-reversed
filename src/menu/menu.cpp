#include "menu.h"

#include "../render/render.h"
#include "../core/settings.h"
#include "../core/logger.h"
#include "../compat/compat.h"
#include "../samp/samp.h"
#include "../samp/samp_sdk.h"   // scoreboard_info (hide the menu while the scoreboard is up)
#include "../cheats/visuals.h"
#include "../cheats/veh.h"
#include "../gta/gtahost.h"   // game::repair_local_vehicle (Repair Vehicle button)
#include "../core/config.h"
#include "../i18n/config.h"
#include "../i18n/engine.h"
#include "../i18n/provider.h" // list_models (model dropdown) + ProviderConfig
#include "../i18n/hooks.h"    // apply_config (live-apply translator settings)
#include "../i18n/outbound.h" // settable-hotkey outgoing-chat + dialog-input toggle (default Ctrl+Shift)
#include "../i18n/indicator.h" // on-screen "translating..." hints
#include "../recorder/recorder.h" // packet recorder (debug): Main tab toggle + REC pill
#include "../fishing/autofish.h"  // Mike's CnR auto fish: Misc tab toggle + status pill

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <d3d9.h>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace {

bool g_init = false;
bool g_open = false;

// The frame's ImGui draw data split in two layers (see on_frame / on_present): the
// world overlays, drawn at GTA's EndScene, and the menu + HUD, drawn after SA:MP's UI.
ImDrawData g_worldLayer, g_uiLayer;
bool       g_uiPending = false;
// Frames since the UI layer was last presented (on_present). While Present is firing
// normally this stays 0-1 and the UI goes over the chat; if Present stops (never hooked,
// or an overlay keeps stealing it), it climbs and we draw the whole UI at EndScene
// instead -- under the chat, but always visible, and the switch is silent (no flicker
// between modes, no log spam).
int        g_framesSincePresent = 1000;

void split_layers( const ImDrawData *dd, const ImDrawList *world ) {
	g_worldLayer = *dd; // display size/pos, scale, texture list
	g_uiLayer    = *dd;
	g_worldLayer.CmdLists.resize( 0 );
	g_uiLayer.CmdLists.resize( 0 );
	g_worldLayer.TotalVtxCount = g_worldLayer.TotalIdxCount = 0;
	g_uiLayer.TotalVtxCount = g_uiLayer.TotalIdxCount = 0;
	for ( ImDrawList *l : dd->CmdLists ) {
		ImDrawData &to = ( l == world ) ? g_worldLayer : g_uiLayer;
		to.CmdLists.push_back( l );
		to.TotalVtxCount += l->VtxBuffer.Size;
		to.TotalIdxCount += l->IdxBuffer.Size;
	}
	g_worldLayer.CmdListsCount = g_worldLayer.CmdLists.Size;
	g_uiLayer.CmdListsCount    = g_uiLayer.CmdLists.Size;
}

// --- translator model dropdown: async fetch of the endpoint's model list -----
// list_models() is a blocking WinHTTP call, so it runs on a detached worker;
// the render thread reads results under g_modelMtx. State: 0 idle, 1 fetching,
// 2 done, 3 error. Only the NEWEST fetch reports (g_modelGen): switching provider
// or editing the key/URL while one is in flight must not show the old list. A
// fetch for exactly what is already being fetched (g_modelAsk) isn't started twice.
std::mutex               g_modelMtx;
std::vector<std::string> g_models;
std::atomic<int>         g_modelState{ 0 };
std::atomic<int>         g_modelGen{ 0 };
std::string              g_modelErr;
std::string              g_modelAsk; // URL + key + provider of the fetch in flight

void fetch_models_async( bool clearList = false ) {
	// Snapshot the endpoint/key now (the user may keep typing while it runs).
	i18n::ProviderConfig pc;
	auto &s = i18n::config::get();
	pc.baseUrl      = s.base_url;
	pc.apiKey       = s.api_key;
	pc.model        = s.model;
	pc.providerKind = s.provider_kind; // e.g. Anthropic /models needs its version header
	const std::string ask = pc.baseUrl + '\n' + pc.apiKey + '\n' + std::to_string( pc.providerKind );

	int gen;
	{
		std::lock_guard<std::mutex> lk( g_modelMtx ); // atomic with a finishing fetch's check + store
		if ( clearList ) g_models.clear();
		if ( pc.baseUrl.empty() ) { // nothing to ask yet (a new Custom provider): no error either
			++g_modelGen;
			g_modelErr.clear();
			g_modelState.store( 0 );
			return;
		}
		if ( g_modelState.load() == 1 && ask == g_modelAsk ) return; // that list is already on its way
		gen = ++g_modelGen;
		g_modelAsk = ask;
		g_modelState.store( 1 );
	}

	// This runs inside the EndScene hook: nothing may escape it, and a worker that
	// can't be started must not leave "loading..." up forever.
	try {
		std::thread( [pc, gen]() {
			std::vector<std::string> out;
			std::string err;
			bool ok = false;
			try { ok = i18n::list_models( pc, out, err ); }
			catch ( ... ) { ok = false; err = "internal error"; }
			std::lock_guard<std::mutex> lk( g_modelMtx );
			if ( gen != g_modelGen.load() ) return; // a newer fetch replaced this one
			if ( ok ) {
				g_models = std::move( out );
				g_modelErr.clear();
				g_modelState.store( 2 );
			} else {
				g_modelErr = err;
				g_modelState.store( 3 );
			}
		} ).detach();
	} catch ( ... ) {
		std::lock_guard<std::mutex> lk( g_modelMtx );
		if ( gen == g_modelGen.load() ) { g_modelErr = "couldn't start"; g_modelState.store( 3 ); }
	}
}

// --- translator self-test: translate a sample line so the user can verify the
// endpoint/model/prompt without joining a server. Async (blocking provider). ---
std::mutex        g_selfMtx;
std::string       g_selfResult;
std::atomic<int>  g_selfState{ 0 }; // 0 idle, 1 running, 2 done, 3 error
long              g_selfMs = 0;

const char *kSelfSample = "Hey Kevy, meet me at Grove Street with the Infernus, this server is fun!";

void run_selftest_async() {
	if ( g_selfState.load() == 1 ) return;
	g_selfState.store( 1 );

	i18n::ProviderConfig pc;
	auto &s = i18n::config::get();
	pc.baseUrl      = s.base_url;
	pc.apiKey       = s.api_key;
	pc.model        = s.model;
	pc.providerKind = s.provider_kind; // apply this provider's request quirks in the self-test too
	pc.readMs  = s.read_timeout_ms > 0 ? s.read_timeout_ms : 30000;
	pc.temperature = ( s.translate_temp >= 0 ? s.translate_temp : 30 ) / 100.0;
	std::string prompt = i18n::config::build_system_prompt( s.target_lang, s.roman_output, s.source_lang );

	try { // (inside the EndScene hook: nothing may escape)
		std::thread( [pc, prompt]() {
			std::vector<std::string> out;
			std::string err;
			DWORD t0 = GetTickCount();
			bool ok = false;
			try {
				auto prov = i18n::make_openai_compatible( pc );
				ok = prov && prov->translate( { kSelfSample }, prompt, "", out, err );
			} catch ( ... ) { ok = false; err = "internal error"; }
			DWORD t1 = GetTickCount();
			std::lock_guard<std::mutex> lk( g_selfMtx );
			g_selfMs = (long)( t1 - t0 );
			if ( ok && !out.empty() ) { g_selfResult = out[0]; g_selfState.store( 2 ); }
			else { g_selfResult = err.empty() ? "(failed)" : err; g_selfState.store( 3 ); }
		} ).detach();
	} catch ( ... ) {
		std::lock_guard<std::mutex> lk( g_selfMtx );
		g_selfMs = 0;
		g_selfResult = "couldn't start the test";
		g_selfState.store( 3 );
	}
}

// Small "(?)" hover-help marker (ImGui demo idiom; the built-in demo helper
// isn't linked into a stripped build, so define our own).
void HelpMarker( const char* desc ) {
	ImGui::TextDisabled( "(?)" );
	if ( ImGui::IsItemHovered() ) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos( ImGui::GetFontSize() * 28.0f );
		ImGui::TextUnformatted( desc );
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// A category header inside a tab, with extra breathing room above it so the sections
// of hacks are clearly separated (plain ImGui::SeparatorText packs them too tightly).
void Section( const char* label ) {
	ImGui::Dummy( ImVec2( 0.0f, ImGui::GetFontSize() * 0.55f ) );
	ImGui::SeparatorText( label );
}

// Wrapped grey text (a hint or an error that can be long).
void HintWrapped( const char* fmt, const char* arg ) {
	ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
	ImGui::TextWrapped( fmt, arg );
	ImGui::PopStyleColor();
}

// A language dropdown (names, not LCID numbers) over config::language_options().
// autoLabel adds a first "0 = detect" entry (the source language).
void LanguageCombo( const char* label, int* code, const char* autoLabel = nullptr ) {
	const auto &langs = i18n::config::language_options();
	const char *preview = ( autoLabel && *code == 0 ) ? autoLabel : "(custom)";
	for ( const auto &l : langs )
		if ( l.code == *code ) { preview = l.name; break; }
	if ( !ImGui::BeginCombo( label, preview ) ) return;
	if ( autoLabel ) {
		if ( ImGui::Selectable( autoLabel, *code == 0 ) ) *code = 0;
		if ( *code == 0 ) ImGui::SetItemDefaultFocus();
	}
	for ( const auto &l : langs ) {
		const bool sel = ( l.code == *code );
		if ( ImGui::Selectable( l.name, sel ) ) *code = l.code;
		if ( sel ) ImGui::SetItemDefaultFocus();
	}
	ImGui::EndCombo();
}

void style() {
	ImGui::StyleColorsDark();
	ImGuiStyle &s = ImGui::GetStyle();
	s.WindowRounding = 6.0f;
	s.FrameRounding  = 4.0f;
	s.GrabRounding   = 4.0f;
	s.WindowBorderSize = 1.0f;
	s.Colors[ImGuiCol_TitleBgActive] = ImVec4( 0.80f, 0.20f, 0.05f, 1.0f ); // fuego orange-red
	s.Colors[ImGuiCol_Header]        = ImVec4( 0.80f, 0.20f, 0.05f, 0.55f );
	s.Colors[ImGuiCol_CheckMark]     = ImVec4( 1.00f, 0.45f, 0.15f, 1.0f );
	s.Colors[ImGuiCol_SliderGrab]    = ImVec4( 1.00f, 0.45f, 0.15f, 1.0f );
	s.ScaleAllSizes( 1.35f ); // bigger padding/controls to match the scaled font
}

void draw_ui() {
	using namespace fx;
	ImGui::SetNextWindowSize( ImVec2( 900, 680 ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSizeConstraints( ImVec2( 640, 480 ), ImVec2( 4000, 4000 ) );
	ImGui::Begin( "Fuego Reversed", &g_open, ImGuiWindowFlags_NoCollapse );

	// The cheats scroll in their own box; the config box (Save + tips) stays pinned
	// below it, always in view, so you never scroll to the bottom to reach Save.
	const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f +
	                      ImGui::GetStyle().WindowPadding.y * 2.0f + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild( "##cheats", ImVec2( 0, -footerH ), false );

	// Scroll (don't shrink) tabs that overflow + a popup button to jump to any tab.
	if ( ImGui::BeginTabBar( "tabs",
	         ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton ) ) {
		// ---------------- Weapon ----------------
		if ( ImGui::BeginTabItem( "Weapon" ) ) {
			ImGui::SliderInt( "Shot Repeater", &g_set.shot_repeater, 0, 100 ); ImGui::SameLine();
			HelpMarker( "Re-sends each bullet you fire this many extra times, so one trigger pull can register as "
			            "several hits. 0 = off." );
			ImGui::Checkbox( "Fake Headshot", &g_set.fake_headshot ); ImGui::SameLine();
			HelpMarker( "Tells the server every hit you deal struck the head (body part 9). Works on servers that "
			            "trust the shooter's hit report; no effect where the victim's own client reports it." );
			ImGui::Checkbox( "Trigger Bot", &g_set.trigger_bot ); ImGui::SameLine();
			HelpMarker( "Auto-fires the instant your crosshair is on a player while you are aiming (never with "
			            "fists). Sends a real left-click." );
			ImGui::Checkbox( "No Spread", &g_set.no_spread ); ImGui::SameLine();
			HelpMarker( "Zeroes the game's bullet-spread value so shots go exactly where the crosshair is." );
			ImGui::SliderInt( "No Spread Trigger", &g_set.no_spread_trigger, 0, 10 ); ImGui::SameLine();
			HelpMarker( "With No Spread on: accuracy is on for only one shot every N bullets (0 = always accurate), "
			            "so perfect aim looks less obvious." );
			ImGui::Checkbox( "No Reload", &g_set.no_reload ); ImGui::SameLine();
			HelpMarker( "The weapon flips back to ready the moment it would reload -- you never wait. Ammo counts "
			            "stay honest, so the server sees normal numbers." );
			ImGui::Checkbox( "Dual Shot", &g_set.dual_shot ); ImGui::SameLine();
			HelpMarker( "Re-fires each bullet once more after the delay below, so a single trigger pull lands twice." );
			ImGui::SliderInt( "Dual Shot Delay", &g_set.dual_shot_delay, 0, 3000 ); ImGui::SameLine();
			HelpMarker( "Milliseconds between the two Dual Shot bullets." );

			Section( "Silent Aim" );
			ImGui::Checkbox( "Silent Aimbot", &g_set.silent_aim ); ImGui::SameLine();
			HelpMarker( "Redirects your bullets to a locked target (picked by the FOV + filters below), registering "
			            "a hit without your crosshair being on them." );
			ImGui::Checkbox( "Random False Shots", &g_set.silent_aim_random_shots ); ImGui::SameLine();
			HelpMarker( "Now and then sends a genuine miss so your accuracy isn't a suspicious 100%." );
			ImGui::Checkbox( "Range Hack", &g_set.silent_aim_range_hack ); ImGui::SameLine();
			HelpMarker( "Lets silent aim hit targets farther than the weapon's normal range." );
			ImGui::Checkbox( "Shoot Through Walls", &g_set.silent_aim_wall_shot ); ImGui::SameLine();
			HelpMarker( "Lets silent aim hit a target even with a wall between you." );
			{ const char *tr[] = { "Off", "ESP Only", "ESP & FOV" }; ImGui::Combo( "Draw Tracer", &g_set.silent_aim_tracer, tr, 3 ); } ImGui::SameLine();
			HelpMarker( "Draw a line to the silent-aim target: off, only when it's on screen, or always (plus the "
			            "search-FOV circle)." );
			ImGui::SliderFloat( "Search FOV", &g_set.silent_aim_fov, 1.0f, 1024.0f, "%.0f" ); ImGui::SameLine();
			HelpMarker( "How wide a circle (in pixels) around your crosshair silent aim looks in for a target." );
			ImGui::Checkbox( "Friend Filter", &g_set.silent_aim_friend_filter ); ImGui::SameLine();
			HelpMarker( "Never target players on your friends list (/friend, /fr)." );
			ImGui::Checkbox( "Color Filter", &g_set.silent_aim_color_filter ); ImGui::SameLine();
			HelpMarker( "Never target players who share your map-marker colour (often your own team/faction)." );
			ImGui::Checkbox( "Skin Filter", &g_set.silent_aim_skin_filter ); ImGui::SameLine();
			HelpMarker( "Never target players wearing the same skin as you." );
			ImGui::Checkbox( "Team Filter", &g_set.silent_aim_team_filter ); ImGui::SameLine();
			HelpMarker( "Never target players on your SA:MP team." );
			ImGui::Checkbox( "AFK Filter", &g_set.silent_aim_afk_filter ); ImGui::SameLine();
			HelpMarker( "Never target players who are AFK." );
			ImGui::Checkbox( "Alive Filter", &g_set.silent_aim_alive_filter ); ImGui::SameLine();
			HelpMarker( "Never target players who are already dead/dying." );
			ImGui::Checkbox( "Bullets Point to Head", &g_set.always_headshot ); ImGui::SameLine();
			HelpMarker( "Silent aim puts the hit on the head bone instead of a random body part. (Whether the "
			            "server is TOLD it was a headshot is the separate Fake Headshot option.)" );
			ImGui::Checkbox( "Lead Prediction", &g_set.silent_aim_prediction ); ImGui::SameLine();
			HelpMarker( "Aims a little ahead of a moving target (along their current speed) so fast runners still "
			            "get hit. No effect on a standing target." );

			Section( "Killer" );
			ImGui::Checkbox( "Auto Killer", &g_set.killer_enabled ); ImGui::SameLine();
			HelpMarker( "Automatically attacks every connected, streamed-in player in turn (damage bullets + a "
			            "GiveDamage report each)." );
			ImGui::SliderInt( "Killer Delay", &g_set.killer_delay, 1, 10000 ); ImGui::SameLine();
			HelpMarker( "Milliseconds to wait between each player the Killer attacks." );
			ImGui::SliderInt( "Bullets Per Person", &g_set.killer_bullets, 1, 92 ); ImGui::SameLine();
			HelpMarker( "How many damage bullets to send each player." );
			ImGui::SliderInt( "Statistics Interval", &g_set.killer_stats_interval, 1, 10 ); ImGui::SameLine();
			HelpMarker( "How often (seconds) the Killer prints how many players and bullets it has sent." );
			ImGui::Checkbox( "Killer Friend Filter", &g_set.killer_friend_filter ); ImGui::SameLine();
			HelpMarker( "Skip players on your friends list." );
			ImGui::EndTabItem();
		}
		// ---------------- Misc ----------------
		if ( ImGui::BeginTabItem( "Misc" ) ) {
			Section( "Invisibility" );
			ImGui::Checkbox( "Invisibility", &g_set.invisibility ); ImGui::SameLine();
			HelpMarker( "Hides you from other players by changing the position your client broadcasts." );
			ImGui::Checkbox( "Smart Invisibility", &g_set.smart_invis ); ImGui::SameLine();
			HelpMarker( "Invisible only while you're NOT clicking: on foot you're sent as a spectator, in a car the "
			            "car is sent as empty -- so you pop back into view the instant you shoot." );
			Section( "Slappers" );
			ImGui::Checkbox( "Aim Slapper", &g_set.aim_slapper ); ImGui::SameLine();
			HelpMarker( "Flings the player you're aiming at (rewrites their sync with an upward speed)." );
			ImGui::SliderInt( "Aim Slapper Power", &g_set.aim_slapper_power, 0, 100 ); ImGui::SameLine();
			HelpMarker( "How hard the Aim Slapper throws the target." );
			ImGui::Checkbox( "Near Slapper", &g_set.near_slapper ); ImGui::SameLine();
			HelpMarker( "Flings any player who gets close to you." );
			ImGui::Checkbox( "Near Slapper Friend Filter", &g_set.near_slapper_friend_filter ); ImGui::SameLine();
			HelpMarker( "Don't slap players on your friends list." );
			Section( "Health" );
			ImGui::Checkbox( "Health Hack", &g_set.health_hack ); ImGui::SameLine();
			HelpMarker( "Makes you invulnerable by answering the server's anti-cheat actor-flags check with the "
			            "invulnerable flags set." );
			ImGui::Checkbox( "Extra Godmode (block vehicle-health drops)", &g_set.enable_extra_godmode ); ImGui::SameLine();
			HelpMarker( "Also blocks the server from lowering your current vehicle's health." );
			ImGui::Checkbox( "Smart Health Hack", &g_set.smart_health_hack ); ImGui::SameLine();
			HelpMarker( "Blocks only SOME incoming damage (by the chance below) so godmode looks less obvious." );
			ImGui::SliderInt( "Smart HH Proc Chance", &g_set.smart_hh_proc, 0, 100 ); ImGui::SameLine();
			HelpMarker( "Percent of incoming damage Smart Health Hack blocks." );
			Section( "Fake Lag" );
			ImGui::Checkbox( "Fake Movement Lag", &g_set.fake_lag ); ImGui::SameLine();
			HelpMarker( "Holds your position syncs back and releases them in bursts, so you look laggy and hard to "
			            "hit to other players." );
			ImGui::Checkbox( "Fake Random Spikes", &g_set.fake_lag_random_spikes ); ImGui::SameLine();
			HelpMarker( "Randomises the hold time so the lag looks natural instead of steady." );
			ImGui::SliderInt( "Lag Delay", &g_set.fake_lag_delay, 0, 10000 ); ImGui::SameLine();
			HelpMarker( "How long (ms) syncs are held back." );
			ImGui::Checkbox( "Teleportish Movement", &g_set.fake_lag_teleportish ); ImGui::SameLine();
			HelpMarker( "Makes the held-back movement land as teleports rather than smooth catch-up." );
			Section( "Skin Trolls" );
			ImGui::Checkbox( "Moonwalker", &g_set.moonwalk ); ImGui::SameLine();
			HelpMarker( "You face backwards while moving (moonwalk), for other players." );
			ImGui::Checkbox( "Upside Down", &g_set.upside_down ); ImGui::SameLine();
			HelpMarker( "Shows your character upside-down to everyone." );
			ImGui::Checkbox( "Ghost Mode", &g_set.ghost_mode ); ImGui::SameLine();
			HelpMarker( "Puts your ped into an unusual special-action state for other clients." );
			ImGui::Checkbox( "Twister Mode", &g_set.twister_mode ); ImGui::SameLine();
			HelpMarker( "Spins your character on the spot for everyone to see." );
			ImGui::Checkbox( "Onfoot Break Dance", &g_set.break_dance ); ImGui::SameLine();
			HelpMarker( "Makes your character break-dance on foot." );
			Section( "LS-RP" );
			ImGui::Checkbox( "Auto Unscrambler", &g_set.auto_unscramble ); ImGui::SameLine();
			HelpMarker( "Auto-answers LS-RP's word-unscramble events from your wordlist (/wordlist_add, "
			            "/autouns_delay)." );

			// Mike's CnR auto fish: runtime only (never saved), no hotkey -- see autofish.h.
			Section( "Mike's CnR" );
			{
				bool fishOn = autofish::enabled();
				if ( ImGui::Checkbox( "Mike's CnR auto fish", &fishOn ) )
					autofish::set_enabled( fishOn );
				ImGui::SameLine();
				HelpMarker( "Fishes for you on Mike's CnR. Stand at the water, take the rod out (/fish) and turn "
				            "this on (or type /.autofish). It casts (aim + fire), waits for a bite, then taps "
				            "SPRINT at a safe pace for the fish's weight -- slowing down by itself if the line "
				            "takes damage -- and casts again; after a rotten fish it waits out the stun. Every step "
				            "waits for the server's answer, so it can't get out of step. It stops by itself if you "
				            "walk away, get in a vehicle, die, press PANIC or reconnect, and if the server stops "
				            "answering (rod put away, not at the water). A dialog pauses it until it's closed, and "
				            "it holds still while the game is paused (ESC menu). While it casts or reels, AIM / "
				            "FIRE / SPRINT are its keys. Only on Mike's Cops and Robbers, and not together with "
				            "cheats that change your syncs (Fake Lag, Invisibility, Moonwalker, slappers...). "
				            "Not saved: off at every game start." );
				if ( fishOn ) {
					ImGui::TextDisabled( "%s", autofish::status_text() );
					ImGui::TextDisabled( "%s", autofish::stats_text() );
				} else {
					ImGui::TextDisabled( "Rod out (/fish) at the water, then turn it on." );
				}
			}
			ImGui::EndTabItem();
		}
// ---------------- Vehicle ----------------
		if ( ImGui::BeginTabItem( "Vehicle" ) ) {
			Section( "iTroll (hold I while driving)" );
			ImGui::TextDisabled( "1)Ground 2)SpeedAway 3)BurnDown 4)BurnUp 5)Spin 6)Ocean 7)SlowSpeed" );
			ImGui::SliderInt( "iTroll Mode", &g_set.itroll_mode, 0, 7 ); ImGui::SameLine();
			HelpMarker( "While driving, hold I to apply the chosen troll to the nearest other occupied car "
			            "(0 = off)." );
			Section( "Generic" );
			ImGui::Checkbox( "Car Shooter", &g_set.car_shooter ); ImGui::SameLine();
			HelpMarker( "Hold fire to launch streamed vehicles toward where you aim." );
			ImGui::Checkbox( "Car Remover", &g_set.remove_cars ); ImGui::SameLine();
			HelpMarker( "Deletes streamed vehicles around you (client-side)." );
			ImGui::Checkbox( "Car Exploder", &g_set.explode_cars ); ImGui::SameLine();
			HelpMarker( "Blows up streamed vehicles around you." );
			ImGui::Checkbox( "Floating Cars", &g_set.floating_cars ); ImGui::SameLine();
			HelpMarker( "Lifts nearby vehicles into the air." );
			ImGui::Checkbox( "Car Exploder Silent", &g_set.explode_cars_silent ); ImGui::SameLine();
			HelpMarker( "Blows up nearby vehicles without the blast being tied to you." );
			ImGui::Checkbox( "Unoccupied Car Remover", &g_set.unoccupied_remover ); ImGui::SameLine();
			HelpMarker( "Removes empty (driverless) vehicles around you." );
			Section( "Actions (on your current vehicle)" );
			// Repair is a menu button, NOT a hotkey -- an instant full repair is server-
			// detectable, so it must be a deliberate click, never an accidental keypress.
			if ( ImGui::Button( "Repair Vehicle" ) )        game::repair_local_vehicle(); ImGui::SameLine();
			HelpMarker( "Patches up the car you're driving (and anything it tows): +50 health each click, and a "
			            "full visual fix once it's near max -- click a few times for a wreck. A button, not a "
			            "hotkey: an instant repair is easy for a server to spot, so it's a deliberate click." );
			Section( "Actions (click a vehicle to pick it)" );
			ImGui::TextDisabled( "Each closes the menu, then you click the target vehicle." );
			if ( ImGui::Button( "Unlock Vehicle" ) )        { veh::start_pick( 1 ); g_open = false; }
			if ( ImGui::Button( "Damage Vehicle Doors" ) )  { veh::start_pick( 2 ); g_open = false; }
			if ( ImGui::Button( "Destroy Vehicle" ) )       { veh::start_pick( 3 ); g_open = false; }
			if ( ImGui::Button( "Attach/Tow Vehicle" ) )    { veh::start_pick( 4 ); g_open = false; }
			ImGui::EndTabItem();
		}
		// ---------------- Movement ----------------
		if ( ImGui::BeginTabItem( "Movement" ) ) {
			ImGui::Checkbox( "Fly (WASD + Space/Ctrl)", &g_set.fly ); ImGui::SameLine();
			HelpMarker( "Fly on foot: WASD to move, Space up, Ctrl down. Cancels gravity while on." );
			ImGui::SliderFloat( "Fly speed", &g_set.fly_speed, 0.1f, 3.0f, "%.2f" );
			ImGui::Separator();
			ImGui::Checkbox( "Air Brake (freeze + WASD)", &g_set.air_brake ); ImGui::SameLine();
			HelpMarker( "Freezes you in place, then WASD nudges you around (works in a vehicle too)." );
			ImGui::SliderFloat( "Air brake speed", &g_set.air_brake_speed, 0.1f, 4.0f, "%.2f" );
			ImGui::EndTabItem();
		}
		// ---------------- Client (safety, join mode, display) ----------------
		if ( ImGui::BeginTabItem( "Client" ) ) {
			Section( "Safety" );
			ImGui::Checkbox( "Disable Unsafe/Big Hacks", &g_set.no_big_hacks ); ImGui::SameLine();
			HelpMarker( "A master safety switch: while on, the riskiest, most obvious cheats are forced off every "
			            "frame so you can't turn them on by accident." );
			ImGui::Checkbox( "PANIC (hide everything)", &g_set.cheat_panic ); ImGui::SameLine();
			HelpMarker( "Instantly stops touching any network traffic and drops godmode -- as if the mod were off. "
			            "Also closes the menu. Untick to resume." );
			Section( "0.3.7 Compat" );
			bool on = compat::in_037_mode();
			ImGui::Text( "Join mode: %s", on ? "0.3.7 (spoofed)" : "0.3.DL" );
			if ( ImGui::Button( on ? "Switch to 0.3.DL" : "Switch to 0.3.7" ) )
				compat::toggle_mode();
			ImGui::SameLine();
			HelpMarker( "Which SA:MP version you join as. 0.3.7 servers are auto-detected on join; use /compat (or "
			            "this button) before connecting to force it." );
			Section( "Display" );
			ImGui::Checkbox( "Uncap FPS (frame limiter off)", &g_set.uncap_fps ); ImGui::SameLine();
			HelpMarker( "Turns off GTA's built-in frame limiter." );
			ImGui::Checkbox( "Borderless window (fill screen)", &g_set.borderless ); ImGui::SameLine();
			HelpMarker( "Runs the game in a borderless window that fills the screen (easy alt-tab)." );
			if ( ImGui::Checkbox( "Exclusive fullscreen: max refresh rate", &g_set.fullscreen_max_hz ) )
				render::set_max_fullscreen_refresh( g_set.fullscreen_max_hz ); // applies at once
			ImGui::SameLine();
			HelpMarker( "Forces GTA's EXCLUSIVE fullscreen to your monitor's HIGHEST refresh at the current "
			            "resolution (GTA hardcodes the lowest >=60Hz). No borderless needed. Applies at once "
			            "when you tick or untick it, and on every game start when saved -- the screen flickers "
			            "once as the display resets. Only matters in exclusive fullscreen." );
			ImGui::EndTabItem();
		}
		// ---------------- Visuals (ESP + the mod's own overlay) ----------------
		if ( ImGui::BeginTabItem( "Visuals" ) ) {
			Section( "Players" );
			ImGui::Checkbox( "Chams", &g_set.chams ); ImGui::SameLine();
			ImGui::Checkbox( "Wireframe", &g_set.chams_wireframe ); ImGui::SameLine();
			HelpMarker( "Chams: draws players as a solid colour visible through walls. Wireframe draws them as an "
			            "outline instead." );
			ImGui::Checkbox( "Nametag", &g_set.nametags ); ImGui::SameLine();
			HelpMarker( "Draws a name above each player ([M] = server-hidden, [F] = friend, AFK shown too)." );
			ImGui::Checkbox( "Nametag Type (SA-MP)", &g_set.nametags_samp ); ImGui::SameLine();
			HelpMarker( "Use SA:MP's own nametag look instead of the custom one." );
			ImGui::Checkbox( "Show Players Guns", &g_set.show_players_guns ); ImGui::SameLine();
			HelpMarker( "Lists the weapons each nearby player is carrying, by their tag." );
			ImGui::SliderFloat( "Nametag Distance", &g_set.player_tags_dist, 20.0f, 500.0f, "%.0f" ); ImGui::SameLine();
			HelpMarker( "How far away player name tags still show." );
			ImGui::Checkbox( "Player ESP (box)", &g_set.player_esp ); ImGui::SameLine();
			HelpMarker( "Draws a box around players, with the details you tick below." );
			if ( g_set.player_esp ) { ImGui::Indent();
				ImGui::Checkbox( "Box", &g_set.esp_box ); ImGui::SameLine(); ImGui::Checkbox( "Name", &g_set.esp_name ); ImGui::SameLine();
				ImGui::Checkbox( "HP", &g_set.esp_health ); ImGui::SameLine(); ImGui::Checkbox( "Dist", &g_set.esp_distance ); ImGui::Unindent(); }
			Section( "World" );
			ImGui::Checkbox( "Locked Vehicle ESP", &g_set.locked_vehicle_esp ); ImGui::SameLine();
			HelpMarker( "Marks locked vehicles in the world (behind-wall ones are hidden)." );
			ImGui::SliderFloat( "Vehicle Tag Distance", &g_set.vehicle_tags_dist, 20.0f, 300.0f, "%.0f" ); ImGui::SameLine();
			HelpMarker( "How far away vehicle tags still show." );
			ImGui::Checkbox( "Weapon Finder", &g_set.weapon_finder ); ImGui::SameLine();
			HelpMarker( "Marks weapon pickups lying in the world." );
			Section( "Overlay (the mod's own HUD)" );
			ImGui::Checkbox( "Cheat state text (top-right)", &g_set.status_text ); ImGui::SameLine();
			HelpMarker( "Shows a short status line, top-right, for what a cheat just did." );
			ImGui::Checkbox( "Active-cheat watermark", &g_set.show_active_list ); ImGui::SameLine();
			HelpMarker( "Lists the cheats you currently have on, bottom-right." );
			ImGui::TextDisabled( "The menu + this overlay draw above SA:MP's chat; ESP/nametags stay in the world." );
			ImGui::EndTabItem();
		}
// ---------------- Debug (packet recorder / tracer) ----------------
		if ( ImGui::BeginTabItem( "Debug" ) ) {
			// Packet recorder: runtime only, never saved -- see recorder.h.
			Section( "Packet recorder / tracer" );
			{
				static bool openFailed = false;
				bool rec = recorder::active();
				char recLabel[96];
				std::snprintf( recLabel, sizeof recLabel, "Record to %s###recorder", recorder::file_name() );
				if ( ImGui::Checkbox( recLabel, &rec ) ) {
					if ( rec ) openFailed = !recorder::start();
					else       recorder::stop();
				}
				ImGui::SameLine();
				HelpMarker( "A packet tracer + state recorder for any server: writes everything the server sends "
				            "and everything you send -- decoded, with the time in seconds -- to fuego_packets.log "
				            "in the GTA folder: game text, textdraws (with the box sizes a progress bar changes), "
				            "chat, every chat-box line, dialogs with what you typed or picked, commands, "
				            "animations, objects, sounds, connection events; every key and mouse button you "
				            "press and release with how long you held it (not what you type in chat or this "
				            "menu); your state (position, health, money, weapons, vehicle, interior, menus) as "
				            "it changes. Players are named next to their ids, vehicles by model. In game: "
				            "/.rec starts/stops it (/.rec others or /.rec all for more detail), /.mark <note> "
				            "adds a numbered note at that moment. Not saved: it's off at every game start." );
				int detail = recorder::detail();
				ImGui::SetNextItemWidth( 210.0f );
				if ( ImGui::Combo( "Detail", &detail, "Normal\0+ other players\0Everything (raw bytes)\0" ) )
					recorder::set_detail( detail );
				ImGui::SameLine();
				HelpMarker( "Normal: you, the server, everyone's chat, joins, quits and kills; other players' own "
				            "traffic, streaming and far-away objects are only counted.\n"
				            "+ other players: also what other players do -- their animations, cars, chat bubbles, "
				            "and their syncs whenever their keys / weapon / vehicle change.\n"
				            "Everything: every packet in and out, unchanged ones too, each with its raw bytes. "
				            "Big files fast -- for short recordings.\n"
				            "Applies at once, also while recording." );
				bool hide = recorder::hide_secrets();
				if ( ImGui::Checkbox( "Hide passwords (for sharing)", &hide ) )
					openFailed = !recorder::set_hide_secrets( hide ); // false: the other file couldn't be opened
				ImGui::SameLine();
				HelpMarker( "Off (default): everything you type is written as is -- dialog passwords, /login "
				            "arguments, RCON, the join packet's auth key, anti-cheat answers.\n"
				            "On: those are left out (a password dialog's reply isn't even its length, RCON answers "
				            "neither), keys pressed while a dialog is open aren't written, and the recording goes "
				            "to fuego_packets_share.log -- a file of its own that never holds a secret (switching "
				            "this while recording carries on in the other file). Turn it on before recording a "
				            "log you'll give to someone." );
				if ( recorder::active() ) {
					const unsigned s = recorder::seconds();
					ImGui::TextDisabled( "recording %u:%02u -- %llu lines, %d marks", s / 60, s % 60, recorder::lines(),
					                     recorder::marks() );
				} else if ( openFailed ) {
					ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "Couldn't open %s.", recorder::file_name() );
				} else {
					ImGui::TextDisabled( "In game: /.rec [normal|others|all] starts/stops, /.mark <note> marks a moment." );
				}
			}
			ImGui::EndTabItem();
		}
		// ---------------- Translator ----------------
		// The everyday settings up top (what to translate, which AI); the rarely touched
		// ones fold away under Advanced. (Its debug tools are gone: /.rec replaced them.)
		if ( ImGui::BeginTabItem( "Translator" ) ) {
			auto &s = i18n::config::get();

			// Auto-populate the model dropdown the first time the tab is opened,
			// so the user doesn't have to click Refresh to see installed models.
			static bool auto_fetched = false;
			if ( !auto_fetched ) { auto_fetched = true; fetch_models_async(); }

			// Edit buffers for the ACTIVE provider's URL / key / model, refilled whenever
			// the provider changes (each provider remembers its own -- switch_provider()).
			static char endpoint_buf[256] = "";
			static char api_key_buf[256]  = "";
			static char model_buf[256]    = "";
			static bool bufs_init         = false;
			static int  bufs_kind         = 0;
			static bool local_key         = false; // Ollama / Local shows its key field only if it has a key
			auto sync_bufs = [&]() {
				bufs_init = true;
				bufs_kind = s.provider_kind;
				local_key = !s.api_key.empty(); // (stays shown while you clear it)
				strncpy_s( endpoint_buf, sizeof endpoint_buf, s.base_url.c_str(), _TRUNCATE );
				strncpy_s( api_key_buf,  sizeof api_key_buf,  s.api_key.c_str(),  _TRUNCATE );
				strncpy_s( model_buf,    sizeof model_buf,    s.model.c_str(),    _TRUNCATE );
			};
			if ( !bufs_init || bufs_kind != s.provider_kind ) sync_bufs();

			// Narrower fields than the window-wide default keep each label next to its field.
			const float fieldW = ImGui::GetFontSize() * 20.0f;
			const float halfW  = ImGui::GetContentRegionAvail().x * 0.55f;
			ImGui::PushItemWidth( fieldW < halfW ? fieldW : halfW );

			// ---- What the server shows me ----
			Section( "Translate for me" );
			if ( ImGui::Checkbox( "Chat", &s.inbound_chat ) )
				i18n::hooks::apply_config(); // live: (re)start the engine if needed
			ImGui::SameLine();
			HelpMarker( "Server chat. A line seen before shows translated at once; a new one shows in the "
			            "original and turns into its translation in place a moment later." );
			ImGui::SameLine( 0.0f, ImGui::GetFontSize() * 1.5f );
			if ( ImGui::Checkbox( "Dialogs, labels + signs", &s.translate_dialogs ) )
				i18n::hooks::apply_config();
			ImGui::SameLine();
			HelpMarker( "Dialog boxes, 3D world text labels and signs/billboards (object text). A dialog you "
			            "haven't seen translated before is held while it's translated ('translating "
			            "dialog...' shows), then appears translated -- in the order the server sent them and "
			            "with the server's original timing between them. If translating takes longer than "
			            "10 s, it appears in the original instead; a text-only one still open when its "
			            "translation is ready switches to it. Dialogs seen before appear translated at once. "
			            "Password dialogs are translated too -- the password you type is never touched. A 3D "
			            "label or sign not translated before shows in the original and switches to the "
			            "translation a moment later." );
			ImGui::SameLine( 0.0f, ImGui::GetFontSize() * 1.5f );
			if ( ImGui::Checkbox( "HUD", &s.translate_hud ) )
				i18n::hooks::apply_config();
			ImGui::SameLine();
			HelpMarker( "HUD textdraws, big center gametext and chat bubbles, line by line. Numbers (timers, "
			            "counts, prices) stay as they are, so a changing number doesn't need a new "
			            "translation. A textdraw not translated before shows in the original and switches to "
			            "the translation a moment later; gametext and chat bubbles show translated from the "
			            "next time. NOTE: textdraws/gametext use GTA's glyph font -- best for English/"
			            "romanized targets; Cyrillic/Arabic may not render there (dialogs/labels/chat are "
			            "fine)." );
			LanguageCombo( "Into (my language)", &s.target_lang );
			ImGui::Checkbox( "Romanized (Latin letters)", &s.roman_output );
			ImGui::SameLine();
			HelpMarker( "Write your language phonetically in the Roman/Latin alphabet (Roman Urdu, Hinglish, "
			            "Arabizi...). REQUIRED for Urdu/Arabic/Hindi and other scripts SA:MP's chat font "
			            "can't draw; harmless for Latin languages." );

			// ---- What I send ----
			Section( "Translate what I send" );
			if ( ImGui::Checkbox( "My chat + dialog replies", &s.outbound_enabled ) )
				i18n::hooks::apply_config(); // apply live
			ImGui::SameLine();
			HelpMarker( "Your typed chat AND your text in input dialogs are translated into the server's "
			            "language before sending. Passwords you type are never touched; picking a row in a "
			            "translated list always sends the server's original row. Message commands are "
			            "translated too, only their message: '/pm 35 hey come here' keeps '/pm 35' "
			            "(same for /me, /do, /b, /w, /report, ...), and any command followed by a whole "
			            "sentence of 3+ words ('/megaphone stop right here and now!'). Short arguments "
			            "('/gps atm', '/engine on') and numbers are never translated. If translation "
			            "fails, your original text is sent." );
			LanguageCombo( "Into (server language)", &s.outbound_target_lang );
			{
				bool c  = ( s.hotkey_mods & 1 ) != 0;
				bool sh = ( s.hotkey_mods & 2 ) != 0;
				bool a  = ( s.hotkey_mods & 4 ) != 0;
				ImGui::AlignTextToFramePadding(); // level with the checkboxes beside it
				ImGui::TextUnformatted( "On/off key:" );
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Ctrl", &c ) ) s.hotkey_mods ^= 1;
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Shift", &sh ) ) s.hotkey_mods ^= 2;
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Alt", &a ) ) s.hotkey_mods ^= 4;
				ImGui::SameLine();
				// Only keys SA:MP itself leaves free (it takes F4, F5, F6, F9 and PageUp/
				// PageDown, besides F1/F7/F8/F12 and this menu's F2).
				static const struct { int vk; const char *name; } kKeys[] = {
					{ 0, "(no key)" }, { VK_F3, "F3" }, { VK_F11, "F11" }, { VK_HOME, "Home" },
					{ VK_END, "End" }, { VK_INSERT, "Insert" }, { VK_PAUSE, "Pause" } };
				const char *cur = "(custom)";
				for ( const auto &k : kKeys )
					if ( k.vk == s.hotkey_vk ) cur = k.name;
				ImGui::SetNextItemWidth( ImGui::GetFontSize() * 6.0f );
				if ( ImGui::BeginCombo( "##hotkey_vk", cur ) ) {
					for ( const auto &k : kKeys )
						if ( ImGui::Selectable( k.name, k.vk == s.hotkey_vk ) ) s.hotkey_vk = k.vk;
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				HelpMarker( "Turns 'Translate what I send' on/off while you play -- a short notice fades in "
				            "on screen. Default Ctrl+Shift with no extra key. Only keys SA:MP and this menu "
				            "leave free are offered: F1, F2, F4-F9, F12 and PageUp/PageDown are taken." );
			}

			// ---- The AI that translates ----
			Section( "AI provider" );
			{
				const auto &presets = i18n::config::provider_presets();
				const char *cur = "(unknown)";
				for ( const auto &p : presets )
					if ( p.kind == s.provider_kind ) cur = p.name;
				if ( ImGui::BeginCombo( "Provider", cur ) ) {
					for ( const auto &p : presets ) {
						const bool sel = ( p.kind == s.provider_kind );
						if ( ImGui::Selectable( p.name, sel ) && !sel ) {
							i18n::config::switch_provider( p.kind ); // stores the old one's key/model, restores this one's
							sync_bufs();
							fetch_models_async( /*clearList=*/true );
						}
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				HelpMarker( "Where the translations come from. Each provider keeps its own API key, model, "
				            "server URL and request gap, so you can switch back and forth without losing "
				            "anything (languages and the rest are shared). Google (has a free tier), "
				            "Anthropic and OpenAI need an API key; Ollama / Local runs offline on your PC; "
				            "Custom is any OpenAI-compatible server." );
			}
			auto endpoint_input = [&]() {
				if ( ImGui::InputTextWithHint( "Server URL", "http://host:port/v1", endpoint_buf, sizeof endpoint_buf ) )
					s.base_url = endpoint_buf;
				if ( ImGui::IsItemDeactivatedAfterEdit() ) fetch_models_async( true );
				ImGui::SameLine();
				HelpMarker( "The provider's OpenAI-compatible API address (usually ends in /v1). Filled in "
				            "for you for Google / Anthropic / OpenAI / Ollama; change it for a server on "
				            "another port or a proxy. Kept per provider." );
			};
			if ( s.provider_kind == 0 ) endpoint_input(); // Custom: the URL is the whole point
			if ( s.provider_kind != 4 || local_key ) {    // Ollama / Local needs no key
				if ( ImGui::InputText( "API key", api_key_buf, sizeof api_key_buf, ImGuiInputTextFlags_Password ) )
					s.api_key = api_key_buf;
				if ( ImGui::IsItemDeactivatedAfterEdit() ) fetch_models_async( true ); // the list this key unlocks
				ImGui::SameLine();
				HelpMarker( s.provider_kind == 4
				    ? "Ollama needs no key -- clear this unless your local server asks for one. Kept "
				      "for this provider only, in fuego_reversed.cfg on this PC."
				    : "Kept for this provider only -- every provider remembers its own key. Stored in "
				      "fuego_reversed.cfg on this PC (never share that file)." );
			}
			// Model: type any name, or pick from what the endpoint actually serves (the arrow).
			{
				const ImGuiStyle &st = ImGui::GetStyle();
				ImGui::SetNextItemWidth( ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - st.ItemInnerSpacing.x );
				if ( ImGui::InputText( "##model", model_buf, sizeof model_buf ) ) s.model = model_buf;
				ImGui::SameLine( 0.0f, st.ItemInnerSpacing.x );
				if ( ImGui::BeginCombo( "##model_list", nullptr,
				         ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge ) ) {
					if ( ImGui::SmallButton( "Refresh list" ) ) fetch_models_async();
					std::lock_guard<std::mutex> lk( g_modelMtx );
					const int mst = g_modelState.load();
					ImGui::SameLine();
					if ( mst == 1 )      ImGui::TextDisabled( "loading..." );
					else if ( mst == 2 ) ImGui::TextDisabled( "%d available", (int) g_models.size() );
					else if ( mst == 3 ) ImGui::TextDisabled( "couldn't load the list" );
					ImGui::Separator();
					for ( const auto &m : g_models ) {
						const bool sel = ( m == s.model );
						if ( ImGui::Selectable( m.c_str(), sel ) ) {
							s.model = m;
							strncpy_s( model_buf, sizeof model_buf, m.c_str(), _TRUNCATE );
						}
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					if ( g_models.empty() && mst != 1 )
						ImGui::TextDisabled( !s.base_url.empty()     ? "none listed -- type the model name instead"
						                     : s.provider_kind == 0 ? "enter the Server URL first"
						                                            : "enter the Server URL first (under Advanced)" );
					ImGui::EndCombo();
				}
				ImGui::SameLine( 0.0f, st.ItemInnerSpacing.x );
				ImGui::TextUnformatted( "Model" );
				ImGui::SameLine();
				HelpMarker( "Type a model name, or open the arrow to pick one this provider offers. Kept per "
				            "provider." );
				if ( g_modelState.load() == 3 ) {
					std::lock_guard<std::mutex> lk( g_modelMtx );
					if ( s.provider_kind == 4 )
						HintWrapped( "Couldn't reach Ollama (%s) -- is it running?", g_modelErr.c_str() );
					else
						HintWrapped( "Couldn't list the models (%s) -- check the key / URL, or type the name.",
						             g_modelErr.c_str() );
				}
			}
			if ( s.provider_kind == 4 ) {
				ImGui::TextDisabled( "Tip: Hy-MT2-7B is a strong local translation model." );
				ImGui::SameLine();
				if ( ImGui::SmallButton( "copy its pull command" ) )
					ImGui::SetClipboardText( "ollama pull hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M" );
				if ( s.roman_output )
					HintWrapped( "%s", "Romanized output: local models romanize poorly -- a cloud provider "
					                   "(Google has a free tier) does it well." );
			}

			ImGui::Spacing();
			{
				ImGui::Text( "Status: %s", i18n::engine::status() );
				ImGui::SameLine();
				auto rs = i18n::engine::response_stats();
				if ( rs.count > 0 )
					ImGui::TextDisabled( "-- avg %lu ms (last %lu ms, %llu calls)",
					                     rs.avgMs, rs.lastMs, (unsigned long long) rs.count );
				else
					ImGui::TextDisabled( "-- no translations yet" );
				ImGui::SameLine();
				HelpMarker( "Average round-trip delay of translation requests since the translator engine last "
				            "(re)started -- turning translation on, or a Save that changes the provider, URL, "
				            "key, model, languages, romanization or request settings -- and the most recent "
				            "one. A Save that changes none of those keeps the numbers. This is the REAL "
				            "end-to-end delay -- it includes the Min request gap wait and any 429/503 retry "
				            "backoff, not just the model's inference time. Cache hits (repeated lines) are "
				            "instant and not counted." );
				std::string le = i18n::engine::last_error();
				if ( !le.empty() ) HintWrapped( "Last error: %s", le.c_str() );
			}

			// Self-test: translate a sample line right now (with the settings above, even
			// unsaved) so you can verify the provider/model without joining a server.
			if ( ImGui::Button( "Test translation" ) ) run_selftest_async();
			ImGui::SameLine();
			int sst = g_selfState.load();
			if ( sst == 1 )      ImGui::TextDisabled( "translating..." );
			else if ( sst == 2 ) ImGui::TextDisabled( "ok (%ld ms)", g_selfMs );
			else if ( sst == 3 ) ImGui::TextDisabled( "failed (%ld ms)", g_selfMs );
			{
				std::lock_guard<std::mutex> lk( g_selfMtx );
				if ( !g_selfResult.empty() ) {
					ImGui::TextWrapped( "sample: %s", kSelfSample );
					ImGui::TextWrapped( "result: %s", g_selfResult.c_str() );
				}
			}

			// ---- Rarely needed: folded away ----
			ImGui::Spacing();
			if ( ImGui::CollapsingHeader( "Advanced" ) ) {
				if ( s.provider_kind != 0 ) endpoint_input(); // (Custom shows it above)

				LanguageCombo( "Incoming language", &s.source_lang, "Auto (detect)" );
				ImGui::SameLine();
				HelpMarker( "Language of INCOMING server text. Leave on Auto -- the model detects it per "
				            "line, which handles servers that mix English with a local language. Pick a "
				            "specific language only to hint the model on a reliably single-language server." );

				{
					static const struct { int cp; const char *name; } kCps[] = {
						{ 0, "Auto (detect)" },
						{ 1252, "1252 Western (English, Spanish, ...)" },
						{ 1251, "1251 Cyrillic (Russian, Ukrainian, ...)" },
						{ 1250, "1250 Central European (Polish, Czech, ...)" },
						{ 1254, "1254 Turkish" }, { 1253, "1253 Greek" }, { 1257, "1257 Baltic" },
						{ 1255, "1255 Hebrew" }, { 1256, "1256 Arabic" }, { 1258, "1258 Vietnamese" },
						{ 874, "874 Thai" } };
					char custom[16];
					const char *cur = nullptr;
					for ( const auto &c : kCps )
						if ( c.cp == s.source_cp ) cur = c.name;
					if ( !cur ) { snprintf( custom, sizeof custom, "%d", s.source_cp ); cur = custom; }
					if ( ImGui::BeginCombo( "Server text encoding", cur ) ) {
						for ( const auto &c : kCps ) {
							const bool sel = ( c.cp == s.source_cp );
							if ( ImGui::Selectable( c.name, sel ) ) s.source_cp = c.cp;
							if ( sel ) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					HelpMarker( "How the server's text bytes are encoded. Leave on Auto: it tells Russian "
					            "(1251) from Western (1252) text by itself. Pick one only if a server's text "
					            "comes out garbled (e.g. 1250 for a Polish or Czech server)." );
				}

				{
					float t = ( s.translate_temp >= 0 ? s.translate_temp : 30 ) / 100.0f;
					if ( ImGui::SliderFloat( "Temperature", &t, 0.0f, 1.0f, "%.2f" ) )
						s.translate_temp = (int) ( t * 100.0f + 0.5f );
					ImGui::SameLine();
					HelpMarker( "How free the model is with wording. 0.30 works well for both Hy-MT2 and "
					            "general models. Higher = more varied; 0 can make Hy-MT2 loop." );
				}

				// (Keep %d in both formats: Ctrl+click typing parses the number with it.)
				ImGui::SliderInt( "Min request gap", &s.request_min_gap_ms, 0, 5000,
				                  s.request_min_gap_ms > 0 ? "%d ms" : "%d (off)" );
				ImGui::SameLine();
				HelpMarker( "Minimum time between translation requests, paced across all workers. Leave off "
				            "for local or paid cloud. For a FREE-tier cloud key (e.g. Gemini free) set "
				            "~1000-4000 ms to stay under its requests-per-minute limit and avoid 429/503 "
				            "throttling (those are also retried with backoff anyway). Kept per provider." );

				{
					int secs = ( s.read_timeout_ms > 0 ? s.read_timeout_ms : 30000 ) / 1000;
					if ( ImGui::SliderInt( "Response timeout", &secs, 5, 120, "%d s" ) )
						s.read_timeout_ms = secs * 1000;
					ImGui::SameLine();
					HelpMarker( "How long to wait for one translation before giving up on it (the original "
					            "text stays). Local models need headroom: the first request loads the model, "
					            "which can take tens of seconds. Default 30 s." );
				}

				ImGui::Checkbox( "On-screen hints", &s.show_translate_indicators );
				ImGui::SameLine();
				HelpMarker( "'Your message will be sent in <language>' while you type in chat or an input "
				            "dialog with outgoing translation on, and 'translating dialog...' while a dialog "
				            "is held for translation." );
			}

			ImGui::PopItemWidth();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::EndChild(); // ##cheats

	// The config box: its own framed box, pinned under the cheats and always in view.
	ImGui::BeginChild( "##config", ImVec2( 0, 0 ), true );
	static DWORD saveFailedAt = 0;
	if ( ImGui::Button( "Save config" ) ) {
		const bool saved = cfg::save(); // unified: cheats + /compat + translator -> fuego_reversed.cfg
		i18n::hooks::apply_config();    // apply translator changes live (start/stop/reconfigure the engine)
		if ( saved ) i18n::config::mark_saved(); // saved + applied: the "changes pending" hint clears
		saveFailedAt = saved ? 0 : GetTickCount();
	}
	ImGui::SameLine();
	if ( saveFailedAt && GetTickCount() - saveFailedAt < 5000 )
		ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "Couldn't write fuego_reversed.cfg -- is it read-only?" );
	else if ( i18n::config::has_unsaved_changes() )
		ImGui::TextColored( ImVec4( 1.0f, 0.65f, 0.25f, 1.0f ), "Translator changes pending -- press Save config." );
	else
		ImGui::TextDisabled( "Saves cheats, join mode + translator to fuego_reversed.cfg (loads on startup)." );
	ImGui::TextDisabled( "Tip: type /fuego_cmds in chat for every command -- the menu doesn't cover them all." );
	ImGui::EndChild(); // ##config
	ImGui::End();
}

// Display tweaks (per-frame): force GTA's frame limiter off + borderless-window the game
// window (edge-triggered, restores the original style when toggled off).
void display_tick() {
	auto &set = fx::g_set;
	if ( set.uncap_fps )
		*reinterpret_cast<BYTE *>( 0xBA6794 ) = 0; // GTA frame-limiter flag (1 = limited)

	static bool     applied    = false;
	static LONG_PTR savedStyle = 0;
	HWND h = render::window();
	if ( !h ) return;
	if ( set.borderless && !applied ) {
		savedStyle = GetWindowLongPtr( h, GWL_STYLE );
		SetWindowLongPtr( h, GWL_STYLE, WS_POPUP | WS_VISIBLE );
		HMONITOR mon = MonitorFromWindow( h, MONITOR_DEFAULTTONEAREST );
		MONITORINFO mi = { sizeof( mi ) };
		if ( GetMonitorInfo( mon, &mi ) )
			SetWindowPos( h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			              mi.rcMonitor.right - mi.rcMonitor.left,
			              mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW );
		applied = true;
	} else if ( !set.borderless && applied ) {
		SetWindowLongPtr( h, GWL_STYLE, savedStyle );
		SetWindowPos( h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER );
		applied = false;
	}
}

void on_frame( IDirect3DDevice9 *dev ) {
	if ( !g_init ) {
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr; // don't write imgui.ini
		// Don't let ImGui manage the OS cursor -- SA:MP's toggle_cursor owns it.
		// (Otherwise ImGui SetCursor()s the arrow every frame -> cursor stuck on.)
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		// Crisp TTF instead of the upscaled bitmap font (fall back gracefully).
		// Oversampling + a larger pixel size = sharper, less "low-res" text.
		ImFontConfig fcfg;
		fcfg.OversampleH = 3;
		fcfg.OversampleV = 2;
		fcfg.PixelSnapH  = true;
		// Cyrillic range (also includes basic Latin) so translated RU/UK text --
		// and the translator self-test result -- renders in the menu instead of
		// as '?' boxes. (Arabic/CJK still won't render; romanize those.)
		const ImWchar *ranges = io.Fonts->GetGlyphRangesCyrillic();
		ImFont *f = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf", 20.0f, &fcfg, ranges );
		if ( !f ) f = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\tahoma.ttf", 19.0f, &fcfg, ranges );
		if ( !f ) { io.Fonts->AddFontDefault(); io.FontGlobalScale = 1.4f; }
		visuals::init_fonts(); // overlay (ESP/nametag) fonts, after the default menu font
		style();
		ImGui_ImplWin32_Init( render::window() );
		ImGui_ImplDX9_Init( dev );
		g_init = true;
		Log( "[menu] ImGui initialised." );
	}

	// Show SA:MP's cursor while the menu is open OR a vehicle pick is in progress (frees
	// the OS cursor for ImGui, suspends the game camera); hide it otherwise. Edge-triggered.
	bool wantCursor = !render::overlay_hidden() && ( g_open || veh::is_picking() );
	static bool prevCursor = false;
	if ( wantCursor != prevCursor ) {
		samp::toggle_cursor( wantCursor );
		prevCursor = wantCursor;
	}

	display_tick(); // uncap FPS + borderless window

	// Outgoing-translation hotkey (config hotkey_mods/hotkey_vk, default Ctrl+Shift),
	// shared by outgoing chat + dialog-input replies. Edge-detected + 400ms cooldown so
	// a held combo fires once; toggle() shows a fading on-screen notice so it's never silent.
	{
		static bool  prevCombo  = false;
		static DWORD lastToggle = 0;
		const auto  &hk   = i18n::config::get();
		auto         down = []( int vk ) { return ( GetAsyncKeyState( vk ) & 0x8000 ) != 0; };
		bool combo = ( hk.hotkey_mods || hk.hotkey_vk ) &&
		             ( !( hk.hotkey_mods & 1 ) || down( VK_CONTROL ) ) &&
		             ( !( hk.hotkey_mods & 2 ) || down( VK_SHIFT ) ) &&
		             ( !( hk.hotkey_mods & 4 ) || down( VK_MENU ) ) &&
		             ( !hk.hotkey_vk || down( hk.hotkey_vk ) );
		DWORD now = GetTickCount();
		if ( combo && !prevCombo && ( now - lastToggle ) > 400 ) {
			i18n::outbound::toggle();
			lastToggle = now;
		}
		prevCombo = combo;
	}

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Click-to-pick: while a vehicle pick is active, feed the ImGui cursor + input to the
	// picker so clicking a car in the world unlocks / damages / destroys it.
	if ( veh::is_picking() ) {
		ImGuiIO &io = ImGui::GetIO();
		veh::run_picker( io.MousePos.x, io.MousePos.y,
		                 ImGui::IsMouseClicked( ImGuiMouseButton_Left ),
		                 ImGui::IsKeyPressed( ImGuiKey_Escape ) || ImGui::IsMouseClicked( ImGuiMouseButton_Right ) );
	}

	visuals::draw(); // ESP + other on-screen cheats (background draw list)
	if ( !render::overlay_hidden() && !fx::g_set.cheat_panic ) {
		i18n::indicator::draw(); // "will be translated" / "translating dialog..." hints
		recorder::draw_overlay(); // the REC pill while the packet recorder runs
		autofish::draw_overlay(); // what the auto fisher is doing
	}

	// Use SA:MP's cursor (shown by toggle_cursor) as the single visible cursor;
	// don't also draw ImGui's software one.
	ImGui::GetIO().MouseDrawCursor = false;
	// Hide the menu while the SA:MP scoreboard is up (Fuego 1:1) so it doesn't cover it.
	bool sbOpen = false;
	if ( samp::stScoreboardInfo *sb = samp::scoreboard_info() ) sbOpen = sb->iIsEnabled != 0;
	if ( g_open && !render::overlay_hidden() && !sbOpen )
		draw_ui();

	ImGui::EndFrame();
	ImGui::Render();
	ImDrawData *dd = ImGui::GetDrawData();
	if ( g_framesSincePresent < 1000 ) ++g_framesSincePresent;
	// Draw over the chat only while Present is actually firing (on_present ran in the last
	// frame or two). If it isn't -- never hooked, or an overlay keeps stealing it -- draw
	// the whole UI here at EndScene instead (under the chat, but always visible). The two
	// frames of slack keep an occasional skipped Present from flipping modes / flickering.
	if ( !render::present_hooked() || g_framesSincePresent > 2 ) {
		g_uiPending = false;
		ImGui_ImplDX9_RenderDrawData( dd );
		return;
	}
	// Two layers: the background list (ESP, nametags, tags -- world overlays) is drawn
	// NOW, under SA:MP's UI like its own nametags; the menu windows + the foreground list
	// (status pills, hints, watermark) wait for on_present, after SA:MP's chat. (The
	// viewport must be named: after Render() there's no current window to take it from.)
	split_layers( dd, ImGui::GetBackgroundDrawList( ImGui::GetMainViewport() ) );
	if ( g_worldLayer.CmdListsCount > 0 ) ImGui_ImplDX9_RenderDrawData( &g_worldLayer );
	g_uiPending = true;
}

// The 2D UI layer, after SA:MP has drawn its chat / dialogs (render::on_present).
void on_present( IDirect3DDevice9 * ) {
	if ( !g_init ) return;
	g_framesSincePresent = 0; // Present is alive: on_frame defers the UI layer to here
	if ( !g_uiPending ) return;
	g_uiPending = false;
	if ( g_uiLayer.CmdListsCount > 0 ) ImGui_ImplDX9_RenderDrawData( &g_uiLayer );
}

void on_reset( IDirect3DDevice9 *, bool preReset ) {
	if ( !g_init ) return;
	g_uiPending = false; // that frame's layer is gone with the device's resources
	if ( preReset ) ImGui_ImplDX9_InvalidateDeviceObjects();
	else            ImGui_ImplDX9_CreateDeviceObjects();
}

bool on_wndproc( HWND h, UINT m, WPARAM w, LPARAM l ) {
	if ( !g_init ) return false;

	if ( m == WM_KEYDOWN || m == WM_SYSKEYDOWN ) {
		// One-time heartbeat: proves the WndProc hook is actually receiving keys.
		static bool firstKey = true;
		if ( firstKey ) { Log( "[menu] wndproc receiving keys (first key vk=0x%02X).", (unsigned) w ); firstKey = false; }
		// F2 toggles the menu (proper window input path).
		if ( w == VK_F2 ) {
			g_open = !g_open;
			Log( "[menu] F2 -> %s.", g_open ? "OPEN" : "closed" );
			return true; // consume
		}
		// F8 is SA:MP's screenshot key: briefly hide our overlays so the shot is clean,
		// but DON'T consume it -- SA:MP still needs the key to take the screenshot.
		if ( w == VK_F8 )
			render::hide_overlay_for( 2000 );
	}

	// Feed ImGui the input, but NEVER block the game: SA:MP's cursor mode already
	// keeps menu clicks out of gameplay, and you should be able to keep playing
	// with the menu open (move, shoot, etc.) -- same as Project Fuego.
	ImGui_ImplWin32_WndProcHandler( h, m, w, l );
	return false;
}

} // namespace

namespace menu {

bool is_open() { return g_open; }
void set_open( bool on ) { g_open = on; }

void install() {
	render::on_frame( on_frame );
	render::on_present( on_present ); // the menu/HUD layer, over SA:MP's chat
	render::on_reset( on_reset );
	render::on_wndproc( on_wndproc );
	Log( "[menu] registered (F2 to toggle)." );
}

} // namespace menu
