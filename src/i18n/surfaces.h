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
namespace i18n::engine { struct Result; }

namespace i18n::surfaces {

// translator(src, out): true if a translation for `src` is available (fills
// `out`); false on a miss. `src`/`out` are raw display-codepage bytes.
using Translator = std::function<bool(const std::string& src, std::string& out)>;

// --- cell-safe dialog translation (pure) ------------------------------------
// A dialog is translated UNIT BY UNIT, never as one blob: units[0] = title,
// units[1] = button 1, units[2] = button 2, units[3..] = the body split on EVERY
// '\n' and '\t' (bodySeps[k] = the separator that followed body unit k). A model
// can therefore never add, drop or move a row/column separator -- rejoining is
// byte-exact by construction. Each unit's leading spaces + {RRGGBB} codes
// (prefix) and trailing spaces (suffix) are re-attached verbatim; only `core` is
// ever translated.
struct Unit {
    std::string prefix;
    std::string core;
    std::string suffix;
    bool        translate = false; // false -> core kept verbatim
};

struct DialogPlan {
    uint16_t             id = 0;
    uint8_t              style = 0;      // 0 msgbox 1 input 2 list 3 password 4 tablist 5 tablist+headers
    bool                 haveBody = false;
    std::vector<uint8_t> rawBody;        // undecodable body bytes, kept verbatim
    std::vector<Unit>    units;
    std::vector<char>    bodySeps;
};

// A LIST/TABLIST cell that must stay verbatim: empty, numbers/units/timers
// ("12m", "1.15km", "$500"), or a single ASCII token that is all-lowercase or
// carries a digit, '/' or '_' -- shortcodes/commands the player types ("intro",
// "/help", "cmd_2"). Capitalised words ("Withdraw", "No") and phrases are labels
// and DO get translated; so is any non-Latin (e.g. cp1251) word.
bool is_verbatim_cell(const std::string& core);

// Split a raw piece into prefix/core/suffix. `cell` selects the list-cell rule;
// otherwise a unit is translated unless empty or mostly numeric.
Unit make_unit(const std::string& raw, bool cell);

// Parse a ShowDialog (RPC 61) payload into units. False if malformed.
bool plan_dialog(const uint8_t* pkt, size_t n, DialogPlan& out);

// The cores that reproduce the packet unchanged (unit.core for every unit).
std::vector<std::string> original_cores(const DialogPlan& plan);

// A translated core with '\t' / '\n' turned into spaces and '\r' dropped, so a
// model's stray separator can never break the dialog's row/column structure.
std::string clean_translation(const std::string& s);

// prefix + core + suffix, using the ORIGINAL core when the unit isn't
// translatable or `core` equals it (keeps identity byte-exact), else the
// cleaned translated core.
std::string unit_text(const Unit& u, const std::string& core);

// The body text from units[3..] + bodySeps with the given cores.
std::string assemble_body(const DialogPlan& plan, const std::vector<std::string>& cores);

// Re-encode a full RPC 61 payload. `cores` is indexed like plan.units (a missing
// entry falls back to the original core).
std::vector<uint8_t> assemble_dialog(const DialogPlan& plan, const std::vector<std::string>& cores);

// Remove {RRGGBB} colour codes (SA:MP strips them from a list row's response text).
std::string strip_colors(const std::string& s);

// A DialogResponse from a TRANSLATED list/tablist dialog carries text SA:MP derived
// from the TRANSLATED row the player picked. Rebuild the text the ORIGINAL dialog
// would have produced for the same row: find which derivation (whole row / first
// column, colour-stripped or raw) turns trans[row] into `resp` and apply it to
// orig[row]. row = listItem (+1 for TABLIST_HEADERS, whose row 0 is the header).
// False if out of range or nothing matches (the caller then sends `resp` unchanged).
bool remap_list_response(int style, const std::vector<std::string>& orig,
                         const std::vector<std::string>& trans, int listItem,
                         const std::string& resp, std::string& out);

// An outgoing dialog-input translation finished (engine surface 4). Main thread.
void on_response_result(const i18n::engine::Result& r);

// A dialog that must never be translated or held: a PASSWORD dialog (style 3) or
// the server's CLOSE signal (dialog id < 0 as int16, i.e. 0x8000..0xFFFF).
bool never_translated(const DialogPlan& d);

// Rebuild a ShowDialog (RPC 61) packet translating it UNIT BY UNIT (plan_dialog):
// `tr` receives each translatable unit's bare core. Returns true and fills `out`
// only if EVERY translatable unit resolved via `tr` (so the caller shows the
// original until all units are cached). Returns false otherwise.
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
// ShowDialog (translate_dialogs): fully cached + nothing held -> rewritten in
// place and shown now (returns false). Otherwise HELD: returns true (the caller
// must NOT pass the RPC to SA:MP); it is translated unit by unit and released
// later through SA:MP's own Show, in arrival order with its original timing.
// Password dialogs and close signals are never translated. Labels/HUD: TM-first
// rewrite in place (returns false). Main thread.
bool on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// A dialog unit's translation finished (engine surface 3). Main thread (pump).
void on_part_result(const i18n::engine::Result& r);

// Release held dialogs that are due (every frame, main thread).
void tick(uint32_t nowMs);

// A dialog is currently held for translation (for the on-screen indicator).
bool holding();

// Register the outbound DialogResponse (RPC 62) handler. Call ONCE at startup,
// BEFORE net::install() (like outbound::install). It (a) maps a picked row of a
// TRANSLATED list/tablist back to the ORIGINAL row text by index, and (b) with
// outgoing translation on, translates INPUT-dialog text into the server's
// language. PASSWORD dialog responses are never touched.
void install();

} // namespace i18n::surfaces
