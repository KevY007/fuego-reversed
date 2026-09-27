// tests/engine_test.cpp
//
// Host test for i18n::engine. Only the pure pipeline (process_one) is
// exercised here, against a FakeProvider -- no threads started, no network
// touched. The threaded start/stop/submit/pump path is integration-only and
// gets its first real exercise in Task 12's in-game smoke test.
//
// This file is a separate translation unit from tests/i18n_tests.cpp (the
// engine test needs its own .cpp so engine.h's IProvider subclass doesn't
// drag WinHTTP into every other test), so it can't see i18n_tests.cpp's
// file-local CHECK/CHECK_EQ macros or counters directly. It declares the
// same two-arg macros against the *same* (now externally-linked) g_ran/
// g_fail counters, so the final "N checks, M failures" tally and exit code
// printed by i18n_tests.cpp's main() still cover these checks too.
#include "i18n/engine.h"
#include "i18n/cache.h"
#include "i18n/charset.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern int g_ran, g_fail;
#define CHECK(cond, msg) do { ++g_ran; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)
#define CHECK_EQ(a, b) do { ++g_ran; auto _a=(a); auto _b=(b); if(!(_a==_b)){ ++g_fail; \
    std::printf("FAIL %s:%d  expected equal\n", __FILE__, __LINE__); } } while(0)

struct FakeProvider : i18n::IProvider {
    std::string canned;
    bool translate(const std::vector<std::string>& u, const std::string&, const std::string&,
                   std::vector<std::string>& out, std::string&) override {
        (void)u;
        out = { canned };
        return true;
    }
    const char* name() const override { return "fake"; }
};

// Records every request's item count. Single-item requests (and, unless the
// layout is broken, each item of a batch) are answered from `single`, keyed by the
// masked text the engine sends. breakLayout: a batch reply loses its numbering
// (the provider hands back all-empty items). batchSkip: that one item comes back
// empty from a batch. fail: every request fails (the network is down).
struct FakeBatch : i18n::IProvider {
    std::vector<size_t> calls;
    std::map<std::string, std::string> single;
    bool breakLayout = false;
    bool fail = false;
    std::string batchSkip;
    bool translate(const std::vector<std::string>& u, const std::string&, const std::string&,
                   std::vector<std::string>& out, std::string& err) override {
        calls.push_back(u.size());
        if (fail) { err = "down"; return false; }
        out.assign(u.size(), std::string());
        if (u.size() > 1 && breakLayout) return true;
        for (size_t i = 0; i < u.size(); ++i) {
            if (u.size() > 1 && u[i] == batchSkip) continue;
            auto it = single.find(u[i]);
            if (it != single.end()) out[i] = it->second;
        }
        return true;
    }
    const char* name() const override { return "fake-batch"; }
};

