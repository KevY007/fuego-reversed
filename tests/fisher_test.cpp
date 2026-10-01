// Host test for fish::Fisher -- the auto fisher's state machine (Mike's CnR fishing).
//
// Build/run: tests\run_fisher_test.cmd

#include "fishing/fisher.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace fish;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

// The server's lines, as recorded on Mike's CnR.
static const char* kCastDone = "The bar on-screen shows line length (full bar is fully cast) and health (green > yellow > orange > red).";
static std::string weight_line(const char* size, double lbs) {
    char b[200];
    std::snprintf(b, sizeof b, "Catch weight: {FFFFFF}%s (%.1f lbs). Reel in slowly (relative to the weight) to avoid damaging the line.", size, lbs);
    return b;
}
static const char* kBite = "You caught something! Keep tapping SPRINT to reel it in! Be gentle as your line may break if the fish fights too hard.";
static std::string stats_td(int line, int health, double lbs) {
    char b[64];
    std::snprintf(b, sizeof b, "%d%%~N~%d%%~N~%.2flbs~N~", line, health, lbs);
    return b;
}

// Steps the fisher in 5 ms frames until `pred` holds (or `limitMs` passes); returns the time.
template <class P>
static uint32_t run_until(Fisher& f, uint32_t now, uint32_t limitMs, P pred, uint16_t* keys = nullptr) {
    for (uint32_t t = now; t <= now + limitMs; t += 5) {
        const uint16_t k = f.tick(t);
        if (keys) *keys = k;
        if (pred(k, t)) return t;
    }
    return 0;
}

// A whole cast, from Ready to the line being out: answers the way the server does.
// Returns the time the line is out, 0 if the fisher never cast.
static uint32_t cast(Fisher& f, uint32_t now) {
    const uint32_t aim = run_until(f, now, 5000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; });
    if (!aim) return 0;
    f.on_anim("GRENADE", "WEAPON_START_THROW", aim + 140);
    const uint32_t fire = run_until(f, aim + 140, 3000, [](uint16_t k, uint32_t) { return (k & KEY_FIRE) != 0; });
    if (!fire) return 0;
    f.on_anim("GRENADE", "WEAPON_THROW", fire + 130);
    const uint32_t up = run_until(f, fire + 130, 3000, [](uint16_t k, uint32_t) { return k == 0; });
    if (!up) return 0;
    f.on_chat(kCastDone, up + 300);
    return up + 300;
}

