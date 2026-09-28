// i18n/hudtext.cpp -- PURE line-by-line HUD / 3D-label translation (see hudtext.h).

#include "hudtext.h"
#include "surfaces.h" // is_dynamic_text (pure)

#include <algorithm>
#include <climits>

namespace i18n::hudtext {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// A byte that belongs to a word: an ASCII letter/digit, '_', or any byte >= 0x80
// (a letter of a non-Latin codepage, e.g. cp1251 Cyrillic).
bool is_word(char c) {
    const unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || is_digit(c) || u == '_' || u >= 0x80;
}

// GTA's colour/style codes: ~r~ ~g~ ~b~ ~w~ ~y~ ~p~ ~l~ ~s~ ~h~ (either case).
// Not ~n~ (a line break), ~k~ (a key name follows) or glyphs like ~u~ / ~>~.
bool is_style_letter(char c) {
    switch (c) {
        case 'r': case 'g': case 'b': case 'w': case 'y': case 'p': case 'l': case 's': case 'h':
        case 'R': case 'G': case 'B': case 'W': case 'Y': case 'P': case 'L': case 'S': case 'H':
            return true;
        default:
            return false;
    }
}

bool is_style_code(const std::string& s, size_t i) {
    return i + 3 <= s.size() && s[i] == '~' && is_style_letter(s[i + 1]) && s[i + 2] == '~';
}

bool is_color_code(const std::string& s, size_t i) {
    if (i + 8 > s.size() || s[i] != '{' || s[i + 7] != '}') return false;
    for (size_t k = i + 1; k < i + 7; ++k)
        if (!is_hex(s[k])) return false;
    return true;
}

// "~k~" / "~K~" ends right before s[i]: the ~...~ at s[i] is then a KEY NAME
// ("~k~~W~" is the key bound to "W"), never a colour code.
bool key_code_before(const std::string& s, size_t i) {
    return i >= 3 && s[i - 3] == '~' && (s[i - 2] == 'k' || s[i - 2] == 'K') && s[i - 1] == '~';
}

// A code copied through verbatim when scanning for numbers: {RRGGBB}, or any
// ~...~ (colour, key name, glyph) of up to 32 bytes. Its length at s[i], else 0.
size_t code_len(const std::string& s, size_t i) {
    if (is_color_code(s, i)) return 8;
    if (s[i] == '~') {
        const size_t j = s.find('~', i + 1);
        if (j != std::string::npos && j - i <= 32) return j + 1 - i;
    }
    return 0;
}

// A digit at s[i] that starts a whole number: not the tail of a word ("AK47").
bool number_start(const std::string& s, size_t i) {
    return is_digit(s[i]) && (i == 0 || !is_word(s[i - 1]));
}

std::string stand_in(size_t k) { return std::string(1, (char)('2' + k)); }

// An ordinal suffix ("st", "nd", "rd", "th", either case) right after the digits
// ending at s[j], not followed by another letter ("2nd", "21st", but not "4thly").
bool ordinal_at(const std::string& s, size_t j) {
    if (j + 2 > s.size()) return false;
    const char a = (char)(s[j] | 0x20), b = (char)(s[j + 1] | 0x20);
    const bool suffix = (a == 's' && b == 't') || (a == 'n' && b == 'd') || (a == 'r' && b == 'd') ||
                        (a == 't' && b == 'h');
    if (!suffix) return false;
    const unsigned char next = (j + 2 < s.size()) ? (unsigned char)s[j + 2] : 0;
    return !((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'));
}

// core -> key (numbers replaced by stand-ins "2", "3", ...) + the numbers.
void template_numbers(const std::string& core, std::string& key, std::vector<std::string>& nums) {
    key.clear();
    nums.clear();
    for (size_t i = 0; i < core.size();) {
        const size_t c = code_len(core, i);
        if (c) {
            key.append(core, i, c);
            i += c;
            continue;
        }
        if (number_start(core, i)) {
            size_t j = i;
            while (j < core.size() && is_digit(core[j])) ++j;
            if (nums.size() == kMaxNumbers || ordinal_at(core, j)) { // this line keeps its numbers
                key = core;
                nums.clear();
                return;
            }
            nums.push_back(core.substr(i, j - i));
            key += stand_in(nums.size() - 1);
            i = j;
            continue;
        }
        key += core[i++];
    }
}

Line make_line(const std::string& raw) {
    Line ln;
    size_t a = 0;
    for (;;) {
        if (a < raw.size() && is_space(raw[a])) { ++a; continue; }
        if (is_style_code(raw, a)) { a += 3; continue; }
        if (is_color_code(raw, a)) { a += 8; continue; }
        break;
    }
    size_t b = raw.size();
    for (;;) {
        if (b > a && is_space(raw[b - 1])) { --b; continue; }
        if (b >= a + 3 && is_style_code(raw, b - 3) && !key_code_before(raw, b - 3)) { b -= 3; continue; }
        if (b >= a + 8 && is_color_code(raw, b - 8)) { b -= 8; continue; }
        break;
    }
    ln.prefix = raw.substr(0, a);
    ln.core   = raw.substr(a, b - a);
    ln.suffix = raw.substr(b);
    ln.translate = ln.core.size() >= 2 && !i18n::surfaces::is_dynamic_text(ln.core);
    if (ln.translate) template_numbers(ln.core, ln.key, ln.nums);
    return ln;
}

size_t count_tildes(const std::string& s) { return (size_t)std::count(s.begin(), s.end(), '~'); }

} // namespace

Plan plan_text(const std::string& text, bool label) {
    Plan p;
    size_t start = 0;
    for (size_t i = 0; i < text.size();) {
        size_t sep = 0;
        if (label) {
            if (text[i] == '\n') sep = 1;
        } else if (i + 3 <= text.size() && text[i] == '~' && (text[i + 1] == 'n' || text[i + 1] == 'N') &&
                   text[i + 2] == '~') {
            sep = 3;
        }
        if (!sep) { ++i; continue; }
        p.lines.push_back(make_line(text.substr(start, i - start)));
        p.seps.push_back(text.substr(i, sep));
        i += sep;
        start = i;
    }
    p.lines.push_back(make_line(text.substr(start)));
    return p;
}

bool is_sprite_name(const std::string& text) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    if (text.find(':', colon + 1) != std::string::npos) return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' ||
                        c == '-' || c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

bool put_numbers(const std::string& t, const std::vector<std::string>& nums, std::string& out) {
    out.clear();
    std::vector<int> seen(nums.size(), 0);
    for (size_t i = 0; i < t.size();) {
        const size_t c = code_len(t, i);
        if (c) {
            out.append(t, i, c);
            i += c;
            continue;
        }
        if (number_start(t, i)) {
            size_t j = i;
            while (j < t.size() && is_digit(t[j])) ++j;
            const std::string run = t.substr(i, j - i);
            const size_t k = (run.size() == 1) ? (size_t)(run[0] - '2') : nums.size(); // "2" -> 0, "3" -> 1, ...
            if (k < nums.size()) {
                if (seen[k]++) return false; // a stand-in twice
                out += nums[k];
            } else {
                out += run;                  // a number of the translation's own
            }
            i = j;
            continue;
        }
        out += t[i++];
    }
    for (int s : seen)
        if (s != 1) return false;            // a stand-in lost
    return true;
}

bool resolve(const Plan& p, const Lookup& look, std::string& out, std::vector<std::string>& missing,
             bool* later, size_t limit) {
    missing.clear();
    if (later) *later = false;
    bool complete = true;
    std::vector<std::string> cores; // each line's core to show: translated, or its own
    cores.reserve(p.lines.size());
    long long total = 0;            // the original text's size
    for (size_t i = 0; i < p.lines.size(); ++i) {
        const Line& ln = p.lines[i];
        total += (long long)(ln.prefix.size() + ln.core.size() + ln.suffix.size());
        if (i < p.seps.size()) total += (long long)p.seps[i].size();
        cores.push_back(ln.core);
        std::string& core = cores.back();
        if (ln.translate) {
            std::string tr;
            switch (look(ln.key, tr)) {
                case Look::Hit: {
                    std::string t;
                    if (put_numbers(tr, ln.nums, t) && !t.empty() && count_tildes(t) == count_tildes(ln.core)) {
                        for (char& ch : t)
                            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                        core = std::move(t);
                    }
                    break;
                }
                case Look::Keep:
                    break;
                case Look::Later:
                    if (later) *later = true;
                    break;
                case Look::Miss:
                    complete = false;
                    if (std::find(missing.begin(), missing.end(), ln.key) == missing.end()) missing.push_back(ln.key);
                    break;
            }
        }
    }
    // Fit `limit`: translated lines in order, each only if the text still fits.
    for (size_t i = 0; i < cores.size(); ++i) {
        if (cores[i] == p.lines[i].core) continue;
        const long long d = (long long)cores[i].size() - (long long)p.lines[i].core.size();
        if (total + d <= (long long)(limit > (size_t)LLONG_MAX ? LLONG_MAX : limit)) total += d;
        else cores[i] = p.lines[i].core;
    }
    std::string o;
    o.reserve((size_t)(total > 0 ? total : 0));
    for (size_t i = 0; i < p.lines.size(); ++i) {
        o += p.lines[i].prefix;
        o += cores[i];
        o += p.lines[i].suffix;
        if (i < p.seps.size()) o += p.seps[i];
    }
    out = std::move(o);
    return complete;
}

} // namespace i18n::hudtext
