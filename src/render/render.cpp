#include "render.h"
#include "../core/logger.h"
#include "../samp/samp.h"      // samp::base / detect
#include "../samp/addresses.h" // samp::addr::D3D_DEVICE (the real device SA:MP forwards to)

#include <d3d9.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace render;

std::vector<FrameFn>   &frameCbs()   { static std::vector<FrameFn> v; return v; }
std::vector<ResetFn>   &resetCbs()   { static std::vector<ResetFn> v; return v; }
std::vector<WndProcFn> &wndCbs()     { static std::vector<WndProcFn> v; return v; }
std::vector<PresentFn> &presentCbs() { static std::vector<PresentFn> v; return v; }

typedef HRESULT( __stdcall *EndScene_t )( IDirect3DDevice9 * );
typedef HRESULT( __stdcall *Reset_t )( IDirect3DDevice9 *, D3DPRESENT_PARAMETERS * );
typedef HRESULT( __stdcall *Present_t )( IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA * );

EndScene_t g_origEndScene = nullptr;
Reset_t    g_origReset    = nullptr;
Present_t  g_origPresent  = nullptr;
void     **g_vtable       = nullptr; // the device GTA calls: SA:MP's wrapper (see samp::addr::D3D_DEVICE)
void     **g_realVtable   = nullptr; // the real device's: its Present comes after SA:MP's UI
IDirect3DDevice9 *const *g_realDeviceSlot = nullptr; // where SA:MP keeps that device (samp.dll data)
bool       g_installed    = false;

IDirect3DDevice9 *g_device = nullptr;
HWND              g_window = nullptr;
WNDPROC           g_origWndProc = nullptr;

DWORD             g_overlayHideUntil = 0; // GetTickCount() deadline for the F8 screenshot hide
bool              g_maxFullscreenHz  = false; // force highest refresh in exclusive fullscreen
std::atomic<bool> g_refreshPending{ false };  // re-apply the refresh setting (see apply_refresh)
UINT              g_injectedHz       = 0;     // the refresh hkReset last put into RW's params (0 = none)
UINT              g_badHz = 0, g_badW = 0, g_badH = 0; // a rate this resolution refused: never injected again

bool refused_rate( UINT hz, const D3DPRESENT_PARAMETERS *pp ) {
	return hz != 0 && hz == g_badHz && pp->BackBufferWidth == g_badW && pp->BackBufferHeight == g_badH;
}

// Posted to the game window by set_max_fullscreen_refresh (from any thread), and a
// timer that retries while the game is still loading or not in front. Both are
// QUEUED messages: they only ever arrive through the game's message loop
// (DispatchMessage, between frames) -- never nested inside a frame -- the one safe
// place for a device reset. The message id is registered (unique per session, no
// clash with anything else on this window).
UINT               g_msgApplyRefresh  = 0;
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
//   0x8D621C  ForegroundApp (bool): the game window is the active app.
D3DPRESENT_PARAMETERS *rw_present_params() { return reinterpret_cast<D3DPRESENT_PARAMETERS *>( 0xC9C040 ); }
int &rw_current_mode() { return *reinterpret_cast<int *>( 0xC97C18 ); }
bool in_game() { return *reinterpret_cast<const int *>( 0xC8D4C0 ) == 9; }
bool foreground_app() { return *reinterpret_cast<const bool *>( 0x8D621C ); }