int main() {
    {   // 1. Parsing what the server says.
        check(Fisher::strip_colors("You caught {FFFFFF}a Trout{1297c4}!") == "You caught a Trout!", "colour codes stripped");
        check(Fisher::strip_colors("{bad} {12345} text") == "{bad} {12345} text", "only real {RRGGBB} codes go");
        int line = 0, health = 0; double lbs = 0;
        check(Fisher::parse_stats("64%~N~90%~N~26.30lbs~N~", line, health, lbs) && line == 64 && health == 90 && lbs > 26.29 && lbs < 26.31,
              "the stats textdraw: line, health, weight");
        check(!Fisher::parse_stats("Line length~N~Line health~N~Catch weight~N~", line, health, lbs), "its label textdraw isn't stats");
        check(!Fisher::parse_stats("100%", line, health, lbs), "a lone percentage isn't stats");
        check(Fisher::parse_weight(weight_line("Huge", 265.8), lbs) && lbs > 265.7 && lbs < 265.9, "the weight from the chat line");
        check(!Fisher::parse_weight("no weight here", lbs), "no weight -> false");
        check(Fisher::base_gap_ms(1.1) < Fisher::base_gap_ms(20.2) && Fisher::base_gap_ms(20.2) < Fisher::base_gap_ms(50.7) &&
              Fisher::base_gap_ms(50.7) < Fisher::base_gap_ms(265.8), "heavier fish -> slower presses");
        check(Fisher::base_gap_ms(265.8) >= 290 && Fisher::base_gap_ms(50.7) >= 240 && Fisher::base_gap_ms(1.1) >= 180,
              "every pace keeps a margin over what cost line health in the recording");
    }
    {   // 2. A whole catch, then the next cast.
        Fisher f(1);
        f.on_rod(true);
        f.enable(true, 0);
        check(f.state() == State::Ready, "enabled with the rod out -> about to cast");
        uint16_t k = 0xFFFF;
        check(f.tick(0) == 0, "no key at once (a human pause first)");
        const uint32_t out = cast(f, 0);
        check(out != 0, "it casts: AIM, then FIRE once the throw started, then lets go");
        check(f.state() == State::WaitBite, "line out -> waiting for a bite");
        check(run_until(f, out, 5000, [](uint16_t k2, uint32_t) { return k2 != 0; }) == 0, "no key while waiting for a bite");

        f.on_chat(weight_line("Very small", 1.1), out + 7000);
        f.on_chat(kBite, out + 7000);
        f.on_textdraw(stats_td(100, 100, 1.1), out + 7000);
        check(f.state() == State::Reeling, "a bite -> reeling");
        const uint32_t first = run_until(f, out + 7000, 2000, [](uint16_t k2, uint32_t) { return (k2 & KEY_SPRINT) != 0; });
        check(first >= out + 7000 + 250 && first <= out + 7000 + 600, "the first press after a human reaction time");

        // Tap it in: the server answers each press with the next stats.
        std::vector<uint32_t> presses;
        uint32_t t = first, pressStart = 0;
        bool down = false;
        int line = 100;
        for (; t < first + 20000 && line > 0; t += 5) {
            k = f.tick(t);
            const bool d = (k & KEY_SPRINT) != 0;
            if (d && !down) { presses.push_back(t); pressStart = t; }
            if (!d && down) {
                check(t - pressStart >= 60 && t - pressStart <= 140, "a press is held like a finger tap");
                line -= 8;
                if (line < 0) line = 0;
                f.on_textdraw(stats_td(line, 100, 1.1), t + 120);
            }
            down = d;
            check((k & ~kControlled) == 0, "never a key outside FIRE / SPRINT / AIM");
        }
        check(line == 0, "it reels the fish all the way in");
        bool spaced = true;
        for (size_t i = 1; i < presses.size(); ++i)
            if (presses[i] - presses[i - 1] < Fisher::base_gap_ms(1.1) * 95 / 100) spaced = false;
        check(spaced, "presses never closer than the safe pace for the weight");
        f.on_chat("You caught {FFFFFF}a Sea Bass{1297c4} weighing {FFFFFF}1.1lbs{1297c4}!", t + 130);
        check(f.state() == State::Cooldown, "caught -> a short break");
        check(f.stats().caught == 1 && f.stats().pounds > 1.0 && f.stats().biggestName == "Sea Bass", "the catch is counted");
        f.on_rod(false);
        f.on_rod(true); // the server swaps the rod back in after the catch
        const uint32_t again = run_until(f, t + 130, 5000, [](uint16_t k2, uint32_t) { return (k2 & KEY_HANDBRAKE) != 0; });
        check(again >= t + 130 + 800 && again <= t + 130 + 2500, "then it casts again after a short break");
        check(f.status(again).find("casting") != std::string::npos, "status says what it does");
    }
    {   // 3. Line damage slows the pace down for the rest of that fish.
        Fisher f(2);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_chat(weight_line("Huge", 265.8), out + 7000);
        f.on_chat(kBite, out + 7000);
        f.on_textdraw(stats_td(100, 100, 265.8), out + 7000);
        uint32_t t = out + 7000;
        std::vector<uint32_t> presses;
        bool down = false;
        int line = 100, health = 100;
        for (; t < out + 60000 && line > 0; t += 5) {
            const uint16_t k = f.tick(t);
            const bool d = (k & KEY_SPRINT) != 0;
            if (d && !down) presses.push_back(t);
            if (!d && down) {
                line -= 4;
                if (presses.size() == 3) health -= 10; // the third press costs health
                f.on_textdraw(stats_td(line, health, 265.8), t + 120);
            }
            down = d;
        }
        check(presses.size() >= 6, "it keeps reeling");
        const uint32_t before = presses[2] - presses[1];
        const uint32_t after = presses[5] - presses[4];
        check(after >= before + 40, "after the line took damage it presses slower");
        check(f.status(t).find("health 90%") != std::string::npos, "status shows the line health");
    }
    {   // 4. A rotten fish: a long break (stunned ~10 s), counted apart.
        Fisher f(3);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_chat(kBite, out + 7000);
        f.on_chat("You caught {FFFFFF}a rotten fish{1297c4}.", out + 9000);
        check(f.stats().rotten == 1 && f.stats().caught == 0, "a rotten fish isn't a catch");
        const uint32_t next = run_until(f, out + 9000, 20000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; });
        check(next >= out + 9000 + 11000, "no cast while stunned (11 s+)");
    }
    {   // 5. Snapped / got away / a wallet.
        Fisher f(4);
        f.on_rod(true);
        f.enable(true, 0);
        uint32_t out = cast(f, 0);
        f.on_chat(kBite, out + 7000);
        f.on_chat("The line snapped and the catch was lost. Reel heavier fish in slowly to avoid breaking the line!", out + 9000);
        check(f.stats().snapped == 1 && f.state() == State::Cooldown, "a snapped line is counted");
        out = cast(f, out + 9000);
        check(out != 0, "it casts again after a snap");
        f.on_chat(kBite, out + 7000);
        f.on_chat("The catch got away.", out + 9000);
        check(f.stats().gotAway == 1, "a catch that got away is counted");
        out = cast(f, out + 9000);
        f.on_chat(kBite, out + 7000);
        f.on_chat("You caught {FFFFFF}a wallet{1297c4}.", out + 9000);
        check(f.stats().other == 1 && f.stats().caught == 0, "a wallet isn't a fish");
    }
    {   // 6. The server doesn't answer the cast: let go, retry, and stop after 3 tries.
        Fisher f(5);
        f.on_rod(true);
        f.enable(true, 0);
        uint32_t t = 0;
        int aims = 0;
        bool down = false;
        for (; t < 60000 && f.enabled(); t += 5) {
            const uint16_t k = f.tick(t);
            const bool d = (k & KEY_HANDBRAKE) != 0;
            if (d && !down) ++aims;
            down = d;
        }
        check(aims == 3, "three tries");
        check(!f.enabled(), "then it stops");
        std::string n;
        check(f.pop_notice(n) && n.find("cast") != std::string::npos, "and says why");
        check(f.tick(t) == 0, "no keys once stopped");
    }
    {   // 7. No rod: wait for it, never press anything.
        Fisher f(6);
        f.enable(true, 0);
        check(f.state() == State::NoRod, "no rod -> waiting for it");
        check(run_until(f, 0, 5000, [](uint16_t k, uint32_t) { return k != 0; }) == 0, "no keys without a rod");
        check(f.status(100).find("/fish") != std::string::npos, "status tells how to get the rod out");
        f.on_rod(true);
        check(run_until(f, 5000, 5000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; }) != 0, "rod out -> it casts");
    }
    {   // 8. A dialog pops up mid-wait: hands off until it's closed.
        Fisher f(7);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_dialog_shown(out + 1000);
        check(f.state() == State::Paused && f.tick(out + 1000) == 0, "paused, keys up");
        f.on_chat(kBite, out + 2000);
        check(f.state() == State::Paused, "even a bite doesn't make it act behind a dialog");
        f.on_dialog_closed(out + 3000);
        check(f.state() != State::Paused, "resumes when the dialog is closed");
    }
    {   // 9. Turned on mid-cycle: picks up where the server is.
        Fisher f(8);
        f.on_rod(true);
        f.enable(true, 0);
        f.on_chat(kCastDone, 10);
        check(f.state() == State::WaitBite, "line already out -> waiting for a bite");
        Fisher g(9);
        g.on_rod(true);
        g.enable(true, 0);
        g.on_chat(weight_line("Small", 20.2), 10);
        g.on_chat(kBite, 10);
        check(g.state() == State::Reeling, "a fish already on -> reeling");
    }
    {   // 10. Turned off: every key up at once; stats stay.
        Fisher f(10);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t aim = run_until(f, 0, 5000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; });
        check(aim != 0, "aiming");
        f.enable(false, aim + 10);
        check(f.tick(aim + 10) == 0 && f.state() == State::Off, "off -> keys up");
    }

    {   // 11. Player chat comes through the same RPC: "[R2AW]Panzer_ (34): The catch got away" is not the server.
        Fisher f(11);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_chat("[R2AW]Panzer_ (34){FFFFFF}: You caught something! Keep tapping SPRINT", out + 1000);
        check(f.state() == State::WaitBite, "a player quoting the bite line doesn't start reeling");
        f.on_chat(weight_line("Huge", 265.8), out + 7000);
        f.on_chat(kBite, out + 7000);
        f.on_chat("[R2AW]Panzer_ (34){FFFFFF}: The catch got away.", out + 7100);
        f.on_chat("Wendino (41){FFFFFF}: You caught a Trout weighing 9.0lbs!", out + 7200);
        check(f.state() == State::Reeling && f.stats().gotAway == 0 && f.stats().caught == 0,
              "a player quoting results changes nothing mid-reel");
    }
    {   // 12. The weight line was missed: the stats textdraw's weight sets the pace.
        Fisher f(12);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_chat(kBite, out + 7000); // no "Catch weight" line before it
        f.on_textdraw(stats_td(100, 100, 265.8), out + 7000);
        std::vector<uint32_t> presses;
        bool down = false;
        for (uint32_t t = out + 7000; t < out + 12000; t += 5) {
            const bool d = (f.tick(t) & KEY_SPRINT) != 0;
            if (d && !down) presses.push_back(t);
            down = d;
        }
        check(presses.size() >= 3 && presses[2] - presses[1] >= Fisher::base_gap_ms(265.8) * 95 / 100,
              "a 265 lb fish is reeled at the huge-fish pace even without the weight line");
    }
    {   // 13. A throw answer that comes late, or never: keys are never left held.
        Fisher f(13);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t aim = run_until(f, 0, 5000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; });
        f.on_anim("GRENADE", "WEAPON_THROW", aim + 50); // stale, from an earlier try
        f.on_anim("GRENADE", "WEAPON_START_THROW", aim + 140);
        uint32_t heldSince = 0, longest = 0;
        for (uint32_t t = aim; t < aim + 20000; t += 5) {
            const uint16_t k = f.tick(t);
            if (k && !heldSince) heldSince = t;
            if (!k && heldSince) { longest = longest > t - heldSince ? longest : t - heldSince; heldSince = 0; }
        }
        if (heldSince) longest = 20000;
        check(longest <= 3500, "no key is ever held more than a few seconds when the server doesn't answer");
    }
    {   // 14. A rotten fish's stun survives a dialog popping up and closing.
        Fisher f(14);
        f.on_rod(true);
        f.enable(true, 0);
        const uint32_t out = cast(f, 0);
        f.on_chat(kBite, out + 7000);
        f.on_dialog_shown(out + 8000);
        f.on_chat("You caught {FFFFFF}a rotten fish{1297c4}.", out + 9000);
        f.on_dialog_closed(out + 9500);
        const uint32_t next = run_until(f, out + 9500, 20000, [](uint16_t k, uint32_t) { return (k & KEY_HANDBRAKE) != 0; });
        check(next >= out + 9000 + 11000, "still no cast during the stun");
    }
    {   // 15. The PC's tick counter wrapping around (every 49.7 days) changes nothing.
        Fisher f(15);
        f.on_rod(true);
        const uint32_t start = 0xFFFFFFFFu - 9000; // wraps ~9 s in
        f.enable(true, start);
        const uint32_t out = cast(f, start);
        check(out != 0, "it casts across the wrap");
        f.on_chat(weight_line("Huge", 265.8), out + 1000);
        f.on_chat(kBite, out + 1000);
        f.on_textdraw(stats_td(100, 100, 265.8), out + 1000);
        std::vector<uint32_t> presses;
        bool down = false;
        for (uint32_t i = 0; i < 12000; i += 5) {
            const uint32_t t = out + 1000 + i;
            const bool d = (f.tick(t) & KEY_SPRINT) != 0;
            if (d && !down) presses.push_back(t);
            down = d;
        }
        bool spaced = presses.size() >= 10;
        for (size_t i = 1; i < presses.size(); ++i)
            if (presses[i] - presses[i - 1] < Fisher::base_gap_ms(265.8) * 95 / 100) spaced = false;
        check(spaced, "presses stay spaced across the wrap");
    }
    {   // 16. It only claims the keys while it's using them.
        Fisher f(16);
        f.on_rod(true);
        f.enable(true, 0);
        check(!f.holds_keys(), "about to cast: your keys are yours");
        const uint32_t out = cast(f, 0);
        check(!f.holds_keys(), "waiting for a bite: your keys are yours");
        f.on_chat(kBite, out + 7000);
        check(f.holds_keys(), "reeling: SPRINT is the fisher's");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
