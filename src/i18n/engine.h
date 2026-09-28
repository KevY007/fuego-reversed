#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include "i18n/provider.h"

// Async translation engine: submit()/pump() are the hook-facing API (Task
// 11); process_one() is the pure synchronous pipeline, unit-tested directly
// against a fake IProvider in tests/engine_test.cpp.
//
// Threading model: submit() and pump() are called from the main (game)
// thread. A small worker pool drains the job queue and does all
// charset/detect/mask/network/validate/unmask work off the main thread, so
// a slow or unreachable provider never stalls a frame. The queue has two lanes
// (JobQueue below): outbound chat, held-dialog units and dialog INPUT replies are
// always picked before inbound chat flips and TM fills, so a dialog never waits
// behind a burst of chat. pump() is the only place a translated Result reaches
// the applier callback, and it must only ever be called from the main thread
// (see pump() below).

namespace i18n::engine {

// Phase 0 surface = 0 (Chat). More surfaces (dialogs, labels, ...) land in
// later phases and reuse this same Job/Result shape.
struct Job {
    uint64_t handle;
    int surface;          // 0 inbound chat, 1 outbound chat, 2 TM fill, 3 dialog unit, 4 dialog response. See hooks::apply_result.
    std::string sourceBytes;
    int srcCodepage; // 0 = auto-detect via charset::guess_codepage
    // Recent chat lines (UTF-8, already labeled) to send as background context
    // so the model keeps conversational relevancy. Filled by submit() from a
    // main-thread ring buffer; leave empty and submit() will populate it.
    // (Outbound jobs set surface=1 and leave this empty -- no shared context.)
    std::string rollingCtx;
    // Per-job direction overrides, for surfaces that translate the OPPOSITE way
    // from the engine default (outbound: player-lang -> server-lang). When set
    // they replace the engine's start()-time prompt/target/codepage for this one
    // job. Empty prompt / 0 lang / 0 codepage mean "use the engine default".
    std::string promptOverride;
    int targetLangOverride = 0;
    int displayCpOverride = 0;
    bool skipContext = false; // don't add this line to the inbound rolling-context ring
    // Outbound sets this: the player may legitimately send the same text twice,
    // and it must never be dropped as a duplicate of an inbound line, so these
    // jobs bypass the in-flight dedup entirely.
    bool noDedup = false;
    // Max bytes of the finished translation. Chat's 144 by default; dialog units
    // (a body line can be longer than one chat line) pass more.
    int capBytes = 144;
    // BATCH (a dialog's cells): when non-empty, this job translates these texts in
    // ONE provider request (see process_batch) and sourceBytes is ignored; the
    // Result carries one entry per text in unitOut. A batch job is never
    // deduplicated and never touches the rolling chat context.
    std::vector<std::string> units;
    // The texts are UI labels (dialog cells, HUD / 3D-label lines), not chat:
    // protect::mask(..., labels) -- "Civilian (8)" is a count there, not a player.
    bool labels = false;
};

struct Result {
    uint64_t handle;
    int surface;
    std::string displayBytes;
    bool ok; // false => pipeline declined to translate; caller should leave the original text as-is
    // BATCH jobs only: the finished text for Job::units[i] ("" = keep that original).
    // May be shorter than units (e.g. empty after an internal error) -> treat the
    // missing tail as "".
    std::vector<std::string> unitOut;
    // BATCH jobs only: a provider request failed outright (down, circuit open,
    // network) -- the "" items then say nothing about the texts themselves.
    bool providerFailed = false;
};

// The engine's two-lane job queue. PURE (the engine guards its one instance with
// its job mutex), so the pick-next-job rule is host-tested (tests/engine_test.cpp).
// Surfaces 1 (outbound chat), 3 (held-dialog units) and 4 (dialog INPUT replies)
// -- text the player or a held dialog is actively WAITING on -- go in the priority
// lane; everything else (inbound chat flips, which are already on screen in the
// original, and background TM fills) in the normal lane. A worker always takes
// the priority lane first; each lane is FIFO.
struct JobQueue {
    std::deque<Job> prio;
    std::deque<Job> normal;

