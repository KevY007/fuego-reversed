#include "recorder.h"

#include "timeline.h"

#include "../net/net.h"
#include "../net/RakClient.h" // RPCParameters, g_RakClient (ping)
#include "../render/render.h"
#include "../samp/samp.h"
#include "../samp/addresses.h" // the dialog's "open" flag
#include "../samp/pools.h"     // local player, names, who is connected
#include "../samp/structs.h"   // stSAMP (server name / address, game state)
#include "../gta/gtahost.h"    // our position, health, vehicle, skin
#include "../core/input.h"     // typing in chat, the GTA menu
#include "../menu/menu.h"      // our menu open
#include "../core/logger.h"

#include "imgui.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace recorder {

namespace {

constexpr const char *kPath         = "fuego_packets.log";
constexpr const char *kOldPath      = "fuego_packets.old.log";
// "Hide passwords" recordings go to a file of their own: a log to share never holds an
// earlier recording's (or the earlier part of this one's) passwords.
constexpr const char *kSharePath    = "fuego_packets_share.log";
constexpr const char *kShareOldPath = "fuego_packets_share.old.log";
constexpr long        kRotateBytes = 64L * 1024 * 1024; // a bigger file is moved aside at the next start
constexpr uint32_t    kRosterEveryMs = 5 * 60 * 1000;   // who is online: at the start and this often
constexpr uint32_t    kFlushEveryMs  = 50;              // "everything": flushed this often, not per line

// Taps run on whatever thread sends (mostly the game thread), the incoming RPC
// dispatch, the chat box, the window's messages and the frame on the game thread:
// one lock around the timeline + file. It's a leaf lock -- nothing called while
// holding it takes another (the name lookups only read SA:MP's / GTA's memory).
std::mutex               g_mtx;
std::atomic<bool>        g_on{ false };
FILE                    *g_file = nullptr;
rec::Timeline            g_tl;
std::chrono::steady_clock::time_point g_t0;
unsigned long long       g_lines = 0;
std::string              g_keys = "none"; // for the pill, refreshed by the sync taps
uint32_t                 g_lastRoster = 0;
uint32_t                 g_lastFlush = 0;
int                      g_wheel = 0;     // mouse wheel steps not yet a whole notch

std::atomic<int>         g_detail{ kNormal };
std::atomic<bool>        g_hideSecrets{ false };
std::atomic<int>         g_dispatching{ -1 }; // the server RPC SA:MP is handling right now
// An RCON response just arrived (until this frame's end): SA:MP prints it in the chat
// box next -- a duplicate, and in a log to share it may hold the RCON password.
bool                     g_rconPending = false;
std::string              g_rconText;
const char              *g_openPath = kPath; // the file being written

uint32_t now_ms() {
	return (uint32_t) std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - g_t0 ).count();
}

rec::Options options_now() {
	rec::Options o;
	const int d = g_detail.load();
	o.others = d >= kOthers;
	o.everything = d >= kEverything;
	o.hideSecrets = g_hideSecrets.load();
	return o;
}

const char *detail_name( int d ) {
	return d >= kEverything ? "everything (every packet, raw bytes)" : d == kOthers ? "+ other players" : "normal";
}

void write_locked( const std::vector<std::string> &lines, bool flush = false ) {
	if ( !g_file || lines.empty() ) return;
	for ( const auto &l : lines ) {
		std::fputs( l.c_str(), g_file );
		std::fputc( '\n', g_file );
	}
	g_lines += lines.size();
	// A crash or Alt+F4 loses nothing -- except in "everything", where thousands of
	// lines a second are flushed together (at most kFlushEveryMs apart).
	const uint32_t now = now_ms();
	if ( flush || g_detail.load( std::memory_order_relaxed ) < kEverything || now - g_lastFlush >= kFlushEveryMs ) {
		std::fflush( g_file );
		g_lastFlush = now;
	}
}

