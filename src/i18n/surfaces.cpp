#include "surfaces.h"

#include "config.h"
#include "engine.h"
#include "dlgq.h"

#include "../net/net.h"         // on_send_rpc (DialogResponse reverse-map)
#include "../net/RakClient.h"   // RPCParameters, g_RakClient
#include "../net/BitStream.h"
#include "../core/logger.h"
#include "../samp/samp.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The pure rebuild_dialog()/rebuild_label() live in surfaces_rebuild.cpp (no game
// types) so they can be host-tested. This TU is the game/engine glue.

namespace i18n::surfaces {

namespace {

std::atomic<uint64_t> g_seq{ 1 };

// List-dialog selection reverse-map (translated row text -> original row text), so a
// selection on a TRANSLATED list dialog still sends what a text-matching server
// expects. Populated when a multi-line dialog body is translated; read on outbound
// RPC 62. Main/net thread only (incoming dispatch + outgoing send both run there).
std::unordered_map<std::string, std::string> g_listRevMap;

void split_lines(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') { out.push_back(s.substr(start, i - start)); start = i + 1; }
}

// Record translated<->original rows of a translated multi-line body (a list dialog's
// items are its newline-separated body). Harmless for non-list bodies (unused unless
// a matching RPC 62 arrives). Capped to bound memory.
void note_list_rows(const std::string& original, const std::string& translated) {
    if (original.find('\n') == std::string::npos) return;
    std::vector<std::string> o, t;
    split_lines(original, o);
    split_lines(translated, t);
    if (o.size() != t.size()) return;
    if (g_listRevMap.size() > 500) g_listRevMap.clear();
    for (size_t i = 0; i < t.size(); ++i)
        if (!t[i].empty() && t[i] != o[i]) g_listRevMap[t[i]] = o[i];
}

// Queue a background cache-fill translation (surface 2 -> apply_result ignores it;
// process_one() translates through the full pipeline -- mask/glossary/validate --
// and stores the result in the TM cache). Dedup ON, so a repeatedly-seen miss is
// only translated once.
void queue_fill(const std::string& src) {
    if (src.size() < 2) return;
    i18n::engine::Job j;
    j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
    j.surface     = 2;
    j.sourceBytes = src;
    j.srcCodepage = i18n::config::get().source_cp;
    j.skipContext = true;
    i18n::engine::submit(j);
}

// TM-first translator for a piece of text: cache hit -> out=translation, true;
// trivial string (<2 chars, e.g. a lone space) -> keep as-is, true; miss -> queue
// a fill and return false (caller shows the original this time).
bool tm_translate(const std::string& src, std::string& out) {
    if (src.size() < 2) { out = src; return true; }
    auto& s = i18n::config::get();
    if (i18n::engine::cache_lookup(src, s.source_cp, s.target_lang, out)) {
        note_list_rows(src, out); // remember rows in case this is a list dialog body
        return true;
    }
    queue_fill(src);
    return false;
}

// HUD variant: additionally SKIP dynamic/numeric-heavy strings (timers, counters,
// stats) so a textdraw that updates every tick with a changing number never spams
// the translator -- return false (pass original) without queueing anything.
bool tm_translate_hud(const std::string& src, std::string& out) {
    if (is_dynamic_text(src)) return false;
    return tm_translate(src, out);
}

// Replace the RPC's payload with `pkt` (all byte-aligned). Mirrors compat's
// incoming-RPC rewrite: build a BitStream, CopyData into rpc->input, set the bit
// count. RenderWare/SA:MP then parses our rebuilt packet exactly as a real one.
void commit(RPCParameters* rpc, const std::vector<uint8_t>& pkt) {
    BitStream bs;
    if (!pkt.empty()) bs.Write((char*)pkt.data(), (int)pkt.size());
    bs.CopyData(&rpc->input);
    rpc->numberOfBitsOfData = bs.GetNumberOfBitsUsed();
}

// ---- held dialogs: translate, then release in order with the original timing ----
// samp.dll 0.3.DL, confirmed by disassembly + an in-game test (2026-09-27):
//   dialog `this`  *(base+0x2AC9E0)
//   Show           base+0x6FA50  __thiscall(this, int id, int style, char* caption,
//                  char* body, char* btn1, char* btn2, int bServerside), ret 0x1c
// The ShowDialog RPC handler only decodes the packet and calls Show (bServerside=1,
// and only when title AND body are non-empty); the cursor/controls are taken each
// frame from iIsActive, so calling Show ourselves is exactly equivalent. An id < 0
// (0x8000..0xFFFF) is the server's CLOSE: Show(-1) hides the dialog.
constexpr uint32_t kDialogPtr  = 0x2AC9E0;
constexpr uint32_t kDialogShow = 0x6FA50;
using ShowFn = void( __thiscall * )( void* self, int id, int style, const char* caption,
                                     const char* body, const char* btn1, const char* btn2, int serverside );

// SEH-isolated call into SA:MP (no C++ objects in this frame, so __except is legal).
bool safe_show(void* fn, void* self, int id, int style, const char* cap, const char* body,
               const char* b1, const char* b2) {
    __try {
        ((ShowFn)fn)(self, id, style, cap, body, b1, b2, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

i18n::dlgq::Queue g_dlgq(10000, 16); // hold at most 10s, at most 16 dialogs

// A held dialog still waiting for some of its unit translations.
struct PendingDialog {
    DialogPlan               plan;
    std::vector<std::string> cores;
    int                      outstanding = 0;
};
std::unordered_map<uint64_t, PendingDialog>               g_pendingDialogs; // seq -> dialog
std::unordered_map<uint64_t, std::pair<uint64_t, size_t>> g_partJobs;       // job -> (seq, unit)

// List/tablist rows of the translated dialog now on screen, per dialog id, so a
// DialogResponse can be mapped back to the ORIGINAL row (used by Task 7).
struct ShownRows {
    uint8_t                  style = 0;
    std::vector<std::string> orig, trans;
};
std::unordered_map<uint16_t, ShownRows> g_shownRows;

// Remember the rows of a translated LIST/TABLIST dialog that is now on screen.
void remember_rows(const std::vector<uint8_t>& original, const std::vector<uint8_t>& shown) {
    DialogPlan o, t;
    if (!plan_dialog(original.data(), original.size(), o) || !plan_dialog(shown.data(), shown.size(), t)) return;
    if (!(o.style == 2 || o.style == 4 || o.style == 5) || !o.haveBody || !t.haveBody) return;
    ShownRows r;
    r.style = o.style;
    split_lines(assemble_body(o, original_cores(o)), r.orig);
    split_lines(assemble_body(t, original_cores(t)), r.trans);
    if (r.orig.size() != r.trans.size()) return;
    if (g_shownRows.size() > 64) g_shownRows.clear();
    g_shownRows[o.id] = std::move(r);
}

// Show a released payload (translated or original) through SA:MP's own Show.
void show_payload(const std::vector<uint8_t>& pkt) {
    DialogPlan d;
    if (!plan_dialog(pkt.data(), pkt.size(), d) || !d.haveBody) return;
    const std::vector<std::string> cores = original_cores(d);
    std::string title = unit_text(d.units[0], cores[0]);
    std::string b1    = unit_text(d.units[1], cores[1]);
    std::string b2    = unit_text(d.units[2], cores[2]);
    std::string body  = assemble_body(d, cores);
    if (title.empty() || body.empty()) return;        // the RPC handler skips these too
    if (title.size() > 255) title.resize(255);        // Show copies it into a 256-byte field
    if (body.size() > 4095) body.resize(4095);        // the handler's decode limit
    const uint32_t base = samp::base();
    if (!base) return;
    void* dlg = *reinterpret_cast<void**>(base + kDialogPtr);
    if (!dlg) return;
    if (!safe_show((void*)(base + kDialogShow), dlg, (int)(int16_t)d.id, d.style,
                   title.c_str(), body.c_str(), b1.c_str(), b2.c_str()))
        Log("[i18n] dialog release: Show failed (crash-guarded) for id %d.", (int)(int16_t)d.id);
}

// A ShowDialog arrived (translate_dialogs on). True = we SWALLOWED it (it will be
// released later through the queue); false = let SA:MP show it now (possibly
// rewritten in place from the translation memory).
bool handle_dialog(RPCParameters* rpc, const uint8_t* p, size_t n) {
    DialogPlan d;
    if (!plan_dialog(p, n, d) || !d.haveBody) return false;   // can't release it ourselves -> native
    const bool passthru = never_translated(d);                 // CLOSE / PASSWORD: never translated
    auto& s = i18n::config::get();

    // Translation-memory pass: fill every unit we already have.
    std::vector<std::string> cores = original_cores(d);
    std::vector<size_t> missing;
    bool anyTranslated = false;
    if (!passthru) {
        for (size_t k = 0; k < d.units.size(); ++k) {
            if (!d.units[k].translate) continue;
            std::string t;
            if (i18n::engine::cache_lookup(d.units[k].core, s.source_cp, s.target_lang, t)) {
                cores[k] = t;
                anyTranslated = true;
            } else {
                missing.push_back(k);
            }
        }
    }
    const std::vector<uint8_t> original(p, p + n);

    // Nothing to wait for and nothing ahead of it: SA:MP shows it right now.
    if (g_dlgq.empty() && missing.empty()) {
        if (anyTranslated) {
            std::vector<uint8_t> pkt = assemble_dialog(d, cores);
            commit(rpc, pkt);
            remember_rows(original, pkt);
        }
        return false;
    }

    // Hold it: translate what's missing; release in order with the original timing.
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    if (!g_dlgq.push(seq, GetTickCount(), original)) {
        // Queue full (something is stuck): show everything held, then this one natively.
        for (auto& r : g_dlgq.flush()) { g_pendingDialogs.erase(r.seq); show_payload(r.payload); }
        return false;
    }
    if (missing.empty()) { // ready now (cached / close / password) -- waits for its turn
        g_dlgq.resolve(seq, anyTranslated ? assemble_dialog(d, cores) : std::vector<uint8_t>{});
        return true;
    }

    if (g_partJobs.size() > 512) g_partJobs.clear(); // results that never came back
    PendingDialog pd;
    pd.plan        = d;
    pd.cores       = std::move(cores);
    pd.outstanding = (int)missing.size();
    g_pendingDialogs[seq] = std::move(pd);
    for (size_t k : missing) {
        i18n::engine::Job j;
        j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
        j.surface     = 3;          // dialog unit -> hooks::apply_result -> on_part_result
        j.sourceBytes = d.units[k].core;
        j.srcCodepage = s.source_cp;
        j.skipContext = true;
        j.noDedup     = true;       // every held dialog must get its own result back
        j.capBytes    = 1024;       // dialog lines can be longer than a chat line
        g_partJobs[j.handle] = { seq, k };
        i18n::engine::submit(j);
    }
    return true;
}

// Outbound RPC 62 (DialogResponse): if the selected LIST-row text is one we recorded
// as a translation, re-send it with the ORIGINAL row text so a text-matching server
// recognises the selection. Bypasses the seam via g_RakClient (like outbound chat) ->
// no re-entry. Index-based servers are unaffected (wListItem is unchanged).
bool on_send_dialogresponse(int id, BitStream* params, PacketPriority, PacketReliability, char, bool) {
    if (id != 62 || !params) return true;
    if (!i18n::config::get().translate_dialogs || g_listRevMap.empty()) return true;

    params->ResetReadPointer();
    int16_t dlgId = 0, listItem = 0;
    uint8_t resp = 0, tlen = 0;
    if (!params->Read(dlgId) || !params->Read(resp) || !params->Read(listItem) || !params->Read(tlen)
        || tlen == 0 || tlen > 200) { params->ResetReadPointer(); return true; }
    char buf[256] = {};
    if (!params->Read(buf, tlen)) { params->ResetReadPointer(); return true; }
    params->ResetReadPointer();

    auto it = g_listRevMap.find(std::string(buf, tlen));
    if (it == g_listRevMap.end()) return true; // not a translated row -> send unchanged

    const std::string& orig = it->second;
    if (!g_RakClient) return true;
    size_t ol = orig.size() > 255 ? 255 : orig.size();
    BitStream bs;
    bs.Write(dlgId);
    bs.Write(resp);
    bs.Write(listItem);
    bs.Write((uint8_t)ol);
    if (ol) bs.Write((char*)orig.data(), (int)ol);
    g_RakClient->RPC(62, &bs);
    return false; // swallow the translated-text response; we sent the original
}

} // namespace

bool on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!rpc || !rpc->input || rpc->numberOfBitsOfData < 8) return false;
    auto& s = i18n::config::get();
    const uint8_t* p = rpc->input;
    size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);

