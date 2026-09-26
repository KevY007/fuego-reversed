#pragma once
/*
	core/input.h  --  hotkey state, 1:1 with Project Fuego's keyhook (KEY_DOWN / KEY_PRESSED).

	Fed from the game's WndProc (not GetAsyncKeyState), with Fuego's gating:
	  * key-downs are ignored while SA:MP's chat input box is open (typing "i" or
	    Backspace in chat must not fire iTroll / the fake-lag backtrack), or while the
	    game window isn't in the foreground;
	  * nothing is recorded while GTA's pause/front-end menu is up;
	  * VK_SHIFT / VK_CONTROL / VK_MENU are split into their left/right codes by scancode,
	    so VK_LCONTROL, VK_LSHIFT, ... work like in Fuego.
	Edges (pressed/released) are per frame: input::install() registers the per-frame
	tick first, so every later frame callback sees the same snapshot.
*/

namespace input {

// Register the WndProc feed + per-frame tick. Call right after render::install(),
// BEFORE any other render::on_frame user (menu, cheats) so their reads are consistent.
void install();

bool down( int vk );     // held now                 (Fuego KEY_DOWN)
bool pressed( int vk );  // went down this frame     (Fuego KEY_PRESSED)
bool released( int vk ); // went up this frame       (Fuego KEY_RELEASED)

bool typing();           // SA:MP chat input box is open (g_Input->iInputEnabled)
bool gta_menu_active();  // GTA pause / front-end menu is up (0xBA67A4)

} // namespace input
