#pragma once
/*
	cheats/status.h  --  transient cheat-status text (Fuego's cheat_state_text).
	A cheat calls status::set(...) while it's active; draw() shows the latest message
	top-right as "<text> <-" for 3s (Fuego's look). Hidden unless fx::g_set.status_text
	is on -- Fuego's shipped config (d3dtext_hud = false) never shows it either.
*/

namespace status {
void set( const char *fmt, ... );
void draw(); // called from visuals::draw() (inside the ImGui frame)
}
