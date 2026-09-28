#include "surfaces.h"

#include "config.h"
#include "engine.h"
#include "dlgq.h"
#include "hudtext.h"
#include "huffman.h"
#include "hooks.h"               // hooks::pump_ready() -- a swallowed reply needs a pump to deliver it
#include "outbound.h"            // outbound::discard_pending() on a new session (InitGame)

#include "../net/net.h"         // on_send_rpc (DialogResponse: list-index remap + input translation)
#include "../net/RakClient.h"   // RPCParameters, g_RakClient
#include "../net/BitStream.h"
#include "../core/logger.h"
#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../samp/pools.h"        // the textdraw / 3D-label pools (slot_in_use)

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The pure rebuild_dialog()/rebuild_label() live in surfaces_rebuild.cpp (no game
// types) so they can be host-tested. This TU is the game/engine glue.

namespace i18n::surfaces {

namespace {

std::atomic<uint64_t> g_seq{ 1 };

void split_lines(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') { out.push_back(s.substr(start, i - start)); start = i + 1; }
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
// samp.dll 0.3.DL dialog offsets (samp::addr::DIALOG_INFO / FUNC_DIALOG_SHOW),
// confirmed by disassembly + an in-game test (2026-09-27). The ShowDialog RPC
// handler only decodes the packet and calls Show (bServerside=1, and only when
// title AND body are non-empty); the cursor/controls are taken each frame from
// iIsActive, so calling Show ourselves is exactly equivalent. An id < 0
// (0x8000..0xFFFF) is the server's CLOSE: Show(-1) hides the dialog.
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

constexpr uint32_t kMaxHoldMs      = 10000; // hold a dialog at most 10 s, then show the original
constexpr size_t   kMaxHeldDialogs = 16;
i18n::dlgq::Queue  g_dlgq(kMaxHoldMs, kMaxHeldDialogs);

// A held dialog still waiting for some of its unit translations.
struct PendingDialog {
    DialogPlan               plan;
    std::vector<std::string> cores;
    int                      outstanding = 0; // engine BATCH jobs still out (one per chunk of missing cells)
};
std::unordered_map<uint64_t, PendingDialog> g_pendingDialogs; // seq -> dialog

// One batch job of a held dialog: groups[i] = every unit whose core is the batch's
// i-th text (identical cores share one entry), filled from Result::unitOut[i].
struct PartJob {
    uint64_t                         seq = 0;
    std::vector<std::vector<size_t>> groups;
};
std::unordered_map<uint64_t, PartJob> g_partJobs; // engine job handle -> its dialog + units

// Batch requests: at most this many cells / this much source text per request, so
// a long list is a few requests (running in parallel), not one oversized answer.
constexpr size_t kBatchMaxItems = 20;
constexpr size_t kBatchMaxBytes = 1500;
// Don't hold a dialog the translator measurably can't finish within this long:
// show it right away and translate it in the background for next time.
constexpr unsigned long kEarlyReleaseMs = 6000;

// List/tablist rows of the translated dialog now on screen, per dialog id, so a
// DialogResponse can be mapped back to the ORIGINAL row (on_send_dialogresponse).
struct ShownRows {
    uint8_t                  style = 0;
    std::vector<std::string> orig, trans;
};
std::unordered_map<uint16_t, ShownRows> g_shownRows;

// Every show of dialog id X either records X's rows (a translated LIST/TABLIST,
// shown != null) or ERASES X's stale entry -- so a later untranslated show of the
// same id (a different dialog reusing the id, translate_dialogs toggled off, a
// queue-full flush, ...) can never leave behind rows that get remapped against the
// WRONG dialog. Call this at EVERY show point, with shown == nullptr meaning "id X
// was just shown untranslated" (native, a close signal, or no cache hit).
void note_shown(const std::vector<uint8_t>& original, const std::vector<uint8_t>* shown) {
    DialogPlan o;
    if (!plan_dialog(original.data(), original.size(), o)) return; // can't even get the id -> nothing to do
    if (shown && o.haveBody) {
        DialogPlan t;
        if (plan_dialog(shown->data(), shown->size(), t) && (o.style == 2 || o.style == 4 || o.style == 5) &&
            t.haveBody) {
            ShownRows r;
            r.style = o.style;
            split_lines(assemble_body(o, original_cores(o)), r.orig);
            split_lines(assemble_body(t, original_cores(t)), r.trans);
            if (r.orig.size() == r.trans.size()) {
                if (g_shownRows.size() > 64) g_shownRows.clear();
                g_shownRows[o.id] = std::move(r);
                return;
            }
        }
    }
    g_shownRows.erase(o.id);
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
    void* dlg = *reinterpret_cast<void**>(base + samp::addr::DIALOG_INFO);
    if (!dlg) return;
    if (!safe_show((void*)(base + samp::addr::FUNC_DIALOG_SHOW), dlg, (int)(int16_t)d.id, d.style,
                   title.c_str(), body.c_str(), b1.c_str(), b2.c_str()))
        Log("[i18n] dialog release: Show failed (crash-guarded) for id %d.", (int)(int16_t)d.id);
}

// Dialog units may be much longer than a chat line; give the engine + the cache
// a generous cap for them (see Job::capBytes / engine::cache_lookup's capBytes)
// instead of the chat default of 144. ONE constant so the translation-memory
// pass and the fill jobs below can never disagree with each other.
constexpr int kDialogUnitCap = 1024;

// One engine BATCH job per chunk (chunk_groups): the chunk's distinct missing cores
// go out as one numbered-list request. A HELD dialog's jobs (surface 3, priority
// lane) report back through on_part_result; a background fill (surface 2, normal
// lane -- the dialog is already on screen) only fills the translation memory.
void submit_batches(const DialogPlan& d, const std::vector<std::vector<size_t>>& groups,
                    const std::vector<std::vector<size_t>>& chunks, uint64_t seq, bool fillOnly) {
    const auto& s = i18n::config::get();
    for (const std::vector<size_t>& chunk : chunks) {
        i18n::engine::Job j;
        j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
        j.surface     = fillOnly ? 2 : 3;
        j.srcCodepage = s.source_cp;
        j.skipContext = true;
        j.noDedup     = true;           // every held dialog must get its own result back
        j.capBytes    = kDialogUnitCap; // dialog lines can be longer than a chat line
        j.labels      = true;           // cells are UI labels: "Police (3)" is a count, not a player
        PartJob pj;
        pj.seq = seq;
        for (size_t g : chunk) {
            j.units.push_back(d.units[groups[g].front()].core);
            pj.groups.push_back(groups[g]);
        }
        if (!fillOnly) g_partJobs[j.handle] = std::move(pj);
        i18n::engine::submit(j);
    }
}

// Cores a background fill was sent for recently (core -> GetTickCount() at submit):
// re-opening the same dialog while a slow provider is still on it must not queue
// the same fill again (and run duplicates in parallel).
std::unordered_map<std::string, uint32_t> g_recentFills;
constexpr uint32_t kFillMemoryMs = 60000;

// The groups whose core has no background fill from the last kFillMemoryMs, now
// marked as filled.
std::vector<std::vector<size_t>> not_recently_filled(const DialogPlan& d,
                                                     const std::vector<std::vector<size_t>>& groups, uint32_t now) {
    if (g_recentFills.size() > 512) {
        for (auto it = g_recentFills.begin(); it != g_recentFills.end();)
            it = (now - it->second >= kFillMemoryMs) ? g_recentFills.erase(it) : std::next(it);
        if (g_recentFills.size() > 512) g_recentFills.clear();
    }
    std::vector<std::vector<size_t>> out;
    for (const std::vector<size_t>& g : groups) {
        const std::string& core = d.units[g.front()].core;
        auto it = g_recentFills.find(core);
        if (it != g_recentFills.end() && now - it->second < kFillMemoryMs) continue;
        g_recentFills[core] = now;
        out.push_back(g);
    }
    return out;
}

// A ShowDialog arrived (translate_dialogs on). True = we SWALLOWED it (it will be
// released later through the queue); false = let SA:MP show it now (possibly
// rewritten in place from the translation memory).
bool handle_dialog(RPCParameters* rpc, const uint8_t* p, size_t n) {
    DialogPlan d;
    if (!plan_dialog(p, n, d) || !d.haveBody) return false;   // can't release it ourselves -> native
    const bool passthru = never_translated(d);                 // CLOSE signal: never translated
    auto& s = i18n::config::get();

    // Translation-memory pass: fill every unit we already have -- in the running
    // engine's target language, the one its jobs translate into (a target picked in
    // the menu applies on Save).
    const int target = i18n::engine::active_target_lang();
    std::vector<std::string> cores = original_cores(d);
    std::vector<size_t> missing;
    bool anyTranslated = false;
    if (!passthru) {
        for (size_t k = 0; k < d.units.size(); ++k) {
            if (!d.units[k].translate) continue;
            // Already the target language / nothing translatable survives protect::mask:
            // keep the original core -- never a miss, never something to hold for.
            if (!i18n::engine::needs_translation(d.units[k].core, s.source_cp, target, /*labels=*/true)) continue;
            std::string t;
            if (i18n::engine::cache_lookup(d.units[k].core, s.source_cp, target, t, kDialogUnitCap)) {
                cores[k] = t;
                anyTranslated = true;
            } else {
                missing.push_back(k);
            }
        }
    }
    const std::vector<uint8_t> original(p, p + n);

    // A held dialog is only ever released by tick() on the per-frame pump: with no
    // running pump (hooks not installed, or the EndScene hook never went live) NEVER
    // hold -- a fully cached dialog is still rewritten in place below, anything else
    // is shown by SA:MP as the original, right now.
    const bool canHold = i18n::hooks::pump_ready();

    // Nothing to wait for and nothing ahead of it: SA:MP shows it right now.
    // fit_list_budgets (here and at every other assembly): a translated list line
    // must never change the item count SA:MP builds, or every pick below it would
    // go out with a shifted listitem.
    if (missing.empty() && (g_dlgq.empty() || !canHold)) {
        if (anyTranslated) {
            std::vector<uint8_t> pkt = assemble_dialog(d, fit_list_budgets(d, cores));
            commit(rpc, pkt);
            note_shown(original, &pkt);
        } else {
            note_shown(original, nullptr); // untranslated (incl. a passthru close signal): erase any stale entry
        }
        return false;
    }
    if (!canHold) {
        note_shown(original, nullptr); // shown natively (untranslated): erase any stale entry
        return false;
    }

    // The missing cells, grouped (a repeated label -- "Buy" on every row -- is
    // translated once and fills every unit that has it) and split into batch
    // requests: a whole dialog is usually ONE request, not one per cell. While
    // batching is off (this model can't keep a numbered list), one job per cell,
    // so the cells run in parallel on the workers instead of one after another.
    const size_t maxItems = i18n::engine::batching_enabled() ? kBatchMaxItems : 1;
    const std::vector<std::vector<size_t>> groups = group_by_core(d, missing);
    const std::vector<std::vector<size_t>> chunks = chunk_groups(d, groups, maxItems, kBatchMaxBytes);

    // Nothing held ahead, but the translator measurably can't finish this one in
    // time (its recent response time x the work already ahead): don't make the
    // player wait for nothing. Show it NOW -- with whatever the translation memory
    // already has -- and translate the rest in the background, so the next time it
    // opens it is translated at once.
    if (g_dlgq.empty() && !chunks.empty()) {
        const i18n::engine::Latency lat = i18n::engine::latency();
        if (likely_too_slow(lat.singleKnown, lat.singleMs, lat.batchKnown, lat.batchMs, i18n::engine::backlog(),
                            chunks.size(), groups.size(), i18n::engine::worker_count(), kEarlyReleaseMs)) {
            const std::vector<std::vector<size_t>> fill = not_recently_filled(d, groups, GetTickCount());
            submit_batches(d, fill, chunk_groups(d, fill, maxItems, kBatchMaxBytes), 0, /*fillOnly=*/true);
            if (anyTranslated) {
                std::vector<uint8_t> pkt = assemble_dialog(d, fit_list_budgets(d, cores));
                commit(rpc, pkt);
                note_shown(original, &pkt);
            } else {
                note_shown(original, nullptr);
            }
            Log("[i18n] dialog %d shown right away: translating it would take too long now (recent single "
                "request ~%.0f ms, batch ~%.0f ms, %u job(s)); it is translated in the background for next time.",
                (int)(int16_t)d.id, lat.singleMs, lat.batchMs, (unsigned)chunks.size());
            return false;
        }
    }

    // Hold it: translate what's missing; release in order with the original timing.
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    if (!g_dlgq.push(seq, GetTickCount(), original)) {
        // Queue full (something is stuck): show everything held, then this one natively.
        for (auto& r : g_dlgq.flush()) {
            g_pendingDialogs.erase(r.seq);
            show_payload(r.payload);
            note_shown(r.original, r.translated ? &r.payload : nullptr);
        }
        note_shown(original, nullptr); // this one shows natively (untranslated) too
        return false;
    }
    // From here on the dialog IS held: tick() releases it whatever happens below (as
    // the original, at the latest once the hold limit passes), so it is reported as
    // held even if something throws -- letting SA:MP show it now too would show it twice.
    try {
        if (chunks.empty()) { // ready now (cached / close signal) -- waits for its turn
            g_dlgq.resolve(seq, anyTranslated ? assemble_dialog(d, fit_list_budgets(d, cores)) : std::vector<uint8_t>{});
            return true;
        }

        if (g_partJobs.size() > 512) g_partJobs.clear(); // results that never came back
        PendingDialog pd;
        pd.plan        = d;
        pd.cores       = std::move(cores);
        pd.outstanding = (int)chunks.size(); // counts BATCH jobs, not units
        g_pendingDialogs[seq] = std::move(pd);
        submit_batches(d, groups, chunks, seq, /*fillOnly=*/false); // -> priority lane -> on_part_result
    } catch (...) {
        // Still held (see above): it is released as the original once its hold expires.
    }
    return true;
}

// DialogResponse wire (client -> server): i16 dialogId, u8 response, i16 listItem,
// u8 textLen, text. Sent via g_RakClient, which wraps the REAL interface and so
// bypasses the send seam -- this can never re-enter on_send_dialogresponse(). False
// only if there's no RakClient to send through (nothing else we can do about it).
bool send_dialog_response(int16_t dlgId, uint8_t resp, int16_t listItem, const std::string& text) {
    if (!g_RakClient) return false;
    const size_t tl = text.size() > 255 ? 255 : text.size();
    BitStream bs;
    bs.Write(dlgId);
    bs.Write(resp);
    bs.Write(listItem);
    bs.Write((uint8_t)tl);
    if (tl) bs.Write((char*)text.data(), (int)tl);
    return g_RakClient->RPC(62, &bs);
}

struct PendingResponse {
    int16_t     dlgId = 0;
    uint8_t     resp = 0;
    int16_t     listItem = 0;
    std::string original;
    uint32_t    t0 = 0; // GetTickCount() when swallowed -- the delivery deadline clock
};
std::unordered_map<uint64_t, PendingResponse> g_pendingResponses;

// Guaranteed delivery: an INPUT reply swallowed pending translation is sent as typed
// if the translation doesn't land within this long (settings/menu access, a stalled
// provider, ...) -- a reply must never be lost or delayed indefinitely.
constexpr uint32_t kResponseDeadlineMs = 5000;

// A translated INPUT reply is capped like the native one: SA:MP's input box never
// sends more than 128 bytes of text.
constexpr int kInputReplyCap = 128;

// Wrap-safe "at least ms have elapsed since t0" for GetTickCount()-style clocks
// (mirrors i18n::dlgq's at_or_after).
bool elapsed_ms(uint32_t nowMs, uint32_t t0, uint32_t ms) { return (int32_t)(nowMs - t0 - ms) >= 0; }

// Send every pending reply's ORIGINAL text now and forget it -- used by the overflow
// guard below and by flush_responses() (engine restart/stop). Unlogged: this is a
// bulk safety net for an unusual condition (too many in flight, or settings applied),
// not a per-reply outcome; send_dialog_response()'s result is ignored, same as
// on_response_result() and the deadline flush -- there's nothing else to do if it fails.
void flush_all_pending() {
    for (auto& kv : g_pendingResponses) {
        const PendingResponse& pr = kv.second;
        send_dialog_response(pr.dlgId, pr.resp, pr.listItem, pr.original);
    }
    g_pendingResponses.clear();
}

// Any reply that has been waiting too long is sent as typed -- called every frame,
// before anything else, so a reply is never held hostage by a stuck dialog queue.
void flush_due_responses(uint32_t nowMs) {
    for (auto it = g_pendingResponses.begin(); it != g_pendingResponses.end(); ) {
        if (elapsed_ms(nowMs, it->second.t0, kResponseDeadlineMs)) {
            send_dialog_response(it->second.dlgId, it->second.resp, it->second.listItem, it->second.original);
            Log("[i18n] dialog %d: input reply sent as typed (translation took too long).", (int)it->second.dlgId);
            it = g_pendingResponses.erase(it);
        } else {
            ++it;
        }
    }
}

// The live dialog's style if it's the one being answered (SA:MP's own state, read at
// send time -- the response is sent before the dialog closes), else -1.
int answered_style(int16_t dlgId) {
    const uint32_t base = samp::base();
    if (!base) return -1;
    const uint8_t* dlg = *reinterpret_cast<const uint8_t* const*>(base + samp::addr::DIALOG_INFO);
    if (!dlg) return -1;
    if (*reinterpret_cast<const int16_t*>(dlg + samp::addr::DIALOG_ID) != dlgId) return -1;
    return *reinterpret_cast<const int*>(dlg + samp::addr::DIALOG_TYPE);
}

bool on_send_dialogresponse(int id, BitStream* params, PacketPriority, PacketReliability, char, bool) {
    if (id != 62 || !params) return true;
    try {
        params->ResetReadPointer();
        int16_t dlgId = 0, listItem = 0;
        uint8_t resp = 0, tlen = 0;
        if (!params->Read(dlgId) || !params->Read(resp) || !params->Read(listItem) || !params->Read(tlen)) {
            params->ResetReadPointer();
            return true;
        }

        // Style is checked BEFORE the text bytes are even read into a buffer: a
        // PASSWORD response (or one for a dialog that isn't the one on screen) must
        // never be touched, so its text is never given the chance to reach a buffer,
        // the translator, g_pendingResponses or a log line.
        const int style = answered_style(dlgId);
        if (style == 3 || style < 0) { params->ResetReadPointer(); return true; }

        char buf[256] = {};
        if (tlen && !params->Read(buf, tlen)) { params->ResetReadPointer(); return true; }
        params->ResetReadPointer();
        const std::string text(buf, tlen);
        auto& s = i18n::config::get();

        // LIST / TABLIST we translated: send the ORIGINAL row's text for the picked index.
        if (style == 2 || style == 4 || style == 5) {
            auto it = g_shownRows.find((uint16_t)dlgId);
            // The stored style must match the LIVE dialog's style too: an id can be
            // reused by a later, differently-styled (or untranslated) dialog.
            if (it == g_shownRows.end() || it->second.style != style) return true;
            std::string orig;
            if (!remap_list_response(style, it->second.orig, it->second.trans, listItem, text, orig) || orig == text)
                return true;
            if (send_dialog_response(dlgId, resp, listItem, orig)) {
                Log("[i18n] dialog %d: list pick %d sent as the server's original row.", (int)dlgId, (int)listItem);
                return false; // swallow the translated-text response; the original went instead
            }
            return true; // couldn't send the replacement -> let the translated-text one go
        }

        // INPUT: translate what I typed into the server's language (shared outgoing
        // toggle). Only the main button; never a command, a single token (password/
        // email/amount -- should_translate_input), dynamic/numeric text, or already
        // the target language; and only if the pump can actually deliver a result
        // back (else this would swallow the reply and never send it).
        if (style == 1 && s.outbound_enabled && resp == 1 && i18n::hooks::pump_ready() &&
            should_translate_input(text) &&
            i18n::engine::needs_translation(text, s.source_cp, s.outbound_target_lang)) {
            if (g_pendingResponses.size() > 64) flush_all_pending(); // never silently drop -- send them first
            i18n::engine::Job job;
            job.handle             = g_seq.fetch_add(1, std::memory_order_relaxed);
            job.surface            = 4; // -> hooks::apply_result -> on_response_result
            job.sourceBytes        = text;
            job.srcCodepage        = s.source_cp;
            job.skipContext        = true;
            job.noDedup            = true;
            job.promptOverride     = i18n::config::build_system_prompt(s.outbound_target_lang, false);
            job.targetLangOverride = s.outbound_target_lang;
            job.displayCpOverride  = i18n::config::display_codepage(s.outbound_target_lang, false);
            job.capBytes           = kInputReplyCap; // never longer than native input can be
            g_pendingResponses[job.handle] = { dlgId, resp, listItem, text, GetTickCount() };
            i18n::engine::submit(job);
            return false; // swallow; on_response_result or the tick() deadline sends it
        }
    } catch (...) {
        params->ResetReadPointer();
    }
    return true;
}

// ---- HUD text + 3D labels: line by line, translation memory first -------------
// (hudtext.h). A text whose lines are all in the translation memory is rewritten
// in the RPC; otherwise the ORIGINAL is shown and its missing lines are translated
// in the background (surface-2 fills). A textdraw or 3D label still on screen when
// they land is switched to the translation right there (reconcile_hud), through
// SA:MP's own RPC handler. Without that, a text the server shows ONCE -- "Vehicle
// locked", a spawn menu, a welcome box -- stayed in English: its translation only
// ever reached the memory, for a next time that never came.

// The longest text each surface really accepts (SA:MP 0.3.DL, verified in samp.dll):
//   gametext    <= 200 bytes -- the RPC 73 handler drops anything longer
//   textdraw    <  800 bytes (RPC 134: len < 800; RPC 105's buffer is 800), and only
//               <= 255 with ~k~ key codes (CTextDraw::SetText rejects longer)
//   3D label    ~k~ codes only expand below 400 bytes; the decoder holds 2048
//   chat bubble <= 144
// A translation longer than that is never cut (a cut can split a ~x~ code -- SA:MP
// then shows "Error: unmatched tilde", or drops the text): the original shows.
bool has_key_code(const std::string& s) { return s.find("~k~") != std::string::npos || s.find("~K~") != std::string::npos; }
int limit_label(const std::string& src)    { return has_key_code(src) ? 399 : 1024; }
int limit_textdraw(const std::string& src) { return has_key_code(src) ? 255 : 799; }
int limit_gametext(const std::string&)     { return 200; }
int limit_bubble(const std::string&)       { return 144; }

constexpr int      kHudLookupCap  = 4095;   // the whole memory entry; each surface's limit is checked after
constexpr uint32_t kFillLostMs    = 600000; // a fill still out after 10 min was lost: send it again
constexpr uint32_t kHudRecheckMs  = 2000;   // re-check texts still waiting (a failed line's retry falls due)
constexpr uint32_t kHudDebounceMs = 100;    // at most this often after a fill lands

// A line whose translation failed shows as it is for a while, then is retried --
// after 30 s, then 5 min, then every 30 min: a line the model keeps rejecting (a
// lost placeholder) isn't re-sent all the time. A request that failed as a whole
// (the provider down or timing out -- Result::unitProviderFailed) says nothing about
// its lines: they are retried after 30 s without counting against them.
uint32_t retry_delay(int failures) { return failures <= 1 ? 30000u : failures == 2 ? 300000u : 1800000u; }
constexpr uint32_t kProviderRetryMs = 30000;

std::unordered_map<std::string, uint32_t> g_fillOut; // key -> GetTickCount() its fill went out
struct Failed {
    uint32_t until = 0; // GetTickCount() until which the line shows as it is
    int      count = 0; // failures of the line itself, in a row
};
std::unordered_map<std::string, Failed>                g_fillFailed; // key -> its last failed fill
std::unordered_map<uint64_t, std::vector<std::string>> g_fillJobs;   // fill job handle -> its keys

// A textdraw or 3D label on screen right now, as far as the server's RPCs say.
struct OnScreen {
    std::string          text;            // the server's text (the original)
    std::string          shown;           // what is displayed: `text`, or our translation of it
    std::vector<uint8_t> pkt;             // label: its Create3DTextLabel payload; textdraw: its last ShowTextDraw
    bool                 isText  = true;  // false: a sprite / model-preview textdraw -- never translated
    bool                 waiting = false; // some line isn't translated yet, or is due for a retry
};
std::unordered_map<uint16_t, OnScreen> g_textdraws; // textdraw id -> (global 0..2047, player 2048..2303)
std::unordered_map<uint16_t, OnScreen> g_labels;    // 3D label id

// SA:MP's own handlers for the RPCs we re-send, as seen at dispatch (the RPC node's
// function): calling one with a payload we built is exactly the server sending it.
// Verified in samp.dll 0.3.DL: TextDrawSetString (105) checks the id is in range and
// the textdraw exists, and takes up to 800 bytes; ShowTextDraw (134) and
// Create3DTextLabel (36) on an id already in use first delete what is there.
using RpcHandler = void (*)(RPCParameters*);
RpcHandler g_onSetString    = nullptr;
RpcHandler g_onShowTextDraw = nullptr;
RpcHandler g_onCreateLabel  = nullptr;

bool     g_hudDirty     = false; // a fill landed: a waiting text may be complete now
bool     g_hudRecheck   = false; // settings changed: recompute every tracked text
bool     g_hudWaiting   = false; // some tracked text is waiting
uint32_t g_hudLastCheck = 0;
bool     g_sawHud       = false; // the toggles recheck_hud() saw last
bool     g_sawLabels    = false;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void put16(std::vector<uint8_t>& v, size_t n) {
    v.push_back((uint8_t)(n & 0xFF));
    v.push_back((uint8_t)((n >> 8) & 0xFF));
}

// The translation memory's answer for one line key (hudtext::Lookup). Looked up in
// the RUNNING engine's target language -- the one a fill queued now is translated
// into (a target picked in the menu applies on Save).
i18n::hudtext::Look hud_look(const std::string& key, std::string& tr) {
    const int cp     = i18n::config::get().source_cp;
    const int target = i18n::engine::active_target_lang();
    if (i18n::engine::cache_lookup(key, cp, target, tr, kHudLookupCap)) return i18n::hudtext::Look::Hit;
    // Waiting out a failed fill (checked before the costlier test below: a key only
    // ever got a fill -- and so a failure -- by passing it).
    auto f = g_fillFailed.find(key);
    if (f != g_fillFailed.end() && (int32_t)(f->second.until - GetTickCount()) > 0) return i18n::hudtext::Look::Later;
    // Already the target language / nothing translatable survives protect::mask.
    if (!i18n::engine::needs_translation(key, cp, target, /*labels=*/true)) return i18n::hudtext::Look::Keep;
    return i18n::hudtext::Look::Miss;
}

// Send the keys that aren't already out as background fills (surface 2, normal
// lane, UI-label masking): one numbered-list request per chunk, like a dialog's
// cells (one key per request while batching is off, so they run in parallel).
void queue_fills(const std::vector<std::string>& keys) {
    const uint32_t now = GetTickCount();
    if (g_fillOut.size() > 4096) g_fillOut.clear();   // (safety nets: results always
    if (g_fillJobs.size() > 4096) g_fillJobs.clear(); //  come back unless the engine restarts)
    std::vector<std::string> todo;
    for (const std::string& k : keys) {
        auto it = g_fillOut.find(k);
        if (it != g_fillOut.end() && now - it->second < kFillLostMs) continue;
        g_fillOut[k] = now;
        todo.push_back(k);
    }
    const size_t maxItems = i18n::engine::batching_enabled() ? kBatchMaxItems : 1;
    const auto& s = i18n::config::get();
    for (size_t i = 0; i < todo.size();) {
        i18n::engine::Job j;
        j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
        j.surface     = 2;
        j.srcCodepage = s.source_cp;
        j.skipContext = true;
        j.noDedup     = true;
        j.capBytes    = kHudLookupCap;
        j.labels      = true;
        size_t bytes = 0;
        while (i < todo.size() && j.units.size() < maxItems &&
               (j.units.empty() || bytes + todo[i].size() <= kBatchMaxBytes)) {
            bytes += todo[i].size();
            j.units.push_back(todo[i++]);
        }
        g_fillJobs[j.handle] = j.units;
        i18n::engine::submit(j);
    }
}

// `src` translated line by line from the translation memory: the text to show --
// every line in memory translated, the others as they are ("progressive": what is
// known shows at once, the rest follows when its fill lands), within `limit` bytes
// (a translated line that would push it over keeps its original line; see
// hudtext::resolve). `pending`: some line isn't translated yet (its fill is queued
// here) or is due for a retry -- look at it again later.
std::string hud_translate(const std::string& src, bool label, int limit, bool& pending) {
    std::vector<std::string> missing;
    std::string t;
    bool later = false;
    const bool complete = i18n::hudtext::resolve(i18n::hudtext::plan_text(src, label), hud_look, t, missing, &later,
                                                 (size_t)limit);
    if (!complete) queue_fills(missing);
    pending = !complete || later;
    return (int)t.size() > limit ? src : t; // (only when the original itself is over the limit)
}

// A Translator (rebuild_*) over hud_translate, for the texts we don't track.
Translator hud_translator(int (*limit)(const std::string&)) {
    return [limit](const std::string& a, std::string& b) {
        bool pending = false;
        std::string t = hud_translate(a, false, limit(a), pending);
        if (t == a) return false;
        b = std::move(t);
        return true;
    };
}

// SEH-isolated call into SA:MP (no C++ objects in this frame, so __except is legal).
bool safe_rpc(RpcHandler fn, RPCParameters* rp) {
    __try {
        fn(rp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Run SA:MP's handler on a payload we built, as if the server had sent it.
bool call_rpc(RpcHandler fn, std::vector<uint8_t> pkt) {
    if (!fn || pkt.empty()) return false;
    const unsigned bits = (unsigned)(pkt.size() * 8);
    pkt.push_back(0); // the handler's BitStream spans (bits / 8) + 1 bytes
    RPCParameters rp{};
    rp.input              = pkt.data();
    rp.numberOfBitsOfData = bits;
    if (safe_rpc(fn, &rp)) return true;
    Log("[i18n] on-screen text update failed (crash-guarded).");
    return false;
}

// Is textdraw / 3D label `id` in use in SA:MP's own pool right now? A game mode
// restart or a lost connection empties the pools without a hide/delete RPC per
// item, and re-sending a label (or a ShowTextDraw) into an empty slot would CREATE
// a ghost. Layouts verified in samp.dll 0.3.DL (the RPC 105/36 handlers and
// CLabelPool::Create): CTextDrawPool = int inUse[2304] at +0, CTextDraw* [2304] at
// +0x2400; CLabelPool = 2048 entries of 0x1D bytes, then int inUse[2048] at +0xE800.
bool slot_in_use_raw(const uint8_t* pool, bool label, uint16_t id) {
    __try {
        if (label) return id < 2048 && *(const int*)(pool + 0xE800 + id * 4) != 0;
        return id < 2304 && *(const int*)(pool + id * 4) != 0 && *(void* const*)(pool + 0x2400 + id * 4) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool slot_in_use(bool label, uint16_t id) {
    samp::stSAMPPools* pools = samp::pools();
    if (!pools) return false;
    const void* pool = label ? pools->pText3D : pools->pTextdraw;
    return pool && slot_in_use_raw((const uint8_t*)pool, label, id);
}

// Put `text` on textdraw `id`: TextDrawSetString (u16 id, u16 len, text); before
// SA:MP's handler for that was ever seen, re-show it with its last ShowTextDraw's
// properties (65-byte header, u16 len, text). CTextDraw::SetText takes < 800 bytes.
bool set_textdraw(uint16_t id, const OnScreen& td, const std::string& text) {
    if (text.size() >= 800 || !slot_in_use(false, id)) return false;
    std::vector<uint8_t> pkt;
    if (g_onSetString) {
        put16(pkt, id);
        put16(pkt, text.size());
        pkt.insert(pkt.end(), text.begin(), text.end());
        return call_rpc(g_onSetString, std::move(pkt));
    }
    if (g_onShowTextDraw && td.pkt.size() >= 67) {
        pkt.assign(td.pkt.begin(), td.pkt.begin() + 65);
        put16(pkt, text.size());
        pkt.insert(pkt.end(), text.begin(), text.end());
        return call_rpc(g_onShowTextDraw, std::move(pkt));
    }
    return false;
}

// Re-create 3D label `id` with `text`: its Create3DTextLabel's 27-byte header + the
// Huffman-encoded text.
bool set_label(uint16_t id, const OnScreen& lb, const std::string& text) {
    if (!g_onCreateLabel || lb.pkt.size() < 27 || !slot_in_use(true, id)) return false;
    std::vector<uint8_t> pkt(lb.pkt.begin(), lb.pkt.begin() + 27);
    const std::vector<uint8_t> enc = i18n::huffman::encode_string(text);
    pkt.insert(pkt.end(), enc.begin(), enc.end());
    return call_rpc(g_onCreateLabel, std::move(pkt));
}

// Whether a textdraw's text is text: not a sprite (font 4 -- the text names a
// texture) or a model preview (font 5). The font is byte 33 of its ShowTextDraw
// (u16 id + 31 bytes of properties; checked against the user's captures). A
// textdraw whose ShowTextDraw we never saw: unless its text looks like a sprite name.
bool textdraw_is_text(const OnScreen& td) {
    if (td.pkt.size() >= 67) return td.pkt[33] != 4 && td.pkt[33] != 5;
    return !i18n::hudtext::is_sprite_name(td.text);
}

// What a tracked text should show now: its translation as far as it goes (see
// hud_translate), or the original (the surface is off, or it isn't text).
std::string wanted(OnScreen& o, bool on, bool label, int limit) {
    o.waiting = false;
    if (!on || !o.isText) return o.text;
    return hud_translate(o.text, label, limit, o.waiting);
}

// A tracked text just arrived in an RPC (ShowTextDraw / SetString / label): what is
// in memory replaces the text right in the RPC (`rewrite` rebuilds the packet: true
// on success); the rest follows through reconcile_hud.
void on_tracked(OnScreen& o, bool on, bool label, int limit, const std::function<bool(const std::string&)>& rewrite) {
    const std::string want = wanted(o, on, label, limit);
    if (o.waiting) g_hudWaiting = true;
    o.shown = (want != o.text && rewrite(want)) ? want : o.text;
}

// Bring every tracked textdraw/label up to date: after a fill landed (debounced),
// every kHudRecheckMs while some text is waiting, and after a settings change --
// then ALL of them, so turning the surface off puts the originals back and a new
// target language re-translates what's on screen. Main thread (pump).
void reconcile_hud(uint32_t now) {
    const bool due = g_hudRecheck || (g_hudDirty && now - g_hudLastCheck >= kHudDebounceMs) ||
                     (g_hudWaiting && now - g_hudLastCheck >= kHudRecheckMs);
    if (!due) return;
    const bool all = g_hudRecheck;
    g_hudRecheck = g_hudDirty = g_hudWaiting = false;
    g_hudLastCheck = now;
    const auto& s = i18n::config::get();
    for (auto& [id, td] : g_textdraws) {
        if (!all && !td.waiting) continue;
        std::string want = wanted(td, s.translate_hud, false, limit_textdraw(td.text));
        if (td.waiting) g_hudWaiting = true;
        if (want != td.shown && set_textdraw(id, td, want)) td.shown = std::move(want);
    }
    for (auto& [id, lb] : g_labels) {
        if (!all && !lb.waiting) continue;
        std::string want = wanted(lb, s.translate_dialogs, true, limit_label(lb.text));
        if (lb.waiting) g_hudWaiting = true;
        if (want != lb.shown && set_label(id, lb, want)) lb.shown = std::move(want);
    }
}

// The pools were emptied (a game mode restart, a new session): forget every
// tracked textdraw and label -- the server shows its own again.
void forget_screen() {
    g_textdraws.clear();
    g_labels.clear();
    g_hudWaiting = false;
}

// Track + translate the HUD/label RPCs. Returns true when the RPC was rewritten
// into `out`.
bool handle_hud(unsigned char id, const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    const auto& s = i18n::config::get();
    switch (id) {
        case RPC_ShowTextDraw: { // u16 id, 63 bytes of properties, u16 len, text
            if (n < 67) return false;
            size_t len = rd16(p + 65);
            if (67 + len > n) len = n - 67;
            OnScreen& td = g_textdraws[rd16(p)];
            td.text.assign((const char*)(p + 67), len);
            td.pkt.assign(p, p + n);
            td.isText = textdraw_is_text(td);
            bool ok = false;
            on_tracked(td, s.translate_hud, false, limit_textdraw(td.text), [&](const std::string& t) {
                ok = rebuild_textdraw_show(p, n, [&t](const std::string&, std::string& o) { o = t; return true; }, out);
                return ok;
            });
            return ok;
        }
        case RPC_TextDrawSetString: { // u16 id, u16 len, text
            if (n < 4) return false;
            size_t len = rd16(p + 2);
            if (4 + len > n) len = n - 4;
            OnScreen& td = g_textdraws[rd16(p)]; // new if its ShowTextDraw was never seen
            td.text.assign((const char*)(p + 4), len);
            td.isText = textdraw_is_text(td);
            bool ok = false;
            on_tracked(td, s.translate_hud, false, limit_textdraw(td.text), [&](const std::string& t) {
                ok = rebuild_textdraw_setstring(p, n, [&t](const std::string&, std::string& o) { o = t; return true; }, out);
                return ok;
            });
            return ok;
        }
        case RPC_TextDrawHideForPlayer: // u16 id: the textdraw is gone
            if (n >= 2) g_textdraws.erase(rd16(p));
            return false;
        case RPC_Create3DTextLabel: { // u16 id + 25 bytes, then the Huffman text
            if (n <= 27) return false;
            std::string text;
            if (!i18n::huffman::decode_string(p + 27, n - 27, text)) return false;
            OnScreen& lb = g_labels[rd16(p)];
            lb.text = std::move(text);
            lb.pkt.assign(p, p + n);
            bool ok = false;
            on_tracked(lb, s.translate_dialogs, true, limit_label(lb.text), [&](const std::string& t) {
                ok = rebuild_label(p, n, [&t](const std::string&, std::string& o) { o = t; return true; }, out);
                return ok;
            });
            return ok;
        }
        case RPC_Delete3DTextLabel: // u16 id: the label is gone
            if (n >= 2) g_labels.erase(rd16(p));
            return false;
        case RPC_DisplayGameText: // gone in seconds: translated as far as the memory goes, never tracked
            return s.translate_hud && rebuild_gametext(p, n, hud_translator(limit_gametext), out);
        case RPC_ChatBubble:
            return s.translate_hud && rebuild_chatbubble(p, n, hud_translator(limit_bubble), out);
        default:
            return false;
    }
}

// A new game session (InitGame): everything still held or pending belongs to the
// OLD server/session -- drop it WITHOUT showing or sending any of it (a held
// dialog, an INPUT reply or a chat line meant for the previous server must never
// reach the new one). Late engine results then find no entry and are ignored.
void discard_session() {
    const size_t dialogs = g_dlgq.size();
    const size_t replies = g_pendingResponses.size();
    const size_t rows    = g_shownRows.size();
    g_dlgq = i18n::dlgq::Queue(kMaxHoldMs, kMaxHeldDialogs); // held dialogs: never shown
    g_pendingDialogs.clear();
    g_partJobs.clear();
    g_recentFills.clear();
    g_pendingResponses.clear();                              // INPUT replies: never sent
    g_shownRows.clear();                                     // no remap against the old dialogs' rows
    forget_screen();                                         // tracked textdraws/labels: the new session shows its own
    const size_t chat = i18n::outbound::discard_pending();   // outgoing chat: never sent
    Log("[i18n] new session (InitGame): discarded %u held dialog(s), %u pending input reply(ies), "
        "%u pending outgoing chat line(s), %u list row table(s).",
        (unsigned)dialogs, (unsigned)replies, (unsigned)chat, (unsigned)rows);
}

} // namespace

bool on_incoming_rpc(unsigned char id, RPCParameters* rpc, void (*handler)(RPCParameters*)) {
    // InitGame (a (re)connect, a server restart/GMX, another server): before any
    // other handling, discard the old session's held/pending state. Never swallowed.
    if (id == RPC_InitGame) {
        try { discard_session(); } catch (...) {} // an RPC-dispatch seam: nothing may escape
        return false;
    }
    // GameModeRestart: SA:MP empties its textdraw and label pools (no hide/delete
    // RPC per item) -- nothing tracked is on screen any more.
    if (id == RPC_GameModeRestart) {
        forget_screen();
        return false;
    }
    // SA:MP's own handlers of the RPCs reconcile_hud re-sends.
    if (handler) {
        if (id == RPC_TextDrawSetString)      g_onSetString    = handler;
        else if (id == RPC_ShowTextDraw)      g_onShowTextDraw = handler;
        else if (id == RPC_Create3DTextLabel) g_onCreateLabel  = handler;
    }
    if (!rpc || !rpc->input || rpc->numberOfBitsOfData < 8) return false;
    auto& s = i18n::config::get();
    const uint8_t* p = rpc->input;
    size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);

    try {
        if (id == RPC_ShowDialog) {
            if (!s.translate_dialogs) {
                note_shown(std::vector<uint8_t>(p, p + n), nullptr); // shown natively -> erase any stale entry
                return false;
            }
            return handle_dialog(rpc, p, n);
        }
        std::vector<uint8_t> out;
        if (handle_hud(id, p, n, out) && !out.empty()) commit(rpc, out);
    } catch (...) {
        // Never let an exception cross back into SA:MP's RPC dispatch.
    }
    return false;
}

void on_fill_result(const i18n::engine::Result& r) {
    g_hudDirty = true; // any fill (a dialog's background batch too) may complete a waiting text
    auto it = g_fillJobs.find(r.handle);
    if (it == g_fillJobs.end()) return;
    const std::vector<std::string> keys = std::move(it->second);
    g_fillJobs.erase(it);
    const uint32_t now    = GetTickCount();
    const int      cp     = i18n::config::get().source_cp;
    const int      target = i18n::engine::active_target_lang();
    std::string    tr;
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& k = keys[i];
        g_fillOut.erase(k);
        const bool came     = i < r.unitOut.size() && !r.unitOut[i].empty();
        const bool provDown = i < r.unitProviderFailed.size() && r.unitProviderFailed[i];
        // Translated -- and findable: a translation the lookup can't see (stored for
        // another source codepage by now) counts as failed, or it would be fetched
        // again round after round.
        if (came && i18n::engine::cache_lookup(k, cp, target, tr, kHudLookupCap)) {
            g_fillFailed.erase(k);
        } else if (!came && provDown) {
            g_fillFailed[k].until = now + kProviderRetryMs; // not the line's fault: not counted
        } else {
            Failed& f = g_fillFailed[k];
            f.until = now + retry_delay(++f.count);
        }
    }
    if (g_fillFailed.size() > 2048) g_fillFailed.clear(); // (only ever a few in practice)
}

void recheck_hud() {
    // Only when a toggle changed: every Save and the outgoing hotkey call this, and a
    // new target language / model restarts the engine (forget_fills rechecks then).
    const auto& s = i18n::config::get();
    if (s.translate_hud != g_sawHud || s.translate_dialogs != g_sawLabels) g_hudRecheck = true;
    g_sawHud    = s.translate_hud;
    g_sawLabels = s.translate_dialogs;
}

void forget_fills() {
    g_fillOut.clear();    // their jobs were dropped with the engine's queue
    g_fillJobs.clear();
    g_fillFailed.clear(); // another model / target language: worth trying again
    g_hudRecheck = true;
}

void on_part_result(const i18n::engine::Result& r) {
    auto pj = g_partJobs.find(r.handle);
    if (pj == g_partJobs.end()) return;
    const uint64_t                         seq    = pj->second.seq;
    const std::vector<std::vector<size_t>> groups = std::move(pj->second.groups);
    g_partJobs.erase(pj);
    auto it = g_pendingDialogs.find(seq);
    if (it == g_pendingDialogs.end()) return; // already released (timed out)
    PendingDialog& pd = it->second;
    // The batch's i-th text fills every unit of groups[i]; "" (or a missing tail,
    // e.g. after an internal error) keeps those units' original text.
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g >= r.unitOut.size() || r.unitOut[g].empty()) continue;
        for (size_t unit : groups[g])
            if (unit < pd.cores.size()) pd.cores[unit] = r.unitOut[g];
    }
    if (--pd.outstanding > 0) return; // other batch jobs of this dialog still out
    g_dlgq.resolve(seq, assemble_dialog(pd.plan, fit_list_budgets(pd.plan, pd.cores)));
    g_pendingDialogs.erase(it);
}

void tick(uint32_t nowMs) {
    flush_due_responses(nowMs); // guaranteed delivery: nothing waits past its deadline
    reconcile_hud(nowMs);       // on-screen textdraws/labels: switch to translations that landed
    if (g_dlgq.empty()) return;
    std::vector<i18n::dlgq::Release> out =
        i18n::config::get().translate_dialogs ? g_dlgq.tick(nowMs) : g_dlgq.flush();
    for (auto& r : out) {
        g_pendingDialogs.erase(r.seq); // a straggler that timed out
        show_payload(r.payload);
        note_shown(r.original, r.translated ? &r.payload : nullptr);
    }
}

bool holding() { return !g_dlgq.empty(); }

void on_response_result(const i18n::engine::Result& r) {
    auto it = g_pendingResponses.find(r.handle);
    if (it == g_pendingResponses.end()) return;
    PendingResponse pr = std::move(it->second);
    g_pendingResponses.erase(it);
    const bool ok = r.ok && !r.displayBytes.empty();
    send_dialog_response(pr.dlgId, pr.resp, pr.listItem, ok ? r.displayBytes : pr.original);
    Log("[i18n] dialog %d: input reply sent %s.", (int)pr.dlgId, ok ? "translated" : "as typed");
}

void flush_responses() { flush_all_pending(); }

void install() {
    net::on_send_rpc(on_send_dialogresponse);
    Log("[i18n] surfaces: DialogResponse handler registered (list remap + input translation).");
}

} // namespace i18n::surfaces
