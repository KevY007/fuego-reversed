#define _CRT_SECURE_NO_WARNINGS 1
#include "compat.h"

#include "../net/net.h"
#include "../net/RakClient.h"
#include "../net/BitStream.h"
#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../core/memory.h"
#include "../core/logger.h"
#include "../core/settings.h"
#include "../gta/gtahost.h"     // game::local_vehicle_health (extra-godmode gate)
#include "../cheats/status.h"
#include "../cheats/cheats.h"
#include "../cheats/checkpoints.h" // where the server's checkpoints are (/triggercp)
#include "../cheats/server.h"
#include "../i18n/capture.h"   // debug: log incoming dialog/label/textdraw/gametext RPCs
#include "../i18n/surfaces.h"  // translate incoming dialogs / 3D labels in place
#include "../i18n/lab.h"       // debug lab: verify in-place chat/dialog replacement
#include "../recorder/recorder.h" // packet recorder (debug): the server's RPCs, as sent
#include "../fishing/autofish.h"  // Mike's CnR auto fish: what the server says while fishing
#include "../hook/CDetour.h"

#include <windows.h>
#include <thread>
#include <cstring>

// SA:MP ConnectionRejected RPC + reason (VERIFIED: open.mp / client src / wiki).
#define RPC_CONNECTION_REJECTED 130
#define REJECT_REASON_BAD_VERSION 1

