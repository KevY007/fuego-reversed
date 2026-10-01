#pragma once
/*
	cheats/visuals.h  --  on-screen visual cheats (nametags, ESP, finders, aim visuals ...).

	draw() is called by the menu's per-frame hook AFTER ImGui::NewFrame() and
	before Render(), so it can use ImGui draw lists. Reads fx::g_set.

	The text/primitive helpers below reproduce Project Fuego's CD3DFont / CD3DRender
	look (outlined + shadowed text, {RRGGBB} colour tags) so any overlay can match it.
	They draw into ImGui's background draw list, i.e. only inside the ImGui frame.
*/

#include <cstdint>

struct ImDrawList;

namespace visuals {

// Add the overlay fonts to the ImGui atlas. Called once by the menu at ImGui init,
// after the menu font (so the menu font stays the default).
void init_fonts();
void draw();

// Fuego's overlay fonts (the CD3DFont instances in proxyIDirect3DDevice9.cpp).
enum class Font : int {
	Main,       // pD3DFont           "Samsung Sans" 10  (cheat state text, finder labels, fake-lag text)
	SampStuff,  // pD3DFont_sampStuff "Samsung Sans" 8.5 (nametags, players' guns, "N Weapons")
	Fixed,      // pD3DFontFixed      "Samsung Sans" 9   ([LOCKED])
	FixedSmall, // pD3DFontFixedSmall "Small Fonts"  6.5 (nametag HP numbers, AFK)
	Chat,       // pD3DFontChat       "Samsung Sans" 11  (vehicle-picker labels)
};

// CD3DFont::PrintShadow: outlined text + a (+1,+1) shadow in the colour's alpha.
// argb is a D3DCOLOR; {RRGGBB} / {AARRGGBB} tags in the text switch colour like SA:MP.
// dl: where to draw -- default the world layer (under SA:MP's chat, like its nametags);
// pass ImGui::GetForegroundDrawList() for HUD text (on top).
void  print_shadow( Font f, float x, float y, uint32_t argb, const char *text, ImDrawList *dl = nullptr );
float draw_length( Font f, const char *text ); // CD3DFont::DrawLength (tags excluded)
float draw_height( Font f );                   // CD3DFont::DrawHeight

// Fuego CalcScreenCoors: world -> screen through GTA's view matrix. false when the point
// is behind the camera (z < 1). Unlike game::world_to_screen, nothing is far-clipped.
bool calc_screen( const float world[3], float &sx, float &sy );

} // namespace visuals
