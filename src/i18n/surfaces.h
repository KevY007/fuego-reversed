#pragma once
// i18n/surfaces.h -- translate incoming non-chat text surfaces (dialogs, 3D text
// labels, HUD text) before the game processes the RPC.
//
// Approach (the synchronous RPC hook never blocks on the network):
//  * DIALOGS: decoded into units (title, buttons, every body row/cell). Fully in
//    the translation memory (and nothing held ahead of it) -> rewritten in place
//    and shown now. Otherwise the ShowDialog is HELD (swallowed), its missing
//    units are translated -- as ONE numbered-list request per chunk of up to 20
//    cells (engine::process_batch), not one request per cell -- and it is
//    released through SA:MP's own Show strictly in arrival order, with the
//    original gap between dialogs -- or as the original if it isn't translated
//    within 10 s. When the measured response time says it can't be done in time
//    (likely_too_slow), it isn't held at all: shown at once with what the
//    translation memory has, and translated in the background for next time.
//    Close signals are never translated; a
//    password dialog's text is, but the password the player types never is. A
//    picked list row is sent back as the ORIGINAL row's text.
//  * 3D LABELS / HUD: translated LINE BY LINE, numbers kept out of it (hudtext.h),
//    translation memory first -- a text whose lines are all in memory is rewritten
//    in place; otherwise the ORIGINAL passes through and its missing lines are
//    translated in the background. A textdraw or label still on screen when they
//    land switches to the translation right there (re-sent through SA:MP's own
//    RPC handler); game text and chat bubbles, gone in seconds, show translated
//    from the next time on.
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

// LIST/TABLIST only (styles 2, 4, 5; any other style returns cores unchanged):
// revert every body LINE whose translation would change SA:MP's item structure
// back to its ORIGINAL units, so the item count and every row index stay native.
// samp.dll 0.3.DL's builders (LIST 0x1006F630, TABLIST 0x1006F730) make one item
// per '\n' line, skip EMPTY lines, cut a line at the reader's 0x100-byte copy (the
// rest becomes ANOTHER item) and a tablist cell at 0x80 bytes (the rest spills into
// the next column). Measured as the bytes SA:MP sees (unit_text incl. prefixes,
// suffixes and '\t' separators), a line -- the style-5 header line included -- is
// reverted when its translation:
//  - is empty or whitespace-only, but the original line wasn't;
//  - is longer than 255 bytes, but the original line wasn't;
//  - (original already longer than 255 bytes) is cut into a different number of
//    items than the original;
//  - (styles 4/5) has a cell longer than 127 bytes, but that original cell wasn't.
// Apply right before EVERY assemble_dialog of a translated dialog.
std::vector<std::string> fit_list_budgets(const DialogPlan& plan, std::vector<std::string> cores);

// Groups the unit indices in `missing` by IDENTICAL core (first-seen order; a
// unit's prefix/suffix doesn't matter), so a held dialog translates a repeated
// label ("Buy" on every row, the same word as a button and a cell) with ONE
// engine job whose result fills every unit of its group.
std::vector<std::vector<size_t>> group_by_core(const DialogPlan& plan, const std::vector<size_t>& missing);

// Split a dialog's missing groups (group_by_core) into BATCH requests: each chunk
// lists group indices, in order, with at most maxItems groups and at most maxBytes
// of source text (a single group bigger than maxBytes gets a chunk of its own).
// One engine batch job per chunk -- a big list becomes a few requests, not dozens.
std::vector<std::vector<size_t>> chunk_groups(const DialogPlan& plan,
                                              const std::vector<std::vector<size_t>>& groups,
                                              size_t maxItems, size_t maxBytes);

