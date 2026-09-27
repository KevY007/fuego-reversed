#include "i18n/engine.h"

#include "i18n/charset.h"
#include "i18n/detect.h"
#include "i18n/protect.h"
#include "i18n/cache.h"
#include "i18n/validate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace i18n::engine {

namespace {

// 2-4 workers per the brief; picked a fixed value in that range since
// Phase 0 has no signal (queue depth, core count) worth tuning against yet.
constexpr int kWorkerCount = 3;

// ---- pipeline-only state -------------------------------------------------

// Provider-profile string folded into cache keys (see cache::key). Set by
// start(); defaults to "" so process_one() is directly callable -- as
// tests/engine_test.cpp does -- without ever starting the threaded engine.
std::string g_profile;

// ---- threaded engine state ------------------------------------------------
// g_provider/g_sysPrompt/g_profile above are written only by start(), and
// only before any worker thread is created (std::thread's constructor
// establishes a happens-before edge with the new thread's first
// instruction), and never mutated again while a worker could be reading
// them -- stop() only resets g_provider after join()ing every worker. So
// worker threads need no extra synchronization to read them safely.
std::unique_ptr<IProvider> g_provider;
std::string g_sysPrompt;
int g_targetLang = 1033;  // written only by start() (same publication rules as g_sysPrompt)
int g_displayCp  = 1252;
bool g_ctxEnabled = false; // send recent chat as context (context_for_provider); set by start(), read by submit() -- both main thread
std::vector<std::thread> g_workers;
std::atomic<bool> g_running{false};

// Job queue (two lanes, see JobQueue in engine.h) + in-flight dedup set: both
// guarded by g_jobMutex.
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
JobQueue g_jobs;
std::unordered_set<size_t> g_inFlight; // hash(sourceBytes) queued or being worked on

// Rolling context: the last few incoming lines (UTF-8), so each translation
// can see the recent conversation and stay relevant. Written and read only
// under g_jobMutex, from submit() (main thread), so it stays in chat order.
constexpr size_t kCtxLines = 4;
std::deque<std::string> g_recentCtx;

// Result queue: guarded by its own mutex, separate from g_jobMutex, so a
// worker finishing a job and the main thread pumping results never block
// on each other's queue.
std::mutex g_resultMutex;
std::deque<Result> g_results;

// Only ever read/written from the main thread (set_apply()/pump() are both
// documented main-thread-only), so this needs no synchronization.
void (*g_applyFn)(const Result&) = nullptr;

// Status is a pointer to a static string literal, swapped atomically. That
// (rather than a mutex-guarded std::string) is what makes status() safe to
// call from the main thread while a worker concurrently updates it: there
// is never a moment where the pointee is being mutated or freed, so a
// reader can hold the returned pointer as long as it likes.
std::atomic<const char*> g_status{"idle"};

// Detailed last-error string (guarded by its own mutex), surfaced by the menu
// next to status() so a failing translate says WHY, not just "error - retrying".
std::mutex g_errMutex;
std::string g_lastErr;

// Provider response-time stats for the menu readout. Updated only after an ACTUAL
// prov->translate() call that was ANSWERED -- including a batch the provider
// reports as a soft failure (see IProvider::translate) -- (cache hits and
// pre-provider skips never reach the provider, so they don't count). avg = running mean since the last
// start(); last = the most recent call. Both are wall time around translate(), so
// they INCLUDE the request-pacer wait and any 429/503 backoff-retries -- i.e. the
// real end-to-end delay a user feels. Guarded by its own mutex; menu reads a snapshot.
std::mutex    g_statsMutex;
uint64_t      g_respCount   = 0;
uint64_t      g_respTotalMs = 0;
unsigned long g_respLastMs  = 0;

// Batches that failed on their own (a broken numbered layout, a timeout, a cut-off
// or rejected request), in a row. At kBatchGiveUp this model/server evidently
// can't do lists: batching is switched off (each batch would just be thrown away
// and retried item by item) and re-tried after kBatchReprobeMs (see
// batching_enabled()). Reset by any batch that keeps its layout.
constexpr int    kBatchGiveUp = 2;
std::atomic<int> g_batchBroken{0};
// When batching was switched off (steady-clock ms; 0 = on). After kBatchReprobeMs
// one more batch is tried (see batching_enabled()).
constexpr int64_t    kBatchReprobeMs = 10 * 60 * 1000;
std::atomic<int64_t> g_batchOffAt{0};

// Recency-weighted latency per request kind, for the held-dialog estimate, and
// when the session's first reply arrived (steady ms; 0 = none yet): a request that
// started before it paid the cold model load and is never offered as a sample.
// Guarded by g_statsMutex.
LatencyTracker g_latSingle;
LatencyTracker g_latBatch;
int64_t        g_firstReplyAt = 0;

// Jobs a worker is processing right now (popped, result not yet pushed).
std::atomic<int> g_active{0};

// Set while stop() waits for the workers: process_batch stops its one-by-one
// retries so a settings change (stop() runs on the render thread) never waits for
// a whole chunk of per-cell requests.
std::atomic<bool> g_stopping{false};

int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- why a text was not translated (drained into the log on the main thread) ----
std::mutex              g_noteMutex;
std::deque<std::string> g_notes;
size_t                  g_notesDropped = 0;
constexpr size_t        kMaxNotes = 20;

void note_failure(const std::string& why, const std::string& src) {
    std::string s = why + ": '" + src.substr(0, 80) + (src.size() > 80 ? "...'" : "'");
    std::lock_guard<std::mutex> lk(g_noteMutex);
    if (!g_notes.empty() && g_notes.back() == s) return; // the same text failing again
    if (g_notes.size() >= kMaxNotes) {
        ++g_notesDropped;
        return;
    }
    g_notes.push_back(std::move(s));
}

// ---- the per-text pipeline, shared by process_one and process_batch ---------

// Length of the placeholder ("\x01" digits "\x02") starting at t[i], or 0.
size_t placeholder_len(const std::string& t, size_t i) {
    if (i >= t.size() || t[i] != '\x01') return 0;
    size_t j = i + 1;
    while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
    return (j > i + 1 && j < t.size() && t[j] == '\x02') ? j + 1 - i : 0;
}

// Split masked text into lead + core + trail: the placeholders (colour / format /
// key codes, names, ...) and spaces at the very START and END are kept out of what
// the model sees and re-attached verbatim afterwards. Models tend to drop exactly
// those (live: Hy-MT2 dropped the leading ~W~ of "~W~Press ~Y~~k~~...~ ~W~to open
// hood" every time, so the reply failed validation and the textdraw never got
// translated); inner placeholders still go to the model, which has to keep them.
// Spaces are only held back together with an edge code (the space in "<name> says:"
// must survive); plain leading/trailing spaces go to the model as before.
void split_edges(const std::string& t, std::string& lead, std::string& core, std::string& trail) {
    size_t a = 0;
    bool leadCode = false;
    for (;;) {
        if (a < t.size() && (t[a] == ' ' || t[a] == '\t')) { ++a; continue; }
        const size_t n = placeholder_len(t, a);
        if (n == 0) break;
        a += n;
        leadCode = true;
    }
    if (!leadCode) a = 0;
    size_t b = t.size();
    bool trailCode = false;
    for (;;) {
        if (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t')) { --b; continue; }
        if (b > a && t[b - 1] == '\x02') {
            const size_t k = t.rfind('\x01', b - 1);
            if (k != std::string::npos && k >= a && placeholder_len(t, k) == b - k) {
                b = k;
                trailCode = true;
                continue;
            }
        }
        break;
    }
    if (!trailCode) b = t.size();
    lead  = t.substr(0, a);
    core  = t.substr(a, b - a);
    trail = t.substr(b);
}

// Steps 1-5 for one text. Skip: not worth translating / nothing translatable
// survives masking. Hit: the cache already has it (`hit` = the capped display
// text). Need: send `send` (the masked text without its edge codes) to the
// provider, then finish().
struct Prepared {
    enum Kind { Skip, Hit, Need } kind = Skip;
    std::string     hit;
    uint64_t        key = 0;
    protect::Masked masked;
    std::string     lead, send, trail; // masked.text = lead + send + trail (see split_edges)
};

Prepared prepare(const std::string& srcBytes, int cp, int targetLang, size_t cap) {
    Prepared p;
    // 1-2: pick the source codepage and decode to a wide string.
    int cp2 = (cp != 0) ? cp : charset::guess_codepage(srcBytes);
    std::wstring w = charset::decode(srcBytes, cp2);

    // 3: skip anything not worth translating (empty, digits/punct-only, or
    // already the target language -- only cheaply detectable for English).
    if (!detect::worth_translating(w, /*targetIsEnglish=*/targetLang == 1033)) return p;

    // 4: cache lookup keyed on the normalized (UTF-8) source text + target. The
    // cache always stores the FULL translation (see finish()) -- cap THIS call's
    // hit to its own consumer's limit rather than trusting whatever length the
    // entry happened to be capped to when a different consumer wrote it.
    // A hit that displays as nothing (master stored "" for some lines; '%'-only
    // text caps to "") is a MISS -- translate it again instead of blanking the text.
    std::string normSrc = charset::to_utf8(w);
    p.key = cache::key(normSrc, cp2, targetLang, g_profile);
    {
        std::string hit;
        if (cache::get(p.key, hit)) {
            hit = charset::cap_display(hit, cap);
            if (!hit.empty()) {
                p.kind = Prepared::Hit;
                p.hit  = std::move(hit);
                return p;
            }
        }
    }

    // 5: mask non-translatable spans (commands, colours, names, coords, ...); the
    // codes at the very edges stay out of the model's input (split_edges).
    p.masked = protect::mask(normSrc);
    if (!p.masked.translatable) return p;
    split_edges(p.masked.text, p.lead, p.send, p.trail);
    p.kind = Prepared::Need;
    return p;
}

// Step 6: one provider request, with the status / last-error / response-time
// bookkeeping. False = the request itself failed (network, HTTP, breaker open).
bool call_provider(IProvider* prov, const std::vector<std::string>& texts, const std::string& sysPrompt,
                   const std::string& rollingCtx, std::vector<std::string>& out, std::string& err) {
    err.clear();
    const int64_t startMs = now_ms();
    auto t0 = std::chrono::steady_clock::now();
    bool callOk = prov->translate(texts, sysPrompt, rollingCtx, out, err);
    unsigned long dt = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    // Latency for the held-dialog estimate: a request answered cleanly, or one that
    // timed out ("timeout" / "timeout (batch)") -- a provider that doesn't answer in
    // time is the slowest of all. A batch that failed fast on its own (rejected, a
    // broken layout) is left out: its cells' one-by-one retries, not that round
    // trip, are what delayed the dialog. The session's first reply, and every
    // request that started before it arrived, paid the cold model load: never a sample.
    const bool timedOut = (err == "timeout" || err == "timeout (batch)");
    if ((callOk && err.empty()) || timedOut) {
        std::lock_guard<std::mutex> lk(g_statsMutex);
        if (g_firstReplyAt == 0)
            g_firstReplyAt = now_ms();
        else if (startMs >= g_firstReplyAt)
            (texts.size() > 1 ? g_latBatch : g_latSingle).add((double)dt);
    }
    if (callOk) {
        g_status.store("local ok", std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(g_errMutex); g_lastErr.clear(); }
        std::lock_guard<std::mutex> lk(g_statsMutex);
        g_respCount++; g_respTotalMs += dt; g_respLastMs = dt;
    } else {
        g_status.store("error - retrying", std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_errMutex); g_lastErr = err;
    }
    return callOk;
}

// Steps 7-10 for one text's model output (the translation of p.send; the edge codes
// are put back around it first). "" = it can't be trusted: keep the original --
// `why` (if given) then says what was wrong, for the log.
std::string finish(const Prepared& p, const std::string& modelOut, int targetLang, int displayCp, size_t cap,
                   const char** why = nullptr) {
    auto reject = [&](const char* reason) {
        if (why) *why = reason;
        return std::string();
    };
    if (modelOut.empty()) return reject("empty reply");
    const std::string full = p.lead + modelOut + p.trail;

    // A one-line text answered with several lines: the model translated something
    // else as well -- typically the chat context it was only meant to read.
    if (full.find('\n') != std::string::npos && p.masked.text.find('\n') == std::string::npos)
        return reject("reply has extra lines (it translated something else, e.g. the chat context)");

    // 7: sanity-check the model's output (placeholders intact, non-empty,
    // not wildly longer than the source) before trusting it.
    if (!validate::result_ok(p.masked.text, full, p.masked.tokens.size())) {
        return reject(protect::placeholders_intact(full, p.masked.tokens.size())
                          ? "reply much longer than the source"
                          : "reply lost or repeated a placeholder (a command, name, colour or number)");
    }

    // 8: restore the masked spans. unmask() returns "" if a placeholder was
    // lost/duplicated/corrupted; when there were no placeholders to begin
    // with it just returns the text unchanged, so un can only be empty here
    // if tokens was non-empty and something really did go wrong.
    std::string un = protect::unmask(full, p.masked.tokens);
    if (un.empty() && !p.masked.tokens.empty()) return reject("placeholders could not be restored");

    // 9: glossary pass -- canonicalises GTA:SA vehicle + San Andreas place
    // names so they never come back mistranslated or misspelled.
    un = protect::apply_glossary(un, targetLang);

    // 10: encode for display; cache the FULL translation (capped only to a
    // generous absolute ceiling, not to this call's consumer-specific limit) so
    // a LATER call with a DIFFERENT capBytes (e.g. a dialog unit reusing a
    // chat-translated string, or vice versa) reads back the untruncated text and
    // caps it to ITS OWN limit instead of inheriting whatever a different
    // consumer happened to ask for.
    std::string disp = charset::cap_display(charset::encode(charset::from_utf8(un), displayCp), 4095);
    cache::put(p.key, disp); // ignores an empty value
    std::string capped = charset::cap_display(disp, cap);
    if (capped.empty()) return reject("nothing left to show after encoding"); // keep the original
    return capped;
}

void worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            // g_jobs.empty() covers BOTH lanes.
            g_jobCv.wait(lock, [] { return !g_running.load() || !g_jobs.empty(); });
            if (!g_running.load()) return; // shutting down: abandon anything still queued
            g_jobs.pop(job);               // priority lane (outbound chat, dialog units/replies) first
            g_active.fetch_add(1);         // counted as work ahead of new jobs until its result is out
        }