    try {
        if (id == 61) return s.translate_dialogs ? handle_dialog(rpc, p, n) : false;

        std::vector<uint8_t> out;
        bool ok = false;
        if (s.translate_dialogs && id == 36) {
            ok = rebuild_label(p, n, tm_translate, out);
        } else if (s.translate_hud && (id == 73 || id == 59 || id == 105 || id == 134)) {
            switch (id) {
                case 73:  ok = rebuild_gametext(p, n, tm_translate_hud, out); break;
                case 59:  ok = rebuild_chatbubble(p, n, tm_translate_hud, out); break;
                case 105: ok = rebuild_textdraw_setstring(p, n, tm_translate_hud, out); break;
                case 134: ok = rebuild_textdraw_show(p, n, tm_translate_hud, out); break;
            }
        } else {
            return false;
        }
        if (ok && !out.empty()) commit(rpc, out);
    } catch (...) {
        // Never let an exception cross back into SA:MP's RPC dispatch.
    }
    return false;
}

void on_part_result(const i18n::engine::Result& r) {
    auto pj = g_partJobs.find(r.handle);
    if (pj == g_partJobs.end()) return;
    const uint64_t seq  = pj->second.first;
    const size_t   unit = pj->second.second;
    g_partJobs.erase(pj);
    auto it = g_pendingDialogs.find(seq);
    if (it == g_pendingDialogs.end()) return; // already released (timed out)
    PendingDialog& pd = it->second;
    if (r.ok && !r.displayBytes.empty() && unit < pd.cores.size()) pd.cores[unit] = r.displayBytes;
    if (--pd.outstanding > 0) return;
    g_dlgq.resolve(seq, assemble_dialog(pd.plan, pd.cores));
    g_pendingDialogs.erase(it);
}

void tick(uint32_t nowMs) {
    if (g_dlgq.empty()) return;
    std::vector<i18n::dlgq::Release> out =
        i18n::config::get().translate_dialogs ? g_dlgq.tick(nowMs) : g_dlgq.flush();
    for (auto& r : out) {
        g_pendingDialogs.erase(r.seq); // a straggler that timed out
        show_payload(r.payload);
        if (r.translated) remember_rows(r.original, r.payload);
    }
}

bool holding() { return !g_dlgq.empty(); }

void install() {
    net::on_send_rpc(on_send_dialogresponse);
    Log("[i18n] surfaces: dialog-response reverse-map registered.");
}

} // namespace i18n::surfaces
