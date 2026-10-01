#pragma once
/*
	recorder/recorder.h  --  the packet tracer + state recorder (debug tool).

	Writes everything that crosses the wire, decoded and time-stamped, to
	fuego_packets.log in the GTA folder -- with every chat-box line, our real key /
	mouse input and our own state around it -- to debug, reverse-engineer or build
	things for any server later. See timeline.h for what each line holds.

	Started/stopped with /.rec in chat or from the menu (Main tab); /.mark <note>
	writes a numbered note at that moment. Runtime only (recording, detail level and
	"hide passwords"): never saved in the config, so it can't be left recording by
	accident from one game start to the next. While it runs, a red REC pill at the top
	of the screen shows it, with the keys we're sending right now.
*/

struct RPCParameters;

namespace recorder {

// The net taps, the chat/input feeds + the per-frame state. Call ONCE during
// bring-up, before net::install() / render::install() (they append to their lists).
void install();

bool start(); // false if the file can't be opened
void stop();
bool active();

// How much is written: 0 normal (us, the server, chat), 1 + other players' actions and
// syncs, 2 everything (every packet, raw bytes too). Applies at once, also mid-recording.
enum Detail { kNormal = 0, kOthers = 1, kEverything = 2 };
void set_detail( int detail );
int  detail();
// For a log to share: passwords and what's typed into dialogs, login-type command
// arguments, RCON, the join packet's auth key and anti-cheat answers left out -- and
// written to a file of its own (file_name()). Switched while recording, the recording
// carries on in the other file; false if that file couldn't be opened (stopped then).
bool set_hide_secrets( bool on );
bool hide_secrets();

void mark( const char *note ); // a numbered note line (no-op when not recording)
int  marks();                  // marks written in this recording

unsigned long long lines(); // lines written in this recording
unsigned seconds();         // since it started
const char *file_name();

// compat's incoming-RPC dispatch, before anything else reads or changes it ...
void on_incoming_rpc( unsigned char id, const RPCParameters *rpc );
// ... and once SA:MP's own handler is done with it (or it was dropped).
void rpc_dispatch_end();

// Every line added to the chat box (the i18n AddEntry hook, before any translation).
// Lines SA:MP adds while handling a server chat RPC aren't written again.
void on_chat_line( int type, const char *text, const char *prefix );

// The REC pill, inside the ImGui frame (main thread).
void draw_overlay();

} // namespace recorder
