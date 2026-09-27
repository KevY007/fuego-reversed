#include "chatbuf.h"

#include <cstring>

namespace i18n::chatbuf {

namespace {

const uint8_t* entry(const uint8_t* chat, int i) { return chat + kEntry0 + (uint32_t)i * kStride; }
uint8_t* entry(uint8_t* chat, int i) { return chat + kEntry0 + (uint32_t)i * kStride; }

uint32_t time_of(const uint8_t* e) {
    uint32_t t;
    std::memcpy(&t, e + kTime, 4);
    return t;
}

std::string text_of(const uint8_t* e) {
    const char* t = (const char*)(e + kText);
    size_t len = 0;
    while (len < kTextMax && t[len]) ++len;
    return std::string(t, len);
}

} // namespace

std::vector<uint8_t> snapshot_newest(const uint8_t* chat) {
    const uint8_t* e = entry(chat, kEntries - 1);
    return std::vector<uint8_t>(e, e + kStride);
}

int added_since(const uint8_t* chat, const std::vector<uint8_t>& snap) {
    if (snap.size() != kStride) return 0;
    for (int i = kEntries - 2; i >= 0; --i)
        if (std::memcmp(entry(chat, i), snap.data(), kStride) == 0) return (kEntries - 1) - i;
    return 0;
}

std::vector<Piece> read_newest(const uint8_t* chat, int n) {
    std::vector<Piece> out;
    if (n <= 0 || n > kEntries) return out;
    for (int i = kEntries - n; i < kEntries; ++i) {
        const uint8_t* e = entry(chat, i);
        out.push_back({ time_of(e), text_of(e) });
    }
    return out;
}

int find_block(const uint8_t* chat, const std::vector<Piece>& pieces) {
    const int n = (int)pieces.size();
    if (n <= 0 || n > kEntries) return -1;
    for (int first = kEntries - n; first >= 0; --first) {
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            const uint8_t* e = entry(chat, first + k);
            ok = time_of(e) == pieces[k].time && text_of(e) == pieces[k].text;
        }
        if (ok) return first;
    }
    return -1;
}

std::vector<std::string> wrap_to(const std::string& text, int n, size_t width) {
    std::vector<std::string> out;
    if (n <= 0) return out;
    const size_t hard = kTextMax - 1;                          // 143 + NUL
    if (width == 0 || width > hard) width = hard;
    size_t pos = 0;
    for (int k = 0; k < n; ++k) {
        while (pos < text.size() && text[pos] == ' ') ++pos;   // no leading space on a row
        const size_t remaining = text.size() - pos;
        if (k == n - 1) {                                       // the last row takes the rest
            if (remaining <= hard) out.push_back(text.substr(pos));
            else out.push_back(text.substr(pos, hard - 3) + "...");
            pos = text.size();
            continue;
        }
        if (remaining <= width) { out.push_back(text.substr(pos)); pos = text.size(); continue; }
        size_t cut = text.rfind(' ', pos + width);             // last space within the width
        if (cut == std::string::npos || cut <= pos) cut = pos + width; // one long word: hard split
        out.push_back(text.substr(pos, cut - pos));
        pos = cut;
    }
    return out;
}

void write_block(uint8_t* chat, int first, const std::vector<std::string>& rows) {
    for (size_t k = 0; k < rows.size(); ++k) {
        const int i = first + (int)k;
        if (i < 0 || i >= kEntries) break;
        char* t = (char*)(entry(chat, i) + kText);
        const size_t len = rows[k].size() > kTextMax - 1 ? kTextMax - 1 : rows[k].size();
        std::memcpy(t, rows[k].data(), len);
        t[len] = '\0';
    }
    const int one = 1;
    std::memcpy(chat + kDirty, &one, 4);
}

} // namespace i18n::chatbuf