// Would holding a dialog for its translation likely take longer than budgetMs?
// single*/batch*: the engine's recency-weighted response time of single-text and
// of batch requests (engine::latency(); "known" false = not measured yet). busy:
// work already ahead (engine::backlog()); jobs: this dialog's engine jobs; items:
// its cells; workers: engine threads. Per job: a job of one cell is a single
// request (single time; the batch time if only that is known), a bigger job a
// batch (batch time; before any batch was measured, the single time x (1 + cells
// per job / 10), a longer list being a longer answer). Estimate = that x rounds of
// work (ceil((busy + jobs) / workers)). Nothing measured -> false (hold: optimistic).
bool likely_too_slow(bool singleKnown, double singleMs, bool batchKnown, double batchMs,
                     size_t busy, size_t jobs, size_t items, int workers, unsigned long budgetMs);

// Remove {RRGGBB} colour codes (SA:MP strips them from a list row's response text).
std::string strip_colors(const std::string& s);

// A DialogResponse from a TRANSLATED list/tablist dialog carries text SA:MP derived
// from the TRANSLATED row the player picked. Rebuild the text the ORIGINAL dialog
// would have produced for the same row: find which derivation (whole row / first
// column, colour-stripped or raw) turns trans[row] into `resp` and apply it to
// orig[row]. SA:MP copies the row (LIST) / first cell (TABLIST) with strncpy(.., 0x80)
// and then strips colours, so a `resp` of 127+ bytes was CUT: the row matches if
// that selection STARTS WITH `resp`, and `out` is the original's selection cut to
// resp.size() bytes, then colour-stripped (SA:MP's order). Shorter responses must
// match exactly. row = listItem (+1 for TABLIST_HEADERS, whose row 0 is the header).
// False if out of range, if a row before the picked one isn't exactly one native
// item (empty, or longer than 255 bytes -- listItem would then name another row),
// or if nothing matches (the caller then sends `resp` unchanged).
bool remap_list_response(int style, const std::vector<std::string>& orig,
                         const std::vector<std::string>& trans, int listItem,
                         const std::string& resp, std::string& out);

// True if outgoing INPUT-dialog text is eligible for translation: non-empty, not a
// "/command", not dynamic/numeric (is_dynamic_text), AND contains at least one
// space or tab. A single token -- a password typed into an INPUT dialog, an email,
// a username, an amount -- is never sent to a translator or off this machine.
bool should_translate_input(const std::string& text);

// An outgoing dialog-input translation finished (engine surface 4). Main thread.
void on_response_result(const i18n::engine::Result& r);

// Send every pending outgoing INPUT reply's ORIGINAL text right now and forget it.
// Call before the engine is stopped or restarted (engine::stop()/start() drop every
// queued job), so a swallowed reply can never be lost. Main thread.
void flush_responses();

// The server's CLOSE signal (dialog id < 0 as int16, i.e. 0x8000..0xFFFF): never
// translated, but it keeps its place in the release order. A PASSWORD dialog
// (style 3) is NOT in this set: its title/body/buttons are translated like any
// dialog's; only the password the player types is never touched (the RPC 62 path
// returns before reading a style-3 reply).
bool never_translated(const DialogPlan& d);

