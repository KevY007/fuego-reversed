#pragma once
#include <string>

namespace i18n {

// Joins a base URL and a path component, normalizing the seam so the result
// never has a doubled '/' -- regardless of whether `base` happens to end in
// a trailing slash (a common config typo, e.g. baseUrl =
// "http://127.0.0.1:11434/v1/") or not. Header-only and WinHTTP-free so it
// can be unit-tested directly by the host harness.
inline std::string join_url(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (!path.empty() && path.front() == '/') return b + path;
    if (path.empty()) return b;
    return b + "/" + path;
}

} // namespace i18n
