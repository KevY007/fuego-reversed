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

// --- plain-text HUD surfaces (no Huffman) ----------------------------------
// True if `text` (ignoring ~x~/{RRGGBB} codes + spaces) is mostly digits/symbols
// -- a timer/counter/stat where translating is pointless (numbers are universal).
// Used to auto-skip dynamic textdraws so they don't spam the translator.
bool is_dynamic_text(const std::string& text);

// ShowGameText (73): u32 style, u32 time, u32 msgLen, char[] message.
bool rebuild_gametext(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// ChatBubble (59): u16 pid, u32 color, f dist, u32 expire, u8 len, char[] text.
bool rebuild_chatbubble(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// TextDrawSetString (105): u16 id, u16 len, char[] text.
bool rebuild_textdraw_setstring(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// ShowTextDraw (134): 65-byte fixed header, u16 szTextLen, char[] szText (last field).
bool rebuild_textdraw_show(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// Called from the incoming RPC-dispatch chokepoint (compat::HandleRPCPacketFunc).
// No-op unless config.translate_dialogs is on. Modifies rpc in place on a cache
// hit; queues a fill + leaves it unchanged on a miss. Main/net thread.
void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// Register the outbound DialogResponse (RPC 62) reverse-map handler. Call ONCE at
// startup, BEFORE net::install() (like outbound::install). When a translated LIST
// dialog row is selected, the client would send the TRANSLATED row text; this maps
// it back to the ORIGINAL so text-matching servers still recognise the selection.
void install();

} // namespace i18n::surfaces