// Highest refresh the device's OWN adapter (GTA may run on a second monitor)
// supports at the reset's target resolution. 0 = no such mode: leave the rate alone
// -- injecting a rate the mode list doesn't have makes Reset fail (a black screen).
// Display modes have no alpha format, so an A8R8G8B8 back buffer is enumerated as
// X8R8G8B8. The desktop rate counts only when the desktop IS that resolution.
UINT best_fullscreen_refresh( IDirect3DDevice9 *dev, const D3DPRESENT_PARAMETERS *pp ) {
	IDirect3D9 *d3d = nullptr;
	if ( FAILED( dev->GetDirect3D( &d3d ) ) || !d3d ) return 0;
	D3DDEVICE_CREATION_PARAMETERS cp = {};
	const UINT adapter = SUCCEEDED( dev->GetCreationParameters( &cp ) ) ? cp.AdapterOrdinal : D3DADAPTER_DEFAULT;
	D3DDISPLAYMODE desktop = {};
	d3d->GetAdapterDisplayMode( adapter, &desktop );
	D3DFORMAT fmt = pp->BackBufferFormat;
	if ( fmt == D3DFMT_UNKNOWN ) fmt = desktop.Format;
	if ( fmt == D3DFMT_A8R8G8B8 ) fmt = D3DFMT_X8R8G8B8;
	if ( fmt == D3DFMT_A1R5G5B5 ) fmt = D3DFMT_X1R5G5B5;
	const UINT w = pp->BackBufferWidth, h = pp->BackBufferHeight;
	UINT best = 0;
	const UINT cnt = d3d->GetAdapterModeCount( adapter, fmt );
	for ( UINT i = 0; i < cnt; ++i ) {
		D3DDISPLAYMODE m = {};
		if ( SUCCEEDED( d3d->EnumAdapterModes( adapter, fmt, i, &m ) ) )
			if ( m.Width == w && m.Height == h && m.RefreshRate > best ) best = m.RefreshRate;
	}
	if ( best == 0 && desktop.Width == w && desktop.Height == h ) best = desktop.RefreshRate;
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
		return best > 0 && !refused_rate( best, pp ) && best != pp->FullScreen_RefreshRateInHz;
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
		Log( "[render] refresh re-apply: RenderWare kept/restored the previous video mode (the reset "
		     "didn't go through); an Alt+Enter or a restart applies the setting." );
	}
}

// Apply a pending refresh change -- on the game's window thread, from a queued message,
// i.e. between frames (the same context the Alt+Enter reset runs in). Not while the
// game is still loading the map, nor while it isn't the active window (alt-tabbed or
// minimized: the device is lost, a reset would just fail): a timer tries again every
// 500 ms until it can run.
void apply_refresh( HWND hwnd ) {
	if ( !in_game() || !foreground_app() || IsIconic( hwnd ) ) {
		SetTimer( hwnd, kTimerApplyRefresh, 500, nullptr ); // same id -> replaces, never stacks
		return;
	}
	KillTimer( hwnd, kTimerApplyRefresh );
	if ( !g_refreshPending.exchange( false ) ) return; // cleared first: a message sent during the reset can't start another
	if ( refresh_reset_needed() ) force_video_reset();
}

LRESULT CALLBACK hkWndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	if ( ( g_msgApplyRefresh && msg == g_msgApplyRefresh ) || ( msg == WM_TIMER && wp == kTimerApplyRefresh ) ) {
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
	// A refresh change requested before the window was hooked (e.g. the saved setting
	// at startup) is applied now.
	if ( g_refreshPending && g_msgApplyRefresh ) PostMessage( g_window, g_msgApplyRefresh, 0, 0 );
}

void present_tick(); // (below: needs hook_real_present)

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
	present_tick();
	HRESULT hr = g_origEndScene( dev );

	s_inside = false;
	return hr;
}

// The real device's Present: SA:MP's wrapper calls it once SA:MP's chat / dialogs /
// nametags are drawn, so this is the one point after SA:MP's UI and before the frame
// is shown. The 2D UI pass runs here in a scene of its own (Present is never called
// inside a scene). BeginScene/EndScene go to the REAL device, which isn't hooked -- no
// way back into hkEndScene (that's what froze an earlier post-chat pass).
HRESULT __stdcall hkPresent( IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty ) {
	static bool s_inside = false;
	// d3d9.dll's vtable is shared by every device in the process: only the one SA:MP
	// forwards the game's frames to gets the UI pass (an overlay's own device doesn't).
	const bool ours = g_realDeviceSlot && dev == *g_realDeviceSlot;
	if ( ours && !s_inside && !presentCbs().empty() ) {
		s_inside = true;
		if ( SUCCEEDED( dev->BeginScene() ) ) {
			for ( auto fn : presentCbs() ) fn( dev );
			dev->EndScene();
		}
		s_inside = false;
	}
	return g_origPresent( dev, src, dst, wnd, dirty );
}

