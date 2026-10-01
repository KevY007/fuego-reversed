#pragma once
/*
	net/net.h  --  RakNet hook install + interception seam.

	The HookedRakClientInterface (a vtable-identical clone of SA:MP's
	RakClientInterface) is swapped into CNetGame::pRakClientInterface (+0x2C).
	SA:MP then unknowingly calls our vtable; each thunk forwards to the saved
	real interface via g_RakClient, calling the On* seam first.

	The On* free functions below are what the thunks call. They DISPATCH to
	handlers registered by higher layers (compat, cheats) — so those layers plug
	in without touching the net core. With no handlers registered everything is a
	clean pass-through and the client behaves exactly as vanilla.
*/

#include "RakClient.h"

// --- seam entry points (called by HookedRakClientInterface) ------------------
// Send handlers: return false to DROP the RPC/packet. Receive: return true to
// DELIVER the packet, false to swallow it.
bool OnSendRPC( int uniqueID, BitStream *parameters, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp );
bool OnSendPacket( BitStream *parameters, PacketPriority priority, PacketReliability reliability, char orderingChannel );
bool OnReceivePacket( Packet *packet );

namespace net {

using SendRpcFn    = bool ( * )( int id, BitStream *, PacketPriority, PacketReliability, char, bool );
using SendPacketFn = bool ( * )( BitStream *, PacketPriority, PacketReliability, char );
using RecvPacketFn = bool ( * )( Packet * );

// Register interception handlers. Run in registration order; first send-handler
// to return false drops the message, first recv-handler to return false swallows
// the packet.
void on_send_rpc( SendRpcFn );
void on_send_packet( SendPacketFn );
void on_recv_packet( RecvPacketFn );

// Read-only taps on what actually goes out on the wire: the game's RPCs/packets
// once every send handler above has passed them (as rewritten), plus everything
// our own code sends straight through g_RakClient (injected = true). For the
// packet recorder; a tap must never modify or keep the data. Set once during
// bring-up, before install().
using WireRpcFn    = void ( * )( int id, BitStream *, bool injected );
using WirePacketFn = void ( * )( const unsigned char *data, int bits, bool injected ); // data[0] = packet id
void set_wire_taps( WireRpcFn, WirePacketFn );

// The same for what arrives: every packet the server sends that isn't an RPC
// (connection events, other players' syncs, ...), before any receive handler
// above can swallow it. Read-only, like the send taps.
using WireRecvFn = void ( * )( const unsigned char *data, int bits ); // data[0] = packet id
void set_recv_tap( WireRecvFn );

// Called by the two send paths (HookedRakClientInterface, RakClient) / the receive path.
void wire_rpc( int id, BitStream *bs, bool injected );
void wire_packet( const unsigned char *data, int bits, bool injected );
void wire_recv( const unsigned char *data, int bits );

// Install the RakClient interface hook (idempotent; installs once SA:MP's
// interface pointer is live). Returns installed().
bool install();

// Restore SA:MP's original interface. Call on unload.
void uninstall();

bool installed();

// Disconnect the RakClient (blockDuration ms). Used by the compat reconnect.
void disconnect( int blockDuration );

} // namespace net
