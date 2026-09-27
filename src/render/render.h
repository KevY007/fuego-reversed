#pragma once
/*
	render/render.h  --  D3D9 overlay hook.

	Hooks the game's IDirect3DDevice9 (EndScene/Reset) via the shared vtable
	(obtained from a throwaway dummy device -- version independent). Gives higher
	layers a per-frame, MAIN-THREAD callback (for the menu and for cheat
	processing) plus a WndProc hook for input. No external DLL.
*/

#include <windows.h>

struct IDirect3DDevice9;
struct _D3DPRESENT_PARAMETERS_;

namespace render {

using FrameFn   = void ( * )( IDirect3DDevice9 *dev );                 // every frame (main thread)
using ResetFn   = void ( * )( IDirect3DDevice9 *dev, bool preReset );  // around device Reset
using WndProcFn = bool ( * )( HWND, UINT, WPARAM, LPARAM );            // return true to CONSUME the message

void on_frame( FrameFn );
void on_reset( ResetFn );
void on_wndproc( WndProcFn );

// Install the EndScene/Reset hook. Returns true on success.
bool install();
bool installed();

IDirect3DDevice9 *device(); // captured on the first frame; null before that
HWND              window(); // game window; null before the first frame

	// Overlay visibility. F8 is SA:MP's screenshot key: pressing it hides our overlays
	// (menu, ESP, watermark, chams) for a short window so the screenshot comes out clean,
	// then they return automatically. Cheat logic keeps running -- this affects drawing only.
	void hide_overlay_for( unsigned ms );
	bool overlay_hidden();

// When on, the Reset hook rewrites D3DPRESENT_PARAMETERS.FullScreen_RefreshRateInHz
// to the highest rate the monitor supports at the current resolution (exclusive
// fullscreen only). Applied at once -- when toggled either way, and when the saved
// setting is restored at startup: if the game is in exclusive fullscreen and its
// refresh differs from what the setting wants, GTA's own video-mode apply is re-run
// (one screen flicker, like a resolution change) on the window thread once the game
// is running. Safe: it only edits the present params GTA passes to its OWN Reset, so
// RenderWare recreates its resources normally.
void set_max_fullscreen_refresh( bool on );

} // namespace render
