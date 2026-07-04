#pragma once
/*
	cheats/server.h  --  server-specific automations ported 1:1 from Project Fuego.
	  * LSRP gun stealer  (say-command sequencer with ping-based timing)
	  * LSRP auto-unscrambler (reads the unscramble textdraw, anagram-solves, /uns)
*/

namespace server {

// Start the LSRP gun-stealer on a streamed vehicle. forceAdjust runs /adjustgun to
// reset gun positions (trunk guns too); respawn suicides afterwards to keep the guns.
void lsrp_stealguns( int vid, bool forceAdjust, bool respawn );

// Per-frame driver for the server automations (called from cheats::on_frame).
void tick();

// True while the gun-stealer is running (cheats freeze our sync + compat swallows
// the server's remove-from-car RPCs so the steal isn't interrupted).
bool stealguns_active();

// Re-read the auto-unscrambler wordlist from disk (cmd_autouns_reload).
void reload_wordlist();

// Force-adjust object tracking (driven from the incoming-RPC seam in compat). track/clear/
// untrack follow the vehicle-attached weapon objects; on_editobject re-emits them onto the
// ped + advances the steal state, returning true to swallow the server's RPC_EditObject.
void stealguns_track_object( int objId, int vid );
void stealguns_clear_vehicle( int vid );
void stealguns_untrack_object( int objId );
bool stealguns_on_editobject( bool playerObject );

} // namespace server
