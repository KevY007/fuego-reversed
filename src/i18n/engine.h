#pragma once
#include <cstdint>
#include <string>
#include "i18n/provider.h"

// Async translation engine: submit()/pump() are the hook-facing API (Task
// 11); process_one() is the pure synchronous pipeline, unit-tested directly
// against a fake IProvider in tests/engine_test.cpp.
//
// Threading model: submit() and pump() are called from the main (game)
// thread. A small worker pool drains the job queue and does all
// charset/detect/mask/network/validate/unmask work off the main thread, so
// a slow or unreachable provider never stalls a frame. pump() is the only
// place a translated Result reaches the applier callback, and it must only
// ever be called from the main thread (see pump() below).

namespace i18n::engine {

// Phase 0 surface = 0 (Chat). More surfaces (dialogs, labels, ...) land in
// later phases and reuse this same Job/Result shape.
struct Job {
    uint64_t handle;
    int surface;          // 0 = inbound chat display; 1 = outbound chat (send). See hooks::apply_result.
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
};

struct Result {
    uint64_t handle;
    int surface;
    std::string displayBytes;
    bool ok; // false => pipeline declined to translate; caller should leave the original text as-is
};

// Starts the worker pool against the given provider config + cacheable
// system prompt. targetLang keys the cache + drives the "already target
// language" skip; displayCp is the Windows codepage finished text is encoded
// in for SA:MP's chat font (see config::display_codepage). Safe to call again
// later (e.g. settings changed): any previously running engine is stopped
// first, so callers don't need to call stop() themselves before a restart.
void start(const ProviderConfig& cfg, const std::string& systemPrompt,
           int targetLang = 1033, int displayCp = 1252);

// Signals shutdown, joins every worker thread, and clears both queues and
// the in-flight set. A job a worker has already popped finishes normally
// (including its provider call) before that worker exits; jobs still
// sitting in the queue are abandoned rather than drained, so stop()'s
// worst-case latency is one in-flight provider call, not the whole queue
// depth. Safe to call when not running (no-op).
void stop();

// Enqueues a translation job and returns immediately -- no blocking I/O.
// A job whose sourceBytes hash matches one already queued/in-flight is
// dropped (dedup): only the first submitter's handle will receive a
// Result, so a duplicate line doesn't trigger a second, redundant provider
// call. Must be called after start() (a submit() with no running worker
// pool just queues the job until/unless start() is called).
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
// text, and the cache has been updated.
bool process_one(const std::string& srcBytes, int cp, IProvider* prov,
                  const std::string& sysPrompt, std::string& displayOut,
                  int targetLang = 1033, int displayCp = 1252,
                  const std::string& rollingCtx = "");

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

// Provider response-time stats for the menu readout. count = successful provider
// calls since the last start(); avgMs = their running-mean wall time; lastMs = the
// most recent. avgMs/lastMs are the REAL end-to-end delay -- they include the
// request-pacer wait and any 429/503 backoff-retries, not just model inference.
// count 0 means no provider call has completed yet. Thread-safe snapshot.
struct RespStats { uint64_t count; unsigned long avgMs; unsigned long lastMs; };
RespStats response_stats();

// Synchronous translation-memory lookup for the non-chat surfaces (dialogs, 3D
// labels, textdraws, ...). Computes the SAME cache key process_one() uses, so a
// hit returns the finished, display-codepage-encoded translation a prior job
// cached. Returns false on a miss -- the caller then queues a fill job (a normal
// submit() with a cache-fill surface) and shows the original this time; the next
// identical string is an instant hit. Safe to call from the main/net thread
// (cache is internally locked). Returns false if the engine isn't running.
bool cache_lookup(const std::string& srcBytes, int cp, int targetLang, std::string& out);

} // namespace i18n::engine