        // Per-job direction overrides (outbound uses them); fall back to the
        // engine defaults set by start().
        const std::string& sp = job.promptOverride.empty() ? g_sysPrompt : job.promptOverride;
        int tl  = job.targetLangOverride ? job.targetLangOverride : g_targetLang;
        int dcp = job.displayCpOverride  ? job.displayCpOverride  : g_displayCp;

        std::string disp;
        std::vector<std::string> unitOut;
        bool ok = false;
        try {
            if (!job.units.empty())
                ok = process_batch(job.units, job.srcCodepage, g_provider.get(), sp, unitOut,
                                   tl, dcp, job.capBytes);
            else
                ok = process_one(job.sourceBytes, job.srcCodepage, g_provider.get(), sp, disp,
                                 tl, dcp, job.rollingCtx, job.capBytes);
        } catch (...) {
            // Realistically only std::bad_alloc anywhere in this call graph
            // (string/vector/wstring allocations along the pipeline). An
            // exception escaping a std::thread's entry function calls
            // std::terminate() and kills the whole game process, so this
            // must never propagate -- treat it like any other pipeline
            // failure (leave the original text showing) instead. Falling
            // through still pushes a Result and erases the in-flight hash
            // below, exactly like the ok==false path, so a throw can
            // neither crash the process nor permanently dedup-block this
            // source.
            ok = false;
            disp.clear();
            unitOut.clear(); // a batch consumer treats missing items as "keep the original"
        }

