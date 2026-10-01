#include "recorder.h"

#include "timeline.h"

#include "../net/net.h"
#include "../net/RakClient.h" // RPCParameters
#include "../render/render.h"
#include "../samp/samp.h"
#include "../samp/pools.h"   // local_player_id
#include "../samp/structs.h" // stSAMP (server name / address for the header)
#include "../core/logger.h"

#include "imgui.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace recorder {

namespace {

constexpr const char *kPath    = "fuego_packets.log";
constexpr const char *kOldPath = "fuego_packets.old.log";
constexpr long        kRotateBytes = 64L * 1024 * 1024; // a bigger file is moved aside at the next start

// Taps run on whatever thread sends (mostly the game thread) and the incoming RPC
// dispatch on the game thread: one lock around the timeline + file. It's a leaf lock
// -- nothing called while holding it takes another.
std::mutex               g_mtx;
std::atomic<bool>        g_on{ false };
FILE                    *g_file = nullptr;
rec::Timeline            g_tl;
std::chrono::steady_clock::time_point g_t0;
unsigned long long       g_lines = 0;
std::string              g_keys = "none"; // for the pill, refreshed by the sync taps

uint32_t now_ms() {
	return (uint32_t) std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - g_t0 ).count();
}

void write_locked( const std::vector<std::string> &lines ) {
	if ( !g_file || lines.empty() ) return;
	for ( const auto &l : lines ) {
		std::fputs( l.c_str(), g_file );
		std::fputc( '\n', g_file );
	}
	std::fflush( g_file ); // a crash or Alt+F4 loses nothing
	g_lines += lines.size();
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

void frame( IDirect3DDevice9 * ) { // the 10-second traffic summary
	if ( !g_on.load( std::memory_order_relaxed ) ) return;
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.tick( now_ms(), out );
		write_locked( out );
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
	render::on_frame( frame );
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

	if ( FILE *f = std::fopen( kPath, "rb" ) ) { // keep it from growing forever
		std::fseek( f, 0, SEEK_END );
		const long size = std::ftell( f );
		std::fclose( f );
		if ( size > kRotateBytes ) {
			std::remove( kOldPath );
			std::rename( kPath, kOldPath );
		}
	}
	g_file = std::fopen( kPath, "ab" );
	if ( !g_file ) {
		Log( "[rec] can't open %s.", kPath );
		return false;
	}

	g_t0 = std::chrono::steady_clock::now();
	g_tl.reset( 0 );
	g_tl.set_local_id( samp::local_player_id() );
	g_lines = 0;
	g_keys = "none";

	char when[64] = "?";
	const std::time_t t = std::time( nullptr );
	std::tm lt{};
	if ( localtime_s( &lt, &t ) == 0 ) std::strftime( when, sizeof when, "%Y-%m-%d %H:%M:%S", &lt );
	std::vector<std::string> head;
	head.push_back( "" );
	head.push_back( std::string( "==== packet recording started " ) + when + " ====" );
	if ( samp::stSAMP *ng = samp::netgame() ) {
		char line[700];
		std::snprintf( line, sizeof line, "server: \"%s\" %s:%u | my player id: %d",
		               printable( ng->szHostname ).c_str(), printable( ng->szIP ).c_str(), (unsigned) ng->ulPort,
		               samp::local_player_id() );
		head.push_back( line );
	}
	head.push_back( "legend: time = seconds since the start | OUT = we sent it, as the server gets it | "
	                "OUT [fuego] = sent by a cheat | IN = the server sent it" );
	head.push_back( "        keys: +KEY pressed, -KEY released (held time) -- FIRE(attack) HANDBRAKE(aim) "
	                "SPRINT(space) JUMP(shift) CROUCH(C) WALK(alt); ud W/S, lr A/D" );
	head.push_back( "        TD = textdraw (#2048+ are per-player); a progress bar is usually a box re-shown "
	                "with a new size. Other players' traffic is only counted in the 10 s summaries." );
	head.push_back( "        never written: dialog input text, login-type command arguments, RCON, "
	                "client-check answers" );
	write_locked( head );
	g_lines = 0; // the header isn't counted
	g_on.store( true );
	Log( "[rec] recording to %s.", kPath );
	return true;
}

void stop_locked() {
	if ( !g_file ) return;
	g_on.store( false );
	std::vector<std::string> out;
	const uint32_t now = now_ms();
	g_tl.tick( now, out, /*force=*/true );
	char line[160];
	std::snprintf( line, sizeof line, "==== recording stopped after %u s: %llu lines, %d marks ====",
	               now / 1000, g_lines + out.size(), g_tl.marks() );
	out.push_back( line );
	write_locked( out );
	std::fclose( g_file );
	g_file = nullptr;
	Log( "[rec] stopped (%llu lines).", g_lines );
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

void mark( const char *note ) {
	try {
		std::lock_guard<std::mutex> lk( g_mtx );
		if ( !g_file ) return;
		std::vector<std::string> out;
		g_tl.mark( now_ms(), note ? note : "", out );
		write_locked( out );
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

const char *file_name() { return kPath; }

void on_incoming_rpc( unsigned char id, const RPCParameters *rpc ) {
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
		char text[256];
		std::snprintf( text, sizeof text, "REC %u:%02u   keys: %s   (/.mark note, /.rec stops)", secs / 60, secs % 60,
		               keys.c_str() );

		ImDrawList *dl = ImGui::GetBackgroundDrawList();
		const ImVec2 sz = ImGui::CalcTextSize( text );
		const float r = ImGui::GetFontSize() * 0.3f;
		const float x = ( ImGui::GetIO().DisplaySize.x - sz.x ) * 0.5f + r;
		const float y = ImGui::GetIO().DisplaySize.y * 0.015f;
		dl->AddRectFilled( ImVec2( x - 2 * r - 14, y - 4 ), ImVec2( x + sz.x + 8, y + sz.y + 4 ), IM_COL32( 0, 0, 0, 170 ), 5.0f );
		// The dot blinks once a second, like a camera's.
		if ( ( GetTickCount() / 500 ) % 2 == 0 )
			dl->AddCircleFilled( ImVec2( x - r - 6, y + sz.y * 0.5f ), r, IM_COL32( 255, 60, 60, 255 ) );
		dl->AddText( ImVec2( x, y ), IM_COL32( 255, 225, 225, 255 ), text );
	} catch ( ... ) {} // inside EndScene: nothing may escape
}

} // namespace recorder
