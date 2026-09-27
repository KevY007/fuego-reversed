#include "render.h"
#include "../core/logger.h"

#include <d3d9.h>
#include <atomic>
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
std::atomic<bool> g_refreshPending{ false };  // re-apply the refresh setting (see apply_refresh)
UINT              g_injectedHz       = 0;     // the refresh hkReset last put into RW's params (0 = none)

// Posted to the game window by set_max_fullscreen_refresh (from any thread), and a
// timer that retries while the game is still loading. Both are QUEUED messages: they
// only ever arrive through the game's message loop (DispatchMessage, between frames)
// -- never nested inside a frame -- the one safe place for a device reset.
constexpr UINT     kMsgApplyRefresh   = WM_APP + 0x4F7;
constexpr UINT_PTR kTimerApplyRefresh = 0x4F7;

// GTA 1.0 US / RenderWare D3D9 internals (verified by disassembling gta_sa.exe):
//   0xC9C040  RW's stored D3DPRESENT_PARAMETERS -- the struct RwD3D9ChangeVideoMode
//             passes to IDirect3DDevice9::Reset (Windowed @ +0x20, refresh @ +0x30).
//   0xC97C18  RW's current video-mode index. RwD3D9ChangeVideoMode (0x7F8640) returns
//             at once when asked for the mode already current ("cmp [0xC97C18],mode /
//             je return"), so re-applying the SAME mode never resets the device; on
//             success it stores the new index back (0x7F88B2).
//   0x745C70  SetVideoMode(int) (__cdecl): GTA's own wrapper -> RwD3D9ChangeVideoMode,
//             then fixes up the post-FX back-buffer vertices and RsGlobal max size.
//   0xC8D4C0  gGameState; 9 = in game (GAME_STATE_IDLE).
D3DPRESENT_PARAMETERS *rw_present_params() { return reinterpret_cast<D3DPRESENT_PARAMETERS *>( 0xC9C040 ); }
int &rw_current_mode() { return *reinterpret_cast<int *>( 0xC97C18 ); }
bool in_game() { return *reinterpret_cast<const int *>( 0xC8D4C0 ) == 9; }

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

// Does the live device need a reset to match the refresh setting? Exclusive fullscreen
// only (windowed presents at the desktop's rate anyway). On: when it isn't at the best
// rate yet. Off: only when it still runs at a rate WE injected.
bool refresh_reset_needed() {
	const D3DPRESENT_PARAMETERS *pp = rw_present_params();
	if ( !g_device || pp->Windowed ) return false;
	if ( g_maxFullscreenHz ) {
		const UINT best = best_fullscreen_refresh( g_device, pp );
		return best > 0 && best != pp->FullScreen_RefreshRateInHz;
	}
	return g_injectedHz != 0 && pp->FullScreen_RefreshRateInHz == g_injectedHz;
}

// Re-apply the CURRENT video mode through GTA's own SetVideoMode: RenderWare releases
// its video memory, calls Reset with its present params -- hkReset injects the max
// refresh (or, when off, no longer does) -- and restores everything, exactly like a
// resolution change in GTA's Display menu. Its current-mode index is invalidated first
// so the request isn't skipped as "already current"; if RW fails before storing the
// index back, the old one is restored.
void force_video_reset() {
	int &cur = rw_current_mode();
	const int mode = cur;
	if ( mode < 0 ) return;
	cur = -1;
	reinterpret_cast<void( __cdecl * )( int )>( 0x745C70 )( mode );
	if ( cur == -1 ) {
		cur = mode;
		Log( "[render] refresh re-apply: the video-mode reset failed; display left as it was." );
	}
}

// Apply a pending refresh change -- on the game's window thread, from a queued message,
// i.e. between frames (the same context the Alt+Enter reset runs in). Not while the
// game is still loading the map: a timer tries again every 500 ms until it runs.
void apply_refresh( HWND hwnd ) {
	if ( !in_game() ) {
		SetTimer( hwnd, kTimerApplyRefresh, 500, nullptr ); // same id -> replaces, never stacks
		return;
	}
	KillTimer( hwnd, kTimerApplyRefresh );
	if ( !g_refreshPending.exchange( false ) ) return; // cleared first: a message sent during the reset can't start another
	if ( refresh_reset_needed() ) force_video_reset();
}

LRESULT CALLBACK hkWndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	if ( msg == kMsgApplyRefresh || ( msg == WM_TIMER && wp == kTimerApplyRefresh ) ) {
		apply_refresh( hwnd );
		return 0; // ours: never passed on to the game / SA:MP
	}

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
	// Re-entrancy guard (render thread only, so a plain static is enough). EndScene is
	// hooked via the device VTABLE, so anything that calls dev->EndScene() while we're
	// inside this hook -- a callback, or an extra scene pass -- lands back HERE. Pass it
	// straight through instead of re-running every callback: an earlier post-chat pass
	// did exactly that and recursed without bound, freezing the game on the first frame.
	static bool s_inside = false;
	if ( s_inside ) return g_origEndScene( dev );
	s_inside = true;

	if ( !g_device ) { g_device = dev; Log( "[render] device captured (0x%p).", dev ); }
	ensure_window( dev );
	for ( auto fn : frameCbs() ) fn( dev );
	HRESULT hr = g_origEndScene( dev );

	s_inside = false;
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
			g_injectedHz = hz; // turning the setting off later has something to undo
		}
	} else if ( pp && pp->Windowed == FALSE ) {
		g_injectedHz = 0; // setting off: GTA's own rate from here on
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
	g_refreshPending  = true;
	if ( g_window ) PostMessage( g_window, kMsgApplyRefresh, 0, 0 ); // -> apply_refresh, between frames
	Log( "[render] max fullscreen refresh %s (applying now if the display needs it).", on ? "ON" : "OFF" );
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
