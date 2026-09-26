#pragma once
// i18n/capture.h -- read-only debug logging of incoming server-text RPCs.
//
// When config.debug_capture is on, on_incoming_rpc() dumps the raw bytes (and
// the easily-decoded plain fields) of the dialog/3D-label/textdraw/gametext RPCs
// to fuego_reversed.log, so their exact wire format -- especially the Huffman-
// compressed dialog szInfo / label text blobs -- can be verified against a real
// server before those translation surfaces are built. It NEVER modifies packets.
// Called from the incoming RPC-dispatch chokepoint (compat::HandleRPCPacketFunc).

struct RPCParameters;

namespace i18n::capture {

void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

} // namespace i18n::capture
