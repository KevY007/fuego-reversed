#pragma once
/*
	recorder/recorder.h  --  the packet recorder (debug tool).

	Writes everything that crosses the wire, decoded and time-stamped, to
	fuego_packets.log in the GTA folder -- to work out how a server's minigame runs
	(what it shows, what it reacts to, which keys and how long). See timeline.h for
	what is written and what never is.

	Started/stopped with /.rec in chat or from the menu (Main tab); /.mark <note>
	writes a numbered note at that moment. Runtime only: never saved in the config,
	so it can't be left recording by accident from one game start to the next. While
	it runs, a red REC pill at the top of the screen shows it, with the keys we're
	sending right now.
*/

struct RPCParameters;

namespace recorder {

// The net taps + the per-frame summary. Call ONCE during bring-up, before
// net::install() / render::install() (they append to their callback lists).
void install();

bool start(); // false if the file can't be opened
void stop();
bool active();

void mark( const char *note ); // a numbered note line (no-op when not recording)
int  marks();                  // marks written in this recording

unsigned long long lines(); // lines written in this recording
unsigned seconds();         // since it started
const char *file_name();

// compat's incoming-RPC dispatch, before anything else reads or changes it.
void on_incoming_rpc( unsigned char id, const RPCParameters *rpc );

// The REC pill, inside the ImGui frame (main thread).
void draw_overlay();

} // namespace recorder
