#pragma once
// i18n/surfaces.h -- translate incoming non-chat text surfaces (dialogs, 3D text
// labels) in place, before the game processes the RPC.
//
// Approach (translation-memory-first, so the synchronous RPC hook never blocks on
// the network): on each dialog/label RPC we decode its text; if a translation is
// already cached we re-encode the packet with the translated text; if not, we
// queue a background fill job and pass the ORIGINAL through this time. The next
// identical dialog/label (they repeat) then hits the cache and shows translated.
//
// The packet-rebuild functions below are PURE (byte buffers + a translator
// callback, no game types) so they are host-tested byte-exact against real
// captured packets (tests/surfaces_test.cpp): with an identity translator the
// rebuilt packet must equal the original, proving the layout + Huffman re-encode
// can't corrupt a dialog the game then parses.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct RPCParameters;

namespace i18n::surfaces {

// translator(src, out): true if a translation for `src` is available (fills
// `out`); false on a miss. `src`/`out` are raw display-codepage bytes.
using Translator = std::function<bool(const std::string& src, std::string& out)>;

// Rebuild a ShowDialog (RPC 61) packet with translated title/buttons/body. Returns
// true and fills `out` only if EVERY translatable piece resolved via `tr` (so the
// caller shows the original until all pieces are cached). Returns false otherwise.
// Layout: u16 dialogId, u8 style, u8+title, u8+btn1, u8+btn2, CSTRING szInfo.
bool rebuild_dialog(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// Rebuild a Create3DTextLabel (RPC 36) packet with translated text. 27-byte fixed
// header then a CSTRING. Returns true/`out` only on a translation hit.
bool rebuild_label(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// Called from the incoming RPC-dispatch chokepoint (compat::HandleRPCPacketFunc).
// No-op unless config.translate_dialogs is on. Modifies rpc in place on a cache
// hit; queues a fill + leaves it unchanged on a miss. Main/net thread.
void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

} // namespace i18n::surfaces
