#pragma once
/*
	compat/compat.h  --  0.3.DL <-> 0.3.7 compatibility layer.

	Lets the 0.3.DL client connect to 0.3.7 servers. Two halves:
	  * OUTGOING (via the net seam): spoof the ClientJoin handshake as 0.3.7 and
	    suppress DL-only download RPCs while in 0.3.7 mode.
	  * INCOMING (via a CDetour on SA:MP's RPC dispatch): re-insert the DL custom-
	    model UINT32 into the 5 skin RPCs a 0.3.7 server sends without it.

	Inert by default (ForceCompat = 0.3.DL): nothing is intercepted until the user
	switches to 0.3.7 mode. The DL client is the wire-format authority.
*/

#include <cstdint>

namespace compat {

// 0.3.DL-R1 vs 0.3.7-R5 (spoofed). ForceCompat drives everything.
constexpr uint32_t VER_037   = 4057;
constexpr uint32_t VER_03DL  = 4062;

bool in_037_mode();          // true when spoofing 0.3.7
void set_037_mode( bool on );// used by the /compat command later
void toggle_mode();

// Install the incoming RPC-dispatch hook + CNetGame destructor restore and
// register the outgoing net-seam handler. Call after the net hook is active.
void install();

} // namespace compat
