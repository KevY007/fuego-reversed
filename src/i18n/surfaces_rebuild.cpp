// i18n/surfaces_rebuild.cpp -- PURE packet-rebuild logic for the translation
// surfaces (no game types, no engine/config), so it can be host-tested byte-exact
// against real captured packets (tests/surfaces_test.cpp). surfaces.cpp holds the
// game/engine glue (on_incoming_rpc, cache translator, BitStream commit).

#include "surfaces.h"
#include "huffman.h"

#include <cstdint>

namespace i18n::surfaces {

namespace {

// Read a u8-length-prefixed plain string at p[off..]; advance off. Bounds-checked.
bool read_u8(const uint8_t* p, size_t n, size_t& off, std::string& out) {
    if (off >= n) return false;
    size_t len = p[off++];
    if (off + len > n) return false;
    out.assign((const char*)(p + off), len);
    off += len;
    return true;
}

// Append a u8-length-prefixed string (truncated to 255 bytes -- the field's max).
void put_u8(std::vector<uint8_t>& v, const std::string& s) {
    size_t len = s.size() > 255 ? 255 : s.size();
    v.push_back((uint8_t)len);
    v.insert(v.end(), s.begin(), s.begin() + (std::string::difference_type)len);
}

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
void put_u16(std::vector<uint8_t>& v, size_t n) {
    if (n > 0xFFFF) n = 0xFFFF;
    v.push_back((uint8_t)(n & 0xFF));
    v.push_back((uint8_t)((n >> 8) & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t n) {
    v.push_back((uint8_t)(n & 0xFF)); v.push_back((uint8_t)((n >> 8) & 0xFF));
    v.push_back((uint8_t)((n >> 16) & 0xFF)); v.push_back((uint8_t)((n >> 24) & 0xFF));
}

} // namespace

bool is_dynamic_text(const std::string& t) {
    int letters = 0, digits = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '~') { size_t j = i + 1; while (j < t.size() && t[j] != '~') ++j; i = j; continue; } // ~x~ code
        if (c == '{') { size_t j = i + 1; while (j < t.size() && t[j] != '}') ++j; i = j; continue; } // {RRGGBB}
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
        else if (c >= '0' && c <= '9') ++digits;
    }
    return letters < 3 || digits >= letters;
}

bool rebuild_gametext(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    if (n < 12) return false;
    uint32_t style = rd32(p), time = rd32(p + 4), msgLen = rd32(p + 8);
    size_t textOff = 12;
    if (textOff + msgLen > n) msgLen = (uint32_t)(n - textOff); // clamp to what's present
    std::string msg((const char*)(p + textOff), msgLen);
    std::string t;
    if (!tr(msg, t)) return false;
    out.clear();
    put_u32(out, style); put_u32(out, time); put_u32(out, (uint32_t)t.size());
    out.insert(out.end(), t.begin(), t.end());
    return true;
}

bool rebuild_chatbubble(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    if (n < 15) return false;
    size_t textLen = p[14];
    if (15 + textLen > n) textLen = n - 15;
    std::string text((const char*)(p + 15), textLen);
    std::string t;
    if (!tr(text, t)) return false;
    out.assign(p, p + 14);   // u16 pid, u32 color, f dist, u32 expire -- verbatim
    put_u8(out, t);          // u8 len + text
    return true;
}

bool rebuild_textdraw_setstring(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    if (n < 4) return false;
    size_t len = rd16(p + 2);
    if (4 + len > n) len = n - 4;
    std::string text((const char*)(p + 4), len);
    std::string t;
    if (!tr(text, t)) return false;
    out.assign(p, p + 2);    // u16 textdrawId
    put_u16(out, t.size());
    out.insert(out.end(), t.begin(), t.begin() + (std::string::difference_type)(t.size() > 0xFFFF ? 0xFFFF : t.size()));
    return true;
}

bool rebuild_textdraw_show(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    // 65-byte fixed header, then u16 szTextLen (bytes 65-66), then szText (last field).
    if (n < 67) return false;
    size_t len = rd16(p + 65);
    if (67 + len > n) len = n - 67;
    std::string text((const char*)(p + 67), len);
    std::string t;
    if (!tr(text, t)) return false;
    out.assign(p, p + 65);   // everything before szTextLen -- verbatim
    put_u16(out, t.size());
    out.insert(out.end(), t.begin(), t.begin() + (std::string::difference_type)(t.size() > 0xFFFF ? 0xFFFF : t.size()));
    return true;
}

bool rebuild_dialog(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    // Layout: u16 dialogId, u8 style, u8+title, u8+btn1, u8+btn2, CSTRING szInfo.
    if (n < 6) return false;
    size_t off = 2;
    uint16_t dlgId = (uint16_t)(p[0] | (p[1] << 8));
    uint8_t style = p[off++];
    std::string title, b1, b2;
    if (!read_u8(p, n, off, title)) return false;
    if (!read_u8(p, n, off, b1)) return false;
    if (!read_u8(p, n, off, b2)) return false;

    size_t szOff = off, szLen = n - off;
    std::string body;
    bool haveBody = szLen > 0 && huffman::decode_string(p + szOff, szLen, body);

    // Every translatable piece must resolve, or the caller passes the original through.
    std::string tTitle, tB1, tB2, tBody;
    if (!tr(title, tTitle)) return false;
    if (!tr(b1, tB1)) return false;
    if (!tr(b2, tB2)) return false;
    if (haveBody && !tr(body, tBody)) return false;

    out.clear();
    out.push_back((uint8_t)(dlgId & 0xFF));
    out.push_back((uint8_t)(dlgId >> 8));
    out.push_back(style);
    put_u8(out, tTitle);
    put_u8(out, tB1);
    put_u8(out, tB2);
    if (haveBody) {
        std::vector<uint8_t> enc = huffman::encode_string(tBody);
        out.insert(out.end(), enc.begin(), enc.end());
    } else if (szLen > 0) {
        out.insert(out.end(), p + szOff, p + n); // undecodable body -> keep verbatim
    }
    return true;
}

bool rebuild_label(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    // u16 id, u32 color, f x, f y, f z, f dist, u8 testLOS, u16 aPlayer, u16 aVeh
    // = 27 fixed bytes, then a CSTRING text.
    if (n <= 27) return false;
    std::string text;
    if (!huffman::decode_string(p + 27, n - 27, text)) return false;
    std::string tText;
    if (!tr(text, tText)) return false;

    out.assign(p, p + 27);
    std::vector<uint8_t> enc = huffman::encode_string(tText);
    out.insert(out.end(), enc.begin(), enc.end());
    return true;
}

} // namespace i18n::surfaces
