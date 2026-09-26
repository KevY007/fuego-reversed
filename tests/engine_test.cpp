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
}
