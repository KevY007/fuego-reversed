#include "input.h"

#include "../render/render.h"
#include "../samp/samp_sdk.h"   // stInputInfo::iInputEnabled (chat input box)
#include "../core/memory.h"

#include <windows.h>

/*
	Fuego's keyhook (keyhook.cpp) records key transitions from its WndProc and
	evaluates KEY_DOWN / KEY_PRESSED once per frame. Same here: the WndProc keeps the
	live `g_down` table (with Fuego's gating), and the per-frame tick snapshots it so a
	whole frame sees one consistent state and pressed/released are frame edges.
*/

namespace {

bool g_down[256] = {};  // live, fed by the WndProc
bool g_cur[256]  = {};  // this frame's snapshot
bool g_prev[256] = {};  // previous frame's snapshot

HWND g_wnd = nullptr;

void set_key( int vk, bool isDown, HWND wnd ) {
	if ( vk <= 0 || vk > 255 ) return;
	if ( isDown ) {
		// Fuego process_key: while typing in SA:MP chat or unfocused, a key-down is not
		// recorded (a key-up still is, so nothing gets stuck down).
		if ( input::typing() || GetForegroundWindow() != wnd ) return;
		g_down[vk] = true;
	} else {
		g_down[vk] = false;
	}
}

// Fuego splits the generic modifier codes by scancode (VK_LSHIFT vs VK_RSHIFT, ...).
int split_modifier( int vk, LPARAM lp ) {
	const UINT sc = ( (UINT) lp >> 16 ) & 0xFF;
	switch ( vk ) {
		case VK_SHIFT:   return sc == MapVirtualKeyA( VK_LSHIFT, 0 ) ? VK_LSHIFT : VK_RSHIFT;
		case VK_CONTROL: return ( lp & ( 1 << 24 ) ) ? VK_RCONTROL : VK_LCONTROL; // extended bit = right
		case VK_MENU:    return ( lp & ( 1 << 24 ) ) ? VK_RMENU    : VK_LMENU;
		default:         return vk;
	}
}

bool on_wndproc( HWND wnd, UINT msg, WPARAM wp, LPARAM lp ) {
	g_wnd = wnd;
	if ( msg == WM_KILLFOCUS ) { // Fuego keyhook_clear_states
		for ( bool &k : g_down ) k = false;
		return false;
	}
	if ( input::gta_menu_active() ) // Fuego: the WndProc skips everything in the pause menu
		return false;
	switch ( msg ) {
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: set_key( VK_LBUTTON, msg == WM_LBUTTONDOWN, wnd ); break;
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: set_key( VK_RBUTTON, msg == WM_RBUTTONDOWN, wnd ); break;
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: set_key( VK_MBUTTON, msg == WM_MBUTTONDOWN, wnd ); break;
		case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
			const bool isDown = ( msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN );
			const int  vk     = (int) wp;
			const int  split  = split_modifier( vk, lp );
			set_key( split, isDown, wnd );
			if ( split != vk ) set_key( vk, isDown, wnd ); // keep the generic code in sync too
			break;
		}
		default: break;
	}
	return false; // observe only; never consume
}

void on_frame( IDirect3DDevice9 * ) {
	for ( int i = 0; i < 256; i++ ) {
		g_prev[i] = g_cur[i];
		g_cur[i]  = g_down[i];
	}
}

} // namespace

namespace input {

void install() {
	render::on_wndproc( on_wndproc );
	render::on_frame( on_frame );
}

bool down( int vk )     { return vk > 0 && vk < 256 && g_cur[vk]; }
bool pressed( int vk )  { return vk > 0 && vk < 256 && g_cur[vk] && !g_prev[vk]; }
bool released( int vk ) { return vk > 0 && vk < 256 && !g_cur[vk] && g_prev[vk]; }

bool typing() {
	const samp::stInputInfo *ii = samp::input_info_st();
	return ii && ii->iInputEnabled;
}

bool gta_menu_active() { return mem::read<uint8_t>( 0xBA67A4 ) != 0; } // FrontEndMenuManager.m_bMenuActive

} // namespace input
