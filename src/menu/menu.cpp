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
#include "../i18n/outbound.h" // Ctrl+Shift outgoing-chat toggle

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
// 2 done, 3 error.
std::mutex               g_modelMtx;
std::vector<std::string> g_models;
std::atomic<int>         g_modelState{ 0 };
std::string              g_modelErr;

void fetch_models_async() {
	if ( g_modelState.load() == 1 ) return; // a fetch is already in flight
	g_modelState.store( 1 );

	// Snapshot the endpoint/key now (the user may keep typing while it runs).
	i18n::ProviderConfig pc;
	auto &s = i18n::config::get();
	pc.baseUrl      = s.base_url;
	pc.apiKey       = s.api_key;
	pc.model        = s.model;
	pc.providerKind = s.provider_kind; // e.g. Anthropic /models needs its version header

	std::thread( [pc]() {
		std::vector<std::string> out;
		std::string err;
		bool ok = i18n::list_models( pc, out, err );
		std::lock_guard<std::mutex> lk( g_modelMtx );
		if ( ok ) {
			g_models = std::move( out );
			g_modelErr.clear();
			g_modelState.store( 2 );
		} else {
			g_modelErr = err;
			g_modelState.store( 3 );
		}
	} ).detach();
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

	std::thread( [pc, prompt]() {
		auto prov = i18n::make_openai_compatible( pc );
		std::vector<std::string> out;
		std::string err;
		DWORD t0 = GetTickCount();
		bool ok = prov && prov->translate( { kSelfSample }, prompt, "", out, err );
		DWORD t1 = GetTickCount();
		std::lock_guard<std::mutex> lk( g_selfMtx );
		g_selfMs = (long)( t1 - t0 );
		if ( ok && !out.empty() ) { g_selfResult = out[0]; g_selfState.store( 2 ); }
		else { g_selfResult = err.empty() ? "(failed)" : err; g_selfState.store( 3 ); }
	} ).detach();
}

