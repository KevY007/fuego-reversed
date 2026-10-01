#include "autofish.h"

#include "fisher.h"

#include "../net/net.h"
#include "../net/RakClient.h"   // g_RakClient, RPCParameters, packet ids
#include "../net/BitStream.h"
#include "../render/render.h"
#include "../samp/samp.h"       // samp::chat, netgame, base
#include "../samp/addresses.h"  // the dialog's "is open" flag
#include "../samp/pools.h"      // local_player / local_player_id
#include "../samp/structs.h"    // stOnFootData, stSAMP
#include "../gta/gtahost.h"     // game::in_vehicle
#include "../core/settings.h"   // fx::g_set (PANIC, conflicting cheats)
#include "../core/logger.h"
#include "../recorder/timeline.h" // rec::BitReader (RakNet bit order)

#include "imgui.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace autofish {

namespace {

constexpr float    kMaxDriftMeters = 6.0f;  // walked/teleported this far from the spot: stop
constexpr uint32_t kSyncFreshMs    = 2000;  // the game sends on-foot syncs 2-5x a second while we
                                            // stand still; none for this long = paused / ESC menu /
                                            // spectating / not spawned: hands off

std::mutex   g_mtx; // RPC dispatch, the send seam and the frame are all the game thread; cheap insurance
fish::Fisher g_fisher( GetTickCount() ^ 0x9E3779B9u );
bool         g_on = false;
uint16_t     g_forced = 0;          // the keys we're holding (within fish::kControlled)
bool         g_holding = false;     // the fisher owns the keys right now (casting / reeling)
uint32_t     g_lastGameSyncAt = 0;  // the game's own last on-foot sync
bool         g_gameSyncing = false;
float        g_anchor[3] = { 0, 0, 0 };
bool         g_haveAnchor = false;
bool         g_dialogOpen = false;
std::string  g_stopWhy;             // set by the send seam (can't print there), handled in frame()
char         g_status[160] = "off";
char         g_stats[200] = "";

void chat( const std::string &s ) {
	samp::chat( ( "{6FC3FF}[auto fish] {FFFFFF}" + s ).c_str() );
}

bool due( uint32_t now, uint32_t at ) { return (int32_t) ( now - at ) >= 0; }

void refresh_texts_locked( uint32_t now ) {
	if ( !g_gameSyncing && g_on )
		std::snprintf( g_status, sizeof g_status, "waiting -- the game isn't syncing (paused / menu?)" );
	else
		std::snprintf( g_status, sizeof g_status, "%s", g_fisher.status( now ).c_str() );
	const fish::Stats &st = g_fisher.stats();
	int n = std::snprintf( g_stats, sizeof g_stats, "%u caught (%.0f lbs)", st.caught, st.pounds );
	if ( st.biggest > 0 && n > 0 && n < (int) sizeof g_stats )
		n += std::snprintf( g_stats + n, sizeof g_stats - n, ", biggest %.1f lbs %s", st.biggest, st.biggestName.c_str() );
	if ( st.snapped + st.gotAway + st.rotten + st.other > 0 && n > 0 && n < (int) sizeof g_stats )
		std::snprintf( g_stats + n, sizeof g_stats - n, " | %u snapped, %u got away, %u rotten, %u other", st.snapped,
		               st.gotAway, st.rotten, st.other );
}

// Our on-foot sync, now, with the fisher's keys: a press/release reaches the server
// at once instead of waiting for the game's next sync (they come slowly while we
// stand still). Built from SA:MP's own last on-foot data, like Fuego's /triggercp.
// Sent straight to the real interface (never back through our send seam).
void send_keys_now( uint16_t keys ) {
	samp::stLocalPlayer *lp = samp::local_player();
	if ( !lp || !g_RakClient || game::in_vehicle() ) return;
	samp::stOnFootData d = lp->onFootData;
	d.sKeys = (uint16_t) ( ( d.sKeys & ~fish::kControlled ) | keys );
	BitStream bs;
	bs.Write( (uint8_t) ID_PLAYER_SYNC );
	bs.Write( (char *) &d, sizeof d );
	g_RakClient->Send( &bs, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0 );
}

bool my_position( float out[3] ) {
	samp::stLocalPlayer *lp = samp::local_player();
	if ( !lp ) return false;
	std::memcpy( out, lp->onFootData.fPosition, sizeof( float ) * 3 );
	return true;
}

// SA:MP's own "a dialog is open" flag: covers dialogs the server closes itself and
// replies that never pass our send seam (the translator re-sends those).
bool dialog_open() {
	const uint32_t base = samp::base();
	if ( !base ) return false;
	const uint8_t *dlg = *reinterpret_cast<const uint8_t *const *>( base + samp::addr::DIALOG_INFO );
	return dlg && *reinterpret_cast<const int *>( dlg + samp::addr::DIALOG_ACTIVE ) != 0;
}

// Cheats that rewrite our on-foot syncs (position / rotation / special action / held
// back): our own syncs would contradict theirs.
const char *conflicting_cheat() {
	const auto &s = fx::g_set;
	if ( s.fake_lag )      return "Fake Lag";
	if ( s.invisibility )  return "Invisibility";
	if ( s.smart_invis )   return "Smart Invisibility";
	if ( s.moonwalk )      return "Moonwalker";
	if ( s.upside_down )   return "Upside Down";
	if ( s.twister_mode )  return "Twister Mode";
	if ( s.ghost_mode )    return "Ghost Mode";
	if ( s.near_slapper )  return "Near Slapper";
	if ( s.aim_slapper )   return "Aim Slapper";
	return nullptr;
}

bool on_mikes_cnr() {
	samp::stSAMP *ng = samp::netgame();
	if ( !ng ) return false;
	std::string host( ng->szHostname, strnlen( ng->szHostname, sizeof ng->szHostname ) );
	for ( char &c : host ) c = (char) std::tolower( (unsigned char) c );
	return host.find( "mike" ) != std::string::npos;
}

// The game's own outgoing syncs: our keys in while the fisher holds them, and a car /
// passenger sync (we got in a vehicle) stops it.
bool on_send_packet( BitStream *bs, PacketPriority, PacketReliability, char ) {
	if ( !bs ) return true;
	try {
		const unsigned char *data = bs->GetData();
		const int bits = bs->GetNumberOfBitsUsed();
		if ( !data || bits < 8 ) return true;
		const uint8_t id = data[0];
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( id == ID_PLAYER_SYNC ) g_lastGameSyncAt = GetTickCount();
		if ( !g_on ) return true;
		if ( id == ID_VEHICLE_SYNC || id == ID_PASSENGER_SYNC ) {
			if ( g_stopWhy.empty() ) g_stopWhy = "you got in a vehicle";
			return true;
		}
		if ( id != ID_PLAYER_SYNC || !g_holding || bits < (int) ( 1 + sizeof( samp::stOnFootData ) ) * 8 ) return true;
		samp::stOnFootData d;
		uint8_t pid;
		bs->ResetReadPointer();
		bs->Read( pid );
		bs->Read( (char *) &d, sizeof d );
		d.sKeys = (uint16_t) ( ( d.sKeys & ~fish::kControlled ) | g_forced );
		bs->ResetWritePointer();
		bs->Write( pid );
		bs->Write( (char *) &d, sizeof d );
	} catch ( ... ) {} // inside RakNet's send path: nothing may escape
	return true;
}

// release: send "every key up" at once (not after a reconnect: we may not be spawned).
void stop_locked( const std::string &why, bool release = true ) {
	g_fisher.enable( false, GetTickCount() );
	g_on = false;
	g_holding = false;
	g_stopWhy.clear();
	if ( g_forced && release && g_gameSyncing ) send_keys_now( 0 );
	g_forced = 0;
	std::snprintf( g_status, sizeof g_status, "off" );
	chat( why );
	Log( "[autofish] %s", why.c_str() );
}

void frame( IDirect3DDevice9 * ) {
	if ( !g_on ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_on ) return;
		const uint32_t now = GetTickCount();

		// Stop conditions the fisher can't see itself.
		samp::stLocalPlayer *lp = samp::local_player();
		float pos[3];
		if ( fx::g_set.cheat_panic ) { stop_locked( "Off (PANIC)." ); return; }
		if ( const char *c = conflicting_cheat() ) {
			stop_locked( std::string( "Off: " ) + c + " changes your syncs -- turn it off to auto fish." );
			return;
		}
		if ( !g_stopWhy.empty() ) { stop_locked( "Off: " + g_stopWhy + "." ); return; }
		if ( !lp || lp->iIsWasted || game::in_vehicle() ) { stop_locked( "Off: you're not on foot." ); return; }
		if ( my_position( pos ) ) {
			if ( !g_haveAnchor ) {
				std::memcpy( g_anchor, pos, sizeof g_anchor );
				g_haveAnchor = true;
			}
			const float dx = pos[0] - g_anchor[0], dy = pos[1] - g_anchor[1];
			if ( dx * dx + dy * dy > kMaxDriftMeters * kMaxDriftMeters ) {
				stop_locked( "Off: you moved away from the fishing spot." );
				return;
			}
		}

		// A dialog: hands off until it's gone (however it goes).
		const bool open = dialog_open();
		if ( open != g_dialogOpen ) {
			g_dialogOpen = open;
			if ( open ) g_fisher.on_dialog_shown( now );
			else        g_fisher.on_dialog_closed( now );
		}

		// Only while the game itself is sending on-foot syncs: in the ESC menu, spectating
		// or not spawned, a sync from us would be the only one -- the server notices.
		g_gameSyncing = g_lastGameSyncAt && !due( now, g_lastGameSyncAt + kSyncFreshMs );
		if ( !g_gameSyncing ) {
			refresh_texts_locked( now );
			return;
		}

		const uint16_t keys = (uint16_t) ( g_fisher.tick( now ) & fish::kControlled );
		g_holding = g_fisher.holds_keys() || keys != 0;
		std::string notice;
		if ( g_fisher.pop_notice( notice ) ) chat( notice );
		if ( !g_fisher.enabled() ) { // it stopped itself (the notice said why)
			g_on = false;
			g_holding = false;
			if ( g_forced ) send_keys_now( 0 );
			g_forced = 0;
			std::snprintf( g_status, sizeof g_status, "off" );
			return;
		}
		if ( keys != g_forced ) {
			g_forced = keys;
			send_keys_now( keys );
		}
		refresh_texts_locked( now );
	} catch ( ... ) {} // inside EndScene: nothing may escape
}