// Names for the ids in each line, from SA:MP's pools (players / vehicles there now).
std::string player_name_of( int id ) {
	if ( id < 0 || id >= samp::SAMP_MAX_PLAYERS ) return std::string();
	return samp::player_name( id );
}

int vehicle_model_of( int id ) {
	if ( id <= 0 || id >= samp::SAMP_MAX_VEHICLES ) return -1;
	float dims[3] = {};
	int model = -1;
	return game::vehicle_dims_model( id, dims, model ) ? model : -1;
}

// SA:MP's own "a dialog is open" flag.
bool dialog_open() {
	const uint32_t base = samp::base();
	if ( !base ) return false;
	const uint8_t *dlg = *reinterpret_cast<const uint8_t *const *>( base + samp::addr::DIALOG_INFO );
	return dlg && *reinterpret_cast<const int *>( dlg + samp::addr::DIALOG_ACTIVE ) != 0;
}

// Our state, from the game (main thread, outside the lock).
rec::LocalState gather() {
	rec::LocalState s;
	samp::stSAMP *ng = samp::netgame();
	if ( !ng ) return s;
	s.gameState = ng->iGameState;
	s.id = samp::local_player_id();
	// From SA:MP's local player only the two flags read right after its sync data (the
	// auto fisher relies on them too); the rest comes from the game's own ped.
	if ( samp::stLocalPlayer *lp = samp::local_player() ) {
		s.spawned = lp->iIsActive != 0;
		s.dead = lp->iIsWasted != 0;
	}
	s.interior = game::local_interior();
	s.weapon = game::local_weapon_type();
	int types[13] = {};
	if ( game::local_weapons( types, s.ammo ) )
		for ( int i = 0; i < 13; ++i ) s.weapons[i] = (uint8_t) ( types[i] > 0 && types[i] < 256 ? types[i] : 0 );
	game::local_player_pos( s.pos );
	game::local_health_armor( s.health, s.armor );
	s.money = *reinterpret_cast<const int *>( 0xB7CE50 ); // CWorld::Players[0].m_nMoney
	s.skin = game::local_skin();
	const int seat = game::cc_local_seat();
	const int vid = seat >= 0 ? game::cc_local_vehicle_id() : -1; // (a car SA:MP doesn't know: on foot to us)
	if ( vid >= 0 ) {
		s.seat = seat;
		s.vehicle = vid;
		s.vehicleModel = vehicle_model_of( vid );
		s.vehicleHealth = game::local_vehicle_health();
	}
	s.chatInput = input::typing();
	s.dialogOpen = dialog_open();
	s.gtaMenu = input::gta_menu_active();
	s.fuegoMenu = menu::is_open();
	s.ping = ( g_RakClient && g_RakClient->GetInterface() ) ? g_RakClient->GetInterface()->GetLastPing() : -1;
	return s;
}

// Who is online (main thread, outside the lock).
std::vector<std::pair<int, std::string>> gather_roster() {
	std::vector<std::pair<int, std::string>> v;
	const int me = samp::local_player_id();
	for ( int id = 0; id < samp::SAMP_MAX_PLAYERS; ++id ) {
		if ( id != me && !samp::is_connected( id ) ) continue;
		std::string name = samp::player_name( id );
		if ( !name.empty() ) v.emplace_back( id, std::move( name ) );
	}
	return v;
}

// --- taps (read-only: never touch the streams) ---------------------------------

void tap_rpc( int id, BitStream *bs, bool injected ) {
	if ( !g_on.load( std::memory_order_relaxed ) || !bs ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.set_local_id( samp::local_player_id() );
		g_tl.outgoing_rpc( now_ms(), id, bs->GetData(), (size_t) bs->GetNumberOfBitsUsed(), injected, out );
		write_locked( out );
	} catch ( ... ) {} // inside RakNet's send path: nothing may escape
}