// Auto-apply the fullscreen refresh override without the user Alt+Entering: run
// GTA's OWN video-mode apply, which resets the same device -> render::hkReset then
// injects the max refresh. Called from on_wndproc (message-pump thread), the same
// safe between-frames context GTA uses for its Alt+Enter reset.
//   GcurSelVM (current video mode index) @ 0x8D6220; SetVideoMode(int) @ 0x745C70
//   (__cdecl) -> RwD3D9ChangeVideoMode. Addresses from gta-reversed VideoMode.cpp.
bool g_reapplyVideo = false;
void reapply_video_mode_now() {
	if ( !render::installed() ) return; // device/game not ready
	int mode = *reinterpret_cast<int *>( 0x8D6220 );
	reinterpret_cast<void( __cdecl * )( int )>( 0x745C70 )( mode );
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
			ImGui::EndTabItem();
		}
		// ---------------- Display ----------------
		if ( ImGui::BeginTabItem( "Display" ) ) {
			ImGui::Checkbox( "Uncap FPS (frame limiter off)", &g_set.uncap_fps );
			ImGui::Checkbox( "Borderless window (fill screen)", &g_set.borderless );
			if ( ImGui::Checkbox( "Exclusive fullscreen: max refresh rate", &g_set.fullscreen_max_hz ) ) {
				render::set_max_fullscreen_refresh( g_set.fullscreen_max_hz );
				if ( g_set.fullscreen_max_hz ) g_reapplyVideo = true; // auto-apply once (one reset/flicker)
			}
			ImGui::SameLine();
			HelpMarker( "Forces GTA's EXCLUSIVE fullscreen to your monitor's HIGHEST refresh at the current "
			            "resolution (GTA hardcodes the lowest >=60Hz). No borderless needed. On enable it "
			            "auto-applies (one screen flicker as the display resets). If it doesn't take, Alt+Enter "
			            "out and back once." );
			ImGui::SeparatorText( "Overlay" );
			ImGui::Checkbox( "Cheat state text (top-right)", &g_set.status_text );
			ImGui::Checkbox( "Active-cheat watermark (top-left)", &g_set.show_active_list );
			ImGui::Spacing();
			ImGui::TextWrapped( "For 1440p @ your monitor's refresh (e.g. 260Hz): set GTA to "
			                    "WINDOWED at 2560x1440 in its own Display menu, then enable both "
			                    "boxes above. Windowed mode presents at the desktop's refresh, so "
			                    "you get the full rate -- GTA's fullscreen mode list is capped at 60Hz." );
			ImGui::EndTabItem();
		}
		// ---------------- Translator ----------------
		if ( ImGui::BeginTabItem( "Translator" ) ) {
			auto &s = i18n::config::get();

			// Auto-populate the model dropdown the first time the tab is opened,
			// so the user doesn't have to click Refresh to see installed models.
			static bool auto_fetched = false;
			if ( !auto_fetched ) { auto_fetched = true; fetch_models_async(); }

			ImGui::TextWrapped( "Translate incoming server chat into your language. Runs against a local "
			                    "Ollama (default) or any OpenAI-compatible endpoint. Everything here saves to "
			                    "the main config and applies when you hit Save config -- no restart." );
			ImGui::Checkbox( "Enable inbound chat translation", &s.inbound_chat );
			if ( ImGui::Checkbox( "Translate dialogs + 3D labels", &s.translate_dialogs ) )
				i18n::hooks::apply_config(); // live: (re)start the engine if needed
			ImGui::SameLine();
			HelpMarker( "Translates incoming dialog boxes and 3D world text labels. Translation-memory-"
			            "first: the FIRST time a given dialog/label appears it shows in the original while "
			            "it's translated in the background; the NEXT time it appears (login, menus, repeated "
			            "labels) it shows translated. One-off text may show once in the original." );
			if ( ImGui::Checkbox( "Translate HUD (textdraws, gametext, chat bubbles)", &s.translate_hud ) )
				i18n::hooks::apply_config();
			ImGui::SameLine();
			HelpMarker( "Translates HUD textdraws, big center gametext, and chat bubbles. Dynamic/numeric "
			            "textdraws (timers, counters, stats) are auto-skipped so they don't spam the "
			            "translator. NOTE: textdraws/gametext use GTA's glyph font -- best for English/"
			            "romanized targets; Cyrillic/Arabic may not render there (dialogs/labels/chat are fine)." );
			ImGui::Separator();

			// Endpoint + model text buffers (declared together so the Provider
			// preset dropdown below can overwrite both when a provider is picked).
			static char endpoint_buf[256] = "";
			static char model_buf[256]    = "";
			static bool bufs_init = false;
			if ( !bufs_init ) {
				bufs_init = true;
				strncpy_s( endpoint_buf, sizeof endpoint_buf, s.base_url.c_str(), _TRUNCATE );
				strncpy_s( model_buf,    sizeof model_buf,    s.model.c_str(),    _TRUNCATE );
			}

			// Provider preset: pick a known API to auto-fill the URL + a default
			// model and apply that provider's request quirks (see provider.cpp),
			// or Custom to type any OpenAI-compatible URL yourself.
			{
				const auto &presets = i18n::config::provider_presets();
				int cur = 0;
				for ( size_t i = 0; i < presets.size(); ++i )
					if ( presets[i].kind == s.provider_kind ) { cur = (int) i; break; }
				if ( ImGui::BeginCombo( "Provider", presets[cur].name ) ) {
					for ( size_t i = 0; i < presets.size(); ++i ) {
						bool sel = ( presets[i].kind == s.provider_kind );
						if ( ImGui::Selectable( presets[i].name, sel ) ) {
							s.provider_kind = presets[i].kind;
							// A concrete provider overwrites the URL + default model
							// (Custom has empty url/model -> leaves them untouched),
							// then refresh the model list against the new endpoint.
							if ( presets[i].url && presets[i].url[0] ) {
								s.base_url = presets[i].url;
								strncpy_s( endpoint_buf, sizeof endpoint_buf, presets[i].url, _TRUNCATE );
							}
							if ( presets[i].model && presets[i].model[0] ) {
								s.model = presets[i].model;
								strncpy_s( model_buf, sizeof model_buf, presets[i].model, _TRUNCATE );
							}
							fetch_models_async();
						}
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				HelpMarker( "Your API provider. Google / Anthropic / OpenAI fill in the URL and a "
				            "default model and apply that provider's required request tweaks "
				            "automatically (paste your API key below). Ollama / Local is the offline "
				            "default. Custom lets you type any OpenAI-compatible base URL." );
			}

			// Endpoint (auto-filled by the preset above; edit freely for Custom)
			ImGui::InputText( "Endpoint (base URL, ends in /v1)", endpoint_buf, sizeof endpoint_buf );
			s.base_url = endpoint_buf;

			// API key (cloud endpoints; blank for local Ollama)
			static char api_key_buf[256] = "";
			static bool api_key_init = false;
			if ( !api_key_init ) { api_key_init = true; strncpy_s( api_key_buf, sizeof api_key_buf, s.api_key.c_str(), _TRUNCATE ); }
			ImGui::InputText( "API key (blank for local Ollama)", api_key_buf, sizeof api_key_buf, ImGuiInputTextFlags_Password );
			s.api_key = api_key_buf;

			// Model: a live dropdown of what the endpoint ACTUALLY serves (so you
			// can't pick one that isn't installed) + a manual-override text field.

			if ( ImGui::Button( "Refresh models" ) ) fetch_models_async();
			ImGui::SameLine();
			int mst = g_modelState.load();
			if ( mst == 1 )      ImGui::TextDisabled( "fetching..." );
			else if ( mst == 3 ) { std::lock_guard<std::mutex> lk( g_modelMtx ); ImGui::TextDisabled( "fetch failed (%s)", g_modelErr.c_str() ); }
			else if ( mst == 2 ) { std::lock_guard<std::mutex> lk( g_modelMtx ); ImGui::TextDisabled( "%d installed", (int) g_models.size() ); }

			{
				std::lock_guard<std::mutex> lk( g_modelMtx );
				const char* preview = s.model.empty() ? "(pick a model)" : s.model.c_str();
				if ( ImGui::BeginCombo( "Installed models", preview ) ) {
					for ( const auto &m : g_models ) {
						bool sel = ( m == s.model );
						if ( ImGui::Selectable( m.c_str(), sel ) ) {
							s.model = m;
							strncpy_s( model_buf, sizeof model_buf, m.c_str(), _TRUNCATE );
						}
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					if ( g_models.empty() )
						ImGui::TextDisabled( "none found -- click Refresh, or type below" );
					ImGui::EndCombo();
				}
			}
			ImGui::InputText( "Model (or type manually)", model_buf, sizeof model_buf );
			s.model = model_buf;

			ImGui::TextDisabled( "Local (Ollama): Hy-MT2-7B is great for native-script languages (Spanish/Russian/etc.)." );
			if ( ImGui::SmallButton( "copy 7B pull" ) )
				ImGui::SetClipboardText( "ollama pull hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M" );
			ImGui::TextDisabled( "  For ROMANIZED output (Roman Urdu/Arabic/Hindi), pick a cloud Provider above and" );
			ImGui::TextDisabled( "  paste your key -- local models can't romanize well. Google has a free tier;" );
			ImGui::TextDisabled( "  Anthropic (Claude Haiku) and OpenAI (gpt-4o-mini) are cheap and excellent." );

			ImGui::Separator();

			// Source language dropdown: "Auto (detect)" (default) + the same list.
			// Auto = the model detects the incoming language per line (best for
			// mixed-language servers); a specific pick just hints the model.
			{
				const auto &langs = i18n::config::language_options();
				const char* preview = "Auto (detect)";
				if ( s.source_lang != 0 )
					for ( const auto &l : langs ) if ( l.code == s.source_lang ) { preview = l.name; break; }
				if ( ImGui::BeginCombo( "Source language", preview ) ) {
					if ( ImGui::Selectable( "Auto (detect)", s.source_lang == 0 ) ) s.source_lang = 0;
					if ( s.source_lang == 0 ) ImGui::SetItemDefaultFocus();
					for ( const auto &l : langs ) {
						bool sel = ( l.code == s.source_lang );
						if ( ImGui::Selectable( l.name, sel ) ) s.source_lang = l.code;
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				HelpMarker( "Language of INCOMING server text. Leave on Auto -- the model detects it per "
				            "line, which handles servers that mix English with a local language. Pick a "
				            "specific language only to hint the model on a reliably single-language server." );
			}

			// Target language dropdown (names, not raw LCID numbers).
			{
				const auto &langs = i18n::config::language_options();
				int cur = -1;
				for ( size_t i = 0; i < langs.size(); ++i )
					if ( langs[i].code == s.target_lang ) { cur = (int) i; break; }
				const char* preview = ( cur >= 0 ) ? langs[cur].name : "(custom)";
				if ( ImGui::BeginCombo( "Target language", preview ) ) {
					for ( size_t i = 0; i < langs.size(); ++i ) {
						bool sel = ( (int) i == cur );
						if ( ImGui::Selectable( langs[i].name, sel ) ) s.target_lang = langs[i].code;
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}

			ImGui::Checkbox( "Romanized output (Latin script)", &s.roman_output );
			ImGui::SameLine();
			HelpMarker( "Write the target language phonetically in the Roman/Latin alphabet "
			            "(Roman Urdu, Hinglish, Arabizi...). REQUIRED for Urdu/Arabic/Hindi and other "
			            "scripts SA:MP's chat font can't draw; harmless for Latin languages." );

			ImGui::SliderInt( "Source codepage (0 = auto)", &s.source_cp, 0, 1258 );
			ImGui::SliderInt( "Temperature (x100)", &s.translate_temp, 0, 100 );
			ImGui::SameLine();
			HelpMarker( "Sampling temperature x100 (30 = 0.30). 0.30 works well for both Hy-MT2 and general "
			            "models. Higher = more creative/varied; too low (0) can make Hy-MT2 loop. Applies on Save." );

			ImGui::SliderInt( "Min request gap (ms, 0=off)", &s.request_min_gap_ms, 0, 5000 );
			ImGui::SameLine();
			HelpMarker( "Minimum time between translation requests, paced across all workers. Leave 0 for "
			            "local or paid cloud. For a FREE-tier cloud key (e.g. Gemini free) set ~1000-4000ms to "
			            "stay under its requests-per-minute limit and avoid 429/503 throttling. (Transient "
			            "429/503 are also auto-retried with backoff regardless.) Applies on Save." );

			const char* marker_styles[] = { "Glyph + tint", "Tint only", "Prefix tag" };
			ImGui::Combo( "Marker style", &s.marker_style, marker_styles, 3 );

			// --- Outgoing chat translation (my messages -> server language) ---
			ImGui::SeparatorText( "Outgoing chat (translate MY messages)" );
			if ( ImGui::Checkbox( "Translate my chat before sending", &s.outbound_enabled ) )
				i18n::hooks::apply_config(); // apply live
			ImGui::SameLine();
			HelpMarker( "Your typed chat is translated into the server's language before it is sent. "
			            "Toggle in-game with Ctrl+Shift. Commands (/...) are never translated. "
			            "If translation fails, your original message is sent unchanged." );
			{
				const auto &langs = i18n::config::language_options();
				int cur = -1;
				for ( size_t i = 0; i < langs.size(); ++i )
					if ( langs[i].code == s.outbound_target_lang ) { cur = (int) i; break; }
				const char* preview = ( cur >= 0 ) ? langs[cur].name : "(custom)";
				if ( ImGui::BeginCombo( "Server language (translate into)", preview ) ) {
					for ( size_t i = 0; i < langs.size(); ++i ) {
						bool sel = ( (int) i == cur );
						if ( ImGui::Selectable( langs[i].name, sel ) ) s.outbound_target_lang = langs[i].code;
						if ( sel ) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}

			ImGui::Separator();
			ImGui::Text( "Status: %s", i18n::engine::status() );
			{
				std::string le = i18n::engine::last_error();
				if ( !le.empty() ) { ImGui::SameLine(); ImGui::TextDisabled( "(%s)", le.c_str() ); }
			}
			{
				auto rs = i18n::engine::response_stats();
				if ( rs.count > 0 )
					ImGui::Text( "Avg response: %lu ms  (last %lu ms, %llu calls)",
					             rs.avgMs, rs.lastMs, (unsigned long long) rs.count );
				else
					ImGui::TextDisabled( "Avg response: (no translations yet)" );
				ImGui::SameLine();
				HelpMarker( "Average round-trip delay of translation requests since the last Save, and the "
				            "most recent one. This is the REAL end-to-end delay -- it includes the Min request "
				            "gap wait and any 429/503 retry backoff, not just the model's inference time. "
				            "Cache hits (repeated lines) are instant and not counted." );
			}

			// Self-test: translate a sample line right now so you can verify the
			// endpoint/model/prompt without joining a server.
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

			ImGui::SeparatorText( "Debug (helps build dialogs/labels/textdraws)" );
			ImGui::Checkbox( "Log dialog/label/textdraw/gametext packets", &s.debug_capture );
			ImGui::SameLine();
			HelpMarker( "Read-only. Enable, join a server, trigger a dialog/3D label/textdraw/gametext, "
			            "then share fuego_reversed.log. Never modifies packets. (Saved with the config.)" );
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Separator();
	if ( ImGui::Button( "Save config" ) ) {
		cfg::save();                 // unified: cheats + /compat + translator -> fuego_reversed.cfg
		i18n::hooks::apply_config(); // apply translator changes live (start/stop/reconfigure the engine)
	}
	ImGui::SameLine();
	ImGui::TextDisabled( "one file (fuego_reversed.cfg), loads on startup.  F2 to toggle." );
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

	// Ctrl+Shift toggles outgoing chat translation (edge-detected + 400ms cooldown
	// so a held combo, or an incidental Ctrl+Shift+key, fires the toggle at most
	// once; toggle() prints a chat notice so it's never silently flipped).
	{
		static bool  prevCombo  = false;
		static DWORD lastToggle = 0;
		bool  combo = ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) && ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 );
		DWORD now   = GetTickCount();
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

	// Apply a pending fullscreen-refresh change here (message-pump thread, between
	// frames -- the safe context for GTA's device reset). Clear first so a message
	// during the reset can't re-enter it.
	if ( g_reapplyVideo ) { g_reapplyVideo = false; reapply_video_mode_now(); }

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
	Log( "[menu] registered (INSERT to toggle)." );
}

} // namespace menu