// The 16-bit text length + text that ends a ShowTextDraw (134): 65 bytes of fixed
// fields come first (id, flags, sizes, colours, font, position, model, rotation).
bool showtextdraw_text( const uint8_t *p, size_t bytes, std::string &out ) {
	if ( !p || bytes < 67 ) return false;
	const size_t len = (size_t) p[65] | ( (size_t) p[66] << 8 );
	if ( 67 + len > bytes ) return false;
	out.assign( (const char *) p + 67, len );
	return true;
}

} // namespace

void install() {
	net::on_send_packet( on_send_packet );
	render::on_frame( frame );
}

bool enabled() { return g_on; }

void set_enabled( bool on ) {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( on == g_on ) return;
		if ( !on ) { stop_locked( "Off." ); return; }
		if ( fx::g_set.cheat_panic ) { chat( "Not while PANIC is on." ); return; }
		if ( !on_mikes_cnr() ) { chat( "This only works on Mike's Cops and Robbers." ); return; }
		if ( const char *c = conflicting_cheat() ) {
			chat( std::string( "Turn off " ) + c + " first -- it changes your syncs." );
			return;
		}
		samp::stLocalPlayer *lp = samp::local_player();
		if ( !lp || lp->iIsWasted || game::in_vehicle() ) { chat( "Spawn / get out of the vehicle first -- it fishes on foot." ); return; }
		g_haveAnchor = my_position( g_anchor );
		g_stopWhy.clear();
		g_forced = 0;
		g_holding = false;
		g_dialogOpen = dialog_open();
		g_fisher.enable( true, GetTickCount() );
		if ( g_dialogOpen ) g_fisher.on_dialog_shown( GetTickCount() );
		g_on = true;
		refresh_texts_locked( GetTickCount() );
		chat( g_fisher.state() == fish::State::NoRod
		          ? "On -- take your rod out (/fish) at the water; it starts casting by itself. /.autofish stops it."
		          : "On -- casting at this spot. Stay here; /.autofish stops it." );
		Log( "[autofish] on." );
	} catch ( ... ) {}
}

