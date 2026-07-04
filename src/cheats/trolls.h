#pragma once
/*
	cheats/trolls.h  --  vehicle-sync trolls (ported 1:1 from Project Fuego).

	Both steal the target's driver seat (RPC_EnterVehicle) and send a spoofed
	ID_VEHICLE_SYNC so the server flings/stucks/burns their car.
*/

// stInCarData is in samp/structs.h (namespace samp) -- fwd-declared for the rewrite below.
namespace samp { struct stInCarData; }

namespace trolls {
void fuckup( int sampId );
void itroll( int sampId, int mode ); // 1=ground 2=speed 3=throw-down 4=throw-up 5=spin 6=ocean 7=lightspeed

// Continuous in-vehicle iTroll (Fuego's I-key path): rewrite our live outgoing vehicle sync
// into the target's car + the mode mutation, in place. Returns false if it shouldn't apply.
bool itroll_incar_rewrite( int sampId, int mode, samp::stInCarData &d );

// Per-frame release for the passenger-hold -> driver-sync flow (call from on_frame).
void tick();
}
