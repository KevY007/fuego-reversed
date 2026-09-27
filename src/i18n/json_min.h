#pragma once
#include <string>
#include <vector>

namespace i18n::json {

// Not a general JSON parser — only handles shapes below.

// JSON-string-escape: quotes, backslash, control chars, \n.
std::string escape(const std::string& s);

struct Unit {
    int id;
    std::string text;
};

// Build [{"id":0,"text":"..."},...]
std::string build_units(const std::vector<std::string>& texts);

// Parse [{"id":..,"text":".."}], tolerant of surrounding whitespace.
// Returns false on malformed input.
bool parse_units(const std::string& body, std::vector<Unit>& out);

// Extract choices[0].message.content from OpenAI-style reply.
bool extract_message_content(const std::string& chatCompletionJson, std::string& out);

// True if the reply was cut off by max_tokens: the first choice's finish_reason is
// "length" (OpenAI / Ollama / Gemini compat) or "max_tokens" (some compat layers).
bool reply_truncated(const std::string& chatCompletionJson);

} // namespace i18n::json