HRESULT __stdcall hkReset( IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp ) {
	// Force the highest refresh for exclusive fullscreen (GTA hardcodes lowest >=60).
	// We only edit the params GTA passes to its own Reset, so RenderWare's normal
	// device-reset resource recreation still runs.
	UINT gtaHz    = 0;     // GTA's own rate, if we replaced it
	bool injected = false;
	if ( g_maxFullscreenHz && pp && pp->Windowed == FALSE ) {
		UINT hz = best_fullscreen_refresh( dev, pp );
		if ( hz > 0 && !refused_rate( hz, pp ) && hz != pp->FullScreen_RefreshRateInHz ) {
			Log( "[render] fullscreen refresh: %u -> %u Hz (%ux%u).",
			     pp->FullScreen_RefreshRateInHz, hz, pp->BackBufferWidth, pp->BackBufferHeight );
			gtaHz    = pp->FullScreen_RefreshRateInHz;
			injected = true;
			pp->FullScreen_RefreshRateInHz = hz;
			g_injectedHz = hz; // turning the setting off later has something to undo
		}
	} else if ( pp && pp->Windowed == FALSE ) {
		g_injectedHz = 0; // setting off: GTA's own rate from here on
	}
	for ( auto fn : resetCbs() ) fn( dev, true );   // pre-reset: release resources
	HRESULT hr = g_origReset( dev, pp );
	// Our rate refused for a reason other than a lost device (the game isn't in front):
	// never leave a black screen -- retry once at GTA's own rate, and don't offer that
	// rate again this session.
	if ( FAILED( hr ) && injected && hr != D3DERR_DEVICELOST ) {
		const UINT    tried = pp->FullScreen_RefreshRateInHz;
		const HRESULT first = hr;
		pp->FullScreen_RefreshRateInHz = gtaHz;
		g_injectedHz = 0;
		hr = g_origReset( dev, pp );
		if ( SUCCEEDED( hr ) ) {
			// Only now is it clear the RATE was the problem: never offer it again at this
			// resolution (a failure for another reason -- e.g. another overlay still
			// holding a resource -- would fail at GTA's rate too, and blacklist nothing).
			g_badHz = tried;
			g_badW  = pp->BackBufferWidth;
			g_badH  = pp->BackBufferHeight;
			Log( "[render] %u Hz was refused at %ux%u (0x%08lX); running at GTA's %u Hz instead.", tried,
			     pp->BackBufferWidth, pp->BackBufferHeight, (unsigned long) first, gtaHz );
		} else {
			Log( "[render] the display reset failed at %u Hz (0x%08lX) and at GTA's %u Hz (0x%08lX).", tried,
			     (unsigned long) first, gtaHz, (unsigned long) hr );
		}
	}
	for ( auto fn : resetCbs() ) fn( dev, false );  // post-reset: recreate
	return hr;
}

void patch_vtable_entry( void **vtable, int index, void *fn, void **outOrig ) {
	DWORD old;
	VirtualProtect( &vtable[index], sizeof( void * ), PAGE_READWRITE, &old );
	if ( outOrig ) *outOrig = vtable[index];
	vtable[index] = fn;
	VirtualProtect( &vtable[index], sizeof( void * ), old, &old );
}

