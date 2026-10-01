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
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace {

bool g_init = false;
bool g_open = false;

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

	// Scroll (don't shrink) tabs that overflow + a popup button to jump to any tab.
	if ( ImGui::BeginTabBar( "tabs",
	         ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton ) ) {
		// ---------------- Weapon Functions (Fuego order) ----------------
		if ( ImGui::BeginTabItem( "Weapon Functions" ) ) {
			ImGui::SliderInt( "Shot Repeater", &g_set.shot_repeater, 0, 100 );
			ImGui::Checkbox( "Fake Headshot", &g_set.fake_headshot );
			ImGui::Checkbox( "Trigger Bot", &g_set.trigger_bot );
			ImGui::Checkbox( "No Spread", &g_set.no_spread );
			ImGui::SliderInt( "No Spread Trigger", &g_set.no_spread_trigger, 0, 10 );
			ImGui::Checkbox( "No Reload", &g_set.no_reload );
			ImGui::Checkbox( "Dual Shot", &g_set.dual_shot );
			ImGui::SliderInt( "Dual Shot Delay", &g_set.dual_shot_delay, 0, 3000 );
			ImGui::SeparatorText( "Silent Aim" );
			ImGui::Checkbox( "Silent Aimbot", &g_set.silent_aim );
			ImGui::Checkbox( "Random False Shots", &g_set.silent_aim_random_shots );
			ImGui::Checkbox( "Range Hack", &g_set.silent_aim_range_hack );
			ImGui::Checkbox( "Shoot Through Walls", &g_set.silent_aim_wall_shot );
			{ const char *tr[] = { "Off", "ESP Only", "ESP & FOV" }; ImGui::Combo( "Draw Tracer", &g_set.silent_aim_tracer, tr, 3 ); }
			ImGui::SliderFloat( "Search FOV", &g_set.silent_aim_fov, 1.0f, 1024.0f, "%.0f" );
			ImGui::Checkbox( "Friend Filter", &g_set.silent_aim_friend_filter );
			ImGui::Checkbox( "Color Filter", &g_set.silent_aim_color_filter );
			ImGui::Checkbox( "Skin Filter", &g_set.silent_aim_skin_filter );
			ImGui::Checkbox( "Team Filter", &g_set.silent_aim_team_filter );
			ImGui::Checkbox( "AFK Filter", &g_set.silent_aim_afk_filter );
			ImGui::Checkbox( "Alive Filter", &g_set.silent_aim_alive_filter );
			// Aim point is randomized across bones each shot; Always Headshot forces the head.
			ImGui::Checkbox( "Always Headshot", &g_set.always_headshot );
			ImGui::Checkbox( "Lead Prediction", &g_set.silent_aim_prediction );
			ImGui::SeparatorText( "Killer" );
			ImGui::Checkbox( "Auto Killer", &g_set.killer_enabled );
			ImGui::SliderInt( "Killer Delay", &g_set.killer_delay, 1, 10000 );
			ImGui::SliderInt( "Bullets Per Person", &g_set.killer_bullets, 1, 92 );
			ImGui::SliderInt( "Statistics Interval", &g_set.killer_stats_interval, 1, 10 );
			ImGui::Checkbox( "Killer Friend Filter", &g_set.killer_friend_filter );
			ImGui::EndTabItem();
		}
		// ---------------- Misc Functions (Fuego order) ----------------
		if ( ImGui::BeginTabItem( "Misc Functions" ) ) {
			ImGui::SeparatorText( "Invisibility" );
			ImGui::Checkbox( "Invisibility", &g_set.invisibility );
			ImGui::Checkbox( "Smart Invisibility", &g_set.smart_invis );
			ImGui::SeparatorText( "Slappers" );
			ImGui::Checkbox( "Aim Slapper", &g_set.aim_slapper );
			ImGui::SliderInt( "Aim Slapper Power", &g_set.aim_slapper_power, 0, 100 );
			ImGui::Checkbox( "Near Slapper", &g_set.near_slapper );
			ImGui::Checkbox( "Near Slapper Friend Filter", &g_set.near_slapper_friend_filter );
			ImGui::SeparatorText( "Visuals" );
			ImGui::Checkbox( "Chams", &g_set.chams ); ImGui::SameLine();
			ImGui::Checkbox( "Wireframe", &g_set.chams_wireframe );
			ImGui::Checkbox( "Nametag", &g_set.nametags );
			ImGui::Checkbox( "Nametag Type (SA-MP)", &g_set.nametags_samp );
			// [M] (server-hidden), [F] (friend) + AFK now show automatically in custom mode.
			ImGui::Checkbox( "Locked Vehicle ESP", &g_set.locked_vehicle_esp );
			ImGui::Checkbox( "Weapon Finder", &g_set.weapon_finder );
			ImGui::Checkbox( "Show Players Guns", &g_set.show_players_guns );
			ImGui::SliderFloat( "Nametag Distance", &g_set.player_tags_dist, 20.0f, 500.0f, "%.0f" );
			ImGui::SliderFloat( "Vehicle Tag Distance", &g_set.vehicle_tags_dist, 20.0f, 300.0f, "%.0f" );
			ImGui::Checkbox( "Player ESP (box)", &g_set.player_esp );
			if ( g_set.player_esp ) { ImGui::Indent();
				ImGui::Checkbox( "Box", &g_set.esp_box ); ImGui::SameLine(); ImGui::Checkbox( "Name", &g_set.esp_name ); ImGui::SameLine();
				ImGui::Checkbox( "HP", &g_set.esp_health ); ImGui::SameLine(); ImGui::Checkbox( "Dist", &g_set.esp_distance ); ImGui::Unindent(); }
			ImGui::Separator();
			ImGui::Checkbox( "Onfoot Break Dance", &g_set.break_dance );
			ImGui::Checkbox( "Auto Unscrambler LSRP", &g_set.auto_unscramble );
			ImGui::SeparatorText( "Health" );
			ImGui::Checkbox( "Health Hack", &g_set.health_hack );
			ImGui::Checkbox( "Extra Godmode (block vehicle-health drops)", &g_set.enable_extra_godmode );
			ImGui::Checkbox( "Smart Health Hack", &g_set.smart_health_hack );
			ImGui::SliderInt( "Smart HH Proc Chance", &g_set.smart_hh_proc, 0, 100 );
			ImGui::SeparatorText( "Fake Lag" );
			ImGui::Checkbox( "Fake Movement Lag", &g_set.fake_lag );
			ImGui::Checkbox( "Fake Random Spikes", &g_set.fake_lag_random_spikes );
			ImGui::SliderInt( "Lag Delay", &g_set.fake_lag_delay, 0, 10000 );
			ImGui::Checkbox( "Teleportish Movement", &g_set.fake_lag_teleportish );
			ImGui::SeparatorText( "Skin Trolls" );
			ImGui::Checkbox( "Moonwalker", &g_set.moonwalk );
			ImGui::Checkbox( "Upside Down", &g_set.upside_down );
			ImGui::Checkbox( "Ghost Mode", &g_set.ghost_mode );
			ImGui::Checkbox( "Twister Mode", &g_set.twister_mode );

			// Mike's CnR auto fish: runtime only (never saved), no hotkey -- see autofish.h.
			ImGui::SeparatorText( "Mike's CnR" );
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
		// ---------------- Vehicle Functions (Fuego order) ----------------
		if ( ImGui::BeginTabItem( "Vehicle Functions" ) ) {
			ImGui::SeparatorText( "iTroll (hold I while driving)" );
			ImGui::TextDisabled( "1)Ground 2)SpeedAway 3)BurnDown 4)BurnUp 5)Spin 6)Ocean 7)SlowSpeed" );
			ImGui::SliderInt( "iTroll Mode", &g_set.itroll_mode, 0, 7 );
			ImGui::SeparatorText( "Generic" );
			ImGui::Checkbox( "Car Shooter", &g_set.car_shooter );
			ImGui::Checkbox( "Car Remover", &g_set.remove_cars );
			ImGui::Checkbox( "Car Exploder", &g_set.explode_cars );
			ImGui::Checkbox( "Floating Cars", &g_set.floating_cars );
			ImGui::Checkbox( "Car Exploder Silent", &g_set.explode_cars_silent );
			ImGui::Checkbox( "Unoccupied Car Remover", &g_set.unoccupied_remover );
			ImGui::SeparatorText( "Actions (on your current vehicle)" );
			// Repair is a menu button, NOT a hotkey -- an instant full repair is server-
			// detectable, so it must be a deliberate click, never an accidental keypress.
			if ( ImGui::Button( "Repair Vehicle" ) )        game::repair_local_vehicle();
			ImGui::SeparatorText( "Actions (click a vehicle to pick it)" );
			if ( ImGui::Button( "Unlock Vehicle" ) )        { veh::start_pick( 1 ); g_open = false; }
			if ( ImGui::Button( "Damage Vehicle Doors" ) )  { veh::start_pick( 2 ); g_open = false; }
			if ( ImGui::Button( "Destroy Vehicle" ) )       { veh::start_pick( 3 ); g_open = false; }
			if ( ImGui::Button( "Attach/Tow Vehicle" ) )    { veh::start_pick( 4 ); g_open = false; }
			ImGui::EndTabItem();
		}
		// ---------------- Movement (our extras) ----------------
		if ( ImGui::BeginTabItem( "Movement" ) ) {
			ImGui::Checkbox( "Fly (WASD + Space/Ctrl)", &g_set.fly );
			ImGui::SliderFloat( "Fly speed", &g_set.fly_speed, 0.1f, 3.0f, "%.2f" );
			ImGui::Separator();
			ImGui::Checkbox( "Air Brake (freeze + WASD)", &g_set.air_brake );
			ImGui::SliderFloat( "Air brake speed", &g_set.air_brake_speed, 0.1f, 4.0f, "%.2f" );
			ImGui::EndTabItem();
		}
		// ---------------- Main / Compat ----------------
		if ( ImGui::BeginTabItem( "Main" ) ) {
			ImGui::Checkbox( "Disable Unsafe/Big Hacks", &g_set.no_big_hacks );
			ImGui::Checkbox( "PANIC (hide everything)", &g_set.cheat_panic );
			ImGui::SeparatorText( "0.3.7 Compat" );
			bool on = compat::in_037_mode();
			ImGui::Text( "Join mode: %s", on ? "0.3.7 (spoofed)" : "0.3.DL" );
			if ( ImGui::Button( on ? "Switch to 0.3.DL" : "Switch to 0.3.7" ) )
				compat::toggle_mode();
			ImGui::TextWrapped( "0.3.7 servers auto-detected on join; /compat before connecting." );

			// Packet recorder (debug): runtime only, never saved -- see recorder.h.
			ImGui::SeparatorText( "Packet recorder (debug)" );
			{
				static bool openFailed = false;
				bool rec = recorder::active();
				if ( ImGui::Checkbox( "Record packets to fuego_packets.log", &rec ) ) {
					if ( rec ) openFailed = !recorder::start();
					else       recorder::stop();
				}
				ImGui::SameLine();
				HelpMarker( "Writes everything the server sends and everything you send -- decoded, with the time "
				            "in seconds -- to fuego_packets.log in the GTA folder: game text, textdraws (with the "
				            "box sizes a progress bar changes), chat, dialogs, animations, objects, sounds, and "
				            "every key you press and release with how long you held it. Use it to work out how a "
				            "server's minigame works. Other players' traffic is only counted. Never written: what "
				            "you type into dialogs, login/password command arguments, RCON. In game: /.rec "
				            "starts/stops it, /.mark <note> adds a numbered note at that moment. Not saved: it's "
				            "off at every game start." );
				if ( recorder::active() ) {
					const unsigned s = recorder::seconds();
					ImGui::TextDisabled( "recording %u:%02u -- %llu lines, %d marks", s / 60, s % 60, recorder::lines(),
					                     recorder::marks() );
				} else if ( openFailed ) {
					ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "Couldn't open fuego_packets.log." );
				} else {
					ImGui::TextDisabled( "In game: /.rec starts/stops, /.mark <note> marks a moment." );
				}
			}
			ImGui::EndTabItem();
		}
		// ---------------- Display ----------------
		if ( ImGui::BeginTabItem( "Display" ) ) {
			ImGui::Checkbox( "Uncap FPS (frame limiter off)", &g_set.uncap_fps );
			ImGui::Checkbox( "Borderless window (fill screen)", &g_set.borderless );
			if ( ImGui::Checkbox( "Exclusive fullscreen: max refresh rate", &g_set.fullscreen_max_hz ) )
				render::set_max_fullscreen_refresh( g_set.fullscreen_max_hz ); // applies at once
			ImGui::SameLine();
			HelpMarker( "Forces GTA's EXCLUSIVE fullscreen to your monitor's HIGHEST refresh at the current "
			            "resolution (GTA hardcodes the lowest >=60Hz). No borderless needed. Applies at once "
			            "when you tick or untick it, and on every game start when saved -- the screen flickers "
			            "once as the display resets. Only matters in exclusive fullscreen." );
			ImGui::SeparatorText( "Overlay" );
			ImGui::Checkbox( "Cheat state text (top-right)", &g_set.status_text );
			ImGui::Checkbox( "Active-cheat watermark", &g_set.show_active_list );
			ImGui::SameLine();
			HelpMarker( "Lists the cheats you have on in the bottom-right corner." );
			ImGui::EndTabItem();
		}
		// ---------------- Translator ----------------
		// The everyday settings up top (what to translate, which AI); the rarely touched
		// ones fold away under Advanced / Debug.
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
			ImGui::SeparatorText( "Translate for me" );
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
			ImGui::SeparatorText( "Translate what I send" );
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
			ImGui::SeparatorText( "AI provider" );
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

			if ( ImGui::CollapsingHeader( "Debug" ) ) {
				ImGui::Checkbox( "Log dialog/label/textdraw/gametext packets", &s.debug_capture );
				ImGui::SameLine();
				HelpMarker( "Read-only. Enable, join a server, trigger a dialog/3D label/textdraw/gametext, "
				            "then share fuego_reversed.log. Never modifies packets. Writes a LOT -- not saved, "
				            "so it's off again at the next game start. (For everything in and out, decoded, "
				            "use the packet recorder in the Main tab.)" );

				ImGui::Checkbox( "Lab: log chat/dialog internals", &s.debug_lab );
				ImGui::SameLine();
				HelpMarker( "Read-only. Logs the newest chat line and each decoded ShowDialog "
				            "(id/style/title/buttons/body -- never a password dialog's body) to "
				            "fuego_reversed.log. Applies immediately; not saved -- off at the next game start." );
			}

			ImGui::PopItemWidth();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Spacing(); // breathing room between the tab content and the footer
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
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
		ImGui::TextDisabled( "one file (fuego_reversed.cfg), loads on startup." );
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
	ImGui_ImplDX9_RenderDrawData( ImGui::GetDrawData() );
}

void on_reset( IDirect3DDevice9 *, bool preReset ) {
	if ( !g_init ) return;
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
	render::on_reset( on_reset );
	render::on_wndproc( on_wndproc );
	Log( "[menu] registered (F2 to toggle)." );
}

} // namespace menu
