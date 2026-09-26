#include "render.h"
#include "../core/logger.h"

#include <d3d9.h>
#include <vector>

namespace {
using namespace render;

std::vector<FrameFn>   &frameCbs() { static std::vector<FrameFn> v; return v; }
std::vector<ResetFn>   &resetCbs() { static std::vector<ResetFn> v; return v; }
std::vector<WndProcFn> &wndCbs()   { static std::vector<WndProcFn> v; return v; }

typedef HRESULT( __stdcall *EndScene_t )( IDirect3DDevice9 * );
typedef HRESULT( __stdcall *Reset_t )( IDirect3DDevice9 *, D3DPRESENT_PARAMETERS * );

EndScene_t g_origEndScene = nullptr;
Reset_t    g_origReset    = nullptr;
void     **g_vtable       = nullptr;
bool       g_installed    = false;

IDirect3DDevice9 *g_device = nullptr;
HWND              g_window = nullptr;
WNDPROC           g_origWndProc = nullptr;

DWORD             g_overlayHideUntil = 0; // GetTickCount() deadline for the F8 screenshot hide
bool              g_maxFullscreenHz  = false; // force highest refresh in exclusive fullscreen

// Highest refresh the adapter supports at the reset's target resolution, for the
// present params' back-buffer format (falls back to the desktop rate, then 0).
UINT best_fullscreen_refresh( IDirect3DDevice9 *dev, const D3DPRESENT_PARAMETERS *pp ) {
	IDirect3D9 *d3d = nullptr;
	if ( FAILED( dev->GetDirect3D( &d3d ) ) || !d3d ) return 0;
	D3DDISPLAYMODE desktop = {};
	d3d->GetAdapterDisplayMode( D3DADAPTER_DEFAULT, &desktop );
	D3DFORMAT fmt = pp->BackBufferFormat;
	if ( fmt == D3DFMT_UNKNOWN ) fmt = desktop.Format;
	const UINT w = pp->BackBufferWidth, h = pp->BackBufferHeight;
	UINT best = 0;
	const UINT cnt = d3d->GetAdapterModeCount( D3DADAPTER_DEFAULT, fmt );
	for ( UINT i = 0; i < cnt; ++i ) {
		D3DDISPLAYMODE m = {};
		if ( SUCCEEDED( d3d->EnumAdapterModes( D3DADAPTER_DEFAULT, fmt, i, &m ) ) )
			if ( m.Width == w && m.Height == h && m.RefreshRate > best ) best = m.RefreshRate;
	}
	if ( best == 0 ) best = desktop.RefreshRate; // fallback: desktop refresh
	d3d->Release();
	return best;
}

LRESULT CALLBACK hkWndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	bool consumed = false;
	for ( auto fn : wndCbs() )
		if ( fn( hwnd, msg, wp, lp ) ) consumed = true;
	if ( consumed )
		return TRUE;
	return CallWindowProc( g_origWndProc, hwnd, msg, wp, lp );
}

void ensure_window( IDirect3DDevice9 *dev ) {
	if ( g_window ) return;
	HWND w = nullptr;
	D3DDEVICE_CREATION_PARAMETERS cp = {};
	if ( SUCCEEDED( dev->GetCreationParameters( &cp ) ) && cp.hFocusWindow )
		w = cp.hFocusWindow;
	if ( !w )
		w = FindWindowA( "Grand theft auto San Andreas", nullptr ); // GTA:SA window class
	if ( !w )
		return; // try again next frame
	g_window = w;
	g_origWndProc = (WNDPROC) SetWindowLongPtr( g_window, GWLP_WNDPROC, (LONG_PTR) hkWndProc );
	Log( "[render] window hooked (0x%p).", g_window );
}

HRESULT __stdcall hkEndScene( IDirect3DDevice9 *dev ) {
	if ( !g_device ) { g_device = dev; Log( "[render] device captured (0x%p).", dev ); }
	ensure_window( dev );

	// Let the game + SA:MP finish their scene FIRST -- including SA:MP's chat window,
	// which it draws inside its own EndScene (the trampoline below). THEN draw our
	// overlay in a fresh scene so it sits ON TOP of the chat: the menu, cheat-state
	// text and active-cheat watermark are no longer covered by the chat box. (Trade-
	// off: ESP/nametags also draw over chat, but they're world-anchored and rarely
	// overlap the chat corner.) Guarded: if BeginScene can't start (device lost /
	// scene still open), we simply skip our overlay this frame rather than risk a
	// bad draw -- never a black screen.
	HRESULT hr = g_origEndScene( dev );
	if ( SUCCEEDED( dev->BeginScene() ) ) {
		for ( auto fn : frameCbs() ) fn( dev );
		dev->EndScene();
	}
	return hr;
}