// The module whose executable code `p` points into ("d3d9.dll"), or null (heap, data,
// unmapped). For the log too: `name` gets the module's file name.
HMODULE code_module( const void *p, char *name = nullptr, size_t nameSize = 0 ) {
	if ( name && nameSize ) name[0] = 0;
	MEMORY_BASIC_INFORMATION mbi = {};
	if ( !p || !VirtualQuery( p, &mbi, sizeof mbi ) || mbi.State != MEM_COMMIT ) return nullptr;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if ( !( mbi.Protect & exec ) ) return nullptr;
	HMODULE m = nullptr;
	if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          reinterpret_cast<LPCSTR>( p ), &m ) || !m )
		return nullptr;
	if ( name && nameSize ) {
		char full[MAX_PATH] = {};
		GetModuleFileNameA( m, full, sizeof full );
		const char *slash = strrchr( full, '\\' );
		lstrcpynA( name, slash ? slash + 1 : full, (int) nameSize );
	}
	return m;
}

HMODULE own_module() {
	HMODULE m = nullptr;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    reinterpret_cast<LPCSTR>( &code_module ), &m );
	return m;
}

// Hook the REAL device's Present (see hkPresent). Only when everything checks out: the
// device SA:MP forwards to exists and isn't the wrapper GTA calls, and its Present /
// BeginScene / EndScene are real code in a module other than samp.dll or ours (d3d9.dll
// -- or an overlay that hooked them first). Where the vtable itself lives doesn't
// matter: d3d9 and overlays like RivaTuner give a device a per-device copy on the heap.
// Otherwise the 2D UI is drawn at EndScene as before; `why` says what was off.
bool set_why( char *why, size_t whySize, const char *f, ... ) {
	va_list ap;
	va_start( ap, f );
	vsnprintf( why, whySize, f, ap );
	va_end( ap );
	return false;
}

bool hook_real_present( IDirect3DDevice9 *gameDev, char *why, size_t whySize ) {
	auto fail = [&]( const char *f, auto... args ) { return set_why( why, whySize, f, args... ); };
	const uint32_t base = samp::base();
	if ( !base || !samp::detected() ) return fail( "SA:MP not detected" );
	IDirect3DDevice9 **slot = reinterpret_cast<IDirect3DDevice9 **>( base + samp::addr::D3D_DEVICE );
	IDirect3DDevice9 *real = *slot;
	if ( !real ) return fail( "SA:MP has no device yet" );
	if ( real == gameDev ) return fail( "SA:MP's device is the one GTA calls (0x%p)", real );
	void **vt = *reinterpret_cast<void ***>( real );
	if ( !vt || vt == g_vtable ) return fail( "device 0x%p has a bad vtable 0x%p", real, vt );
	const HMODULE samp = reinterpret_cast<HMODULE>( (uintptr_t) base ), self = own_module();
	char mod[MAX_PATH] = {};
	for ( int slotIdx : { 17, 41, 42 } ) { // Present, BeginScene, EndScene
		const HMODULE m = code_module( vt[slotIdx], mod, sizeof mod );
		if ( !m || m == samp || m == self )
			return fail( "device 0x%p vtable 0x%p slot %d -> 0x%p (%s)", real, vt, slotIdx, vt[slotIdx], m ? mod : "not code" );
	}
	code_module( vt[17], mod, sizeof mod );
	g_realDeviceSlot = slot;
	patch_vtable_entry( vt, 17, (void *) hkPresent, (void **) &g_origPresent ); // IDirect3DDevice9::Present
	g_realVtable = vt;
	snprintf( why, whySize, "device 0x%p, vtable 0x%p, Present in %s", real, vt, mod );
	return true;
}

