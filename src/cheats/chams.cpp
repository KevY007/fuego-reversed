#define _CRT_SECURE_NO_WARNINGS 1
#include "chams.h"

#include "../core/settings.h"
#include "../core/logger.h"
#include "../render/render.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <cstring>

#pragma intrinsic( _ReturnAddress )

namespace {

typedef HRESULT( __stdcall *DIP_t )( IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT );

DIP_t                  g_origDIP = nullptr;
IDirect3DPixelShader9 *g_blue    = nullptr; // behind-wall pass
IDirect3DPixelShader9 *g_green   = nullptr; // in-front pass
bool                   g_armed   = false;

// GTA:SA 1.0 US ped-render call site: DrawIndexedPrimitive invoked from here means
// the primitive being drawn is an actor/ped (Fuego's detection).
constexpr DWORD kPedDrawRetAddr = 0x761142;

DWORD f2dw( float f ) { DWORD d; std::memcpy( &d, &f, sizeof d ); return d; }

// Flat-colour pixel shader (s0beit/Azorbix): outputs a constant RGBA. This is the
// token stream D3DXAssembleShader produces for
//     ps.1.1 / def c0, r, g, b, a / mov r0, c0
// emitted directly, so there's no d3dx9 (DirectX SDK) build or runtime dependency.
// ps.1.1 can still be refused by a driver -- then the shader stays null and chams is inert.
HRESULT gen_shader( IDirect3DDevice9 *dev, IDirect3DPixelShader9 **out, float a, float r, float g, float b ) {
	const DWORD code[] = {
		0xFFFF0101,                                         // ps_1_1
		0x00000051, 0xA00F0000, f2dw( r ), f2dw( g ), f2dw( b ), f2dw( a ), // def c0, r, g, b, a
		0x00000001, 0x800F0000, 0xA0E40000,                 // mov r0, c0
		0x0000FFFF,                                         // end
	};
	return dev->CreatePixelShader( code, out );
}

HRESULT __stdcall hkDIP( IDirect3DDevice9 *dev, D3DPRIMITIVETYPE pt, INT bvi, UINT mvi, UINT nv, UINT si, UINT pc ) {
	if ( fx::g_set.chams && !fx::g_set.cheat_panic && !render::overlay_hidden() && g_blue && g_green &&
	     (DWORD) _ReturnAddress() == kPedDrawRetAddr ) {
		dev->SetRenderState( D3DRS_FILLMODE, fx::g_set.chams_wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID );
		dev->SetRenderState( D3DRS_PATCHEDGESTYLE, D3DPATCHEDGE_CONTINUOUS );

		// pass 1: behind walls (z-test off) in blue
		dev->SetRenderState( D3DRS_ZENABLE, FALSE );
		dev->SetPixelShader( g_blue );
		g_origDIP( dev, pt, bvi, mvi, nv, si, pc );

		// pass 2: in front (z-test on) in green
		dev->SetPixelShader( g_green );
		dev->SetRenderState( D3DRS_ZENABLE, TRUE );
		g_origDIP( dev, pt, bvi, mvi, nv, si, pc );

		dev->SetPixelShader( nullptr );
		dev->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
		return D3D_OK;
	}
	return g_origDIP( dev, pt, bvi, mvi, nv, si, pc );
}

// Once, on the first frame: build the shaders and patch the DIP vtable slot (82).
void first_frame( IDirect3DDevice9 *dev ) {
	if ( g_armed || !dev )
		return;
	g_armed = true;

	if ( FAILED( gen_shader( dev, &g_blue,  0.8f, 0.0f, 0.0f, 1.0f ) ) ) g_blue  = nullptr;
	if ( FAILED( gen_shader( dev, &g_green, 0.8f, 0.0f, 1.0f, 0.0f ) ) ) g_green = nullptr;

	void **vt = *reinterpret_cast<void ***>( dev );  // device D3D9 vtable
	DWORD  old;
	VirtualProtect( &vt[82], sizeof( void * ), PAGE_READWRITE, &old );
	g_origDIP = reinterpret_cast<DIP_t>( vt[82] );
	vt[82]    = reinterpret_cast<void *>( hkDIP );
	VirtualProtect( &vt[82], sizeof( void * ), old, &old );

	Log( "[chams] DrawIndexedPrimitive hook installed (shaders %s).",
	     ( g_blue && g_green ) ? "ok" : "FAILED -- ps.1.1 unsupported, chams inert" );
}

} // namespace

namespace chams {

void install() {
	render::on_frame( first_frame ); // device only exists once frames start
	Log( "[chams] armed (hook + shaders deferred to first frame)." );
}

} // namespace chams