void on_incoming_rpc( unsigned char id, const RPCParameters *rpc ) {
	try {
		// A new session first: these may come with no payload at all.
		if ( id == RPC_InitGame || id == RPC_GameModeRestart ) {
			std::lock_guard<std::mutex> lk( g_mtx );
			g_fisher.on_rod( false );
			g_lastGameSyncAt = 0;
			if ( g_on ) stop_locked( "Off: a new session.", /*release=*/false );
			return;
		}
		if ( !rpc || !rpc->input ) return;
		const uint8_t *p = rpc->input;
		const size_t bits = (size_t) rpc->numberOfBitsOfData;
		const size_t bytes = ( bits + 7 ) / 8;
		const int me = samp::local_player_id();
		rec::BitReader r( p, bits );
		std::lock_guard<std::mutex> lk( g_mtx );
		const uint32_t now = GetTickCount();

		if ( id == RPC_SetPlayerAttachedObject ) { // our rod, tracked even while off
			uint16_t pid = 0; uint32_t slot = 0; bool create = false; int32_t model = 0;
			if ( !( r.get( pid ) && r.get( slot ) && r.bit( create ) ) || me < 0 || pid != (uint16_t) me || slot != 0 ) return;
			if ( !create ) g_fisher.on_rod( false );
			else if ( r.get( model ) ) g_fisher.on_rod( model == fish::kRodModel );
			return;
		}
		if ( !g_on ) return;

		switch ( id ) {
		case RPC_ClientMessage: {
			uint32_t color = 0; std::string text;
			if ( r.get( color ) && r.str32( text ) ) g_fisher.on_chat( text, now );
			break; }
		case RPC_ApplyAnimation: {
			uint16_t pid = 0; std::string lib, name;
			if ( r.get( pid ) && me >= 0 && pid == (uint16_t) me && r.str8( lib ) && r.str8( name ) )
				g_fisher.on_anim( lib, name, now );
			break; }
		case RPC_TextDrawSetString: {
			uint16_t td = 0; std::string text;
			if ( r.get( td ) && r.str16( text ) ) g_fisher.on_textdraw( text, now );
			break; }
		case RPC_ShowTextDraw: {
			std::string text;
			if ( showtextdraw_text( p, bytes, text ) ) g_fisher.on_textdraw( text, now );
			break; }
		default:
			break;
		}
	} catch ( ... ) {} // inside SA:MP's RPC dispatch: nothing may escape
}

const char *status_text() { return g_status; }
const char *stats_text() { return g_stats; }

void draw_overlay() {
	if ( !g_on ) return;
	try {
		char text[400];
		{
			std::lock_guard<std::mutex> lk( g_mtx );
			std::snprintf( text, sizeof text, "AUTO FISH   %s   |   %s", g_status, g_stats );
		}
		ImDrawList *dl = ImGui::GetForegroundDrawList(); // the HUD layer: over SA:MP's chat
		const ImVec2 sz = ImGui::CalcTextSize( text );
		const float x = ( ImGui::GetIO().DisplaySize.x - sz.x ) * 0.5f;
		const float y = ImGui::GetIO().DisplaySize.y * 0.036f;
		dl->AddRectFilled( ImVec2( x - 10, y - 4 ), ImVec2( x + sz.x + 10, y + sz.y + 4 ), IM_COL32( 0, 0, 0, 170 ), 5.0f );
		dl->AddText( ImVec2( x, y ), IM_COL32( 140, 210, 255, 255 ), text );
	} catch ( ... ) {} // inside EndScene
}

} // namespace autofish