void tap_packet( const unsigned char *data, int bits, bool injected ) {
	if ( !g_on.load( std::memory_order_relaxed ) || !data || bits < 8 ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.outgoing_packet( now_ms(), data, (size_t) bits, injected, out );
		g_keys = g_tl.keys_now();
		write_locked( out );
	} catch ( ... ) {}
}

void tap_recv( const unsigned char *data, int bits ) {
	if ( !g_on.load( std::memory_order_relaxed ) || !data || bits < 8 ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.set_local_id( samp::local_player_id() );
		g_tl.incoming_packet( now_ms(), data, (size_t) bits, out );
		write_locked( out );
		if ( data[0] == 202 ) { // RCON response: u32 length + text
			g_rconPending = true;
			g_rconText.clear();
			rec::BitReader r( data + 1, (size_t) bits - 8 );
			r.str32( g_rconText );
		}
	} catch ( ... ) {} // inside RakNet's receive path
}

// Shift / Ctrl / Alt: left or right, by scancode / the extended bit.
unsigned split_modifier( unsigned vk, LPARAM lp ) {
	const UINT sc = ( (UINT) lp >> 16 ) & 0xFF;
	switch ( vk ) {
	case VK_SHIFT:   return sc == MapVirtualKeyA( VK_LSHIFT, 0 ) ? VK_LSHIFT : VK_RSHIFT;
	case VK_CONTROL: return ( lp & ( 1 << 24 ) ) ? VK_RCONTROL : VK_LCONTROL;
	case VK_MENU:    return ( lp & ( 1 << 24 ) ) ? VK_RMENU : VK_LMENU;
	default:         return vk;
	}
}

// Our real input, from the game window's messages. Presses typed into the chat box
// (they're in the chat line sent) or our menu (an API key, say) aren't written, nor --
// in a log to share -- into a dialog. Releases always go through: the timeline only
// writes the release of a press it wrote.
bool on_wndproc( HWND, UINT msg, WPARAM wp, LPARAM lp ) {
	if ( !g_on.load( std::memory_order_relaxed ) ) return false;
	try {
		unsigned vk = 0;
		bool down = false, isKey = true;
		switch ( msg ) {
		case WM_KEYDOWN: case WM_SYSKEYDOWN: vk = split_modifier( (unsigned) wp, lp ); down = true; break;
		case WM_KEYUP: case WM_SYSKEYUP:     vk = split_modifier( (unsigned) wp, lp ); break;
		case WM_LBUTTONDOWN: vk = VK_LBUTTON; down = true; break;
		case WM_LBUTTONUP:   vk = VK_LBUTTON; break;
		case WM_RBUTTONDOWN: vk = VK_RBUTTON; down = true; break;
		case WM_RBUTTONUP:   vk = VK_RBUTTON; break;
		case WM_MBUTTONDOWN: vk = VK_MBUTTON; down = true; break;
		case WM_MBUTTONUP:   vk = VK_MBUTTON; break;
		case WM_XBUTTONDOWN: vk = HIWORD( wp ) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; down = true; break;
		case WM_XBUTTONUP:   vk = HIWORD( wp ) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
		case WM_MOUSEWHEEL: case WM_ACTIVATEAPP: case WM_SETFOCUS: case WM_KILLFOCUS: isKey = false; break;
		default: return false;
		}
		if ( isKey && down && ( input::typing() || menu::is_open() || ( g_hideSecrets.load() && dialog_open() ) ) )
			return false;

		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return false;
		std::vector<std::string> out;
		const uint32_t now = now_ms();
		if ( isKey ) {
			g_tl.input( now, vk, down, out );
		} else if ( msg == WM_MOUSEWHEEL ) {
			if ( menu::is_open() ) return false;
			g_wheel += GET_WHEEL_DELTA_WPARAM( wp );
			const int notches = g_wheel / WHEEL_DELTA;
			g_wheel -= notches * WHEEL_DELTA;
			g_tl.wheel( now, notches, out );
		} else {
			g_tl.focus( now, msg == WM_SETFOCUS || ( msg == WM_ACTIVATEAPP && wp ), out );
		}
		write_locked( out );
	} catch ( ... ) {} // inside the window procedure: nothing may escape
	return false; // only watches: never takes a message from the game
}