        {
            Result r{ job.handle, job.surface, disp, ok };
            r.unitOut = std::move(unitOut);
            std::lock_guard<std::mutex> lock(g_resultMutex);
            g_results.push_back(std::move(r));
        }
        g_active.fetch_sub(1);
        if (!job.noDedup && job.units.empty()) { // batch jobs were never entered into the set
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_inFlight.erase(std::hash<std::string>{}(job.sourceBytes) * 31u + (size_t)job.surface);
        }
    }
}

} // namespace

bool process_one(const std::string& srcBytes, int cp, IProvider* prov,
                  const std::string& sysPrompt, std::string& displayOut,
                  int targetLang, int displayCp, const std::string& rollingCtx, int capBytes) {
    const size_t cap = (size_t)(capBytes > 0 ? capBytes : 144);
    Prepared p = prepare(srcBytes, cp, targetLang, cap); // steps 1-5
    if (p.kind == Prepared::Hit) { displayOut = std::move(p.hit); return true; }
    if (p.kind == Prepared::Skip) return false;

    // 6: translate the masked text, passing recent chat as background context.
    // No provider, a network failure, or an empty reply all mean "can't trust
    // this" -- leave the original text.
    if (!prov) return false;
    std::vector<std::string> out;
    std::string err;
    if (!call_provider(prov, { p.send }, sysPrompt, rollingCtx, out, err) || out.empty()) {
        note_failure("provider failed (" + (err.empty() ? std::string("no reply") : err) + ")", srcBytes);
        return false;
    }

    const char* why = nullptr;
    std::string s = finish(p, out[0], targetLang, displayCp, cap, &why); // steps 7-10
    if (s.empty()) {
        note_failure(why ? why : "reply rejected", srcBytes);
        return false;
    }
    displayOut = std::move(s);
    return true;
}

