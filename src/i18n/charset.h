#ifndef FUEGO_I18N_CHARSET_H
#define FUEGO_I18N_CHARSET_H

#include <string>

namespace i18n::charset {

// Convert from bytes (codepage-encoded) to wide string
std::wstring decode(const std::string& bytes, int codepage);

// Convert from wide string to bytes (codepage-encoded)
// Uses '?' as the default character for unmappable wide chars
std::string encode(const std::wstring& text, int codepage);

// Convert wide string to UTF-8
std::string to_utf8(const std::wstring& text);

// Convert UTF-8 to wide string
std::wstring from_utf8(const std::string& s);

// Guess codepage: high-byte histogram
// Mostly 0xC0-0xFF => cp1251 (Cyrillic)
// Otherwise => cp1252 (Western)
int guess_codepage(const std::string& bytes);

// Remove all '%' characters and truncate to maxBytes on a UTF-8 boundary
// (never cut mid-character)
std::string cap_and_sanitize(const std::string& s, size_t maxBytes);

}

#endif // FUEGO_I18N_CHARSET_H