HRESULT __stdcall hkReset( IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp ) {
	// Force the highest refresh for exclusive fullscreen (GTA hardcodes lowest >=60).
	// We only edit the params GTA passes to its own Reset, so RenderWare's normal
	// device-reset resource recreation still runs.
	if ( g_maxFullscreenHz && pp && pp->Windowed == FALSE ) {
		UINT hz = best_fullscreen_refresh( dev, pp );
		if ( hz > 0 && hz != pp->FullScreen_RefreshRateInHz ) {
			Log( "[render] fullscreen refresh: %u -> %u Hz (%ux%u).",
			     pp->FullScreen_RefreshRateInHz, hz, pp->BackBufferWidth, pp->BackBufferHeight );
			pp->FullScreen_RefreshRateInHz = hz;
		}
	}
	for ( auto fn : resetCbs() ) fn( dev, true );   // pre-reset: release resources
	HRESULT hr = g_origReset( dev, pp );
	for ( auto fn : resetCbs() ) fn( dev, false );  // post-reset: recreate
	return hr;
}

void patch_vtable_entry( int index, void *fn, void **outOrig ) {
	DWORD old;
	VirtualProtect( &g_vtable[index], sizeof( void * ), PAGE_READWRITE, &old );
	if ( outOrig ) *outOrig = g_vtable[index];
	g_vtable[index] = fn;
	VirtualProtect( &g_vtable[index], sizeof( void * ), old, &old );
}

// GTA:SA's live D3D9 device: RenderWare's _RwD3DDevice at 0xC97C28 (confirmed in
// gta-reversed rwcore.h). Hooking the game's OWN device is reliable even in
// exclusive fullscreen -- unlike creating a throwaway device (which fails while
// the game holds the adapter, the cause of the earlier "FAILED to obtain vtable").
IDirect3DDevice9 *game_device() {
	return *reinterpret_cast<IDirect3DDevice9 **>( 0xC97C28 );
}

void **game_device_vtable() {
	IDirect3DDevice9 *dev = game_device();
	if ( !dev ) return nullptr;
	return *reinterpret_cast<void ***>( dev ); // the device's D3D9 vtable
}
} // namespace

namespace render {

void on_frame( FrameFn fn )     { if ( fn ) frameCbs().push_back( fn ); }
void on_reset( ResetFn fn )     { if ( fn ) resetCbs().push_back( fn ); }
void on_wndproc( WndProcFn fn ) { if ( fn ) wndCbs().push_back( fn ); }

bool installed() { return g_installed; }
IDirect3DDevice9 *device() { return g_device; }
HWND window() { return g_window; }

void hide_overlay_for( unsigned ms ) { g_overlayHideUntil = GetTickCount() + ms; }
// Wrap-safe (signed delta); the sentinel 0 never counts as "hidden".
bool overlay_hidden() { return g_overlayHideUntil && (int) ( g_overlayHideUntil - GetTickCount() ) > 0; }

void set_max_fullscreen_refresh( bool on ) {
	g_maxFullscreenHz = on;
	Log( "[render] max fullscreen refresh %s (applies on next device reset).", on ? "ON" : "OFF" );
}

bool install() {
	if ( g_installed ) return true;
	g_vtable = game_device_vtable();
	if ( !g_vtable ) { Log( "[render] GTA D3D9 device not ready yet (0xC97C28 null)." ); return false; }
	g_device = game_device();          // real device -> device() valid immediately
	ensure_window( g_device );         // subclass the game window now
	patch_vtable_entry( 42, (void *) hkEndScene, (void **) &g_origEndScene ); // IDirect3DDevice9::EndScene
	patch_vtable_entry( 16, (void *) hkReset,    (void **) &g_origReset );    // IDirect3DDevice9::Reset
	g_installed = true;
	Log( "[render] EndScene/Reset hook installed on game device (0x%p).", g_device );
	return true;
}

} // namespace render
