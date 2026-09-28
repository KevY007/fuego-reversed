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

// Length of a colour code (~r~ ... ~h~, {RRGGBB}) at s[i] that really is one -- not
// a key name right after ~k~ ("~k~~W~") -- else 0.
size_t colour_code_len(const std::string& s, size_t i) {
    if (key_code_before(s, i)) return 0;
    if (is_style_code(s, i)) return 3;
    if (is_color_code(s, i)) return 8;
    return 0;
}

// Split a core at its inner colour codes: `plain` = the core without them (a colour
// change inside a word -- "Evo~R~lve", a gradient's letters -- joins it back),
// `marks` = every stretch that follows a run of codes (the text before the first
// run keeps the line's own colour).
void split_marks(const std::string& core, std::string& plain, std::vector<Mark>& marks) {
    plain.clear();
    marks.clear();
    std::string open, text; // the codes that opened the current stretch ("" = none yet), its text
    for (size_t i = 0; i < core.size();) {
        size_t c = colour_code_len(core, i);
        if (!c) {
            plain += core[i];
            text += core[i++];
            continue;
        }
        std::string codes;
        while (c) {
            codes.append(core, i, c);
            i += c;
            c = colour_code_len(core, i);
        }
        if (!open.empty()) marks.push_back({ open, text, codes });
        open = std::move(codes);
        text.clear();
    }
    if (!open.empty()) marks.push_back({ open, text, std::string() });
}

// A run of colour codes with a word character on both sides: a gradient colouring
// letters of one word ("h{FF1D00}e").
bool code_inside_word(const std::string& core) {
    for (size_t i = 0; i < core.size();) {
        size_t j = i;
        while (size_t c = colour_code_len(core, j)) j += c;
        if (j > i) {
            if (i > 0 && is_word(core[i - 1]) && j < core.size() && is_word(core[j])) return true;
            i = j;
        } else {
            ++i;
        }
    }
    return false;
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
    if (ln.translate) {
        template_numbers(ln.core, ln.key, ln.nums);
        std::string plain;
        split_marks(ln.core, plain, ln.marks);
        if (!ln.marks.empty()) {
            template_numbers(plain, ln.plainKey, ln.plainNums);
            ln.plainOnly = ln.marks.size() > 6 || code_inside_word(ln.core);
        }
    }
    return ln;
}

size_t count_tildes(const std::string& s) { return (size_t)std::count(s.begin(), s.end(), '~'); }

// A line's core without its colour codes (count_tildes of it is what a plain
// translation must have: the key names' tildes).
std::string plain_core(const Line& ln) {
    std::string plain;
    std::vector<Mark> marks;
    split_marks(ln.core, plain, marks);
    return plain;
}

// A stretch's text without its edge spaces.
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Colour a plain translation's stretches again where their text survives verbatim
// -- exactly once, as a whole word (a key name, a command, a player's name). The
// colour after it is the original's -- unless the translation moved things: a
// stretch that ran to the end of the line, or whose next stretch no longer follows
// it, is closed with the line's own colour (`base`: its prefix's codes); with no
// colour of its own known, a stretch that ran to the end stays uncoloured (else
// everything after it would take its colour).
std::string recolour(std::string t, const std::vector<Mark>& marks, const std::string& base) {
    for (size_t mi = 0; mi < marks.size(); ++mi) {
        const Mark& m = marks[mi];
        const std::string w = trimmed(m.text);
        if (w.size() < 2) continue;
        size_t at = std::string::npos;
        int found = 0;
        for (size_t pos = t.find(w); pos != std::string::npos; pos = t.find(w, pos + 1)) {
            const bool left  = pos == 0 || !is_word(t[pos - 1]);
            const bool right = pos + w.size() >= t.size() || !is_word(t[pos + w.size()]);
            if (left && right) { ++found; at = pos; }
        }
        if (found != 1) continue;
        std::string close = m.close;
        if (close.empty()) {
            size_t r = at + w.size();
            while (r < t.size() && !is_word(t[r])) ++r;
            if (r < t.size()) { // words follow it now
                if (base.empty()) continue;
                close = base;
            }
        } else if (!base.empty() && close != base && mi + 1 < marks.size()) {
            // Its close is the NEXT stretch's colour: only right while that stretch
            // still follows it.
            const std::string next = trimmed(marks[mi + 1].text);
            size_t r = at + w.size();
            while (r < t.size() && is_space(t[r])) ++r;
            if (next.empty() || t.compare(r, next.size(), next) != 0) close = base;
        }
        t.insert(at + w.size(), close);
        t.insert(at, m.open);
    }
    return t;
}