    static bool is_priority(int surface) { return surface == 1 || surface == 3 || surface == 4; }
    void push(Job job) { (is_priority(job.surface) ? prio : normal).push_back(std::move(job)); }
    // The next job to work on (priority lane first). False if both lanes are empty.
    bool pop(Job& out) {
        std::deque<Job>& lane = !prio.empty() ? prio : normal;
        if (lane.empty()) return false;
        out = std::move(lane.front());
        lane.pop_front();
        return true;
    }
    bool empty() const { return prio.empty() && normal.empty(); }
    void clear() { prio.clear(); normal.clear(); }
};

// Recency-weighted response time of ONE kind of provider request (a single text,
// or a batch) for the held-dialog estimate (surfaces::likely_too_slow). Fewer than
// kMinSamples samples mean "unknown"; a recent slowdown (a rate limit, a model
// reload, a timeout counted as a sample) moves it within a few requests. The
// engine never offers it a cold-start sample: a request that started before the
// session's first reply arrived paid the model load, which would otherwise make
// every dialog of a new session look "too slow". PURE; the engine guards its two
// instances with its stats mutex.
struct LatencyTracker {
    static constexpr double kAlpha      = 0.3;
    static constexpr int    kMinSamples = 3;
    int    n    = 0;
    double ewma = 0.0; // ms
    void add(double ms) {
        ewma = (n == 0) ? ms : ewma + kAlpha * (ms - ewma);
        ++n;
    }
    bool known() const { return n >= kMinSamples; }
    void reset() { n = 0; ewma = 0.0; }
};

// Snapshot of both trackers (thread-safe copy).
struct Latency {
    bool   singleKnown = false;
    double singleMs    = 0.0;
    bool   batchKnown  = false;
    double batchMs     = 0.0;
};

// Starts the worker pool against the given provider config + cacheable
// system prompt. targetLang keys the cache + drives the "already target
// language" skip; displayCp is the Windows codepage finished text is encoded
// in for SA:MP's chat font (see config::display_codepage). Safe to call again
// later (e.g. settings changed): any previously running engine is stopped
// first, so callers don't need to call stop() themselves before a restart.
void start(const ProviderConfig& cfg, const std::string& systemPrompt,
           int targetLang = 1033, int displayCp = 1252);

// Signals shutdown, joins every worker thread, and clears the job queue (both
// lanes), the result queue and the in-flight set. A job a worker has already popped finishes normally
// (including its provider call) before that worker exits; jobs still
// sitting in the queue are abandoned rather than drained, so stop()'s
// worst-case latency is one in-flight provider call, not the whole queue
// depth. Safe to call when not running (no-op).
void stop();

// Enqueues a translation job and returns immediately -- no blocking I/O.
// A job whose sourceBytes hash matches one already queued/in-flight is
// dropped (dedup): only the first submitter's handle will receive a
// Result, so a duplicate line doesn't trigger a second, redundant provider
// call. Surfaces 1, 3 and 4 go in the priority lane (see JobQueue). Must be
// called after start() (a submit() with no running worker pool just queues
// the job until/unless start() is called).
void submit(const Job& job);

// Installs the main-thread callback pump() delivers results to. Set-once:
// call exactly once, before the first call to pump() can happen -- in
// practice, before render::install() wires up the frame callback (and so
// starts invoking pump_tick() -> pump()) on the render/game thread. It does
// NOT need to be called from that same thread: hooks::install() calls this
// from the init thread, strictly before that same function goes on to call
// render::install(); g_applyFn is a plain pointer written exactly once, and
// that program-order "set_apply() before render::install()" ordering is what
// publishes the write so it's visible by the time any frame callback can
// first read it (published-before-use), with no separate synchronization
// required.
void set_apply(void (*fn)(const Result&));

// Drains whatever results are ready and invokes the applier for each, one
// at a time, OUTSIDE the result-queue lock -- so the applier is free to
// call back into engine::submit() or read engine state without
// deadlocking. MAIN THREAD ONLY: call once per frame (e.g. from on_frame).
void pump();

// The pure pipeline: cache -> decode -> detect -> mask -> translate ->
// validate -> unmask -> glossary -> encode -> cap. `prov` performs the
// actual translation call (a real IProvider in production, a fake one in
// tests) -- this is what makes the function directly unit-testable without
// threads or a network. Returns false to mean "leave the original text
// as-is", whether because translation wasn't warranted (already the target
// language, nothing translatable) or because a later step couldn't be
// trusted (provider failure, corrupted placeholders, garbage output). On
// true, `displayOut` holds the finished, capped, display-codepage-encoded
// text (never empty), and the cache has been updated. A cached entry that is
// empty once capped for display is treated as a miss, never returned.
bool process_one(const std::string& srcBytes, int cp, IProvider* prov,
                  const std::string& sysPrompt, std::string& displayOut,
                  int targetLang = 1033, int displayCp = 1252,
                  const std::string& rollingCtx = "", int capBytes = 144);

// The same pipeline for MANY texts (the cells of one dialog) with ONE provider
// request: every text is skipped / served from the cache / masked exactly like
// process_one, the ones still needing the model go out together as a numbered
// list (batch.h), and each returned item is validated, unmasked and cached on its
// own. An item the batch didn't bring back (the model broke the numbered layout,
// the batch timed out / was rejected / was cut off -- the provider's soft batch
// failures -- or that line failed validation) is retried alone; if the provider
// itself fails (down, circuit open) nothing is retried, and the retries stop when
// the engine is stopping. After two batch failures in a row batching is switched
// off (see batching_enabled(): re-tried after 10 minutes) and texts go one per
// request. out[i] = the finished text for units[i], or "" to keep its original.
// Returns true if any text was translated.
// labels: the texts are UI labels (Job::labels). *providerFailed (if given) is set
// when a provider request failed outright.
bool process_batch(const std::vector<std::string>& units, int cp, IProvider* prov,
                   const std::string& sysPrompt, std::vector<std::string>& out,
                   int targetLang = 1033, int displayCp = 1252, int capBytes = 144,
                   bool labels = false, bool* providerFailed = nullptr);

// Whether a dialog's cells may go out as one batch right now: false after two
// broken layouts in a row -- until, 10 minutes later, one more batch is tried
// (a kept layout turns batching back on; another broken one switches it off for
// another 10 minutes). While it is off, a held dialog sends one job per cell so
// the cells run in parallel on the workers. Thread-safe.
bool batching_enabled();

// Test-only: turn batching back on (forget the broken-layout streak), and make the
// 10-minute re-probe due now.
void reset_batch_for_test();
void expire_batch_off_for_test();

// The recency-weighted provider latency (see LatencyTracker). Thread-safe.
Latency latency();

// Test-only: forget all latency samples and the session's first reply (so the
// next answered request counts as the cold one again).
void reset_latency_for_test();

// Whether chat translation sends the recent conversation as background context:
// only for the cloud chat-model presets (1 Google, 2 Anthropic, 3 OpenAI). A
// dedicated translation model -- the local default, Hy-MT2 -- translates the context
// lines INSTEAD of the line (live: 5 of 6 replies), so Custom (0) and Local (4) get
// none. PURE.
bool context_for_provider(int providerKind);

// Why texts were NOT translated since the last call, oldest first -- e.g. "reply
// lost or repeated a placeholder (...): 'Use /actions to view ...'", or "provider
// failed (timeout)" (a provider failure is noted once, without text: while it is
// down every line fails the same way) -- for the log (hooks::pump_tick drains it
// about once a second). Control bytes in the text are blanked. Capped at 20 per
// drain (a last entry says how many were left out); an identical note is kept once.
// Thread-safe.
std::vector<std::string> drain_failure_notes();

// Work ahead of a new priority job: priority-lane jobs still queued plus about half
// of the jobs the workers are processing right now (a running job is, on average,
// half done); and the number of worker threads -- inputs for the held-dialog
// latency estimate. Thread-safe.
size_t backlog();
int worker_count();

// Short human-readable status for the menu (Task 12), e.g. "local ok" or
// "offline - retrying". Reflects the outcome of the most recent provider
// call made by any worker (or by a direct process_one() call); a job that
// was skipped before ever reaching the provider leaves status unchanged,
// since there's nothing new to report about connectivity.
const char* status();

// The most recent provider error string (empty if none / last call succeeded).
// Thread-safe copy for the menu, so a failing translate says WHY (timeout, HTTP
// status, parse) instead of just "error - retrying".
std::string last_error();

// Provider response-time stats for the menu readout. count = answered provider
// calls since the last start() (including batches the provider reported as a soft
// failure, e.g. a batch that timed out); avgMs = their running-mean wall time; lastMs = the
// most recent. avgMs/lastMs are the REAL end-to-end delay -- they include the
// request-pacer wait and any 429/503 backoff-retries, not just model inference.
// count 0 means no provider call has completed yet. Thread-safe snapshot.
struct RespStats { uint64_t count; unsigned long avgMs; unsigned long lastMs; };
RespStats response_stats();

// Synchronous translation-memory lookup for the non-chat surfaces (dialogs, 3D
// labels, textdraws, ...). Computes the SAME cache key process_one() uses, so a
// hit returns the finished, display-codepage-encoded translation a prior job
// cached -- the cache always stores the FULL translation (see process_one()
// step 10), so a hit is capped to THIS call's own capBytes (default 144, the
// chat line limit; dialog units pass a larger cap -- see Job::capBytes)
// independently of whatever cap a different consumer used when the entry was
// written. Returns false on a miss -- the caller then queues a fill job (a
// normal submit() with a cache-fill surface) and shows the original this time;
// the next identical string is an instant hit. An entry that is empty once
// capped for display is a miss too (it would blank the text it replaces). Safe to
// call from the main/net thread (cache is internally locked). It never checks
// whether the engine is running: it reads the translation memory directly (loaded
// at startup), keyed with the provider profile of the last engine::start() ("" if
// the engine was never started), so a hit is possible with the engine stopped.
bool cache_lookup(const std::string& srcBytes, int cp, int targetLang, std::string& out, int capBytes = 144);

// The same pre-network declines process_one() makes (already empty, already the
// target language, or nothing survives protect::mask()), exposed synchronously
// and WITHOUT touching the cache or the network, so a caller (e.g. a held
// dialog's translation-memory pass) can tell "this unit doesn't need
// translating" up front instead of treating it as a cache miss and holding for
// a translation that would never change it anyway. labels: the text is a UI
// label (Job::labels), masked the way its translation job will mask it.
bool needs_translation(const std::string& srcBytes, int cp, int targetLang, bool labels = false);

// The target language the RUNNING engine translates into (start()'s targetLang;
// 1033 before any start). The menu edits config::Settings::target_lang at once, but
// the engine only takes it on Save (apply_config): a translation-memory lookup that
// must match what a job it queues will store uses THIS. Thread-safe.
int active_target_lang();

} // namespace i18n::engine
