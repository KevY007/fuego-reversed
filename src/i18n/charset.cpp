#include "i18n/charset.h"
#include <windows.h>

namespace i18n::charset {

std::wstring decode(const std::string& bytes, int codepage) {
    if (bytes.empty()) return {};

    int needed = MultiByteToWideChar(
        codepage, 0,
        bytes.c_str(), (int)bytes.size(),
        nullptr, 0
    );
    if (needed <= 0) return {};

    std::wstring result(needed, L'\0');
    MultiByteToWideChar(
        codepage, 0,
        bytes.c_str(), (int)bytes.size(),
        result.data(), needed
    );
    return result;
}

std::string encode(const std::wstring& text, int codepage) {
    if (text.empty()) return {};

    int needed = WideCharToMultiByte(
        codepage, 0,
        text.c_str(), (int)text.size(),
        nullptr, 0,
        "?", nullptr
    );
    if (needed <= 0) return {};

    std::string result(needed, '\0');
    WideCharToMultiByte(
        codepage, 0,
        text.c_str(), (int)text.size(),
        result.data(), needed,
        "?", nullptr
    );
    return result;
}

std::string to_utf8(const std::wstring& text) {
    return encode(text, CP_UTF8);
}

std::wstring from_utf8(const std::string& s) {
    return decode(s, CP_UTF8);
}

int guess_codepage(const std::string& bytes) {
    if (bytes.empty()) return 1252;

    // Count high bytes (0x80-0xFF) - substantial presence indicates Cyrillic (cp1251)
    int high_count = 0;
    int total = (int)bytes.size();

    for (unsigned char c : bytes) {
        if (c >= 0x80) {
            high_count++;
        }
    }

    // If more than ~33% of bytes are high (0x80-0xFF),
    // it's likely Cyrillic (cp1251); otherwise Western (cp1252)
    if (high_count * 3 > total) {
        return 1251;
    }

    return 1252;
}

std::string cap_and_sanitize(const std::string& s, size_t maxBytes) {
    // First, remove all '%' characters
    std::string result;
    result.reserve(s.size());

    for (char c : s) {
        if (c != '%') {
            result.push_back(c);
        }
    }

    // Truncate to maxBytes if needed
    if (result.size() > maxBytes) {
        result.resize(maxBytes);
    }

    // Drop any trailing incomplete UTF-8 sequence
    // Algorithm: from the end, count trailing continuation bytes (10xxxxxx, i.e. (b & 0xC0)==0x80)
    // The byte before them must be a lead byte; check if it declares the right number of continuations
    // If not enough continuations are present, remove the lead byte and all trailing continuations

    size_t i = result.size();
    while (i > 0) {
        unsigned char last = (unsigned char)result[i - 1];

        // Single-byte character (0xxxxxxx) - valid boundary
        if ((last & 0x80) == 0) {
            break;
        }

        // Continuation byte (10xxxxxx) - need to count and find the lead byte
        if ((last & 0xC0) == 0x80) {
            // Count trailing continuation bytes
            size_t cont_count = 0;
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)result[j] & 0xC0) == 0x80) {
                cont_count++;
                j--;
            }

            // Now check the byte before the continuations (should be a lead byte)
            if (j < result.size()) {
                unsigned char lead = (unsigned char)result[j];
                int expected_continuations = 0;

                if ((lead & 0xE0) == 0xC0) {        // 110xxxxx - 2-byte sequence
                    expected_continuations = 1;
                } else if ((lead & 0xF0) == 0xE0) { // 1110xxxx - 3-byte sequence
                    expected_continuations = 2;
                } else if ((lead & 0xF8) == 0xF0) { // 11110xxx - 4-byte sequence
                    expected_continuations = 3;
                } else {
                    // Invalid lead byte or another continuation byte - remove it and all trailing continuations
                    result.resize(j);
                    i = j;
                    continue;
                }

                // Check if we have the expected number of continuations
                if (cont_count == expected_continuations) {
                    // Complete sequence - valid boundary
                    break;
                } else {
                    // Incomplete sequence - remove the lead byte and continuations
                    result.resize(j);
                    i = j;
                    continue;
                }
            } else {
                // Continuation bytes with no lead byte - remove them
                result.resize(i - cont_count);
                i -= cont_count;
                continue;
            }
        } else if ((last & 0xC0) == 0xC0) {
            // Lead byte with no continuation - this is an incomplete sequence
            // Remove it
            result.resize(i - 1);
            i--;
            continue;
        } else {
            // Should not reach here, but just in case
            break;
        }
    }

    return result;
}

}
