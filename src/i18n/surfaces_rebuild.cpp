// i18n/surfaces_rebuild.cpp -- PURE packet-rebuild logic for the translation
// surfaces (no game types, no engine/config), so it can be host-tested byte-exact
// against real captured packets (tests/surfaces_test.cpp). surfaces.cpp holds the
// game/engine glue (on_incoming_rpc, cache translator, BitStream commit).

#include "surfaces.h"
#include "huffman.h"

#include <cstdint>
#include <unordered_map>

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

// "{RRGGBB}" at s[i].
bool is_color_code(const std::string& s, size_t i) {
    if (i + 8 > s.size() || s[i] != '{' || s[i + 7] != '}') return false;
    for (size_t k = i + 1; k < i + 7; ++k) {
        char c = s[k];
        bool hx = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hx) return false;
    }
    return true;
}

// Letters vs digits, skipping ~x~ and {RRGGBB} codes. Bytes >= 0x80 are letters of
// a non-Latin codepage (cp1251 Cyrillic etc.): counting only ASCII made every
// Cyrillic label look "numeric" and it was never translated.
void count_letters_digits(const std::string& t, int& letters, int& digits) {
    letters = digits = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = (unsigned char)t[i];
        if (c == '~') { size_t j = i + 1; while (j < t.size() && t[j] != '~') ++j; i = j; continue; }
        if (c == '{') { size_t j = i + 1; while (j < t.size() && t[j] != '}') ++j; i = j; continue; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) ++letters;
        else if (c >= '0' && c <= '9') ++digits;
    }
}

// Dialog units: nothing worth translating ("12m", "1.15km", "$500", "7").
bool mostly_numeric(const std::string& t) {
    int letters, digits;
    count_letters_digits(t, letters, digits);
    return letters < 2 || digits >= letters;
}

} // namespace

bool is_dynamic_text(const std::string& t) {
    int letters, digits;
    count_letters_digits(t, letters, digits);
    return letters < 3 || digits >= letters;
}

bool is_verbatim_cell(const std::string& core) {
    if (core.empty() || mostly_numeric(core)) return true;
    if (core.find(' ') != std::string::npos) return false;      // a phrase -> translate
    bool anyUpper = false;
    for (unsigned char c : core) {
        if (c >= 0x80) return false;                             // non-Latin word -> translate
        if (c >= 'A' && c <= 'Z') anyUpper = true;
        if ((c >= '0' && c <= '9') || c == '/' || c == '_') return true;
    }
    return !anyUpper;                                            // all-lowercase token = shortcode
}

Unit make_unit(const std::string& raw, bool cell) {
    Unit u;
    size_t a = 0, b = raw.size();
    while (a < b) {                                  // leading spaces + colour codes
        if (raw[a] == ' ') { ++a; continue; }
        if (is_color_code(raw, a)) { a += 8; continue; }
        break;
    }
    while (b > a && raw[b - 1] == ' ') --b;          // trailing spaces
    u.prefix = raw.substr(0, a);
    u.core   = raw.substr(a, b - a);
    u.suffix = raw.substr(b);
    u.translate = cell ? !is_verbatim_cell(u.core) : !(u.core.empty() || mostly_numeric(u.core));
    return u;
}

bool plan_dialog(const uint8_t* p, size_t n, DialogPlan& d) {
    d = DialogPlan{};
    if (!p || n < 6) return false;
    d.id = rd16(p);
    size_t off = 2;
    d.style = p[off++];
    std::string title, b1, b2;
    if (!read_u8(p, n, off, title) || !read_u8(p, n, off, b1) || !read_u8(p, n, off, b2)) return false;
    d.units.push_back(make_unit(title, false));
    d.units.push_back(make_unit(b1, false));
    d.units.push_back(make_unit(b2, false));
    if (off >= n) return true;                                    // no body bytes at all
    std::string body;
    d.haveBody = huffman::decode_string(p + off, n - off, body);
    if (!d.haveBody) { d.rawBody.assign(p + off, p + n); return true; }
    const bool list = (d.style == 2 || d.style == 4 || d.style == 5);
    std::string cur;
    for (char c : body) {
        if (c == '\n' || c == '\t') {
            d.units.push_back(make_unit(cur, list));
            d.bodySeps.push_back(c);
            cur.clear();
        } else {
            cur += c;
        }
    }
    d.units.push_back(make_unit(cur, list));
    return true;
}

