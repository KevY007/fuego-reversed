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
// prov->translate() call that SUCCEEDED (cache hits and pre-provider skips never
// reach the provider, so they don't count). avg = running mean since the last
// start(); last = the most recent call. Both are wall time around translate(), so
// they INCLUDE the request-pacer wait and any 429/503 backoff-retries -- i.e. the
// real end-to-end delay a user feels. Guarded by its own mutex; menu reads a snapshot.
std::mutex    g_statsMutex;
uint64_t      g_respCount   = 0;
uint64_t      g_respTotalMs = 0;
unsigned long g_respLastMs  = 0;

// Batch replies that came back with their numbered layout broken, in a row. At
// kBatchGiveUp the model evidently can't keep a numbered list: process_batch stops
// sending batches (each would just be thrown away and retried item by item) until
// the next start(). Reset by any batch that keeps its layout.
constexpr int    kBatchGiveUp = 2;
std::atomic<int> g_batchBroken{0};
// When batching was switched off (steady-clock ms; 0 = on). After kBatchReprobeMs
// one more batch is tried (see batching_enabled()).
constexpr int64_t    kBatchReprobeMs = 10 * 60 * 1000;
std::atomic<int64_t> g_batchOffAt{0};

// Recency-weighted latency per request kind, for the held-dialog estimate.
// Guarded by g_statsMutex.
LatencyTracker g_latSingle;
LatencyTracker g_latBatch;

// Jobs a worker is processing right now (popped, result not yet pushed).
std::atomic<int> g_active{0};

int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the per-text pipeline, shared by process_one and process_batch ---------

// Steps 1-5 for one text. Skip: not worth translating / nothing translatable
// survives masking. Hit: the cache already has it (`hit` = the capped display
// text). Need: send `masked.text` to the provider, then finish().
struct Prepared {
    enum Kind { Skip, Hit, Need } kind = Skip;
    std::string     hit;
    uint64_t        key = 0;
    protect::Masked masked;
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

    // 5: mask non-translatable spans (commands, colours, names, coords, ...).
    p.masked = protect::mask(normSrc);
    if (!p.masked.translatable) return p;
    p.kind = Prepared::Need;
    return p;
}

// Step 6: one provider request, with the status / last-error / response-time
// bookkeeping. False = the request itself failed (network, HTTP, breaker open).
bool call_provider(IProvider* prov, const std::vector<std::string>& texts, const std::string& sysPrompt,
                   const std::string& rollingCtx, std::vector<std::string>& out) {
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool callOk = prov->translate(texts, sysPrompt, rollingCtx, out, err);
    unsigned long dt = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    // Latency for the held-dialog estimate: every answered request, AND a timeout
    // (the provider reports "timeout"): a provider that doesn't answer in time is
    // the slowest provider of all, not one to leave out of the estimate.
    if (callOk || err == "timeout") {
        std::lock_guard<std::mutex> lk(g_statsMutex);
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

// Steps 7-10 for one text's model output. "" = it can't be trusted: keep the
// original.
std::string finish(const Prepared& p, const std::string& modelOut, int targetLang, int displayCp, size_t cap) {
    if (modelOut.empty()) return std::string();

    // 7: sanity-check the model's output (placeholders intact, non-empty,
    // not wildly longer than the source) before trusting it.
    if (!validate::result_ok(p.masked.text, modelOut, p.masked.tokens.size())) return std::string();

    // 8: restore the masked spans. unmask() returns "" if a placeholder was
    // lost/duplicated/corrupted; when there were no placeholders to begin
    // with it just returns the text unchanged, so un can only be empty here
    // if tokens was non-empty and something really did go wrong.
    std::string un = protect::unmask(modelOut, p.masked.tokens);
    if (un.empty() && !p.masked.tokens.empty()) return std::string();

    // 9: glossary pass -- canonicalises GTA:SA vehicle + San Andreas place
    // names so they never come back mistranslated or misspelled.
    un = protect::apply_glossary(un, targetLang);

    // 10: encode for display; cache the FULL translation (capped only to a
    // generous absolute ceiling, not to this call's consumer-specific limit) so
    // a LATER call with a DIFFERENT capBytes (e.g. a dialog unit reusing a
    // chat-translated string, or vice versa) reads back the untruncated text and
    // caps it to ITS OWN limit instead of inheriting whatever a different
    // consumer happened to ask for.
    std::string full = charset::cap_display(charset::encode(charset::from_utf8(un), displayCp), 4095);
    cache::put(p.key, full); // ignores an empty value
    return charset::cap_display(full, cap); // nothing left to show -> "" -> keep the original
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
    if (!call_provider(prov, { p.masked.text }, sysPrompt, rollingCtx, out) || out.empty()) return false;

    std::string s = finish(p, out[0], targetLang, displayCp, cap); // steps 7-10
    if (s.empty()) return false;
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
        for (size_t i : need) texts.push_back(prep[i].masked.text);
        std::vector<std::string> res;
        if (!call_provider(prov, texts, sysPrompt, std::string(), res)) return any; // provider down: no retry storm
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
        std::vector<std::string> res;
        if (!call_provider(prov, { prep[i].masked.text }, sysPrompt, std::string(), res)) break; // provider down: stop
        if (res.empty()) continue;
        std::string s = finish(prep[i], res[0], targetLang, displayCp, cap);
        if (!s.empty()) {
            out[i] = std::move(s);
            any = true;
        }
    }
    return any;
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
    g_status.store("idle", std::memory_order_relaxed);
    // Reset response-time stats: a new provider/model has its own latency profile,
    // so don't average across a switch (e.g. local Hy-MT2 -> a cloud endpoint).
    {
        std::lock_guard<std::mutex> lk(g_statsMutex);
        g_respCount = 0; g_respTotalMs = 0; g_respLastMs = 0;
        g_latSingle.reset(); // the new provider/model's first request is a cold load again
        g_latBatch.reset();
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
    g_jobCv.notify_all();
    for (auto& t : g_workers) {
        if (t.joinable()) t.join();
    }
    g_workers.clear();

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
    if (!isBatch && !job.skipContext) {
        int cp = (job.srcCodepage != 0) ? job.srcCodepage : charset::guess_codepage(job.sourceBytes);
        utf8Line = charset::to_utf8(charset::decode(job.sourceBytes, cp));
    }

    Job j = job;
    std::lock_guard<std::mutex> lock(g_jobMutex);
    if (!job.noDedup && !isBatch && !g_inFlight.insert(h).second) return; // identical source already queued/in-flight: dedup

    // Rolling context is the inbound chat conversation. Outbound jobs (which the
    // player is composing, translated the other way) opt out via skipContext so
    // they neither read nor pollute the inbound context ring.
    if (!job.skipContext && !isBatch) {
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
    return g_jobs.prio.size() + (size_t)(active > 0 ? active : 0);
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
