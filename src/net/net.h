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

// Install the RakClient interface hook (idempotent; installs once SA:MP's
// interface pointer is live). Returns installed().
bool install();

// Restore SA:MP's original interface. Call on unload.
void uninstall();

bool installed();

// Disconnect the RakClient (blockDuration ms). Used by the compat reconnect.
void disconnect( int blockDuration );

} // namespace net
