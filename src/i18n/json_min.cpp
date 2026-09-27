#include "i18n/json_min.h"
#include <cctype>
#include <cstdio>

namespace i18n::json {

std::string escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '"') {
            result += "\\\"";
        } else if (c == '\\') {
            result += "\\\\";
        } else if (c == '\n') {
            result += "\\n";
        } else if (c == '\t') {
            result += "\\t";
        } else if (c == '\r') {
            result += "\\r";
        } else if (c == '\b') {
            result += "\\b";
        } else if (c == '\f') {
            result += "\\f";
        } else if ((unsigned char)c < 0x20) {
            // Control characters: escape as \uXXXX
            char buf[7];
            std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
            result += buf;
        } else {
            result += c;
        }
    }
    return result;
}

std::string build_units(const std::vector<std::string>& texts) {
    std::string result = "[";
    for (size_t i = 0; i < texts.size(); ++i) {
        if (i > 0) result += ",";
        result += "{\"id\":" + std::to_string(i) + ",\"text\":\"" + escape(texts[i]) + "\"}";
    }
    result += "]";
    return result;
}

// Helper to skip whitespace
static size_t skip_ws(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace((unsigned char)s[pos])) {
        ++pos;
    }
    return pos;
}

// Helper to append UTF-8 bytes for a code point
static void append_utf8(std::string& out, int codepoint) {
    if (codepoint <= 0x7F) {
        // 1 byte
        out += (char)codepoint;
    } else if (codepoint <= 0x7FF) {
        // 2 bytes
        out += (char)(0xC0 | (codepoint >> 6));
        out += (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        // 3 bytes
        out += (char)(0xE0 | (codepoint >> 12));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        // 4 bytes
        out += (char)(0xF0 | (codepoint >> 18));
        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    } else {
        // Invalid code point, emit replacement character U+FFFD
        out += (char)0xEF;
        out += (char)0xBF;
        out += (char)0xBD;
    }
}

// Helper to unescape a JSON string value (assumes we're positioned after the opening quote)
static bool unescape_string(const std::string& s, size_t& pos, std::string& out) {
    out.clear();
    int pending_high_surrogate = -1;  // -1 means none pending

    while (pos < s.size()) {
        if (s[pos] == '"') {
            // If we have a pending high surrogate, emit replacement character
            if (pending_high_surrogate != -1) {
                append_utf8(out, 0xFFFD);
            }
            ++pos;
            return true;
        } else if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"': out += '"'; ++pos; break;
                case '\\': out += '\\'; ++pos; break;
                case 'n': out += '\n'; ++pos; break;
                case 't': out += '\t'; ++pos; break;
                case 'r': out += '\r'; ++pos; break;
                case 'b': out += '\b'; ++pos; break;
                case 'f': out += '\f'; ++pos; break;
                case '/': out += '/'; ++pos; break;
                case 'u': {
                    // \uXXXX escape
                    if (pos + 4 >= s.size()) return false;
                    int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char c = s[++pos];
                        code = code * 16;
                        if (c >= '0' && c <= '9') code += c - '0';
                        else if (c >= 'a' && c <= 'f') code += c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') code += c - 'A' + 10;
                        else return false;
                    }
                    ++pos;

                    // Handle surrogate pairs
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // High surrogate
                        if (pending_high_surrogate != -1) {
                            // Previous high surrogate without matching low - emit replacement
                            append_utf8(out, 0xFFFD);
                        }
                        pending_high_surrogate = code;
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        // Low surrogate
                        if (pending_high_surrogate != -1) {
                            // Combine high and low surrogate
                            int high = pending_high_surrogate;
                            int low = code;
                            int codepoint = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
                            append_utf8(out, codepoint);
                            pending_high_surrogate = -1;
                        } else {
                            // Lone low surrogate - emit replacement
                            append_utf8(out, 0xFFFD);
                        }
                    } else {
                        // Regular code point (non-surrogate)
                        if (pending_high_surrogate != -1) {
                            // High surrogate without matching low - emit replacement
                            append_utf8(out, 0xFFFD);
                            pending_high_surrogate = -1;
                        }
                        append_utf8(out, code);
                    }
                    break;
                }
                default:
                    return false;
            }
        } else {
            if (pending_high_surrogate != -1) {
                // High surrogate without matching low - emit replacement
                append_utf8(out, 0xFFFD);
                pending_high_surrogate = -1;
            }
            out += s[pos];
            ++pos;
        }
    }
    return false;  // Unclosed string
}

