#include "detect.h"
#include <cwctype>
#include <algorithm>
#include <cctype>

namespace i18n::detect {

Script script_of(const std::wstring& text) {
    int latin_count = 0, cyrillic_count = 0, other_count = 0;

    for (wchar_t ch : text) {
        // Latin: A-Z, a-z, and other Latin extended characters
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) {
            latin_count++;
        }
        // Cyrillic: U+0400 to U+04FF
        else if (ch >= 0x0400 && ch <= 0x04FF) {
            cyrillic_count++;
        }
        // Other letter-like characters
        else if (std::iswalpha(ch)) {
            other_count++;
        }
    }

    if (cyrillic_count > 0 && cyrillic_count >= latin_count && cyrillic_count >= other_count) {
        return Script::Cyrillic;
    } else if (latin_count > 0 && latin_count >= cyrillic_count && latin_count >= other_count) {
        return Script::Latin;
    } else {
        return Script::Other;
    }
}

bool is_probably_english(const std::wstring& text) {
    // Must be Latin script
    if (script_of(text) != Script::Latin) {
        return false;
    }

    // Must be mostly ASCII
    int ascii_count = 0, non_ascii_count = 0;
    for (wchar_t ch : text) {
        if (ch < 128) {
            ascii_count++;
        } else {
            non_ascii_count++;
        }
    }

    if (non_ascii_count > 0 && ascii_count < non_ascii_count * 2) {
        // Too many non-ASCII characters
        return false;
    }

    // Convert to lowercase for comparison
    std::wstring lower_text;
    for (wchar_t ch : text) {
        lower_text += std::towlower(ch);
    }

    // Common English stopwords
    const char* stopwords[] = {
        "the", "you", "is", "to", "and", "a", "i", "in", "that", "it",
        "of", "for", "on", "are", "as", "was", "be", "this", "by", "or",
        "an", "with", "at", "from", "he", "she", "we", "they", "what",
        "which", "who", "when", "where", "why", "how", "can", "will",
        "would", "could", "should", "may", "do", "does", "did", "have",
        "has", "had", "but", "not", "no", "yes", "if", "then", "else",
        "all", "each", "every", "both", "neither", "nor", "some", "most",
        "any", "many", "few", "more", "much", "get", "go", "make", "know",
        "think", "see", "say", "find", "give", "tell", "ask", "work",
        "need", "try", "use", "help", "turn", "show", "hear", "let",
        "mean", "leave", "seem", "feel", "want", "meet", "call", "talk"
    };

    // Check for stopwords
    int stopword_hits = 0;
    for (const char* word : stopwords) {
        std::wstring wword;
        for (char c : std::string(word)) {
            wword += (wchar_t)c;
        }

        // Simple word boundary check
        size_t pos = 0;
        while ((pos = lower_text.find(wword, pos)) != std::wstring::npos) {
            // Check word boundaries
            bool start_ok = (pos == 0 || !std::iswalpha(lower_text[pos - 1]));
            bool end_ok = (pos + wword.length() == lower_text.length() ||
                          !std::iswalpha(lower_text[pos + wword.length()]));

            if (start_ok && end_ok) {
                stopword_hits++;
                break;  // count each stopword once
            }
            pos++;
        }
    }

    // Consider it probably English if we found at least 2 stopwords
    return stopword_hits >= 2;
}

bool worth_translating(const std::wstring& text, bool targetIsEnglish) {
    // Skip if empty
    if (text.empty()) {
        return false;
    }

    // Check if already English and target is English
    if (targetIsEnglish && is_probably_english(text)) {
        return false;
    }

    // Skip if all digits and punctuation
    bool has_letter = false;
    for (wchar_t ch : text) {
        if (std::iswalpha(ch)) {
            has_letter = true;
            break;
        }
    }

    if (!has_letter) {
        return false;
    }

    return true;
}

}