std::vector<std::string> original_cores(const DialogPlan& d) {
    std::vector<std::string> c;
    c.reserve(d.units.size());
    for (const Unit& u : d.units) c.push_back(u.core);
    return c;
}

std::string clean_translation(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\r') continue;
        o += (c == '\t' || c == '\n') ? ' ' : c;
    }
    return o;
}

std::string unit_text(const Unit& u, const std::string& core) {
    if (!u.translate || core == u.core) return u.prefix + u.core + u.suffix;
    return u.prefix + clean_translation(core) + u.suffix;
}

std::string assemble_body(const DialogPlan& d, const std::vector<std::string>& cores) {
    std::string body;
    for (size_t k = 3; k < d.units.size(); ++k) {
        body += unit_text(d.units[k], k < cores.size() ? cores[k] : d.units[k].core);
        if (k - 3 < d.bodySeps.size()) body += d.bodySeps[k - 3];
    }
    return body;
}

std::vector<uint8_t> assemble_dialog(const DialogPlan& d, const std::vector<std::string>& cores) {
    auto core = [&](size_t k) -> const std::string& { return k < cores.size() ? cores[k] : d.units[k].core; };
    std::vector<uint8_t> out;
    out.push_back((uint8_t)(d.id & 0xFF));
    out.push_back((uint8_t)(d.id >> 8));
    out.push_back(d.style);
    put_u8(out, unit_text(d.units[0], core(0)));
    put_u8(out, unit_text(d.units[1], core(1)));
    put_u8(out, unit_text(d.units[2], core(2)));
    if (d.haveBody) {
        std::vector<uint8_t> enc = huffman::encode_string(assemble_body(d, cores));
        out.insert(out.end(), enc.begin(), enc.end());
    } else {
        out.insert(out.end(), d.rawBody.begin(), d.rawBody.end());
    }
    return out;
}

namespace {

// SA:MP's list line reader copies at most 0x100 bytes (255 + NUL) per item and
// parses the REST of a longer line as another item; the tablist cell reader copies
// at most 0x80 bytes (127 + NUL) per cell and spills the rest into the next column.
constexpr size_t kLineBudget = 255;
constexpr size_t kCellBudget = 127;
// A DialogResponse's list text is strncpy(.., 0x80)'d from the picked row: a text
// this long was (or may have been) cut there.
constexpr size_t kNativeRespCut = 127;

bool blank_line(const std::string& s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r') return false;
    return true;
}

// Items a line of `len` bytes becomes when each item copies at most `cut` bytes
// (0 for an empty line, which SA:MP skips).
size_t line_items(size_t len, size_t cut) { return (len + cut - 1) / cut; }

} // namespace

std::vector<std::string> fit_list_budgets(const DialogPlan& d, std::vector<std::string> cores) {
    if ((d.style != 2 && d.style != 4 && d.style != 5) || !d.haveBody) return cores;
    const bool cells = (d.style != 2);
    auto core = [&](size_t k) -> const std::string& { return k < cores.size() ? cores[k] : d.units[k].core; };

    size_t first = 3;                            // first unit of the current line (line 0 = style 5's header)
    while (first < d.units.size()) {
        size_t end = first;                      // one past the line's last unit
        while (end < d.units.size()) {
            const size_t sep = end - 3;          // bodySeps[sep] follows unit `end`
            ++end;
            if (sep >= d.bodySeps.size() || d.bodySeps[sep] == '\n') break;
        }
        // The exact bytes SA:MP's reader sees for this line, original vs translated.
        std::string o, t;
        bool cellOver = false;
        for (size_t k = first; k < end; ++k) {
            const std::string oc = unit_text(d.units[k], d.units[k].core);
            const std::string tc = unit_text(d.units[k], core(k));
            if (k > first) { o += '\t'; t += '\t'; }
            o += oc;
            t += tc;
            if (cells && tc.size() > kCellBudget && oc.size() <= kCellBudget) cellOver = true;
        }
        // An original already over the line budget is cut natively too: keep a
        // translation only if it's cut into the SAME number of items (under either
        // reading of the 0x100 copy), else every row below it would shift.
        const bool itemsDiffer = o.size() > kLineBudget &&
            (line_items(t.size(), kLineBudget) != line_items(o.size(), kLineBudget) ||
             line_items(t.size(), kLineBudget + 1) != line_items(o.size(), kLineBudget + 1));
        const bool revert = (blank_line(t) && !blank_line(o)) ||
                            (t.size() > kLineBudget && o.size() <= kLineBudget) ||
                            itemsDiffer || cellOver;
        if (revert)
            for (size_t k = first; k < end && k < cores.size(); ++k) cores[k] = d.units[k].core;
        first = end;
    }
    return cores;
}

