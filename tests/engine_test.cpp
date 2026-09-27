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

#include <cstdio>
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
}
