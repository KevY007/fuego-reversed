#include "status.h"
#include "visuals.h"
#include "../core/settings.h"
#include "imgui.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>

/*
	Fuego cheat_state_text (proxyIDirect3DDevice9.cpp:3937-3949): the latest message,
	top-right as "<text> <-", white, for 3s -- fading out over the last second. Fuego
	only draws it with d3dtext_hud on, which its shipped fuego.ini has OFF, so this is
	gated on fx::g_set.status_text (default off).
*/

namespace {
char  g_text[256] = { 0 };
DWORD g_time      = 0;
constexpr DWORD kShowMs = 3000;
constexpr DWORD kFadeMs = 1000;
}

namespace status {

void set( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	_vsnprintf( g_text, sizeof( g_text ) - 1, fmt, ap );
	va_end( ap );
	g_text[sizeof( g_text ) - 1] = '\0';
	g_time = GetTickCount();
}

void draw() {
	if ( !fx::g_set.status_text || !g_time )
		return;
	const DWORD age = GetTickCount() - g_time;
	if ( age >= kShowMs )
		return;
	uint32_t alpha = 255;
	if ( age > kShowMs - kFadeMs )
		alpha -= ( age - ( kShowMs - kFadeMs ) ) * 255 / kFadeMs;

	char buf[300];
	_snprintf( buf, sizeof( buf ) - 1, "%s <-", g_text );
	buf[sizeof( buf ) - 1] = '\0';
	const float x = ImGui::GetIO().DisplaySize.x - visuals::draw_length( visuals::Font::Main, buf ) - 3.0f;
	visuals::print_shadow( visuals::Font::Main, x, 1.0f, ( alpha << 24 ) | 0x00FFFFFF, buf,
	                       ImGui::GetForegroundDrawList() ); // HUD: over SA:MP's chat
}

} // namespace status
