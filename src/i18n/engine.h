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
    int surface;
    std::string sourceBytes;
    int srcCodepage; // 0 = auto-detect via charset::guess_codepage
};

struct Result {
    uint64_t handle;
    int surface;
    std::string displayBytes;
    bool ok; // false => pipeline declined to translate; caller should leave the original text as-is
};

// Starts the worker pool against the given provider config + cacheable
// system prompt. Safe to call again later (e.g. settings changed): any
// previously running engine is stopped first, so callers don't need to
// call stop() themselves before a restart.
void start(const ProviderConfig& cfg, const std::string& systemPrompt);

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

// Installs the main-thread callback pump() delivers results to. Call once,
// before the first pump(), from the same thread that will call pump().
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
                  const std::string& sysPrompt, std::string& displayOut);

// Short human-readable status for the menu (Task 12), e.g. "local ok" or
// "offline - retrying". Reflects the outcome of the most recent provider
// call made by any worker (or by a direct process_one() call); a job that
// was skipped before ever reaching the provider leaves status unchanged,
// since there's nothing new to report about connectivity.
const char* status();

} // namespace i18n::engine