// Called every frame from hkEndScene. Two jobs:
//  * before we've ever hooked Present: try it on a slow cadence (SA:MP's device may not
//    be ready at install), for up to ~10 minutes.
//  * once hooked: if an overlay (RivaTuner/ReShade, ...) or anything else has put the
//    real Present back in slot 17, re-apply our hook RIGHT NOW -- at EndScene, before
//    this frame's Present runs -- so the menu keeps landing over the chat every frame
//    instead of flickering while a once-a-second keepalive catches up. The fast path
//    (slot 17 already ours) is a single pointer compare, so per-frame is cheap.
void present_tick() {
	char why[200] = {};
	if ( !g_realVtable ) { // not hooked yet: slow retry
		static unsigned frames = 0, tries = 0;
		if ( ++frames % 60 ) return;
		if ( tries >= 600 ) return; // ~10 minutes: it isn't coming, stay at EndScene
		if ( hook_real_present( g_device, why, sizeof why ) )
			Log( "[render] menu/HUD now drawn after SA:MP's UI -- over the chat (%s).", why );
		else if ( ++tries == 600 )
			Log( "[render] gave up hooking Present (%s): menu/HUD stay at EndScene.", why );
		return;
	}
	if ( !g_realDeviceSlot || !*g_realDeviceSlot ) return;
	void **vt = *reinterpret_cast<void ***>( *g_realDeviceSlot );
	if ( !vt || vt[17] == (void *) hkPresent ) return; // still ours (the usual case): nothing to do
	// Someone restored the vtable. Re-hook (hook_real_present rejects our own / samp.dll
	// in slot 17, so g_origPresent is always the real chain below us, never ourselves).
	g_realVtable = nullptr;
	static unsigned relog = 0;
	if ( hook_real_present( g_device, why, sizeof why ) ) {
		if ( relog++ < 3 ) Log( "[render] re-applied the Present hook after the device vtable was restored (%s).", why );
	} else if ( relog++ < 3 ) {
		Log( "[render] Present hook lost and not restorable (%s): menu/HUD at EndScene.", why );
	}
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
void on_present( PresentFn fn ) { if ( fn ) presentCbs().push_back( fn ); }
bool present_hooked()           { return g_realVtable != nullptr; }

bool installed() { return g_installed; }
IDirect3DDevice9 *device() { return g_device; }
HWND window() { return g_window; }

void hide_overlay_for( unsigned ms ) { g_overlayHideUntil = GetTickCount() + ms; }
// Wrap-safe (signed delta); the sentinel 0 never counts as "hidden".
bool overlay_hidden() { return g_overlayHideUntil && (int) ( g_overlayHideUntil - GetTickCount() ) > 0; }

void set_max_fullscreen_refresh( bool on ) {
	g_maxFullscreenHz = on;
	g_refreshPending  = true;
	if ( g_window && g_msgApplyRefresh ) PostMessage( g_window, g_msgApplyRefresh, 0, 0 ); // -> apply_refresh, between frames
	Log( "[render] max fullscreen refresh %s (applying now if the display needs it).", on ? "ON" : "OFF" );
}

bool install() {
	if ( g_installed ) return true;
	if ( !g_msgApplyRefresh ) g_msgApplyRefresh = RegisterWindowMessageA( "FuegoReversed.ApplyFullscreenRefresh" );
	g_vtable = game_device_vtable();
	if ( !g_vtable ) { Log( "[render] GTA D3D9 device not ready yet (0xC97C28 null)." ); return false; }
	g_device = game_device();          // real device -> device() valid immediately
	ensure_window( g_device );         // subclass the game window now
	patch_vtable_entry( g_vtable, 42, (void *) hkEndScene, (void **) &g_origEndScene ); // IDirect3DDevice9::EndScene
	patch_vtable_entry( g_vtable, 16, (void *) hkReset,    (void **) &g_origReset );    // IDirect3DDevice9::Reset
	g_installed = true;
	Log( "[render] EndScene/Reset hook installed on game device (0x%p).", g_device );
	char why[200] = {};
	if ( hook_real_present( g_device, why, sizeof why ) )
		Log( "[render] menu/HUD drawn after SA:MP's UI -- over the chat (%s).", why );
	else
		Log( "[render] Present not hooked yet (%s): menu/HUD at EndScene, retrying.", why );
	return true;
}

} // namespace render
