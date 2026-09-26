#include "i18n/engine.h"

#include "i18n/charset.h"
#include "i18n/detect.h"
#include "i18n/protect.h"
#include "i18n/cache.h"
#include "i18n/validate.h"

#include <atomic>
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

// Job queue + in-flight dedup set: both guarded by g_jobMutex.
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<Job> g_jobs;
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

void worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            g_jobCv.wait(lock, [] { return !g_running.load() || !g_jobs.empty(); });
            if (!g_running.load()) return; // shutting down: abandon anything still queued
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }

        std::string disp;
        bool ok = false;
        try {
            ok = process_one(job.sourceBytes, job.srcCodepage, g_provider.get(), g_sysPrompt, disp,
                             g_targetLang, g_displayCp, job.rollingCtx);
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
        }

        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            g_results.push_back(Result{ job.handle, job.surface, disp, ok });
        }
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_inFlight.erase(std::hash<std::string>{}(job.sourceBytes));
        }
    }
}

} // namespace

bool process_one(const std::string& srcBytes, int cp, IProvider* prov,
                  const std::string& sysPrompt, std::string& displayOut,
                  int targetLang, int displayCp, const std::string& rollingCtx) {
    // 1-2: pick the source codepage and decode to a wide string.
    int cp2 = (cp != 0) ? cp : charset::guess_codepage(srcBytes);
    std::wstring w = charset::decode(srcBytes, cp2);

    // 3: skip anything not worth translating (empty, digits/punct-only, or
    // already the target language -- only cheaply detectable for English).
    if (!detect::worth_translating(w, /*targetIsEnglish=*/targetLang == 1033)) return false;

    // 4: cache lookup keyed on the normalized (UTF-8) source text + target.
    std::string normSrc = charset::to_utf8(w);
    uint64_t k = cache::key(normSrc, cp2, targetLang, g_profile);
    if (cache::get(k, displayOut)) return true;

    // 5: mask non-translatable spans (commands, colours, names, coords, ...).
    protect::Masked m = protect::mask(normSrc);
    if (!m.translatable) return false;

    // 6: translate the masked text, passing recent chat as background context.
    // No provider, a network failure, or an empty reply all mean "can't trust
    // this" -- leave the original text.
    if (!prov) return false;
    std::vector<std::string> out;
    std::string err;
    bool callOk = prov->translate({ m.text }, sysPrompt, rollingCtx, out, err);
    if (callOk) {
        g_status.store("local ok", std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(g_errMutex); g_lastErr.clear(); }
    } else {
        g_status.store("error - retrying", std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_errMutex); g_lastErr = err;
    }
    if (!callOk || out.empty()) return false;

    // 7: sanity-check the model's output (placeholders intact, non-empty,
    // not wildly longer than the source) before trusting it.
    if (!validate::result_ok(m.text, out[0], m.tokens.size())) return false;

    // 8: restore the masked spans. unmask() returns "" if a placeholder was
    // lost/duplicated/corrupted; when there were no placeholders to begin
    // with it just returns the text unchanged, so un can only be empty here
    // if tokens was non-empty and something really did go wrong.
    std::string un = protect::unmask(out[0], m.tokens);
    if (un.empty() && !m.tokens.empty()) return false;

    // 9: glossary pass -- canonicalises GTA:SA vehicle + San Andreas place
    // names so they never come back mistranslated or misspelled.
    un = protect::apply_glossary(un, targetLang);

    // 10: encode for display, cap to the chat line limit, remember it.
    displayOut = charset::cap_and_sanitize(
        charset::encode(charset::from_utf8(un), displayCp), 144);
    cache::put(k, displayOut);
    return true;
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
        g_jobs.clear();
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
    size_t h = std::hash<std::string>{}(job.sourceBytes);

    // Decode this line to UTF-8 once, outside the lock, so it's JSON-safe to
    // reuse as future context (raw cp1251/cp1252 bytes would corrupt the
    // request body). Short chat strings -> negligible cost on the game thread.
    int cp = (job.srcCodepage != 0) ? job.srcCodepage : charset::guess_codepage(job.sourceBytes);
    std::string utf8Line = charset::to_utf8(charset::decode(job.sourceBytes, cp));

    Job j = job;
    std::lock_guard<std::mutex> lock(g_jobMutex);
    if (!g_inFlight.insert(h).second) return; // identical source already queued/in-flight: dedup

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

    g_jobs.push_back(std::move(j));
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

} // namespace i18n::engine