bool process_batch(const std::vector<std::string>& units, int cp, IProvider* prov,
                   const std::string& sysPrompt, std::vector<std::string>& out,
                   int targetLang, int displayCp, int capBytes) {
    const size_t cap = (size_t)(capBytes > 0 ? capBytes : 144);
    out.assign(units.size(), std::string());

    // Steps 1-5 per text: skips and cache hits never reach the provider.
    std::vector<Prepared> prep(units.size());
    std::vector<size_t> need;
    bool any = false;
    for (size_t i = 0; i < units.size(); ++i) {
        prep[i] = prepare(units[i], cp, targetLang, cap);
        if (prep[i].kind == Prepared::Hit) {
            out[i] = std::move(prep[i].hit);
            any = true;
        } else if (prep[i].kind == Prepared::Need) {
            need.push_back(i);
        }
    }
    if (need.empty() || !prov) return any;

    // One numbered-list request for everything still missing (a lone text goes as
    // a normal single request). Each returned item is finished on its own; an item
    // that is missing or fails validation is retried alone below.
    std::vector<size_t> retry;
    if (need.size() >= 2 && batching_enabled()) {
        std::vector<std::string> texts;
        texts.reserve(need.size());
        for (size_t i : need) texts.push_back(prep[i].send);
        std::vector<std::string> res;
        std::string err;
        if (!call_provider(prov, texts, sysPrompt, std::string(), res, err)) { // provider down: no retry storm
            note_failure("provider failed (" + (err.empty() ? std::string("no reply") : err) + ") for a dialog of " +
                             std::to_string(need.size()) + " lines, first",
                         units[need.front()]);
            return any;
        }
        bool layoutKept = false;
        for (size_t j = 0; j < need.size(); ++j) {
            const std::string item = (j < res.size()) ? res[j] : std::string();
            if (!item.empty()) layoutKept = true;
            std::string s = finish(prep[need[j]], item, targetLang, displayCp, cap);
            if (s.empty()) {
                retry.push_back(need[j]);
            } else {
                out[need[j]] = std::move(s);
                any = true;
            }
        }
        // The provider hands back ALL-empty items when the batch failed on its own
        // (a broken numbered layout, a timeout, a cut-off or rejected request). Two
        // in a row: this model/server can't do lists -- switch batching off.
        if (layoutKept) {
            g_batchBroken.store(0);
        } else if (g_batchBroken.fetch_add(1) + 1 == kBatchGiveUp) {
            g_batchOffAt.store(now_ms());
        }
    } else {
        retry = need;
    }

    for (size_t i : retry) {
        if (g_stopping.load()) break; // a settings change is waiting for this worker
        std::vector<std::string> res;
        std::string err;
        if (!call_provider(prov, { prep[i].send }, sysPrompt, std::string(), res, err)) { // provider down: stop
            note_failure("provider failed (" + (err.empty() ? std::string("no reply") : err) + ")", units[i]);
            break;
        }
        if (res.empty()) continue;
        const char* why = nullptr;
        std::string s = finish(prep[i], res[0], targetLang, displayCp, cap, &why);
        if (s.empty()) {
            note_failure(why ? why : "reply rejected", units[i]);
        } else {
            out[i] = std::move(s);
            any = true;
        }
    }
    return any;
}

