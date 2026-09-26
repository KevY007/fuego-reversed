#include "i18n/protect.h"

namespace i18n::protect {

namespace {

// ---- ASCII classification -------------------------------------------------
// Hand-rolled instead of <cctype> to avoid signed-char/locale surprises;
// everything this scanner cares about (commands, colour codes, coordinates,
// URLs) is plain ASCII, and non-ASCII bytes just fall through as literal
// (translatable) text, which is what we want.
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_alpha(char c) { return is_upper(c) || is_lower(c); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
bool is_hex(char c) { return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_word_char(char c) { return is_alnum(c) || c == '_'; }

std::string make_placeholder(size_t k) {
    std::string s;
    s.push_back('\x01');
    s += std::to_string(k);
    s.push_back('\x02');
    return s;
}

// Scans a "Name" token: an uppercase letter followed by letters/digits.
// Returns the end index (== start when there's no match).
size_t scan_name(const std::string& s, size_t start) {
    if (start >= s.size() || !is_upper(s[start])) return start;
    size_t i = start + 1;
    while (i < s.size() && is_alnum(s[i])) ++i;
    return i;
}

// Scans a run of ASCII digits. Returns the end index (== start when none).
size_t scan_digits(const std::string& s, size_t start) {
    size_t i = start;
    while (i < s.size() && is_digit(s[i])) ++i;
    return i;
}

// A short ALLCAPS+digits token (no lowercase) with at least one of each, e.g.
// a vehicle plate like "ABC1234". Plate formats aren't standardised in SA:MP,
// so this only recognises the visually-obvious "code, not a word" shape.
bool looks_like_plate(const std::string& s, size_t start, size_t end) {
    if (end <= start || end - start > 8) return false;
    bool hasDigit = false, hasAlpha = false;
    for (size_t i = start; i < end; ++i) {
        if (is_lower(s[i])) return false;
        if (is_digit(s[i])) hasDigit = true;
        else if (is_upper(s[i])) hasAlpha = true;
        else return false;
    }
    return hasDigit && hasAlpha;
}

// Case-insensitive match of a NUL-terminated ASCII literal against s starting
// at pos. Returns the literal's length on a full match, 0 otherwise (0 is
// unambiguous: every verb literal used below is non-empty).
size_t match_ci_literal(const std::string& s, size_t pos, const char* lit) {
    size_t i = 0;
    for (; lit[i] != '\0'; ++i) {
        size_t sp = pos + i;
        if (sp >= s.size()) return 0;
        char a = s[sp];
        char b = lit[i];
        if (is_upper(a)) a = char(a - 'A' + 'a');
        if (is_upper(b)) b = char(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return i;
}

} // namespace

Masked mask(const std::string& src) {
    Masked out;
    out.translatable = false;
    const size_t n = src.size();
    size_t i = 0;

    auto emit = [&](size_t start, size_t end) {
        out.tokens.push_back(src.substr(start, end - start));
        out.text += make_placeholder(out.tokens.size() - 1);
    };

    while (i < n) {
        const char c = src[i];
        bool matched = false;

        // Leading "/command" only (only at a word boundary, so "w/o"
        // mid-sentence isn't mistaken for one). Deliberately does NOT swallow
        // following words: roleplay commands like /me, /do, /b, /ooc, /low,
        // /s, /w carry narrative text as their "argument" that MUST still be
        // translated, e.g. "/me smiles warmly" -- only "/me" is data, "smiles
        // warmly" is prose. A following {colour}/~x~/number arg is already
        // masked by its own rule below; a following plain word is simply left
        // translatable, which is the safe default.
        if (c == '/' && i + 1 < n && is_alpha(src[i + 1]) &&
            (i == 0 || is_space(src[i - 1]))) {
            size_t j = i + 1;
            while (j < n && is_word_char(src[j])) ++j;
            emit(i, j);
            i = j;
            matched = true;
        }

        // {RRGGBB} / {AARRGGBB} colour embed.
        if (!matched && c == '{') {
            size_t j = i + 1;
            while (j < n && is_hex(src[j])) ++j;
            size_t hexLen = j - (i + 1);
            if ((hexLen == 6 || hexLen == 8) && j < n && src[j] == '}') {
                emit(i, j + 1);
                i = j + 1;
                matched = true;
            }
        }

        // "~k~~KEY~" key-icon embed, else plain "~x~" colour/format embed.
        if (!matched && c == '~') {
            if (i + 4 <= n && src.compare(i, 4, "~k~~") == 0) {
                size_t j = i + 4;
                size_t keyStart = j;
                while (j < n && (is_alnum(src[j]) || src[j] == '_')) ++j;
                if (j > keyStart && j < n && src[j] == '~') {
                    emit(i, j + 1);
                    i = j + 1;
                    matched = true;
                }
            }
            if (!matched && i + 2 < n && src[i + 2] == '~') {
                emit(i, i + 3);
                i += 3;
                matched = true;
            }
        }

        // '(' family: "(radio)" marker, "((" OOC wrapper glyph, "(ID) Name".
        if (!matched && c == '(') {
            if (i + 7 <= n && src.compare(i, 7, "(radio)") == 0) {
                emit(i, i + 7);
                i += 7;
                matched = true;
            } else if (i + 1 < n && src[i + 1] == '(') {
                emit(i, i + 2);
                i += 2;
                matched = true;
            } else {
                size_t j = scan_digits(src, i + 1);
                if (j > i + 1 && j < n && src[j] == ')' && j + 1 < n && src[j + 1] == ' ') {
                    size_t nameEnd = scan_name(src, j + 2);
                    if (nameEnd > j + 2) {
                        emit(i, nameEnd);
                        i = nameEnd;
                        matched = true;
                    }
                }
            }
        }

        // "))" OOC wrapper glyph (close side).
        if (!matched && c == ')' && i + 1 < n && src[i + 1] == ')') {
            emit(i, i + 2);
            i += 2;
            matched = true;
        }

        // "**" emote/emphasis wrapper glyph.
        if (!matched && c == '*' && i + 1 < n && src[i + 1] == '*') {
            emit(i, i + 2);
            i += 2;
            matched = true;
        }

        // Name_Surname / Name[ID] / Name (ID) / leading "Name says:"-style
        // speech-verb chat prefix / ALLCAPS+digit plate-ish code.
        if (!matched && is_upper(c)) {
            size_t nameEnd = scan_name(src, i);
            if (nameEnd > i) {
                if (nameEnd < n && src[nameEnd] == '[') {
                    size_t j = scan_digits(src, nameEnd + 1);
                    if (j > nameEnd + 1 && j < n && src[j] == ']') {
                        emit(i, j + 1);
                        i = j + 1;
                        matched = true;
                    }
                }
                if (!matched && nameEnd + 1 < n && src[nameEnd] == ' ' && src[nameEnd + 1] == '(') {
                    size_t j = scan_digits(src, nameEnd + 2);
                    if (j > nameEnd + 2 && j < n && src[j] == ')') {
                        emit(i, j + 1);
                        i = j + 1;
                        matched = true;
                    }
                }
                if (!matched && nameEnd < n && src[nameEnd] == '_') {
                    size_t surnameEnd = scan_name(src, nameEnd + 1);
                    if (surnameEnd > nameEnd + 1) {
                        emit(i, surnameEnd);
                        i = surnameEnd;
                        matched = true;
                    }
                }
                // Chat speaker prefix ("Kevin says: ..." / "Kevin shouts:
                // ...") is only recognised at the very start of the message,
                // and only when a known speech verb (optionally with a
                // bracketed qualifier like "[low]") leads into a colon --
                // NOT on a bare "Name:". That distinction matters: a plain
                // lead word followed by ':' is extremely common as ordinary
                // prose ("Warning: server restarting", "Note:", "Server:",
                // "Error:", "Tip:") and must stay translatable, whereas a
                // verb-qualified prefix is specifically how this game's chat
                // narrates who is speaking.
                if (!matched && i == 0 && nameEnd < n && src[nameEnd] == ' ') {
                    static const char* kSpeechVerbs[] = {
                        "says", "shouts", "whispers", "yells", "exclaims", "asks"
                    };
                    for (const char* verb : kSpeechVerbs) {
                        size_t verbLen = match_ci_literal(src, nameEnd + 1, verb);
                        if (verbLen == 0) continue;
                        size_t p = nameEnd + 1 + verbLen;
                        if (p < n && is_alpha(src[p])) continue; // e.g. "sayshi" isn't the verb "says"
                        // optional bracketed qualifier before the colon, e.g. "says [low]:"
                        if (p + 1 < n && src[p] == ' ' && src[p + 1] == '[') {
                            size_t q = p + 2;
                            while (q < n && src[q] != ']' && src[q] != '\n') ++q;
                            if (q < n && src[q] == ']') p = q + 1;
                        }
                        if (p < n && src[p] == ':') {
                            emit(i, nameEnd);
                            i = nameEnd;
                            matched = true;
                        }
                        break; // the verb text matched (or didn't); no other verb can start here
                    }
                }
                if (!matched && looks_like_plate(src, i, nameEnd)) {
                    emit(i, nameEnd);
                    i = nameEnd;
                    matched = true;
                }
            }
        }

        // Generic bracket wrapper glyphs (Name[ID] is already consumed above
        // by the time we'd get here, so a bare '[' means it wasn't one).
        if (!matched && (c == '[' || c == ']')) {
            emit(i, i + 1);
            i += 1;
            matched = true;
        }

        // http:// and https:// URLs, consumed up to the next whitespace.
        if (!matched && c == 'h') {
            size_t schemeLen = 0;
            if (i + 8 <= n && src.compare(i, 8, "https://") == 0) schemeLen = 8;
            else if (i + 7 <= n && src.compare(i, 7, "http://") == 0) schemeLen = 7;
            if (schemeLen) {
                size_t j = i + schemeLen;
                while (j < n && !is_space(src[j])) ++j;
                emit(i, j);
                i = j;
                matched = true;
            }
        }

        // "$1234" / standalone integers / simple signed coordinates like
        // "-12.5". Requires word boundaries on both sides so this doesn't
        // chop a digit suffix off a glued token like "AK47" or "24/7".
        if (!matched && (c == '$' || is_digit(c) || (c == '-' && i + 1 < n && is_digit(src[i + 1])))) {
            bool leadBoundary = (i == 0) || !(is_alnum(src[i - 1]) || src[i - 1] == '_');
            if (leadBoundary) {
                size_t j = i;
                if (src[j] == '$' || src[j] == '-') ++j;
                size_t digitsStart = j;
                j = scan_digits(src, j);
                if (j > digitsStart) {
                    if (j < n && src[j] == '.') {
                        size_t k = scan_digits(src, j + 1);
                        if (k > j + 1) j = k;
                    }
                    if (j >= n || !(is_alpha(src[j]) || src[j] == '_')) {
                        emit(i, j);
                        i = j;
                        matched = true;
                    }
                }
            }
        }

        if (!matched) {
            if (!is_space(c)) out.translatable = true;
            out.text.push_back(c);
            ++i;
        }
    }

    return out;
}

bool placeholders_intact(const std::string& translated, size_t n) {
    for (size_t k = 0; k < n; ++k) {
        const std::string ph = make_placeholder(k);
        size_t count = 0;
        size_t pos = 0;
        while ((pos = translated.find(ph, pos)) != std::string::npos) {
            ++count;
            pos += ph.size();
        }
        if (count != 1) return false;
    }
    // Reject any well-formed "\x01<digits>\x02" placeholder whose index is
    // out of range too -- a model can hallucinate/corrupt an index that was
    // never handed out, and that must fail just like a missing one does, so
    // unmask() (which gates on this function) never has to decide what to do
    // with a token that doesn't exist.
    for (size_t i = 0; i < translated.size(); ) {
        if (translated[i] == '\x01') {
            size_t j = i + 1;
            size_t k = 0;
            bool anyDigit = false;
            while (j < translated.size() && is_digit(translated[j])) {
                k = k * 10 + static_cast<size_t>(translated[j] - '0');
                ++j;
                anyDigit = true;
            }
            if (anyDigit && j < translated.size() && translated[j] == '\x02') {
                if (k >= n) return false;
                i = j + 1;
                continue;
            }
        }
        ++i;
    }
    return true;
}

std::string unmask(const std::string& translated, const std::vector<std::string>& tokens) {
    if (!placeholders_intact(translated, tokens.size())) return std::string();

    std::string out;
    out.reserve(translated.size());
    const size_t n = translated.size();
    size_t i = 0;
    while (i < n) {
        if (translated[i] == '\x01') {
            size_t j = i + 1;
            size_t k = 0;
            bool anyDigit = false;
            while (j < n && is_digit(translated[j])) {
                k = k * 10 + static_cast<size_t>(translated[j] - '0');
                ++j;
                anyDigit = true;
            }
            if (anyDigit && j < n && translated[j] == '\x02') {
                // Well-formed placeholder syntax. placeholders_intact() above
                // already guarantees k < tokens.size(); this check is defence
                // in depth so an out-of-range index can never fall through to
                // the raw-byte copy below and leak \x01/\x02 into the output.
                if (k >= tokens.size()) return std::string();
                out += tokens[k];
                i = j + 1;
                continue;
            }
        }
        out.push_back(translated[i]);
        ++i;
    }
    return out;
}

std::string apply_glossary(const std::string& text, int /*tgtLang*/) {
    // Phase 0 stub: no glossary table yet (lands in Phase 3+). Identity passthrough.
    return text;
}

} // namespace i18n::protect
