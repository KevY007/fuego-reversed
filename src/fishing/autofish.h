#pragma once
/*
	fishing/autofish.h  --  Mike's CnR auto fish (runtime glue for fish::Fisher).

	Feeds the fisher what the server says (chat, our animations, textdraw text, our
	rod, dialogs) and puts the keys it wants (AIM / FIRE / SPRINT) into our on-foot
	syncs: the game's own syncs are rewritten while it's on, and a sync is sent the
	moment a key changes, so taps reach the server with the timing the fisher chose.
	While it's on, those three keys are the fisher's: a stray real press can't spoil a
	cast or damage the line.

	Toggle: the menu (Misc Functions) or /.autofish. Never saved (a bot must not start
	by itself) and there's no hotkey. It stops by itself -- with a chat line saying why
	-- when you move away from the spot, get in a vehicle, die, go PANIC, reconnect,
	or when the server stops answering it.
*/

struct RPCParameters;

namespace autofish {

// Net handlers + the per-frame driver. Once during bring-up, before net::install()
// / render::install().
void install();

bool enabled();
void set_enabled( bool on ); // with a chat line saying so

// compat's incoming-RPC dispatch (read-only).
void on_incoming_rpc( unsigned char id, const RPCParameters *rpc );

// The status pill while it's on (inside the ImGui frame).
void draw_overlay();

// For the menu: what it's doing, and the catch so far.
const char *status_text();
const char *stats_text();

} // namespace autofish