void test_engine() {
    using namespace i18n;

    // Isolate from the real translation-memory file, and make sure the
    // first process_one below is a genuine cache miss rather than a
    // leftover from a previous run of this exe.
    cache::save_path("engine_test_tm.txt");
    cache::clear_for_test();

    FakeProvider p;

    // A Cyrillic (cp1251) "привет" is worth translating; mask() finds no
    // placeholders in a bare word, so the fake just needs to echo "hello".
    std::string src = "\xEF\xF0\xE8\xE2\xE5\xF2";
    p.canned = "hello";
    std::string disp;
    bool ok = engine::process_one(src, 1251, &p, "sys", disp);
    CHECK(ok, "cyrillic (cp1251) input should be translated");
    CHECK_EQ(disp, std::string("hello"));

    // Already-English input is skipped before the provider is ever called.
    // NOTE: this uses "the door is open" rather than the brief's "hello
    // there" -- detect::is_probably_english requires 2 whole-word stopword
    // hits, and "hello"/"there" don't clear that bar ("the" only appears as
    // part of "there", which fails the word-boundary check), so "hello
    // there" is (perhaps surprisingly) NOT recognized as English by the
    // existing, already-reviewed detect module and would incorrectly reach
    // the provider. "the door is open" is the exact phrase test_detect.cpp
    // already proves trips worth_translating's English-skip path.
    CHECK(!engine::process_one("the door is open", 1252, &p, "sys", disp),
          "already-English input should be skipped, not translated");

    // ---- Fix round 1, Important 1: the cache stores the FULL translation; ----
    // ---- each caller caps to its OWN limit. ----------------------------------
    cache::clear_for_test();
    FakeProvider bigP;
    bigP.canned = std::string(300, 'y'); // longer than a chat line (144), under the 4095 ceiling

    // (a) A dialog-length call (capBytes=1024) gets the full 300 bytes back;
    // a later CHAT lookup (default cap) of the SAME entry is truncated to 144;
    // a later DIALOG lookup (cap=1024) of the same entry gets the full 300.
    // (validate::result_ok rejects output > 4x the masked source length, so the
    // source is a long-enough repeated Cyrillic phrase to license a 300-byte reply.)
    std::string srcA; // "кот " (cp1251) x30 -- fresh source, not yet cached
    for (int i = 0; i < 30; ++i) srcA += "\xEA\xEE\xF2 ";
    std::string dispA;
    bool okA = engine::process_one(srcA, 1251, &bigP, "sys", dispA, 1033, 1252, "", 1024);
    CHECK(okA, "dialog-length job should translate");
    CHECK_EQ(dispA.size(), (size_t)300);

    std::string chatHit;
    CHECK(engine::cache_lookup(srcA, 1251, 1033, chatHit), "chat reads back the same cache entry");
    CHECK_EQ(chatHit.size(), (size_t)144);

    std::string dialogHit;
    CHECK(engine::cache_lookup(srcA, 1251, 1033, dialogHit, 1024), "dialog capBytes reads back the same entry");
    CHECK_EQ(dialogHit.size(), (size_t)300);

    // (b) Chat translates first (capBytes=144 default, truncated for display);
    // a LATER dialog-length call for the SAME source is a cache hit that must
    // return the full 300 bytes, not the 144-byte string chat displayed.
    FakeProvider bigP2;
    bigP2.canned = std::string(300, 'z');
    std::string srcB; // "дом " (cp1251) x30 -- fresh source, not yet cached
    for (int i = 0; i < 30; ++i) srcB += "\xE4\xEE\xEC ";
    std::string dispB1;
    bool okB1 = engine::process_one(srcB, 1251, &bigP2, "sys", dispB1); // chat defaults: capBytes=144
    CHECK(okB1, "chat job should translate");
    CHECK_EQ(dispB1.size(), (size_t)144);

    std::string dispB2;
    bool okB2 = engine::process_one(srcB, 1251, &bigP2, "sys", dispB2, 1033, 1252, "", 1024); // cache hit
    CHECK(okB2, "second call for the same source should be a cache hit");
    CHECK_EQ(dispB2.size(), (size_t)300);

    // ---- Fix round 1, Important 2: needs_translation() lets a caller skip ----
    // ---- an already-fine unit WITHOUT treating it as a cache miss. -----------
    CHECK(!engine::needs_translation("the door is open", 1252, 1033),
          "English source + English target -> no translation needed");
    CHECK(engine::needs_translation("the door is open", 1252, 1049),
          "English source + Russian target -> translation needed");
    CHECK(engine::needs_translation("\xEF\xF0\xE8\xE2\xE5\xF2", 1251, 1033),
          "Cyrillic source + English target -> translation needed");
    CHECK(!engine::needs_translation("12345", 1252, 1033), "pure digits -> no translation needed");
    CHECK(!engine::needs_translation("", 1252, 1033), "empty source -> no translation needed");
    CHECK(!engine::needs_translation("/quit", 1252, 1033),
          "protect::mask()-untranslatable (pure command, per test_protect) -> no translation needed");

    // ---- I1: a cached value that is empty once capped for display (master stored
    // "" for some cp1251 lines; '%'-only text caps to "") is a MISS, never a hit that
    // blanks a dialog unit / title or goes out as empty outgoing text. ----
    cache::clear_for_test();
    const std::string srcE = "\xEC\xE0\xE3\xE0\xE7\xE8\xED"; // "магазин" (cp1251), never cached above
    const uint64_t kE = cache::key(charset::to_utf8(charset::decode(srcE, 1251)), 1251, 1033, "");
    cache::put(kE, "%%");
    std::string hitE = "stale";
    CHECK(!engine::cache_lookup(srcE, 1251, 1033, hitE, 1024), "empty-after-cap cached value is a miss (dialog cap)");
    CHECK(!engine::cache_lookup(srcE, 1251, 1033, hitE), "empty-after-cap cached value is a miss (chat cap)");
    FakeProvider shop;
    shop.canned = "shop";
    std::string dispE;
    CHECK(engine::process_one(srcE, 1251, &shop, "sys", dispE, 1033, 1252, "", 1024) && dispE == "shop",
          "process_one: an empty cache hit falls through to the provider");
    std::string hitE2;
    CHECK(engine::cache_lookup(srcE, 1251, 1033, hitE2, 1024) && hitE2 == "shop",
          "the provider's translation replaces the empty entry");
    cache::clear_for_test();

    // ---- I5: dialogs (and outgoing text) must not queue behind inbound chat ----
    {
        auto mk = [](uint64_t h, int surface) { engine::Job j{}; j.handle = h; j.surface = surface; return j; };
        engine::JobQueue q;
        q.push(mk(1, 0)); // inbound chat flip
        q.push(mk(2, 2)); // TM fill (labels / HUD)
        q.push(mk(3, 0)); // inbound chat flip
        q.push(mk(4, 3)); // dialog unit -- submitted AFTER the normal jobs
        q.push(mk(5, 1)); // outbound chat
        q.push(mk(6, 4)); // dialog INPUT reply
        CHECK(!q.empty(), "queue holds the jobs");
        std::vector<uint64_t> order;
        engine::Job j{};
        while (q.pop(j)) order.push_back(j.handle);
        CHECK(!order.empty() && order.front() == 4,
              "a dialog-unit job submitted after normal jobs is processed first");
        CHECK(order == std::vector<uint64_t>({ 4, 5, 6, 1, 2, 3 }),
              "priority lane (surfaces 1/3/4) drains first, FIFO within each lane");
        CHECK(q.empty(), "both lanes drained");
        q.push(mk(7, 0));
        q.push(mk(8, 3));
        q.clear();
        CHECK(q.empty() && !q.pop(j), "clear() empties both lanes");
        CHECK(engine::JobQueue::is_priority(1) && engine::JobQueue::is_priority(3) &&
              engine::JobQueue::is_priority(4) && !engine::JobQueue::is_priority(0) &&
              !engine::JobQueue::is_priority(2), "priority surfaces are exactly 1, 3 and 4");
    }

    // ---- batched dialogs: one request for many cells, strict fallback ----
    {
        // cp1251 sources and their UTF-8 (the masked text the provider receives).
        const std::string privet  = "\xEF\xF0\xE8\xE2\xE5\xF2";                     // привет
        const std::string mir     = "\xEC\xE8\xF0";                                 // мир
        const std::string bank    = "\xE1\xE0\xED\xEA";                             // банк
        const std::string privet8 = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
        const std::string mir8    = "\xD0\xBC\xD0\xB8\xD1\x80";
        const std::string bank8   = "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA";
        FakeBatch fb;
        fb.single = { { privet8, "hello" }, { mir8, "world" }, { bank8, "bank" } };
        const std::vector<std::string> three = { privet, mir, bank };
        const std::vector<std::string> done3 = { "hello", "world", "bank" };
        std::vector<std::string> out;

        // A kept layout: ONE request for all three; every cell cached on its own.
        cache::clear_for_test();
        engine::reset_batch_for_test();
        CHECK(engine::process_batch(three, 1251, &fb, "sys", out) && out == done3, "batch: three cells translated");
        CHECK(fb.calls == std::vector<size_t>({ 3 }), "batch: ONE provider request for three cells");
        std::string hit;
        CHECK(engine::cache_lookup(mir, 1251, 1033, hit) && hit == "world", "batch: each cell is cached on its own");

        // A broken layout: every cell is retried alone (and still translated).
        cache::clear_for_test();
        fb.calls.clear();
        fb.breakLayout = true;
        CHECK(engine::process_batch(three, 1251, &fb, "sys", out) && out == done3, "batch: broken layout -> per-cell fallback");
        CHECK(fb.calls == std::vector<size_t>({ 3, 1, 1, 1 }), "batch: one batch request, then one per cell");

        // Two broken layouts in a row: batching is switched off (no wasted request).
        cache::clear_for_test();
        fb.calls.clear();
        engine::process_batch(three, 1251, &fb, "sys", out);
        CHECK(fb.calls == std::vector<size_t>({ 3, 1, 1, 1 }), "batch: a second broken layout");
        cache::clear_for_test();
        fb.calls.clear();
        CHECK(engine::process_batch(three, 1251, &fb, "sys", out) && out == done3, "batch: still translated with batching off");
        CHECK(fb.calls == std::vector<size_t>({ 1, 1, 1 }), "batch: after two broken layouts, cells go one by one");
        engine::reset_batch_for_test();
        fb.breakLayout = false;

        // Mixed: a cached cell, an English cell (skipped), one missing cell -> a single call.
        cache::clear_for_test();
        std::string d;
        engine::process_one(privet, 1251, &fb, "sys", d); // caches "hello"
        fb.calls.clear();
        CHECK(engine::process_batch({ privet, "the door is open", mir }, 1251, &fb, "sys", out) &&
              out == std::vector<std::string>({ "hello", "", "world" }), "batch: cached + skipped + missing");
        CHECK(fb.calls == std::vector<size_t>({ 1 }), "batch: a lone missing cell is one single request");

        // One cell missing from an otherwise good batch: only that cell is retried.
        cache::clear_for_test();
        fb.calls.clear();
        fb.batchSkip = mir8;
        CHECK(engine::process_batch(three, 1251, &fb, "sys", out) && out == done3, "batch: a skipped cell is retried");
        CHECK(fb.calls == std::vector<size_t>({ 3, 1 }), "batch: only the skipped cell gets its own request");
        fb.batchSkip.clear();

        // The provider is down: no per-cell retry storm, nothing translated.
        cache::clear_for_test();
        fb.calls.clear();
        fb.fail = true;
        CHECK(!engine::process_batch(three, 1251, &fb, "sys", out) &&
              out == std::vector<std::string>({ "", "", "" }), "batch: provider down -> originals");
        CHECK(fb.calls == std::vector<size_t>({ 3 }), "batch: a failed request is not retried per cell");
        fb.fail = false;
        cache::clear_for_test();
        engine::reset_batch_for_test();

        // Switched off after two broken layouts, batching gets another chance after a
        // while (re-probe): one kept layout turns it on for good, one more broken
        // layout switches it off again.
        fb.breakLayout = true;
        engine::process_batch(three, 1251, &fb, "sys", out); cache::clear_for_test();
        engine::process_batch(three, 1251, &fb, "sys", out); cache::clear_for_test();
        CHECK(!engine::batching_enabled(), "batch: off after two broken layouts");
        engine::expire_batch_off_for_test();
        CHECK(engine::batching_enabled(), "batch: re-probed after the off interval");
        engine::process_batch(three, 1251, &fb, "sys", out); cache::clear_for_test();
        CHECK(!engine::batching_enabled(), "batch: one more broken layout -> off again");
        engine::expire_batch_off_for_test();
        fb.breakLayout = false;
        fb.calls.clear();
        engine::process_batch(three, 1251, &fb, "sys", out); cache::clear_for_test();
        CHECK(fb.calls == std::vector<size_t>({ 3 }) && engine::batching_enabled(),
              "batch: a kept layout on the re-probe turns batching back on");
        engine::reset_batch_for_test();
    }

    // ---- provider latency for the held-dialog estimate (recency-weighted) ----
    {
        engine::LatencyTracker t;
        CHECK(!t.known(), "latency: unknown with no samples");
        t.add(500);
        t.add(500);
        CHECK(!t.known(), "latency: still unknown after two samples");
        t.add(500);
        CHECK(t.known() && t.ewma > 499 && t.ewma < 501, "latency: known after three samples");
        t.add(10000); // a sudden slowdown moves the estimate quickly (alpha 0.3)
        CHECK(t.ewma > 3000, "latency: a recent slow response weighs heavily");
        for (int i = 0; i < 10; ++i) t.add(500);
        CHECK(t.ewma < 700, "latency: recovers after the slowdown passes");
        t.reset();
        CHECK(!t.known(), "latency: reset -> unknown");

        // The engine never offers the session's first reply (the cold model load)
        // as a sample: one answered request -> still unknown; three more -> known.
        const std::string dom  = "\xE4\xEE\xEC";                 // дом
        const std::string dom8 = "\xD0\xB4\xD0\xBE\xD0\xBC";
        FakeBatch f1;
        f1.single = { { "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "hello" },
                      { "\xD0\xBC\xD0\xB8\xD1\x80", "world" },
                      { "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA", "bank" },
                      { dom8, "house" } };
        cache::clear_for_test();
        engine::reset_latency_for_test();
        std::string d1;
        engine::process_one("\xEF\xF0\xE8\xE2\xE5\xF2", 1251, &f1, "sys", d1);
        CHECK(!engine::latency().singleKnown, "latency: the session's first reply is never a sample");
        engine::process_one("\xEC\xE8\xF0", 1251, &f1, "sys", d1);
        engine::process_one("\xE1\xE0\xED\xEA", 1251, &f1, "sys", d1);
        CHECK(!engine::latency().singleKnown, "latency: two warm samples are not enough");
        engine::process_one(dom, 1251, &f1, "sys", d1);
        CHECK(engine::latency().singleKnown && !engine::latency().batchKnown,
              "latency: three warm single samples -> single known, batch still unknown");
        cache::clear_for_test();
        engine::reset_latency_for_test();
    }
}
