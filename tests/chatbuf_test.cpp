// Host test for i18n::chatbuf -- the in-place chat entry rewrite -- against a fake
// CChat buffer laid out like SA:MP 0.3.DL's (offsets confirmed by disassembly).
//
// Build/run: tests\run_chatbuf_test.cmd

#include "i18n/chatbuf.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace i18n::chatbuf;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

static uint8_t* entry(std::vector<uint8_t>& b, int i) { return b.data() + kEntry0 + (size_t)i * kStride; }

// Exactly what CChat::AddEntry does per appended entry: shift [1..99] -> [0..98],
// then write the new entry into [99].
static void sim_add(std::vector<uint8_t>& b, uint32_t t, const char* text) {
    std::memmove(entry(b, 0), entry(b, 1), (size_t)(kEntries - 1) * kStride);
    std::memset(entry(b, 99), 0, kStride);
    std::memcpy(entry(b, 99) + kTime, &t, 4);
    std::strncpy((char*)entry(b, 99) + kText, text, kTextMax - 1);
}
static std::string text_at(std::vector<uint8_t>& b, int i) { return std::string((const char*)entry(b, i) + kText); }
static int dirty(std::vector<uint8_t>& b) { int d; std::memcpy(&d, b.data() + kDirty, 4); return d; }

int main() {
    std::vector<uint8_t> b(kMinBytes, 0);

    // One line into an empty chat.
    std::vector<uint8_t> snap = snapshot_newest(b.data());
    sim_add(b, 1000, "Privet brat");
    check(added_since(b.data(), snap) == 1, "one appended entry detected on an empty buffer");

    // A long line that SA:MP wrapped into two entries.
    snap = snapshot_newest(b.data());
    sim_add(b, 2000, "part one of a long line");
    sim_add(b, 2000, "and its second half");
    check(added_since(b.data(), snap) == 2, "two appended entries detected");
    std::vector<Piece> pieces = read_newest(b.data(), 2);
    check(pieces.size() == 2 && pieces[0].text == "part one of a long line" &&
          pieces[1].text == "and its second half" && pieces[0].time == 2000,
          "read_newest: oldest piece first, with times");

    // Newer lines push the block up; it is still found by content + time.
    sim_add(b, 3000, "x");
    sim_add(b, 3001, "y");
    int first = find_block(b.data(), pieces);
    check(first == 96, "block found after shifting two slots");

    // Same text, different time: not confused with our block.
    std::vector<Piece> other = { { 9999, "part one of a long line" }, { 9999, "and its second half" } };
    check(find_block(b.data(), other) == -1, "same text with a different time is not a match");

    // Rewrite in place: our two rows change, neighbours don't, redraw flag set.
    write_block(b.data(), first, wrap_to("una parte larga y su segunda mitad", 2, 20));
    check(text_at(b, 96) == "una parte larga y su" && text_at(b, 97) == "segunda mitad",
          "translation wrapped across the same two rows");
    check(text_at(b, 95) == "Privet brat" && text_at(b, 98) == "x" && text_at(b, 99) == "y",
          "neighbouring lines untouched");
    check(dirty(b) == 1, "redraw flag (+0x63DA) set");

    // Scrolled off: after 100 more lines the block is gone.
    for (int k = 0; k < 100; ++k) sim_add(b, 5000 + k, "spam");
    check(find_block(b.data(), pieces) == -1, "block that scrolled off is not found");

    // wrap_to edge cases.
    std::vector<std::string> one = wrap_to("short", 1, 72);
    check(one.size() == 1 && one[0] == "short", "short text -> one row");
    std::vector<std::string> pad = wrap_to("short", 3, 72);
    check(pad.size() == 3 && pad[0] == "short" && pad[1].empty() && pad[2].empty(), "spare rows become empty");
    std::vector<std::string> cut = wrap_to(std::string(300, 'a'), 1, 72);
    check(cut.size() == 1 && cut[0].size() == kTextMax - 1 && cut[0].substr(cut[0].size() - 3) == "...",
          "overflow truncated to 143 bytes ending in '...'");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