void frame( IDirect3DDevice9 * ) { // our state, the roster, the 10-second traffic summary
	if ( !g_on.load( std::memory_order_relaxed ) ) return;
	try {
		const rec::LocalState st = gather();
		std::vector<std::pair<int, std::string>> roster;
		bool rosterDue = false;
		{
			std::lock_guard<std::mutex> lk( g_mtx );
			rosterDue = g_file && now_ms() - g_lastRoster >= kRosterEveryMs;
		}
		if ( rosterDue ) roster = gather_roster();

		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		const uint32_t now = now_ms();
		g_tl.set_local_id( st.id );
		g_tl.state( now, st, out ); // (connecting, reconnecting, ... too: the game state is in it)
		if ( rosterDue ) {
			g_tl.roster( now, roster, out );
			g_lastRoster = now;
		}
		g_tl.tick( now, out );
		write_locked( out, /*flush=*/true );
		g_rconPending = false; // its chat line came (or never will)
	} catch ( ... ) {}
}

std::string printable( const char *s ) { // a header field from the server: keep it on one line
	std::string r;
	for ( ; s && *s; ++s ) r += ( (unsigned char) *s >= 0x20 && *s != 0x7F ) ? *s : '?';
	return r;
}

} // namespace

void install() {
	net::set_wire_taps( tap_rpc, tap_packet );
	net::set_recv_tap( tap_recv );
	render::on_frame( frame );
	render::on_wndproc( on_wndproc );
}

