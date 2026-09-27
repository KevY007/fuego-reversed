#pragma once
// In-place edits of SA:MP's chat entry buffer (CChat, 0.3.DL). PURE: every
// function takes the raw CChat pointer (the AddEntry `this`), so it is
// host-tested against a fake buffer laid out identically (tests/chatbuf_test.cpp).
// Confirmed by disassembly of the user's samp.dll + an in-game test (2026-09-27):
// newest entry = [99]; AddEntry shifts older entries toward [0] and writes the new
// one at [99]; a long line becomes several entries (wrapped at insert time, never
// at render); setting the redraw flag makes CChat rebuild its cached texture from
// the entries' szText on the next frame.

#include <cstdint>
#include <string>
#include <vector>

namespace i18n::chatbuf {

constexpr uint32_t kEntry0   = 0x132;  // chatEntry[0] within CChat
constexpr uint32_t kStride   = 0xFC;   // sizeof(stChatEntry) = 252
constexpr uint32_t kTime     = 0x00;   // entry.SystemTime (u32)
constexpr uint32_t kText     = 0x20;   // entry.szText[144]
constexpr uint32_t kTextMax  = 144;
constexpr int      kEntries  = 100;
constexpr uint32_t kDirty    = 0x63DA; // the redraw flag AddEntry itself sets (NOT the header's m_iRedraw)
constexpr uint32_t kMinBytes = kDirty + 4;

struct Piece {
    uint32_t    time;
    std::string text;
};

// Copy of entry [99] -- take it BEFORE calling AddEntry.
std::vector<uint8_t> snapshot_newest(const uint8_t* chat);

// After AddEntry: how many entries it appended (it shifted older entries down one
// per appended entry, so the pre-call newest entry now sits at 99 - n). 0 if the
// snapshot can't be found.
int added_since(const uint8_t* chat, const std::vector<uint8_t>& snap);

// The newest n entries as one message's pieces, oldest piece first.
std::vector<Piece> read_newest(const uint8_t* chat, int n);

// Index of the first entry of the consecutive block matching `pieces` exactly
// (time + text), newest match first; -1 if gone (scrolled off / cleared).
int find_block(const uint8_t* chat, const std::vector<Piece>& pieces);

// Exactly n rows for n entries: greedy word wrap at `width` for all but the last
// row, the last row takes the rest; rows are capped at 143 bytes (overflow ends in
// "..."); spare rows are "".
std::vector<std::string> wrap_to(const std::string& text, int n, size_t width);

// Overwrite szText of entries [first, first + rows.size()) and set the redraw flag.
void write_block(uint8_t* chat, int first, const std::vector<std::string>& rows);

} // namespace i18n::chatbuf