bool context_for_provider(int providerKind) {
    return providerKind == 1 || providerKind == 2 || providerKind == 3;
}

std::vector<std::string> drain_failure_notes() {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::vector<std::string> v(g_notes.begin(), g_notes.end());
    g_notes.clear();
    if (g_notesDropped) {
        v.push_back("(" + std::to_string(g_notesDropped) + " more not shown)");
        g_notesDropped = 0;
    }
    return v;
}

bool batching_enabled() {
    if (g_batchBroken.load() < kBatchGiveUp) return true;
    const int64_t off = g_batchOffAt.load();
    if (off != 0 && now_ms() - off >= kBatchReprobeMs) {
        // Give it one more try after a while (a model reload, other menus, ...): a
        // kept layout turns batching back on for good; one more broken layout
        // switches it off again for another interval.
        g_batchOffAt.store(0);
        g_batchBroken.store(kBatchGiveUp - 1);
        return true;
    }
    return false;
}

void reset_batch_for_test() {
    g_batchBroken.store(0);
    g_batchOffAt.store(0);
}

void expire_batch_off_for_test() {
    if (g_batchOffAt.load() != 0) g_batchOffAt.store(now_ms() - kBatchReprobeMs - 1);
}

void reset_latency_for_test() {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    g_latSingle.reset();
    g_latBatch.reset();
    g_firstReplyAt = 0;
}