// ---------------------------------------------------------------------------
namespace {

const char *STR_037  = "0.3.7-R5";   // spoofs a 0.3.7 client to the server
const char *STR_03DL = "0.3.DL-R1";

struct ClientJoinInfo_t {
	uint32_t    iVersion = 0;
	uint8_t     byteMod = 0;
	uint8_t     byteNicknameLen = 0;
	char        NickName[256] = {};
	uint32_t    uiClientChallengeResponse = 0;
	uint8_t     byteAuthKeyLen = 0;
	char        auth_key[256] = {};
	uint8_t     iClientVerLen = 0;
	char        ClientVersion[256] = {};
	uint32_t    ForceCompat    = compat::VER_03DL;
	const char *ForceCompatStr = STR_03DL;
} g_join;

uint32_t g_sampBase  = 0;
bool     g_rpcHooked = false;
bool     g_autoKicked = false;

void SendClientJoin( unsigned int dwDelay );
void on_version_rejected();

// --- INCOMING: 0.3.7 -> 0.3.DL skin-RPC rewrites (re-insert custom-model UINT32).
//     Bounds-checked: a short packet is left untouched (passes through). ---

bool Rewrite_SetPlayerSkin( RPCParameters *rpc ) {
	BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
	uint32_t wPlayerID, dSkinID;
	if ( !bs.Read( wPlayerID ) || !bs.Read( dSkinID ) ) return false;
	BitStream copied;
	copied.Write( (short)wPlayerID ); copied.Write( dSkinID ); copied.Write( (uint32_t)0 );
	copied.CopyData( &rpc->input );
	rpc->numberOfBitsOfData = copied.GetNumberOfBitsUsed();
	return true;
}

bool Rewrite_WorldPlayerAdd( RPCParameters *rpc ) {
	BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
	uint16_t wPlayerID; uint8_t team; uint32_t dSkinID;
	float PosX, PosY, PosZ, facing; uint32_t color; uint8_t style;
	if ( !bs.Read( wPlayerID ) || !bs.Read( team ) || !bs.Read( dSkinID ) ||
	     !bs.Read( PosX ) || !bs.Read( PosY ) || !bs.Read( PosZ ) ||
	     !bs.Read( facing ) || !bs.Read( color ) || !bs.Read( style ) ) return false;
	BitStream copied;
	copied.Write( wPlayerID ); copied.Write( team ); copied.Write( dSkinID ); copied.Write( (uint32_t)0 );
	copied.Write( PosX ); copied.Write( PosY ); copied.Write( PosZ );
	copied.Write( facing ); copied.Write( color ); copied.Write( style );
	copied.CopyData( &rpc->input );
	rpc->numberOfBitsOfData = copied.GetNumberOfBitsUsed();
	return true;
}

bool Rewrite_ShowActor( RPCParameters *rpc ) {
	BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
	uint16_t wActorID; uint32_t SkinID; float X, Y, Z, Angle, health; uint8_t inv;
	if ( !bs.Read( wActorID ) || !bs.Read( SkinID ) || !bs.Read( X ) || !bs.Read( Y ) ||
	     !bs.Read( Z ) || !bs.Read( Angle ) || !bs.Read( health ) || !bs.Read( inv ) ) return false;
	BitStream copied;
	copied.Write( wActorID ); copied.Write( SkinID ); copied.Write( (uint32_t)0 );
	copied.Write( X ); copied.Write( Y ); copied.Write( Z ); copied.Write( Angle ); copied.Write( health ); copied.Write( inv );
	copied.CopyData( &rpc->input );
	rpc->numberOfBitsOfData = copied.GetNumberOfBitsUsed();
	return true;
}

bool Rewrite_RequestClass( RPCParameters *rpc ) {
	BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
	uint8_t resp;
	if ( !bs.Read( resp ) ) return false;
	if ( resp == 0 ) return false; // invalid class => empty bitstream
	uint8_t team; uint32_t skin; uint8_t unk; float x, y, z, rot;
	uint32_t w0, w1, w2, a0, a1, a2;
	if ( !bs.Read( team ) || !bs.Read( skin ) || !bs.Read( unk ) || !bs.Read( x ) || !bs.Read( y ) ||
	     !bs.Read( z ) || !bs.Read( rot ) || !bs.Read( w0 ) || !bs.Read( w1 ) || !bs.Read( w2 ) ||
	     !bs.Read( a0 ) || !bs.Read( a1 ) || !bs.Read( a2 ) ) return false;
	BitStream copied;
	copied.Write( resp ); copied.Write( team ); copied.Write( skin ); copied.Write( (uint32_t)0 );
	copied.Write( unk ); copied.Write( x ); copied.Write( y ); copied.Write( z ); copied.Write( rot );
	copied.Write( w0 ); copied.Write( w1 ); copied.Write( w2 ); copied.Write( a0 ); copied.Write( a1 ); copied.Write( a2 );
	copied.CopyData( &rpc->input );
	rpc->numberOfBitsOfData = copied.GetNumberOfBitsUsed();
	return true;
}

bool Rewrite_SetSpawnInfo( RPCParameters *rpc ) {
	BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
	uint8_t team; uint32_t skin; uint8_t unk; float x, y, z, rot;
	uint32_t w0, w1, w2, a0, a1, a2;
	if ( !bs.Read( team ) || !bs.Read( skin ) || !bs.Read( unk ) || !bs.Read( x ) || !bs.Read( y ) ||
	     !bs.Read( z ) || !bs.Read( rot ) || !bs.Read( w0 ) || !bs.Read( w1 ) || !bs.Read( w2 ) ||
	     !bs.Read( a0 ) || !bs.Read( a1 ) || !bs.Read( a2 ) ) return false;
	BitStream copied;
	copied.Write( team ); copied.Write( skin ); copied.Write( (uint32_t)0 ); copied.Write( unk );
	copied.Write( x ); copied.Write( y ); copied.Write( z ); copied.Write( rot );
	copied.Write( w0 ); copied.Write( w1 ); copied.Write( w2 ); copied.Write( a0 ); copied.Write( a1 ); copied.Write( a2 );
	copied.CopyData( &rpc->input );
	rpc->numberOfBitsOfData = copied.GetNumberOfBitsUsed();
	return true;
}

// Incoming RPC dispatch interception. Runs the ConnectionRejected auto-detect
// (always) and, while spoofing, the skin rewrites. Then SA:MP's original handler.
void HandleRPCPacketFunc( unsigned char id, RPCParameters *rpc, void ( *callback )( RPCParameters * ) ) {
	// Packet recorder (no-op unless recording): the server's bytes exactly as they came,
	// before anything below holds, translates or rewrites them. Read-only.
	recorder::on_incoming_rpc( id, rpc );

	// Mike's CnR auto fish: reads the server's own words (chat, our animations, the
	// reel stats textdraw) before the translator below could rewrite them. Read-only.
	autofish::on_incoming_rpc( id, rpc );

	// Read-only capture for building the dialog/label/textdraw/gametext surfaces
	// (no-op unless config.debug_capture is on; never modifies the packet).
	i18n::capture::on_incoming_rpc( id, rpc );

	// Debug lab (read-only): decode + log ShowDialog (61). Never modifies the packet.
	i18n::lab::on_incoming_rpc( id, rpc );

	// Translate incoming dialogs / 3D labels / HUD text. A ShowDialog that still needs
	// translating is HELD (swallowed here) and released later, translated and in order,
	// through SA:MP's own Show. Runs before the rewrites/reads below so they see the
	// final text.
	if ( i18n::surfaces::on_incoming_rpc( id, rpc, callback ) ) return;

	if ( rpc != nullptr && rpc->numberOfBitsOfData >= 8 ) {
		// wrong-version reject -> tell the user to enable compat manually
		if ( id == RPC_CONNECTION_REJECTED && !compat::in_037_mode() ) {
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			uint8_t reason = 0;
			if ( bs.Read( reason ) && reason == REJECT_REASON_BAD_VERSION )
				on_version_rejected();
		}
		// 0.3.7 skin RPCs -> DL layout (only while spoofing)
		if ( compat::in_037_mode() ) {
			switch ( id ) {
				case RPC_SetPlayerSkin:  Rewrite_SetPlayerSkin( rpc );  break;
				case RPC_WorldPlayerAdd: Rewrite_WorldPlayerAdd( rpc ); break;
				case RPC_ShowActor:      Rewrite_ShowActor( rpc );      break;
				case RPC_RequestClass:   Rewrite_RequestClass( rpc );   break;
				case RPC_SetSpawnInfo:   Rewrite_SetSpawnInfo( rpc );    break;
				default: break;
			}
		}
	}

	// Where the server's checkpoint / race checkpoint is, so /triggercp can enter it
	// (and never a removed one). Read-only; the RPCs still reach SA:MP.
	{
		const unsigned char *in   = rpc ? rpc->input : nullptr;
		const unsigned       bits = rpc ? (unsigned) rpc->numberOfBitsOfData : 0;
		float p[3];
		switch ( id ) {
		case RPC_SetCheckpoint:
			if ( checkpoints::read_checkpoint( in, bits, p ) ) checkpoints::set( checkpoints::kCheckpoint, p[0], p[1], p[2] );
			break;
		case RPC_SetRaceCheckpoint:
			if ( checkpoints::read_race_checkpoint( in, bits, p ) ) checkpoints::set( checkpoints::kRace, p[0], p[1], p[2] );
			break;
		case RPC_DisableCheckpoint:     checkpoints::clear( checkpoints::kCheckpoint ); break;
		case RPC_DisableRaceCheckpoint: checkpoints::clear( checkpoints::kRace );       break;
		case RPC_InitGame:              // a new connection / server, or
		case RPC_GameModeRestart:       // a restarted gamemode, which sets its own
			checkpoints::reset();
			break;
		default: break;
		}
	}

	// LSRP gun-stealer object tracking (Fuego 1:1): follow vehicle-attached weapon objects
	// so force-adjust can re-emit them onto the ped -- that re-emit is what advances the
	// steal server-paced (otherwise force-adjust only ever times out).
	if ( rpc != nullptr && rpc->numberOfBitsOfData >= 8 ) {
		if ( id == RPC_CreateObject ) {
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			uint16_t objId = 0, attachedVid = 0;
			bs.Read( objId );
			bs.IgnoreBits( 32 );     // model id (uint32)
			bs.IgnoreBits( 32 * 7 ); // 7 floats (position + rotation + draw distance)
			bs.IgnoreBits( 8 );      // 1 byte
			bs.Read( attachedVid );
			if ( attachedVid != 0xFFFF )
				server::stealguns_track_object( objId, attachedVid );
		} else if ( id == RPC_WorldVehicleRemove ) {
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			uint16_t vid = 0; bs.Read( vid );
			server::stealguns_clear_vehicle( vid );
		} else if ( id == RPC_DestroyObject ) {
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			uint16_t objId = 0; bs.Read( objId );
			server::stealguns_untrack_object( objId );
		} else if ( id == RPC_EditObject ) {
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			bool playerObject = false; bs.Read( playerObject );
			if ( server::stealguns_on_editobject( playerObject ) )
				return; // swallowed -- re-emitted onto the ped + stage advanced
		}
	}

	// Held-state protection (Fuego 1:1, RPC dispatch 1324/1334): while car control /
	// ninja-kill / the gun-stealer is running, swallow the server's kick-from-car and
	// reposition RPCs so it can't pull us out mid-procedure and break the method.
	if ( ( id == RPC_RemovePlayerFromVehicle || id == RPC_SetPlayerPos ) &&
	     ( cheats::car_control_active() || cheats::njkill_active() || server::stealguns_active() ) ) {
		status::set( "Held: blocked server remove-from-car / setpos" );
		return;
	}

	// --- Health Hack (Fuego "god mode" / hp_cheat: swallow the server's health-set
	//     RPCs so it can never lower our HP). Player health blocked outright (like
	//     Fuego's `if (hp_cheat) return;`); vehicle health only when it's *lowered*.
	//     (Smart Health Hack is a SEPARATE toggle -- outgoing, in cheats::on_send_rpc.)
	if ( fx::g_set.health_hack && !fx::g_set.cheat_panic && rpc != nullptr ) {
		if ( id == RPC_SetPlayerHealth ) {
			status::set( "Health Hack: blocked server health change" );
			return;
		} else if ( id == RPC_SetVehicleHealth && fx::g_set.enable_extra_godmode &&
		            rpc->numberOfBitsOfData >= 48 ) {
			// Fuego 1:1: only with "extra godmode" on, only for OUR OWN vehicle (sId ==
			// our current vehicle id -- else we'd swallow every other car's health too),
			// and only a genuine DROP (fHealth < our vehicle's current health) so legit
			// heals still get through.
			BitStream bs( rpc->input, rpc->numberOfBitsOfData / 8, false );
			short sId = 0; float fHealth = 0.f;
			bs.Read( sId );
			bs.Read( fHealth );
			int myVid = game::local_vehicle_samp_id();
			float cur = game::local_vehicle_health();
			if ( myVid >= 0 && sId == (short) myVid && cur >= 0.0f && fHealth < cur ) {
				status::set( "Health Hack: blocked vehicle-health change" );
				return;
			}
		}
	}

	callback( rpc );
}

// --- naked trampolines detouring SA:MP's internal RPC dispatch (verbatim from
//     Project Fuego; register/exit conventions exact) ---
__declspec( naked ) void hook_handle_rpc_packet() {
	static RPCParameters *pRPCParams = nullptr;
	static RPCNode       *pRPCNode   = nullptr;
	static DWORD          dwTmp      = 0;
	__asm pushad;
	__asm mov pRPCParams, eax;
	__asm mov pRPCNode, edi;
	HandleRPCPacketFunc( pRPCNode->uniqueIdentifier, pRPCParams, pRPCNode->staticFunctionPointer );
	dwTmp = g_sampBase + samp::addr::HOOKEXIT_HANDLE_RPC;
	__asm popad;
	__asm add esp, 4;
	__asm jmp dwTmp;
}

__declspec( naked ) void hook_handle_rpc_packet2() {
	static RPCParameters *pRPCParams = nullptr;
	static RPCNode       *pRPCNode   = nullptr;
	static DWORD          dwTmp      = 0;
	__asm pushad;
	__asm mov pRPCParams, ecx;
	__asm mov pRPCNode, edi;
	HandleRPCPacketFunc( pRPCNode->uniqueIdentifier, pRPCParams, pRPCNode->staticFunctionPointer );
	dwTmp = g_sampBase + samp::addr::HOOKEXIT_HANDLE_RPC2;
	__asm popad;
	__asm jmp dwTmp;
}

void install_rpc_dispatch_hook() {
	if ( g_rpcHooked || !g_sampBase ) return;
	CDetour d1, d2;
	d1.Create( (BYTE *)( g_sampBase + samp::addr::HOOKENTER_HANDLE_RPC ),  (BYTE *)hook_handle_rpc_packet,  DETOUR_TYPE_JMP, 6 );
	d2.Create( (BYTE *)( g_sampBase + samp::addr::HOOKENTER_HANDLE_RPC2 ), (BYTE *)hook_handle_rpc_packet2, DETOUR_TYPE_JMP, 8 );
	g_rpcHooked = true;
	Log( "[compat] incoming RPC-dispatch hook installed." );
}

// --- OUTGOING: ClientJoin handshake spoof + DL-only RPC suppression ----------
void capture_join( BitStream *p ) {
	p->ResetReadPointer();
	p->Read( g_join.iVersion );
	p->Read( g_join.byteMod );
	p->Read( g_join.byteNicknameLen );
	p->Read( g_join.NickName, g_join.byteNicknameLen );
	g_join.NickName[g_join.byteNicknameLen] = '\0';
	p->Read( g_join.uiClientChallengeResponse );
	p->Read( g_join.byteAuthKeyLen );
	p->Read( g_join.auth_key, g_join.byteAuthKeyLen );
	g_join.auth_key[g_join.byteAuthKeyLen] = '\0';
	p->Read( g_join.iClientVerLen );
	p->Read( g_join.ClientVersion, g_join.iClientVerLen );
	g_join.ClientVersion[g_join.iClientVerLen] = '\0';
	p->ResetReadPointer();
}

void SendClientJoin( unsigned int dwDelay ) {
	Sleep( dwDelay );
	if ( !g_RakClient ) return;
	BitStream p;
	uint32_t modifiedResponse = g_join.uiClientChallengeResponse ^ 4062 ^ g_join.ForceCompat;
	p.Reset();
	p.Write( g_join.ForceCompat );
	p.Write( g_join.byteMod );
	p.Write( g_join.byteNicknameLen );
	p.Write( g_join.NickName, g_join.byteNicknameLen );
	p.Write( modifiedResponse );
	p.Write( g_join.byteAuthKeyLen );
	p.Write( g_join.auth_key, g_join.byteAuthKeyLen );
	p.Write<uint8_t>( (uint8_t)strlen( g_join.ForceCompatStr ) );
	p.Write( g_join.ForceCompatStr, (int)strlen( g_join.ForceCompatStr ) );
	p.Write( modifiedResponse );
	g_RakClient->RPC( RPC_ClientJoin, &p );
}

// Called (once) when the server rejects us for wrong version.
//
// Detection ONLY -- compat is ALWAYS enabled manually. We never auto-switch mode
// or auto-reconnect; we just tell the user to run /compat (after the game loads,
// before connecting) and join the server themselves.
void on_version_rejected() {
	if ( g_autoKicked || compat::in_037_mode() ) return;
	g_autoKicked = true; // notify once per DL join attempt
	samp::chat( "{FFAA00}[COMPAT] {FFFFFF}Server rejected the client version (likely a 0.3.7 server)." );
	samp::chat( "{FFAA00}[COMPAT] {FFFFFF}Type {FFFF00}/compat{FFFFFF} before connecting, then join the server." );
}

bool compat_on_send_rpc( int id, BitStream *params, PacketPriority, PacketReliability, char, bool ) {
	if ( id == RPC_ClientJoin ) {
		capture_join( params ); // always snapshot (for spoof / retry)
		if ( compat::in_037_mode() ) {
			std::thread( SendClientJoin, 5000 ).detach();
			return false; // drop original; send the spoofed 0.3.7 join
		}
		g_autoKicked = false; // re-arm 0.3.7 auto-detect for this DL join attempt
		return true;          // let the join proceed; RPC-130 reject handles the rest
	}
	if ( compat::in_037_mode() &&
	     ( id == RPC_RequestDffFile || id == RPC_RequestTxdFile || id == RPC_FinishedDownloading ) )
		return false; // suppress DL-only download RPCs on a 0.3.7 server
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
namespace compat {

bool in_037_mode() { return g_join.ForceCompat == VER_037; }

void set_037_mode( bool on ) {
	g_join.ForceCompat    = on ? VER_037 : VER_03DL;
	g_join.ForceCompatStr = on ? STR_037 : STR_03DL;
	fx::g_set.compat_037  = on; // keep the persisted preference in sync (menu / /compat / startup)
	Log( "[compat] mode = %s", on ? STR_037 : STR_03DL );
}

void toggle_mode() { set_037_mode( !in_037_mode() ); }

void install() {
	g_sampBase = samp::base();
	install_rpc_dispatch_hook();            // always: needed to catch RPC 130 in DL mode
	net::on_send_rpc( compat_on_send_rpc ); // handshake spoof / DL-only suppression
	Log( "[compat] ready (mode=%s). Auto-detects 0.3.7 servers; /compat to force.", g_join.ForceCompatStr );
}

} // namespace compat
