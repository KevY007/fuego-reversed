// Host test for i18n::dlgq -- ordered, timing-faithful release of held dialogs.
//
// Build/run: tests\run_dlgq_test.cmd

#include "i18n/dlgq.h"

#include <cstdio>
#include <vector>

using namespace i18n::dlgq;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}
static std::vector<uint8_t> P(uint8_t tag) { return std::vector<uint8_t>{ tag }; }

int main() {
    {   // 1. Held until translated, then released translated.
        Queue q;
        q.push(1, 0, P('a'));
        check(q.tick(100).empty(), "nothing released while translating");
        q.resolve(1, P('A'));
        auto r = q.tick(150);
        check(r.size() == 1 && r[0].translated && r[0].payload == P('A') && r[0].original == P('a'), "released translated");
        check(q.empty(), "queue drained");
    }
    {   // 2. Faithful gaps: A@0, B@300, C@1000, all translated late (t=2000).
        Queue q;
        q.push(1, 0, P('a')); q.push(2, 300, P('b')); q.push(3, 1000, P('c'));
        q.resolve(1, P('A')); q.resolve(2, P('B')); q.resolve(3, P('C'));
        auto r = q.tick(2000);
        check(r.size() == 1 && r[0].seq == 1, "A released first");
        check(q.tick(2299).empty(), "B waits for its original 300ms gap");
        r = q.tick(2300);
        check(r.size() == 1 && r[0].seq == 2, "B released 300ms after A");
        check(q.tick(2999).empty(), "C waits for its original 700ms gap");
        r = q.tick(3000);
        check(r.size() == 1 && r[0].seq == 3, "C released 700ms after B");
    }
    {   // 3. A later page translated first never overtakes an earlier one.
        Queue q;
        q.push(1, 0, P('a')); q.push(2, 100, P('b'));
        q.resolve(2, P('B'));
        check(q.tick(50).empty(), "B ready but A is not -> nothing");
        q.resolve(1, P('A'));
        auto r = q.tick(500);
        check(r.size() == 1 && r[0].seq == 1, "A first");
        r = q.tick(600);
        check(r.size() == 1 && r[0].seq == 2, "then B, keeping the 100ms gap");
    }
    {   // 4. Translation never arrives -> original at maxHold.
        Queue q(1000);
        q.push(1, 0, P('a'));
        check(q.tick(999).empty(), "held until maxHold");
        auto r = q.tick(1000);
        check(r.size() == 1 && !r[0].translated && r[0].payload == P('a'), "released as the ORIGINAL at maxHold");
    }
    {   // 5. A close keeps its place: it cannot jump ahead of the page before it.
        Queue q;
        q.push(1, 0, P('a'));                        // page, translating
        q.push(2, 200, P('x')); q.resolve(2, {});    // close signal: ready, shown as original
        check(q.tick(500).empty(), "close waits behind the untranslated page");
        q.resolve(1, P('A'));
        auto r = q.tick(1000);
        check(r.size() == 1 && r[0].seq == 1, "page first");
        r = q.tick(1200);
        check(r.size() == 1 && r[0].seq == 2 && !r[0].translated && r[0].payload == P('x'),
              "close 200ms later, as the original");
    }
    {   // 6. A new burst on an empty queue does not inherit the previous gap chain.
        Queue q;
        q.push(1, 0, P('a')); q.resolve(1, P('A'));
        check(q.tick(1000).size() == 1, "A released (1s translation delay)");
        q.push(2, 5000, P('b')); q.resolve(2, P('B'));
        auto r = q.tick(5010);
        check(r.size() == 1 && r[0].seq == 2, "new burst released as soon as ready");
    }
    {   // 7. Full queue refuses; flush releases originals in order.
        Queue q(10000, 2);
        check(q.push(1, 0, P('a')) && q.push(2, 10, P('b')), "two fit");
        check(!q.push(3, 20, P('c')), "third refused when full");
        q.resolve(1, P('A'));
        auto r = q.flush();
        check(r.size() == 2 && r[0].seq == 1 && r[1].seq == 2 && !r[0].translated && r[0].payload == P('a'),
              "flush: originals, in order");
        check(q.empty(), "flushed");
    }
    {   // 8. drain: everything at once, in order -- translated where it is ready.
        Queue q(10000, 4);
        q.push(1, 0, P('a'));
        q.push(2, 10, P('b'));
        q.push(3, 20, P('c'));
        q.resolve(2, P('B'));
        q.resolve(3, std::vector<uint8_t>()); // ready, as the original
        auto r = q.drain();
        check(r.size() == 3 && r[0].seq == 1 && r[1].seq == 2 && r[2].seq == 3, "drain: in order");
        check(!r[0].translated && r[0].payload == P('a'), "not ready: the original");
        check(r[1].translated && r[1].payload == P('B'), "ready: the translation");
        check(!r[2].translated && r[2].payload == P('c'), "ready without one: the original");
        check(q.empty(), "drained");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
