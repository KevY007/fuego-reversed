#include "i18n/protect.h"
#include "i18n/glossary_data.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_set>

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

std::mutex g_namesMtx;
std::shared_ptr<const std::vector<std::string>> g_names =
    std::make_shared<const std::vector<std::string>>();

std::shared_ptr<const std::vector<std::string>> names_snapshot() {
    std::lock_guard<std::mutex> lk(g_namesMtx);
    return g_names;
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (is_upper(c)) c = char(c - 'A' + 'a');
    return s;
}

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

// An ordinal suffix ("st", "nd", "rd", "th", either case) at s[j], right after a
// number, not followed by another letter: "2nd", "21st" -- but not "4thly".
bool ordinal_at(const std::string& s, size_t j) {
    if (j + 2 > s.size()) return false;
    const char a = (char)(s[j] | 0x20), b = (char)(s[j + 1] | 0x20); // (only 'S'/'s' fold to 's', etc.)
    const bool suffix = (a == 's' && b == 't') || (a == 'n' && b == 'd') || (a == 'r' && b == 'd') ||
                        (a == 't' && b == 'h');
    return suffix && (j + 2 == s.size() || !is_alpha(s[j + 2]));
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

// ---- Glossary canonicalisation ---------------------------------------------
// Case-insensitive, whole-word rewrite of known GTA:SA proper nouns (vehicle
// model names, San Andreas place names) back to canonical spelling. ASCII-only:
// to_lower() only touches 'A'-'Z', so non-ASCII bytes compare unequal to
// anything and are always copied through verbatim by the caller.
char to_lower(unsigned char c) {
    return is_upper(char(c)) ? char(c - 'A' + 'a') : char(c);
}

// Canonical terms bucketed by lowercase first letter, each bucket sorted by
// descending length. Trying candidates longest-first within a bucket means a
// left-to-right scan naturally prefers the longest term that matches at a
// given position (e.g. "Los Santos International" over "Los Santos").
struct GlossaryIndex {
    std::vector<const std::string*> buckets[26];
};

const GlossaryIndex& glossary_index() {
    static const GlossaryIndex index = [] {
        GlossaryIndex built;
        for (const std::string& term : glossary_terms()) {
            if (term.empty()) continue;
            char lower = to_lower(static_cast<unsigned char>(term[0]));
            if (lower < 'a' || lower > 'z') continue;
            built.buckets[static_cast<size_t>(lower - 'a')].push_back(&term);
        }
        for (auto& bucket : built.buckets) {
            std::sort(bucket.begin(), bucket.end(),
                      [](const std::string* a, const std::string* b) {
                          return a->size() > b->size();
                      });
        }
        return built;
    }();
    return index;
}

// Case-insensitive ASCII comparison of text[pos, pos+term.size()) against
// term. False (without reading out of bounds) if term doesn't fit at pos.
bool match_ci_term(const std::string& text, size_t pos, const std::string& term) {
    const size_t len = term.size();
    if (pos + len > text.size()) return false;
    for (size_t k = 0; k < len; ++k) {
        if (to_lower(static_cast<unsigned char>(text[pos + k])) !=
            to_lower(static_cast<unsigned char>(term[k]))) {
            return false;
        }
    }
    return true;
}

// text[start, end) equals one of `words` (lowercase), ignoring ASCII case.
bool is_one_of(const std::string& text, size_t start, size_t end, const char* const* words, size_t count) {
    for (size_t w = 0; w < count; ++w) {
        const char* k = words[w];
        size_t q = 0;
        while (k[q] && start + q < end && to_lower(static_cast<unsigned char>(text[start + q])) == k[q]) ++q;
        if (!k[q] && start + q == end) return true;
    }
    return false;
}

// Commands whose words are a MESSAGE, not arguments -- roleplay, chat, reports:
// "/me smiles warmly", "/pm 12 see you there", "/report 5 cheating". Their words
// are prose and get translated; only the command itself is kept.
bool message_command(const std::string& s, size_t start, size_t end) {
    static const char* const kCmds[] = {
        "me", "do", "my", "ame", "ado", "b", "o", "ooc", "l", "low", "s", "shout", "w", "whisper", "cw", "t",
        "say", "yell", "pm", "r", "reply", "sms", "text", "call", "radio", "f", "fam", "d", "dept", "gov",
        "m", "megaphone", "news", "ad", "advert", "ask", "report", "a", "g", "n", "newb", "newbie", "try", "rp",
    };
    return is_one_of(s, start, end, kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}

// Little words that end a command's arguments: in "use /gps to find it" it has none,
// in "use /gps atm then go south" it has "atm".
bool arg_stopword(const std::string& s, size_t start, size_t end) {
    static const char* const kWords[] = {
        "a", "an", "the", "to", "for", "and", "or", "but", "nor", "so", "yet", "if", "then", "than", "that",
        "this", "these", "those", "there", "here", "it", "its", "is", "are", "was", "were", "be", "been", "am",
        "do", "does", "did", "have", "has", "had", "will", "would", "can", "could", "shall", "should", "may",
        "might", "must", "not", "no", "yes", "of", "in", "on", "at", "by", "with", "from", "into", "onto", "over",
        "under", "near", "via", "about", "after", "before", "until", "while", "when", "where", "which", "who",
        "what", "why", "how", "all", "any", "each", "every", "some", "more", "most", "again", "also", "just",
        "only", "even", "still", "too", "very", "now", "soon", "later", "first", "next", "once", "please", "pls",
        "plz", "thanks", "thx", "ty", "ok", "okay", "lol", "lmao", "xd", "bro", "man", "dude", "u", "ur", "you",
        "your", "me", "my", "i", "we", "us", "our", "they", "them", "their", "he", "him", "his", "she", "her",
        "command", "cmd", "instead", "like", "etc", "find", "get", "go", "see", "check", "look", "know", "learn",
        "read",
    };
    return is_one_of(s, start, end, kWords, sizeof(kWords) / sizeof(kWords[0]));
}

// The end of one command argument starting at s[p], or p when there is none: a
// word of ASCII letters/digits/'_' (a '-' or '.' inside it: "los-santos", "1.5"),
// or a bracketed name ("[playerid]", "<amount>"). A word running into a non-ASCII
// letter is prose of another language, not an argument.
size_t scan_arg(const std::string& s, size_t p) {
    const size_t n = s.size();
    if (p < n && (s[p] == '[' || s[p] == '<')) {
        const char close = (s[p] == '[') ? ']' : '>';
        size_t q = p + 1;
        while (q < n && q - p <= 24 && (is_word_char(s[q]) || s[q] == ' ' || s[q] == '-')) ++q;
        return (q < n && s[q] == close && q > p + 1) ? q + 1 : p;
    }
    if (p >= n || !is_word_char(s[p])) return p;
    size_t q = p + 1;
    while (q < n && (is_word_char(s[q]) || ((s[q] == '-' || s[q] == '.') && q + 1 < n && is_alnum(s[q + 1])))) ++q;
    if (q < n && static_cast<unsigned char>(s[q]) >= 0x80) return p;
    return q;
}

} // namespace

Masked mask(const std::string& src, bool labels) {
    Masked out;
    out.translatable = false;
    const size_t n = src.size();
    size_t i = 0;
    const auto names = names_snapshot();

    auto emit = [&](size_t start, size_t end) {
        out.tokens.push_back(src.substr(start, end - start));
        out.text += make_placeholder(out.tokens.size() - 1);
    };

    while (i < n) {
        const char c = src[i];
        bool matched = false;

        // "/command" at a word boundary (so "w/o" mid-sentence isn't one) -- which
        // includes the end of a colour code and an opening bracket or quote: HUD text
        // colours its commands ("Use ~Y~/gps~W~ for help", "(~Y~/scrap~W~ for more
        // info)"). Its ARGUMENTS go with it -- they are typed words ("use /gps atm"
        // came back as "/gps en el cajero automático") -- up to three, each after one
        // space: a word, a number or a "[name]"; a little word ("to", "for"),
        // punctuation, a code, another command or a non-English word ends them. Not
        // for a message command (/me, /do, /pm, /report, ...): its words are prose
        // that MUST still be translated -- "/me smiles warmly" keeps only "/me".
        if (c == '/' && i + 1 < n && is_alpha(src[i + 1]) &&
            (i == 0 || is_space(src[i - 1]) || src[i - 1] == '~' || src[i - 1] == '}' || src[i - 1] == '(' ||
             src[i - 1] == '"' || src[i - 1] == '\'')) {
            size_t j = i + 1;
            while (j < n && is_word_char(src[j])) ++j;
            size_t end = j;
            if (!message_command(src, i + 1, j)) {
                for (int args = 0; args < 3 && end < n && src[end] == ' '; ++args) {
                    const size_t p = end + 1;
                    const size_t q = scan_arg(src, p);
                    if (q == p || arg_stopword(src, p, q)) break;
                    end = q;
                }
            }
            emit(i, end);
            i = end;
            matched = true;
        }

        // Live player names (set_names): whole word, case-insensitive, longest first.
        if (!matched && !names->empty() && (i == 0 || !is_word_char(src[i - 1]))) {
            for (const std::string& nm : *names) {
                size_t len = match_ci_literal(src, i, nm.c_str());
                if (len == 0) continue;
                if (i + len < n && is_word_char(src[i + len])) continue; // "Khalidx" isn't "Khalid"
                emit(i, i + len);
                i += len;
                matched = true;
                break;
            }
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
            } else if (!labels) { // "(ID) Name" -- in a UI label "(3) Police" is a count
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
                // "Name (ID)" -- in a UI label "Civilian (8)" is a count, not a player.
                if (!matched && !labels && nameEnd + 1 < n && src[nameEnd] == ' ' && src[nameEnd + 1] == '(') {
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
                    // (a UI label's number may be glued to its unit: "2m", "999hp";
                    // an ordinal -- "2nd" -- stays a word the model can localize)
                    if (j >= n || !(is_alpha(src[j]) || src[j] == '_') || (labels && !ordinal_at(src, j))) {
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
    // Canonical Latin spellings don't depend on the target language, so
    // tgtLang is unused -- one table serves every language.
    const GlossaryIndex& index = glossary_index();
    const size_t n = text.size();
    std::string out;
    out.reserve(n);

    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const char lower = to_lower(c);
        bool matched = false;

        if (lower >= 'a' && lower <= 'z' && (i == 0 || !is_alnum(text[i - 1]))) {
            for (const std::string* termPtr : index.buckets[static_cast<size_t>(lower - 'a')]) {
                const std::string& term = *termPtr;
                const size_t end = i + term.size();
                if (end > n || !match_ci_term(text, i, term)) continue;
                if (end < n && is_alnum(text[end])) continue; // not a whole-word match
                out += term; // rewrite to canonical spelling
                i = end;
                matched = true;
                break;
            }
        }

        if (!matched) {
            out.push_back(text[i]);
            ++i;
        }
    }

    return out;
}

void set_names(std::vector<std::string> terms) {
    std::stable_sort(terms.begin(), terms.end(),
                     [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
    auto snap = std::make_shared<const std::vector<std::string>>(std::move(terms));
    std::lock_guard<std::mutex> lk(g_namesMtx);
    g_names = std::move(snap);
}

std::vector<std::string> name_variants(const std::vector<std::string>& names) {
    // Common words that also show up as parts of RP names ("John_Black", "The_Man"):
    // protecting them alone would freeze ordinary chat words, so such a part is only
    // protected inside its full name.
    static const std::unordered_set<std::string> kCommon = {
        "the", "and", "you", "are", "for", "not", "but", "all", "any", "can", "had", "her", "was",
        "one", "our", "out", "day", "get", "has", "him", "his", "how", "man", "men", "new", "now",
        "old", "see", "two", "way", "who", "boy", "did", "its", "let", "put", "say", "she", "too",
        "use", "big", "bad", "car", "cop", "gun", "god", "sir", "king", "lord", "lady", "miss",
        "mister", "black", "white", "brown", "green", "gray", "grey", "red", "blue", "gold",
        "silver", "young", "long", "short", "little", "hunter", "baker", "cook", "smith", "will",
        "may", "june", "mark", "hope", "rose", "grace", "faith", "joy", "sky", "star", "stone",
        "wood", "woods", "hill", "hills", "lake", "rich", "love", "dead", "death", "ghost", "snake",
        "wolf", "fox", "bear", "lion", "hawk", "eagle", "bird", "fish", "ice", "fire", "storm",
        "rain", "snow", "frost", "night", "dark", "light", "shadow", "angel", "devil", "bishop",
        "knight", "cross", "church", "street", "park", "west", "east", "north", "south", "money",
        "cash", "boss", "chief", "doctor", "officer", "police"
    };
    std::vector<std::string> out;
    std::unordered_set<std::string> seen; // lower-cased, for case-insensitive dedupe
    auto add = [&](const std::string& s) {
        if (s.size() < 3) return;
        if (seen.insert(lower_ascii(s)).second) out.push_back(s);
    };
    auto all_letters = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s)
            if (!is_alpha(c)) return false;
        return true;
    };
    for (const std::string& nm : names) {
        add(nm);
        if (nm.find('_') == std::string::npos) continue;
        std::string spaced = nm;
        std::replace(spaced.begin(), spaced.end(), '_', ' ');
        add(spaced);
        size_t a = 0;
        while (a <= nm.size()) {
            size_t b = nm.find('_', a);
            if (b == std::string::npos) b = nm.size();
            std::string part = nm.substr(a, b - a);
            if (all_letters(part) && !kCommon.count(lower_ascii(part))) add(part);
            a = b + 1;
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
    return out;
}

} // namespace i18n::protect
