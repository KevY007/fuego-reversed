#define _CRT_SECURE_NO_WARNINGS 1
#include "status.h"
#include "imgui.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace {
char  g_text[256] = { 0 };
DWORD g_time      = 0;
constexpr DWORD kShowMs = 1400; // how long a message lingers after last set
}

namespace status {

void set( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	_vsnprintf( g_text, sizeof( g_text ) - 1, fmt, ap );
	va_end( ap );
	g_time = GetTickCount();
}

void draw() {
	if ( !g_time )
		return;
	DWORD age = GetTickCount() - g_time;
	if ( age > kShowMs )
		return;
	float alpha = 1.0f - (float) age / (float) kShowMs;
	if ( alpha < 0 ) alpha = 0;

	ImGuiIO &io = ImGui::GetIO();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImVec2 sz = ImGui::CalcTextSize( g_text );
	float x = ( io.DisplaySize.x - sz.x ) * 0.5f;
	float y = io.DisplaySize.y * 0.72f;
	int a = (int) ( alpha * 255 );
	dl->AddText( ImVec2( x + 1, y + 1 ), IM_COL32( 0, 0, 0, ( a * 220 ) / 255 ), g_text );
	dl->AddText( ImVec2( x, y ), IM_COL32( 255, 120, 40, a ), g_text );
}

} // namespace status