std::vector<std::vector<size_t>> group_by_core(const DialogPlan& d, const std::vector<size_t>& missing) {
    std::vector<std::vector<size_t>> groups;
    std::unordered_map<std::string, size_t> groupOf; // core -> index into groups
    for (size_t k : missing) {
        if (k >= d.units.size()) continue;
        auto ins = groupOf.emplace(d.units[k].core, groups.size());
        if (ins.second) groups.emplace_back();
        groups[ins.first->second].push_back(k);
    }
    return groups;
}

std::string strip_colors(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (is_color_code(s, i)) { i += 8; continue; }
        o += s[i++];
    }
    return o;
}

bool remap_list_response(int style, const std::vector<std::string>& orig,
                         const std::vector<std::string>& trans, int listItem,
                         const std::string& resp, std::string& out) {
    if (listItem < 0 || orig.size() != trans.size()) return false;
    const size_t firstDataRow = (style == 5 ? 1u : 0u); // style 5's row 0 is the header, never a list item
    const size_t row = (size_t)listItem + firstDataRow;
    if (row >= orig.size()) return false;
    // SA:MP skips an EMPTY list line (no item) and cuts a line longer than its
    // 255-byte reader into several items (verified in samp.dll's disassembly), so
    // either one before the picked row means listItem no longer names row `row`:
    // refuse to remap rather than send a different row's text.
    for (size_t i = firstDataRow; i < row; ++i)
        if (orig[i].empty() || trans[i].empty() || orig[i].size() > kLineBudget || trans[i].size() > kLineBudget)
            return false;
    auto first_col = [](const std::string& s) {
        size_t t = s.find('\t');
        return t == std::string::npos ? s : s.substr(0, t);
    };
    const std::string& o = orig[row];
    const std::string& t = trans[row];

    // SA:MP's response text is the picked row (LIST) or its FIRST CELL (TABLIST)
    // copied with strncpy(.., 0x80), then colour-stripped. A response at that cap
    // (>= 127 bytes) was CUT -- and holds no complete {RRGGBB} (stripping one would
    // leave it under 120 bytes) -- so mirror the cut: the picked row matches if its
    // selection STARTS WITH resp, and the original's selection gets the same cut,
    // then the same colour strip, exactly as SA:MP would have produced it.
    if (resp.size() >= kNativeRespCut) {
        const std::string selT = (style == 2) ? t : first_col(t);
        const std::string selO = (style == 2) ? o : first_col(o);
        if (selT.size() < resp.size() || selT.compare(0, resp.size(), resp) != 0) return false;
        out = strip_colors(selO.substr(0, resp.size()));
        return true;
    }

    // Below the cap: exact matching (whole row / first column, colour-stripped or raw).
    const std::string candT[4] = { strip_colors(t), strip_colors(first_col(t)), t, first_col(t) };
    const std::string candO[4] = { strip_colors(o), strip_colors(first_col(o)), o, first_col(o) };
    for (int k = 0; k < 4; ++k)
        if (candT[k] == resp) { out = candO[k]; return true; }
    return false;
}

bool should_translate_input(const std::string& text) {
    if (text.empty() || text[0] == '/' || is_dynamic_text(text)) return false;
    return text.find(' ') != std::string::npos || text.find('\t') != std::string::npos;
}

bool never_translated(const DialogPlan& d) {
    return (int16_t)d.id < 0;
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
    DialogPlan d;
    if (!plan_dialog(p, n, d)) return false;
    std::vector<std::string> cores = original_cores(d);
    // Every translatable unit must resolve, or the caller passes the original through.
    for (size_t k = 0; k < d.units.size(); ++k)
        if (d.units[k].translate && !tr(d.units[k].core, cores[k])) return false;
    out = assemble_dialog(d, fit_list_budgets(d, cores));
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