namespace {

// Closes the file and turns the recorder off (after a failure mid-start/stop).
void abort_locked() {
	g_on.store( false );
	if ( g_file ) {
		std::fclose( g_file );
		g_file = nullptr;
	}
}

bool start_locked() {
	if ( g_file ) return true;

	const bool share = g_hideSecrets.load();
	const char *path = share ? kSharePath : kPath, *oldPath = share ? kShareOldPath : kOldPath;
	if ( FILE *f = std::fopen( path, "rb" ) ) { // keep it from growing forever
		std::fseek( f, 0, SEEK_END );
		const long size = std::ftell( f );
		std::fclose( f );
		if ( size > kRotateBytes ) {
			std::remove( oldPath );
			std::rename( path, oldPath );
		}
	}
	g_file = std::fopen( path, "ab" );
	if ( !g_file ) {
		Log( "[rec] can't open %s.", path );
		return false;
	}
	g_openPath = path;
	g_rconPending = false;

	g_t0 = std::chrono::steady_clock::now();
	g_tl.reset( 0 );
	g_tl.set_local_id( samp::local_player_id() );
	g_tl.set_options( options_now() );
	g_tl.set_name_lookups( player_name_of, vehicle_model_of );
	g_lines = 0;
	g_keys = "none";
	g_lastRoster = 0 - kRosterEveryMs; // the first frame writes it
	g_lastFlush = 0;
	g_wheel = 0;

	char when[64] = "?";
	const std::time_t t = std::time( nullptr );
	std::tm lt{};
	if ( localtime_s( &lt, &t ) == 0 ) std::strftime( when, sizeof when, "%Y-%m-%d %H:%M:%S", &lt );
	std::vector<std::string> head;
	head.push_back( "" );
	head.push_back( std::string( "==== recording started " ) + when + " ====" );
	if ( samp::stSAMP *ng = samp::netgame() ) {
		char line[700];
		std::snprintf( line, sizeof line, "server: \"%s\" %s:%u | my player id: %d",
		               printable( ng->szHostname ).c_str(), printable( ng->szIP ).c_str(), (unsigned) ng->ulPort,
		               samp::local_player_id() );
		head.push_back( line );
	}
	head.push_back( std::string( "detail: " ) + detail_name( g_detail.load() ) +
	                ( g_hideSecrets.load() ? " | passwords, login arguments, RCON, auth key, anti-cheat answers: HIDDEN"
	                                       : " | everything typed is written as is, passwords too (Hide passwords is off)" ) );
	head.push_back( "legend: time = seconds since the start | OUT = we sent it, as the server gets it | "
	                "OUT [fuego] = sent by a cheat | IN = the server sent it | CHATBOX = a chat-box line that "
	                "isn't a server chat message | KEY = real keyboard/mouse | STATE = us (full every 30 s, "
	                "changes at once) | FOCUS = alt-tab | PLAYERS = who is online (id name)" );
	head.push_back( "        keys: +KEY pressed, -KEY released (held time) -- FIRE(attack) HANDBRAKE(aim) "
	                "SPRINT(space) JUMP(shift) CROUCH(C) WALK(alt); ud W/S, lr A/D" );
	head.push_back( "        TD = textdraw (#2048+ are per-player); a progress bar is usually a box re-shown "
	                "with a new size. Players are \"player 9 (Name)\", vehicles \"vehicle 412 (Voodoo)\". "
	                "What isn't written is counted in the 10 s summaries." );
	write_locked( head, true );
	g_lines = 0; // the header isn't counted
	g_on.store( true );
	Log( "[rec] recording to %s (detail: %s%s).", path, detail_name( g_detail.load() ),
	     g_hideSecrets.load() ? ", secrets hidden" : "" );
	return true;
}

void stop_locked( const char *why = nullptr ) {
	if ( !g_file ) return;
	g_on.store( false );
	std::vector<std::string> out;
	const uint32_t now = now_ms();
	g_tl.tick( now, out, /*force=*/true );
	if ( why ) g_tl.note( now, why, out );
	char line[160];
	std::snprintf( line, sizeof line, "==== recording stopped after %u s: %llu lines, %d marks ====",
	               now / 1000, g_lines + out.size(), g_tl.marks() );
	out.push_back( line );
	write_locked( out, true );
	std::fclose( g_file );
	g_file = nullptr;
	Log( "[rec] stopped (%llu lines).", g_lines );
}

// A setting changed: the timeline follows at once, and the log says so.
void options_changed( const std::string &what ) {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		g_tl.set_options( options_now() );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.note( now_ms(), what, out );
		write_locked( out, true );
	} catch ( ... ) {}
}

} // namespace

// The entry points below run from a chat command (SA:MP's command dispatch) or the
// menu (inside EndScene): nothing may escape them, not even an out-of-memory.
bool start() {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		try { return start_locked(); }
		catch ( ... ) { abort_locked(); return false; }
	} catch ( ... ) { return false; }
}

void stop() {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		try { stop_locked(); }
		catch ( ... ) { abort_locked(); }
	} catch ( ... ) {}
}

bool active() { return g_on.load(); }

void set_detail( int d ) {
	d = d < kNormal ? kNormal : d > kEverything ? kEverything : d;
	if ( g_detail.exchange( d ) == d ) return;
	options_changed( std::string( "detail: " ) + detail_name( d ) );
}

int detail() { return g_detail.load(); }

bool set_hide_secrets( bool on ) {
	if ( g_hideSecrets.exchange( on ) == on ) return true;
	// Mid-recording the recording moves to the other file: the one to share then never
	// holds a secret, and the normal one never gets a hole in it.
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		g_tl.set_options( options_now() );
		if ( !g_file ) return true;
		const char *next = on ? kSharePath : kPath;
		const std::string why = std::string( on ? "passwords hidden from here on: continued in " : "passwords written again: continued in " ) + next;
		try {
			stop_locked( why.c_str() );
			return start_locked();
		} catch ( ... ) { abort_locked(); return false; }
	} catch ( ... ) { return false; }
}

bool hide_secrets() { return g_hideSecrets.load(); }

void mark( const char *note ) {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.mark( now_ms(), note ? note : "", out );
		write_locked( out, true );
	} catch ( ... ) {}
}

