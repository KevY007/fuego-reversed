#include "menu.h"

#include "../render/render.h"
#include "../core/settings.h"
#include "../core/logger.h"
#include "../compat/compat.h"
#include "../samp/samp.h"
#include "../cheats/visuals.h"
#include "../cheats/veh.h"
#include "../core/config.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <d3d9.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace {

bool g_init = false;
bool g_open = false;

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
	ImGui::SetNextWindowSize( ImVec2( 620, 640 ), ImGuiCond_FirstUseEver );
	ImGui::Begin( "Fuego Reversed", &g_open, ImGuiWindowFlags_NoCollapse );

	if ( ImGui::BeginTabBar( "tabs" ) ) {
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
			ImGui::SeparatorText( "Actions (click a vehicle to pick it)" );
			if ( ImGui::Button( "Unlock Vehicle" ) )        { veh::start_pick( 1 ); g_open = false; }
			if ( ImGui::Button( "Damage Vehicle Doors" ) )  { veh::start_pick( 2 ); g_open = false; }
			if ( ImGui::Button( "Destroy Vehicle" ) )       { veh::start_pick( 3 ); g_open = false; }
			if ( ImGui::Button( "Attach/Tow Vehicle" ) )    veh::attach_trailer();
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
			ImGui::Spacing();
			ImGui::TextWrapped( "For 1440p @ your monitor's refresh (e.g. 260Hz): set GTA to "
			                    "WINDOWED at 2560x1440 in its own Display menu, then enable both "
			                    "boxes above. Windowed mode presents at the desktop's refresh, so "
			                    "you get the full rate -- GTA's fullscreen mode list is capped at 60Hz." );
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Separator();
	if ( ImGui::Button( "Save config" ) ) cfg::save();
	ImGui::SameLine();
	if ( ImGui::Button( "Load config" ) ) cfg::load();
	ImGui::SameLine();
	ImGui::TextDisabled( "F2 to toggle" );
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
		ImFont *f = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf", 18.0f );
		if ( !f ) f = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\tahoma.ttf", 17.0f );
		if ( !f ) { io.Fonts->AddFontDefault(); io.FontGlobalScale = 1.35f; }
		style();
		ImGui_ImplWin32_Init( render::window() );
		ImGui_ImplDX9_Init( dev );
		g_init = true;
		Log( "[menu] ImGui initialised." );
	}

	// Show SA:MP's cursor while the menu is open OR a vehicle pick is in progress (frees
	// the OS cursor for ImGui, suspends the game camera); hide it otherwise. Edge-triggered.
	bool wantCursor = g_open || veh::is_picking();
	static bool prevCursor = false;
	if ( wantCursor != prevCursor ) {
		samp::toggle_cursor( wantCursor );
		prevCursor = wantCursor;
	}

	display_tick(); // uncap FPS + borderless window

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
	if ( g_open )
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

void install() {
	render::on_frame( on_frame );
	render::on_reset( on_reset );
	render::on_wndproc( on_wndproc );
	Log( "[menu] registered (INSERT to toggle)." );
}

} // namespace menu