// A line's own colour: the codes of its prefix, without the spaces.
std::string base_colour(const Line& ln) {
    std::string b;
    for (char c : ln.prefix)
        if (!is_space(c)) b += c;
    return b;
}

// A translation `tr` of a key for line `ln` -> the core to show, or "" when it
// would break the line: numbers put back, the same number of '~' as `origTildes`
// (codes are restored exactly; one more or less is an unmatched tilde), line
// breaks turned into spaces.
std::string use_translation(const std::string& tr, const std::vector<std::string>& nums, size_t origTildes) {
    std::string t;
    if (!put_numbers(tr, nums, t) || t.empty() || count_tildes(t) != origTildes) return std::string();
    for (char& ch : t)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    return t;
}

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
    auto fetch = [&](const std::string& key) {
        complete = false;
        if (std::find(missing.begin(), missing.end(), key) == missing.end()) missing.push_back(key);
    };
    std::vector<std::string> cores; // each line's core to show: translated, or its own
    cores.reserve(p.lines.size());
    long long total = 0;            // the original text's size
    for (size_t i = 0; i < p.lines.size(); ++i) {
        const Line& ln = p.lines[i];
        total += (long long)(ln.prefix.size() + ln.core.size() + ln.suffix.size());
        if (i < p.seps.size()) total += (long long)p.seps[i].size();
        cores.push_back(ln.core);
        std::string& core = cores.back();
        if (!ln.translate) continue;

        if (ln.plainOnly) { // decoration: only the plain key, recoloured where it can be
            std::string ptr;
            switch (look(ln.plainKey, ptr, false)) {
                case Look::Hit: {
                    std::string t = use_translation(ptr, ln.plainNums, count_tildes(plain_core(ln)));
                    if (!t.empty()) core = recolour(std::move(t), ln.marks, base_colour(ln));
                    break;
                }
                case Look::Keep:
                    break;
                case Look::Later:
                    if (later) *later = true;
                    break;
                case Look::Retry:
                case Look::Miss:
                    fetch(ln.plainKey);
                    break;
            }
            continue;
        }

        std::string tr;
        const Look r = look(ln.key, tr, false);
        if (r == Look::Keep) continue;
        bool failed = false; // the coloured key can't be used: it failed before, or its translation breaks the line
        if (r == Look::Hit) {
            std::string t = use_translation(tr, ln.nums, count_tildes(ln.core));
            if (!t.empty()) { core = std::move(t); continue; }
            failed = true;
        } else if (r == Look::Later) {
            if (later) *later = true;
            failed = true;
        } else {
            fetch(ln.key); // Miss, or a due Retry
            failed = (r == Look::Retry);
        }
        // Not translated in colour: the plain translation if it is in memory; fetched
        // only once the coloured key has failed (at first sight the coloured one goes).
        if (ln.plainKey.empty()) continue;
        std::string ptr;
        const Look pr = look(ln.plainKey, ptr, /*hitOnly=*/!failed);
        if (pr == Look::Hit) {
            std::string t = use_translation(ptr, ln.plainNums, count_tildes(plain_core(ln)));
            if (!t.empty()) core = recolour(std::move(t), ln.marks, base_colour(ln));
        } else if (failed) {
            if (pr == Look::Later) {
                if (later) *later = true;
            } else if (pr != Look::Keep) {
                fetch(ln.plainKey);
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
