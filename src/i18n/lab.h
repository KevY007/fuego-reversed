#pragma once
// Debug lab (read-only): with config.debug_lab on, log the newest chat entry and
// every decoded ShowDialog (id/style/title/buttons/body) to fuego_reversed.log --
// handy when checking the in-place chat/dialog translation against a live server.

struct RPCParameters; // net/RakClient.h

namespace i18n::lab {

// Incoming-RPC seam (compat::HandleRPCPacketFunc): logs ShowDialog (61) fields.
void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// Main-thread per-frame tick (hooks::pump_tick): logs each new bottom chat line.
void on_frame();

} // namespace i18n::lab
