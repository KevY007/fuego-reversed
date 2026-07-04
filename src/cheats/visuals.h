#pragma once
/*
	cheats/visuals.h  --  on-screen visual cheats (ESP, ...).

	draw() is called by the menu's per-frame hook AFTER ImGui::NewFrame() and
	before Render(), so it can use ImGui draw lists. Reads fx::g_set.
*/

namespace visuals {
void draw();
}