Latency latency() {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    Latency l;
    l.singleKnown = g_latSingle.known();
    l.singleMs    = g_latSingle.ewma;
    l.batchKnown  = g_latBatch.known();
    l.batchMs     = g_latBatch.ewma;
    return l;
}

void start(const ProviderConfig& cfg, const std::string& systemPrompt,
           int targetLang, int displayCp) {
    if (g_running.load()) {
        stop(); // restart cleanly rather than leaking the previous pool
    }

    g_provider = make_openai_compatible(cfg);
    g_profile = cfg.model;
    g_sysPrompt = systemPrompt;
    g_targetLang = targetLang;
    g_displayCp = displayCp;
    g_ctxEnabled = context_for_provider(cfg.providerKind);
    { std::lock_guard<std::mutex> lock(g_jobMutex); g_recentCtx.clear(); }
    g_status.store("idle", std::memory_order_relaxed);
    // Reset response-time stats: a new provider/model has its own latency profile,
    // so don't average across a switch (e.g. local Hy-MT2 -> a cloud endpoint).
    {
        std::lock_guard<std::mutex> lk(g_statsMutex);
        g_respCount = 0; g_respTotalMs = 0; g_respLastMs = 0;
        g_latSingle.reset(); // the new provider/model's first request is a cold load again
        g_latBatch.reset();
        g_firstReplyAt = 0;
    }
    g_batchBroken.store(0); // a new model/provider gets a fresh chance to keep numbered lists
    g_batchOffAt.store(0);

    // Safe without g_jobMutex, unlike stop()'s flip: stop() above (if we
    // were running) already joined every prior worker before returning, and
    // the workers created below don't exist yet, so there is provably no
    // thread that could be parked in cv.wait() reading g_running right now.
    // Each new worker's first predicate check is ordered after this store
    // by std::thread's constructor happens-before guarantee.
    g_running.store(true);
    g_workers.clear();
    g_workers.reserve(kWorkerCount);
    for (int i = 0; i < kWorkerCount; ++i) {
        g_workers.emplace_back(worker_loop);
    }
}

void stop() {
    {
        // g_running must flip to false WHILE holding g_jobMutex: a worker's
        // wait() predicate reads g_running under that same mutex, and
        // condition_variable's unlock+block is only atomic with respect to
        // another thread that also holds the mutex. Flipping the flag (and
        // therefore publishing it) without the lock leaves a real window
        // where a worker reads the stale "still running" value, decides to
        // block, and hasn't yet fully parked as a CV waiter when
        // notify_all() fires on another core -- notify_all() only wakes
        // already-parked waiters (it queues no signal), so that worker
        // would then wait forever and stop()'s join() below would hang.
        std::lock_guard<std::mutex> lock(g_jobMutex);
        if (!g_running.exchange(false)) return; // already stopped
    }
    g_stopping.store(true); // a worker mid-batch stops its one-by-one retries
    g_jobCv.notify_all();
    for (auto& t : g_workers) {
        if (t.joinable()) t.join();
    }
    g_workers.clear();
    g_stopping.store(false);

    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        g_jobs.clear(); // both lanes
        g_inFlight.clear();
        g_recentCtx.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        g_results.clear();
    }
    g_provider.reset();
}