// Rebuild a ShowDialog (RPC 61) packet translating it UNIT BY UNIT (plan_dialog):
// `tr` receives each translatable unit's bare core. Returns true and fills `out`
// only if EVERY translatable unit resolved via `tr` (so the caller shows the
// original until all units are cached). Returns false otherwise. List lines are
// kept within SA:MP's item structure (fit_list_budgets).
bool rebuild_dialog(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// Rebuild a Create3DTextLabel (RPC 36) packet with translated text. 27-byte fixed
// header then a CSTRING. Returns true/`out` only on a translation hit.
bool rebuild_label(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// SetObjectMaterial (RPC 84) with material TEXT (type 2) -- a billboard or sign:
// u16 objectId, u8 type (2), u8 materialIndex, u8 materialSize, u8 fontNameLen,
// char fontName[len], u8 fontSize, u8 bold, u32 fontColor, u32 backColor, u8
// alignment, then the text as a Huffman CSTRING (the handler, samp.dll 0.3.DL
// base+0x1B1D0, decodes up to 2048 bytes, needs a font name under 32 bytes and a
// non-empty text, and ignores an object that doesn't exist). `textOffset` = where
// the CSTRING starts. False for a texture material (type 1) or a malformed packet.
bool parse_material_text(const uint8_t* pkt, size_t n, size_t& textOffset, std::string& text);
// The same packet with `tr`'s translation of its text (the header verbatim).
bool rebuild_material_text(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// --- plain-text HUD surfaces (no Huffman) ----------------------------------
// True if `text` (ignoring ~x~/{RRGGBB} codes + spaces) is mostly digits/symbols
// -- a timer/counter/stat where translating is pointless (numbers are universal).
// A HUD/label LINE like that is shown as it is (hudtext::plan_text).
bool is_dynamic_text(const std::string& text);

// ShowGameText (73): u32 style, u32 time, u32 msgLen, char[] message.
bool rebuild_gametext(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// ChatBubble (59): u16 pid, u32 color, f dist, u32 expire, u8 len, char[] text.
bool rebuild_chatbubble(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// TextDrawSetString (105): u16 id, u16 len, char[] text.
bool rebuild_textdraw_setstring(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);
// ShowTextDraw (134): 65-byte fixed header, u16 szTextLen, char[] szText (last field).
bool rebuild_textdraw_show(const uint8_t* pkt, size_t n, const Translator& tr, std::vector<uint8_t>& out);

// Called from the incoming RPC-dispatch chokepoint (compat::HandleRPCPacketFunc);
// `handler` is SA:MP's own function for this RPC (kept for the ones re-sent to
// update on-screen text: TextDrawSetString, ShowTextDraw, Create3DTextLabel).
// ShowDialog (translate_dialogs): fully cached + nothing held -> rewritten in
// place and shown now (returns false). Otherwise HELD: returns true (the caller
// must NOT pass the RPC to SA:MP); its missing cells are translated in batch
// requests (one per chunk of up to 20 cells; one per cell while batching is off)
// and it is released later through SA:MP's own Show, in arrival order with its
// original timing. If the measured provider latency says it can't be translated
// in time (likely_too_slow) and nothing is held ahead, it is NOT held: shown now
// with what the translation memory has (returns false), the rest translated in
// the background (surface-2 fills) for next time.
// Without a running pump (hooks::pump_ready()) nothing is ever held: SA:MP shows
// the original (or the in-place rewrite when fully cached). Close signals are
// never translated (a password dialog's text is; the typed password never is).
// Labels/HUD: rewritten in place when every line is in memory (returns false);
// shown textdraws and labels are tracked (show / set text / hide / delete) so
// reconcile can update them later. InitGame (RPC 139, a new session): first
// discards everything the old session left held or pending -- held dialogs, INPUT
// replies, outgoing chat, list row tables, tracked textdraws/labels -- without
// showing or sending any of it (returns false). Main thread.
bool on_incoming_rpc(unsigned char id, RPCParameters* rpc, void (*handler)(RPCParameters*));

// A background translation-memory fill finished (engine surface 2): its lines
// are in memory now, so the next tick() puts them into every waiting on-screen
// textdraw/label. A line that failed shows as it is and is retried while its text
// is still on screen -- after 30 s, then 5 min, then every 30 min (a failed
// request, e.g. the provider down, after 30 s without counting against the line).
// Main thread (pump).
void on_fill_result(const i18n::engine::Result& r);

// Settings were applied: when the HUD or dialog + label toggle changed, the next
// tick() recomputes EVERY tracked textdraw/label -- turned off, the originals go
// back on screen; turned on, what's there gets translated. Main thread.
void recheck_hud();

// The engine's queue was dropped (engine stopped/restarted -- a new target
// language, model or provider): the fills that were out will never report back and
// earlier failures may not repeat -- forget both, and recompute every tracked
// textdraw/label on the next tick(). Main thread.
void forget_fills();

// A dialog unit's translation finished (engine surface 3). Main thread (pump).
void on_part_result(const i18n::engine::Result& r);

// Every frame, main thread: release held dialogs that are due, and switch waiting
// on-screen textdraws/labels to translations that landed.
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
