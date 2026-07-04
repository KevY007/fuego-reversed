#pragma once
/*
	cheats/status.h  --  transient on-screen cheat-status text (Fuego's
	cheat_state_text). A cheat calls status::set(...) while it's active; draw()
	renders the latest message (fading) so you can see what's on.
*/

namespace status {
void set( const char *fmt, ... );
void draw(); // called from visuals::draw() (inside the ImGui frame)
}
