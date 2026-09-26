#include "i18n/json_min.h"
#include <cctype>

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

// Helper to unescape a JSON string value (assumes we're positioned after the opening quote)
static bool unescape_string(const std::string& s, size_t& pos, std::string& out) {
    out.clear();
    while (pos < s.size()) {
        if (s[pos] == '"') {
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
                    // For simplicity, only handle ASCII codes (0-127)
                    if (code >= 0 && code <= 127) {
                        out += (char)code;
                    }
                    break;
                }
                default:
                    return false;
            }
        } else {
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
        bool has_id = false, has_text = false;

        while (true) {
            pos = skip_ws(body, pos);

            // Check for end of object
            if (pos < body.size() && body[pos] == '}') {
                ++pos;
                break;
            }

            // Expect ','
            if (has_id || has_text) {
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