void submit(const Job& job) {
    // A batch (a dialog's cells) has no sourceBytes of its own: it is never
    // deduplicated and never part of the rolling chat context.
    const bool isBatch = !job.units.empty();
    size_t h = std::hash<std::string>{}(job.sourceBytes) * 31u + (size_t)job.surface;

    // Decode this line to UTF-8 once, outside the lock, so it's JSON-safe to
    // reuse as future context (raw cp1251/cp1252 bytes would corrupt the
    // request body). Short chat strings -> negligible cost on the game thread.
    std::string utf8Line;
    if (g_ctxEnabled && !isBatch && !job.skipContext) {
        int cp = (job.srcCodepage != 0) ? job.srcCodepage : charset::guess_codepage(job.sourceBytes);
        utf8Line = charset::to_utf8(charset::decode(job.sourceBytes, cp));
    }

    Job j = job;
    std::lock_guard<std::mutex> lock(g_jobMutex);
    if (!job.noDedup && !isBatch && !g_inFlight.insert(h).second) return; // identical source already queued/in-flight: dedup

    // Rolling context is the inbound chat conversation. Outbound jobs (which the
    // player is composing, translated the other way) opt out via skipContext so
    // they neither read nor pollute the inbound context ring. Only for providers
    // whose models read context instead of translating it (context_for_provider).
    if (g_ctxEnabled && !job.skipContext && !isBatch) {
        // Attach the recent conversation (oldest -> newest) as background context
        // for this job, BEFORE recording the current line so a line is never its
        // own context.
        if (!g_recentCtx.empty()) {
            std::string ctx = "Recent chat (context only, do NOT translate):\n";
            for (const std::string& line : g_recentCtx) {
                ctx += "- ";
                ctx += line;
                ctx += "\n";
            }
            j.rollingCtx = std::move(ctx);
        }

        // Remember this line for the next few jobs' context.
        g_recentCtx.push_back(std::move(utf8Line));
        if (g_recentCtx.size() > kCtxLines) g_recentCtx.pop_front();
    }

    g_jobs.push(std::move(j)); // surfaces 1/3/4 -> the priority lane
    g_jobCv.notify_one();
}

void set_apply(void (*fn)(const Result&)) {
    g_applyFn = fn;
}

void pump() {
    std::deque<Result> ready;
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        ready.swap(g_results);
    }
    if (!g_applyFn) return;
    for (const Result& r : ready) {
        g_applyFn(r); // called OUTSIDE g_resultMutex: applier may call back into submit()
    }
}

const char* status() {
    return g_status.load(std::memory_order_relaxed);
}

std::string last_error() {
    std::lock_guard<std::mutex> lk(g_errMutex);
    return g_lastErr;
}

size_t backlog() {
    std::lock_guard<std::mutex> lock(g_jobMutex);
    const int active = g_active.load();
    return g_jobs.prio.size() + (size_t)(active > 0 ? (active + 1) / 2 : 0); // a running job is ~half done
}

int worker_count() {
    return kWorkerCount;
}

RespStats response_stats() {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    RespStats r;
    r.count  = g_respCount;
    r.avgMs  = g_respCount ? (unsigned long)(g_respTotalMs / g_respCount) : 0;
    r.lastMs = g_respLastMs;
    return r;
}

bool cache_lookup(const std::string& srcBytes, int cp, int targetLang, std::string& out, int capBytes) {
    // Mirror process_one()'s key computation EXACTLY so a hit here matches what a
    // fill job stored (decode -> normalized UTF-8 -> key(normSrc, cp, target, profile)).
    if (srcBytes.empty()) return false;
    int cp2 = (cp != 0) ? cp : charset::guess_codepage(srcBytes);
    std::string normSrc = charset::to_utf8(charset::decode(srcBytes, cp2));
    uint64_t k = cache::key(normSrc, cp2, targetLang, g_profile);
    if (!cache::get(k, out)) return false;
    out = charset::cap_display(out, (size_t)(capBytes > 0 ? capBytes : 144));
    // An entry that displays as nothing is a MISS: an empty dialog unit blanks it
    // (an empty title makes the whole dialog vanish) and empty text poisons chat.
    if (out.empty()) return false;
    return true;
}

bool needs_translation(const std::string& srcBytes, int cp, int targetLang) {
    // Mirrors process_one()'s pre-network declines (steps 1-3 and 5) EXACTLY,
    // without ever touching the cache or the network.
    if (srcBytes.empty()) return false;
    int cp2 = (cp != 0) ? cp : charset::guess_codepage(srcBytes);
    std::wstring w = charset::decode(srcBytes, cp2);
    if (!detect::worth_translating(w, /*targetIsEnglish=*/targetLang == 1033)) return false;
    if (!protect::mask(charset::to_utf8(w)).translatable) return false;
    return true;
}

} // namespace i18n::engine
