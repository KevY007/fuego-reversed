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

std::string cap_display(const std::string& s, size_t maxBytes) {
    // Display text is single-byte-per-character (cp1252/cp1251) -- strip the
    // game's '%' format-string char, then plain-truncate. No boundary logic:
    // there is no multi-byte sequence in a single-byte codepage to protect.
    std::string result;
    result.reserve(s.size());

    for (char c : s) {
        if (c != '%') {
            result.push_back(c);
        }
    }

    if (result.size() > maxBytes) {
        result.resize(maxBytes);
    }

    return result;
}

}