int marks() {
	std::lock_guard<std::mutex> lk( g_mtx );
	return g_tl.marks();
}

unsigned long long lines() {
	std::lock_guard<std::mutex> lk( g_mtx );
	return g_lines;
}

unsigned seconds() {
	std::lock_guard<std::mutex> lk( g_mtx );
	return g_file ? now_ms() / 1000 : 0;
}

const char *file_name() {
	std::lock_guard<std::mutex> lk( g_mtx );
	return g_file ? g_openPath : g_hideSecrets.load() ? kSharePath : kPath;
}

void on_incoming_rpc( unsigned char id, const RPCParameters *rpc ) {
	g_dispatching.store( id, std::memory_order_relaxed );
	if ( !g_on.load( std::memory_order_relaxed ) || !rpc ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.set_local_id( samp::local_player_id() );
		g_tl.incoming_rpc( now_ms(), id, rpc->input, (size_t) rpc->numberOfBitsOfData, out );
		write_locked( out );
	} catch ( ... ) {} // inside SA:MP's RPC dispatch: nothing may escape
}

void rpc_dispatch_end() { g_dispatching.store( -1, std::memory_order_relaxed ); }

void on_chat_line( int type, const char *text, const char *prefix ) {
	if ( !g_on.load( std::memory_order_relaxed ) || !text ) return;
	const int rpc = g_dispatching.load( std::memory_order_relaxed );
	if ( rpc == 93 || rpc == 101 ) return; // a server chat message: written as its RPC
	try {
		std::string line = ( prefix && *prefix ) ? std::string( prefix ) + ": " + text : std::string( text );
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		// SA:MP printing the RCON response that just came: written as the packet already
		// (in a log to share, nothing at all goes in until this frame is over).
		if ( g_rconPending && ( g_hideSecrets.load() || line == g_rconText ) ) return;
		std::vector<std::string> out;
		g_tl.chat_line( now_ms(), type, line, out );
		write_locked( out );
	} catch ( ... ) {} // inside SA:MP's chat: nothing may escape
}

void draw_overlay() {
	if ( !g_on.load( std::memory_order_relaxed ) ) return;
	try {
		std::string keys;
		unsigned secs = 0;
		{
			std::lock_guard<std::mutex> lk( g_mtx );
			if ( !g_file ) return;
			keys = g_keys;
			secs = now_ms() / 1000;
		}
		const int d = g_detail.load();
		char text[300];
		std::snprintf( text, sizeof text, "REC%s%s %u:%02u   keys: %s   (/.mark note, /.rec stops)",
		               d == kOthers ? " +others" : d >= kEverything ? " ALL" : "", g_hideSecrets.load() ? " (no passwords)" : "",
		               secs / 60, secs % 60, keys.c_str() );

		ImDrawList *dl = ImGui::GetForegroundDrawList(); // the HUD layer: over SA:MP's chat
		const ImVec2 sz = ImGui::CalcTextSize( text );
		const float r = ImGui::GetFontSize() * 0.3f;
		const float x = ( ImGui::GetIO().DisplaySize.x - sz.x ) * 0.5f + r;
		const float y = ImGui::GetIO().DisplaySize.y * 0.015f;
		dl->AddRectFilled( ImVec2( x - 2 * r - 14, y - 4 ), ImVec2( x + sz.x + 8, y + sz.y + 4 ), IM_COL32( 0, 0, 0, 170 ), 5.0f );
		// The dot blinks once a second, like a camera's.
		if ( ( GetTickCount() / 500 ) % 2 == 0 )
			dl->AddCircleFilled( ImVec2( x - r - 6, y + sz.y * 0.5f ), r, IM_COL32( 255, 60, 60, 255 ) );
		dl->AddText( ImVec2( x, y ), IM_COL32( 255, 225, 225, 255 ), text );
	} catch ( ... ) {} // inside the frame: nothing may escape
}

} // namespace recorder
