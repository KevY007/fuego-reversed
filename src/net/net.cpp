#include "net.h"
#include "HookedRakClient.h"

#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../core/memory.h"
#include "../core/logger.h"
#include "../hook/CDetour.h"

#include <vector>

// Meyers-singleton registries (avoid static-init-order issues if a module
// registers a handler during its own static init).
namespace {
std::vector<net::SendRpcFn>    &sendRpcHandlers()    { static std::vector<net::SendRpcFn> v;    return v; }
std::vector<net::SendPacketFn> &sendPacketHandlers() { static std::vector<net::SendPacketFn> v; return v; }
std::vector<net::RecvPacketFn> &recvPacketHandlers() { static std::vector<net::RecvPacketFn> v; return v; }

bool     g_installed  = false;
void   **g_ifaceSlot  = nullptr; // &CNetGame::pRakClientInterface
void    *g_realIface  = nullptr; // SA:MP's original interface (saved)
void    *g_hookedIface = nullptr; // our HookedRakClientInterface
uint32_t g_base       = 0;       // samp.dll base (for the dtor hook)

// Called INSTEAD of SA:MP's ~CNetGame call sites (DETOUR_TYPE_CALL_FUNC). We put
// the real RakClient interface back BEFORE the destructor runs, so SA:MP frees
// ITS OWN object (correct allocator) instead of ours -> no exit crash. Our hook
// object simply leaks (the process is exiting).
void __stdcall CNetGame_dtor_restore() {
	if ( g_ifaceSlot && g_realIface )
		*g_ifaceSlot = g_realIface;
	const uint32_t sampInfo = mem::read<uint32_t>( g_base + samp::addr::INFO );
	if ( sampInfo )
		mem::thiscall<void>( g_base + samp::addr::FUNC_CNETGAME_DTOR, (void *)sampInfo );
}
}

// --- seam dispatch (invoked by HookedRakClientInterface thunks) --------------

bool OnSendRPC( int id, BitStream *bs, PacketPriority p, PacketReliability r, char ch, bool ts ) {
	for ( auto fn : sendRpcHandlers() )
		if ( !fn( id, bs, p, r, ch, ts ) ) return false; // a handler dropped it
	return true;
}

bool OnSendPacket( BitStream *bs, PacketPriority p, PacketReliability r, char ch ) {
	for ( auto fn : sendPacketHandlers() )
		if ( !fn( bs, p, r, ch ) ) return false;
	return true;
}

bool OnReceivePacket( Packet *pkt ) {
	if ( pkt ) net::wire_recv( pkt->data, (int) pkt->bitSize ); // the recorder sees it before anyone can swallow it
	for ( auto fn : recvPacketHandlers() )
		if ( !fn( pkt ) ) return false; // a handler swallowed it
	return true;
}

namespace net {

void on_send_rpc( SendRpcFn fn )       { if ( fn ) sendRpcHandlers().push_back( fn ); }
void on_send_packet( SendPacketFn fn ) { if ( fn ) sendPacketHandlers().push_back( fn ); }
void on_recv_packet( RecvPacketFn fn ) { if ( fn ) recvPacketHandlers().push_back( fn ); }

namespace {
WireRpcFn    g_wireRpc    = nullptr;
WirePacketFn g_wirePacket = nullptr;
WireRecvFn   g_wireRecv   = nullptr;
}

void set_wire_taps( WireRpcFn r, WirePacketFn p ) { g_wireRpc = r; g_wirePacket = p; }
void set_recv_tap( WireRecvFn r ) { g_wireRecv = r; }

void wire_recv( const unsigned char *data, int bits ) {
	if ( g_wireRecv && data && bits > 0 ) g_wireRecv( data, bits );
}

void wire_rpc( int id, BitStream *bs, bool injected ) {
	if ( g_wireRpc && bs ) g_wireRpc( id, bs, injected );
}

void wire_packet( const unsigned char *data, int bits, bool injected ) {
	if ( g_wirePacket && data && bits > 0 ) g_wirePacket( data, bits, injected );
}

bool installed() { return g_installed; }

bool install() {
	if ( g_installed ) return true;

	const uint32_t b = samp::base();
	if ( !b ) return false;

	// g_SAMP = *(stSAMP**)(base + INFO)
	const uint32_t sampInfo = mem::read<uint32_t>( b + samp::addr::INFO );
	if ( !sampInfo ) return false;

	// slot = &CNetGame::pRakClientInterface  (offset +0x2C)
	g_ifaceSlot = mem::ptr<void *>( sampInfo + samp::addr::NETGAME_RAKCLIENT_IFACE );
	if ( IsBadReadPtr( g_ifaceSlot, sizeof( void * ) ) ) { g_ifaceSlot = nullptr; return false; }

	g_realIface = *g_ifaceSlot;
	if ( !g_realIface ) return false; // SA:MP hasn't created its interface yet

	// Wrap the real interface, swap in our vtable-identical hook.
	g_RakClient  = new RakClient( g_realIface );
	g_hookedIface = new HookedRakClientInterface();
	*g_ifaceSlot = g_hookedIface;
	g_base = b;

	// Hook SA:MP's ~CNetGame call sites so we restore the real interface before
	// shutdown -> clean exit (no cross-heap free of our object).
	{
		CDetour d1, d2;
		d1.Create( (BYTE *)( b + samp::addr::HOOKENTER_CNETGAME_DTOR ),  (BYTE *)CNetGame_dtor_restore, DETOUR_TYPE_CALL_FUNC, 5 );
		d2.Create( (BYTE *)( b + samp::addr::HOOKENTER_CNETGAME_DTOR2 ), (BYTE *)CNetGame_dtor_restore, DETOUR_TYPE_CALL_FUNC, 5 );
	}

	g_installed = true;
	Log( "[net] RakClient hook installed (iface swap @ CNetGame+0x%02X) + shutdown restore.", samp::addr::NETGAME_RAKCLIENT_IFACE );
	return true;
}

void uninstall() {
	if ( !g_installed ) return;
	if ( g_ifaceSlot && g_realIface )
		*g_ifaceSlot = g_realIface; // restore SA:MP's original interface
	g_installed = false;            // leak our objects deliberately (process is exiting)
	Log( "[net] RakClient hook uninstalled." );
}

void disconnect( int blockDuration ) {
	if ( g_RakClient )
		g_RakClient->GetInterface()->Disconnect( (unsigned int)blockDuration );
}

} // namespace net