bool parse_units(const std::string& body, std::vector<Unit>& out) {
    out.clear();
    size_t pos = skip_ws(body, 0);

    // Expect '['
    if (pos >= body.size() || body[pos] != '[') return false;
    ++pos;

    while (true) {
        pos = skip_ws(body, pos);

        // Check for end of array
        if (pos < body.size() && body[pos] == ']') {
            ++pos;
            return true;
        }

        // Expect '{'
        if (pos >= body.size() || body[pos] != '{') return false;
        ++pos;

        Unit unit;
        unit.id = -1;
        bool has_id = false, has_text = false, has_field = false;

        while (true) {
            pos = skip_ws(body, pos);

            // Check for end of object
            if (pos < body.size() && body[pos] == '}') {
                ++pos;
                break;
            }

            // Expect ',' if we've already consumed a field
            if (has_field) {
                if (pos >= body.size() || body[pos] != ',') return false;
                ++pos;
                pos = skip_ws(body, pos);
            }

            // Expect '"key"'
            if (pos >= body.size() || body[pos] != '"') return false;
            ++pos;

            std::string key;
            if (!unescape_string(body, pos, key)) return false;

            pos = skip_ws(body, pos);

            // Expect ':'
            if (pos >= body.size() || body[pos] != ':') return false;
            ++pos;
            pos = skip_ws(body, pos);

            // Mark that we've consumed a field
            has_field = true;

            // Parse value
            if (key == "id") {
                // Expect number
                if (pos >= body.size() || !std::isdigit((unsigned char)body[pos])) return false;
                unit.id = 0;
                while (pos < body.size() && std::isdigit((unsigned char)body[pos])) {
                    unit.id = unit.id * 10 + (body[pos] - '0');
                    ++pos;
                }
                has_id = true;
            } else if (key == "text") {
                // Expect string value
                if (pos >= body.size() || body[pos] != '"') return false;
                ++pos;
                if (!unescape_string(body, pos, unit.text)) return false;
                has_text = true;
            } else {
                // Unknown key, skip value
                if (pos >= body.size()) return false;
                if (body[pos] == '"') {
                    ++pos;
                    std::string dummy;
                    if (!unescape_string(body, pos, dummy)) return false;
                } else if (std::isdigit((unsigned char)body[pos]) || body[pos] == '-') {
                    while (pos < body.size() && (std::isdigit((unsigned char)body[pos]) || body[pos] == '-' || body[pos] == '.')) {
                        ++pos;
                    }
                } else if (body.substr(pos, 4) == "true") {
                    pos += 4;
                } else if (body.substr(pos, 5) == "false") {
                    pos += 5;
                } else if (body.substr(pos, 4) == "null") {
                    pos += 4;
                } else {
                    return false;
                }
            }
        }

        if (!has_id || !has_text) return false;
        out.push_back(unit);

        pos = skip_ws(body, pos);

        // Check for ',' or ']'
        if (pos >= body.size()) return false;
        if (body[pos] == ',') {
            ++pos;
        } else if (body[pos] == ']') {
            ++pos;
            return true;
        } else {
            return false;
        }
    }
}

bool reply_truncated(const std::string& chatCompletionJson) {
    const size_t choices = chatCompletionJson.find("\"choices\"");
    if (choices == std::string::npos) return false;
    size_t pos = chatCompletionJson.find("\"finish_reason\"", choices);
    if (pos == std::string::npos) return false;
    pos = skip_ws(chatCompletionJson, pos + 15); // skip "finish_reason"
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != ':') return false;
    pos = skip_ws(chatCompletionJson, pos + 1);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != '"') return false; // null / missing
    std::string reason;
    ++pos; // past the opening quote
    if (!unescape_string(chatCompletionJson, pos, reason)) return false;
    return reason == "length" || reason == "max_tokens";
}

bool extract_message_content(const std::string& chatCompletionJson, std::string& out) {
    out.clear();
    size_t pos = 0;

    // Find "choices" key
    size_t choices_pos = chatCompletionJson.find("\"choices\"");
    if (choices_pos == std::string::npos) return false;
    pos = choices_pos + 9;  // skip "choices"

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != ':') return false;
    ++pos;

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != '[') return false;
    ++pos;

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != '{') return false;
    ++pos;

    // Find "message" key
    size_t message_pos = chatCompletionJson.find("\"message\"", pos);
    if (message_pos == std::string::npos) return false;
    pos = message_pos + 9;  // skip "message"

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != ':') return false;
    ++pos;

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != '{') return false;
    ++pos;

    // Find "content" key
    size_t content_pos = chatCompletionJson.find("\"content\"", pos);
    if (content_pos == std::string::npos) return false;
    pos = content_pos + 9;  // skip "content"

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != ':') return false;
    ++pos;

    pos = skip_ws(chatCompletionJson, pos);
    if (pos >= chatCompletionJson.size() || chatCompletionJson[pos] != '"') return false;
    ++pos;

    // Extract the string value
    return unescape_string(chatCompletionJson, pos, out);
}

} // namespace i18n::json
