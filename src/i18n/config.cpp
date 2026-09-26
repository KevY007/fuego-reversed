#include "config.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>

namespace i18n::config {

static Settings g_settings;

Settings& get() {
    return g_settings;
}

// Helper: trim leading/trailing whitespace
static std::string trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start)) ++start;
    auto end = s.end();
    do {
        --end;
    } while (std::distance(start, end) > 0 && std::isspace(*end));
    return std::string(start, end + 1);
}

// Helper: unescape special characters
static std::string unescape(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == 'n') {
                result += '\n';
                ++i;
            } else if (next == 't') {
                result += '\t';
                ++i;
            } else if (next == 'r') {
                result += '\r';
                ++i;
            } else if (next == '\\') {
                result += '\\';
                ++i;
            } else {
                result += s[i];
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

// Helper: escape special characters
static std::string escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '\n') {
            result += "\\n";
        } else if (c == '\t') {
            result += "\\t";
        } else if (c == '\r') {
            result += "\\r";
        } else if (c == '\\') {
            result += "\\\\";
        } else {
            result += c;
        }
    }
    return result;
}

void load(const char* path) {
    std::ifstream file(path);
    if (!file) {
        return;  // file not found, keep defaults
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim and skip blank lines
        line = trim(line);
        if (line.empty()) continue;

        // Skip comments
        if (line[0] == '#') continue;

        // Find '=' separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;  // malformed, skip

        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        // Parse based on key
        if (key == "inbound_chat") {
            g_settings.inbound_chat = (value == "true" || value == "1");
        } else if (key == "target_lang") {
            g_settings.target_lang = std::stoi(value);
        } else if (key == "source_cp") {
            g_settings.source_cp = std::stoi(value);
        } else if (key == "base_url") {
            g_settings.base_url = unescape(value);
        } else if (key == "api_key") {
            g_settings.api_key = unescape(value);
        } else if (key == "model") {
            g_settings.model = unescape(value);
        } else if (key == "marker_style") {
            g_settings.marker_style = std::stoi(value);
        }
        // unknown keys are silently ignored
    }
}

void load() {
    load("fuego_i18n.cfg");
}

void save(const char* path) {
    std::ofstream file(path);
    file << "inbound_chat=" << (g_settings.inbound_chat ? "true" : "false") << "\n";
    file << "target_lang=" << g_settings.target_lang << "\n";
    file << "source_cp=" << g_settings.source_cp << "\n";
    file << "base_url=" << escape(g_settings.base_url) << "\n";
    file << "api_key=" << escape(g_settings.api_key) << "\n";
    file << "model=" << escape(g_settings.model) << "\n";
    file << "marker_style=" << g_settings.marker_style << "\n";
}

void save() {
    save("fuego_i18n.cfg");
}

std::string build_system_prompt(int targetLang) {
    std::string langName;

    // Map common language codes to names
    if (targetLang == 1033) {
        langName = "English";
    } else if (targetLang == 1031) {
        langName = "German";
    } else if (targetLang == 1036) {
        langName = "French";
    } else if (targetLang == 1040) {
        langName = "Italian";
    } else if (targetLang == 1034) {
        langName = "Spanish";
    } else if (targetLang == 1049) {
        langName = "Russian";
    } else {
        langName = "the configured language";
    }

    return std::string(
        "You are a professional translator. Translate the following SA-MP chat messages to ") +
        langName + ".\n\n"
        "CRITICAL RULES:\n"
        "1. Keep placeholder markers like \\x01..\\x02 verbatim — do not translate or remove them.\n"
        "2. Do NOT translate player names, server commands (e.g. /help), or color codes.\n"
        "3. Output ONLY a JSON array of objects: [{\"id\":0,\"text\":\"translated text\"}, ...]\n"
        "4. Preserve the exact structure and do not add any extra text.\n";
}

}
