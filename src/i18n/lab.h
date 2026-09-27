#pragma once
// Debug lab: verify the in-place CHAT and DIALOG replacement mechanisms in-game
// before the real translate-and-replace feature is built on them. All behavior is
// gated by config.debug_lab (read-only logging) / config.debug_lab_replace (the
// live experiment that actually rewrites a shown chat line + re-shows a dialog).
// Offsets/addresses were confirmed by disassembly of the user's samp.dll (0.3.DL).

struct RPCParameters; // net/RakClient.h

namespace i18n::lab {

// Incoming-RPC seam (called from compat::HandleRPCPacketFunc). Decodes + logs
// ShowDialog (RPC 61) fields and, when debug_lab_replace is on, queues a re-show
// experiment. Read-only otherwise; never modifies the packet.
void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// Main-thread per-frame tick (called from hooks::pump_tick). Watches/logs the
// bottom chat entry, and fires the queued chat-flip + dialog-re-show experiments
// (both must run on the game/render thread).
void on_frame();

} // namespace i18n::lab
