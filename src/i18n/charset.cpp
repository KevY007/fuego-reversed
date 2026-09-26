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

    // Histogram of high bytes (0xC0-0xFF)
    int high_count = 0;
    int cyrillic_count = 0;

    for (unsigned char c : bytes) {
        if (c >= 0xC0) {
            high_count++;
            // Cyrillic in cp1251 is typically 0xC0-0xFF
            if (c >= 0xC0) {
                cyrillic_count++;
            }
        }
    }

    // If more than 50% of bytes are in the 0xC0-0xFF range,
    // it's likely Cyrillic (cp1251)
    if (high_count > 0 && cyrillic_count * 2 > high_count) {
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

    // Now truncate to maxBytes on a UTF-8 code-point boundary
    if (result.size() > maxBytes) {
        result.resize(maxBytes);

        // Walk backwards to find a valid UTF-8 boundary
        // UTF-8 continuation bytes start with 10xxxxxx (0x80-0xBF)
        // Start bytes are 0xxxxxxx (single byte) or 11xxxxxx (multi-byte)
        while (!result.empty()) {
            unsigned char last = (unsigned char)result.back();

            // If it's a start byte (0xxxxxxx or 11xxxxxx), we're at a boundary
            if ((last & 0x80) == 0 || (last & 0xC0) == 0xC0) {
                break;
            }

            // It's a continuation byte (10xxxxxx), remove it
            result.pop_back();
        }
    }

    return result;
}

}
