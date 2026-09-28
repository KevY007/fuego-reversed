// Host test for checkpoints -- where the server's checkpoints are, for /triggercp.
//
// Build/run: tests\run_checkpoints_test.cmd

#include "cheats/checkpoints.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

// A payload laid out the way the server writes it (raw little-endian floats).
static void put(std::vector<unsigned char>& b, float f) {
    unsigned char raw[4];
    std::memcpy(raw, &f, 4);
    b.insert(b.end(), raw, raw + 4);
}
static std::vector<unsigned char> set_checkpoint_rpc(float x, float y, float z, float radius) {
    std::vector<unsigned char> b;                  // SetCheckpoint (107): x, y, z, radius
    put(b, x); put(b, y); put(b, z); put(b, radius);
    return b;
}
static std::vector<unsigned char> set_race_rpc(unsigned char type, float x, float y, float z) {
    std::vector<unsigned char> b{ type };          // SetRaceCheckpoint (38): u8 type, x, y, z,
    put(b, x); put(b, y); put(b, z);               // next x, y, z, radius
    put(b, 9.0f); put(b, 8.0f); put(b, 7.0f); put(b, 5.0f);
    return b;
}
static bool at(const float p[3], float x, float y, float z) { return p[0] == x && p[1] == y && p[2] == z; }
static unsigned bits(const std::vector<unsigned char>& b) { return (unsigned)b.size() * 8; }

int main() {
    using namespace checkpoints;
    float p[3] = { 0, 0, 0 };

    {   // 1. The payload readers.
        auto cp = set_checkpoint_rpc(1234.5f, -1650.25f, 13.5f, 3.0f);
        check(read_checkpoint(cp.data(), bits(cp), p) && at(p, 1234.5f, -1650.25f, 13.5f),
              "SetCheckpoint: x, y, z are the first three floats");
        check(!read_checkpoint(cp.data(), 95, p), "SetCheckpoint: shorter than x, y, z -> no position");
        check(!read_checkpoint(nullptr, 128, p), "SetCheckpoint: no payload -> no position");

        auto race = set_race_rpc(1, -2000.0f, 150.75f, 30.0f);
        check(read_race_checkpoint(race.data(), bits(race), p) && at(p, -2000.0f, 150.75f, 30.0f),
              "SetRaceCheckpoint: x, y, z come after the type byte");
        check(!read_race_checkpoint(race.data(), 8 + 95, p), "SetRaceCheckpoint: cut short -> no position");
        check(!read_race_checkpoint(nullptr, 300, p), "SetRaceCheckpoint: no payload -> no position");
    }
    {   // 2. None until the server sets one.
        reset();
        check(active(p) == kNone, "nothing set -> no checkpoint to enter");
    }
    {   // 3. A checkpoint; moved; removed.
        reset();
        set(kCheckpoint, 10, 20, 30);
        check(active(p) == kCheckpoint && at(p, 10, 20, 30), "the set checkpoint is the one to enter");
        set(kCheckpoint, 11, 21, 31);
        check(active(p) == kCheckpoint && at(p, 11, 21, 31), "a re-set checkpoint moves (the server set the next one)");
        clear(kCheckpoint);
        check(active(p) == kNone, "a removed checkpoint is never entered (no stale position)");
    }
    {   // 4. Both kinds at once: the most recently set one; removing it falls back to the other.
        reset();
        set(kCheckpoint, 1, 1, 1);
        set(kRace, 2, 2, 2);
        check(active(p) == kRace && at(p, 2, 2, 2), "race checkpoint set last -> it is entered");
        set(kCheckpoint, 3, 3, 3);
        check(active(p) == kCheckpoint && at(p, 3, 3, 3), "checkpoint set again last -> it is entered");
        clear(kCheckpoint);
        check(active(p) == kRace && at(p, 2, 2, 2), "checkpoint removed -> the race checkpoint is still there");
        clear(kRace);
        check(active(p) == kNone, "both removed -> none");
    }
    {   // 5. A new session (InitGame / GameModeRestart) forgets both.
        reset();
        set(kCheckpoint, 4, 4, 4);
        set(kRace, 5, 5, 5);
        reset();
        check(active(p) == kNone, "new session -> the old server's checkpoints are gone");
        set(kRace, 6, 6, 6);
        check(active(p) == kRace && at(p, 6, 6, 6), "the new session's race checkpoint works");
    }
    {   // 6. Removing a kind that isn't set changes nothing.
        reset();
        set(kCheckpoint, 7, 7, 7);
        clear(kRace);
        check(active(p) == kCheckpoint && at(p, 7, 7, 7), "removing the other kind keeps this one");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
