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

// Helper: trim leading/trailing whitespace (incl. a stray '\r' from CRLF files)
static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
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

void from_text(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        try {
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
            } else if (key == "read_timeout_ms") {
                g_settings.read_timeout_ms = std::stoi(value);
            } else if (key == "roman_output") {
                g_settings.roman_output = (value == "true" || value == "1");
            } else if (key == "outbound_enabled") {
                g_settings.outbound_enabled = (value == "true" || value == "1");
            } else if (key == "outbound_target_lang") {
                g_settings.outbound_target_lang = std::stoi(value);
            } else if (key == "debug_capture") {
                g_settings.debug_capture = (value == "true" || value == "1");
            } else if (key == "translate_dialogs") {
                g_settings.translate_dialogs = (value == "true" || value == "1");
            } else if (key == "translate_hud") {
                g_settings.translate_hud = (value == "true" || value == "1");
            }
            // unknown keys are silently ignored
        } catch (...) {
            // a malformed integer value leaves that field at its default
        }
    }
}

std::string to_text() {
    std::ostringstream out;
    out << "inbound_chat=" << (g_settings.inbound_chat ? "true" : "false") << "\n";
    out << "target_lang=" << g_settings.target_lang << "\n";
    out << "source_cp=" << g_settings.source_cp << "\n";
    out << "base_url=" << escape(g_settings.base_url) << "\n";
    out << "api_key=" << escape(g_settings.api_key) << "\n";
    out << "model=" << escape(g_settings.model) << "\n";
    out << "marker_style=" << g_settings.marker_style << "\n";
    out << "read_timeout_ms=" << g_settings.read_timeout_ms << "\n";
    out << "roman_output=" << (g_settings.roman_output ? "true" : "false") << "\n";
    out << "outbound_enabled=" << (g_settings.outbound_enabled ? "true" : "false") << "\n";
    out << "outbound_target_lang=" << g_settings.outbound_target_lang << "\n";
    out << "debug_capture=" << (g_settings.debug_capture ? "true" : "false") << "\n";
    out << "translate_dialogs=" << (g_settings.translate_dialogs ? "true" : "false") << "\n";
    out << "translate_hud=" << (g_settings.translate_hud ? "true" : "false") << "\n";
    return out.str();
}

void load(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return;  // file not found, keep defaults
    std::ostringstream ss;
    ss << file.rdbuf();
    from_text(ss.str());
}

void load() {
    load("fuego_i18n.cfg");
}

void save(const char* path) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    file << to_text();
}

void save() {
    save("fuego_i18n.cfg");
}

const std::vector<LangOption>& language_options() {
    // Codes are Windows LCIDs so they line up with the source-codepage world;
    // the exact value only matters as a stable key for build_system_prompt()
    // and display_codepage(). Roman-only scripts (Urdu/Arabic/Hindi/...) are
    // included because roman_output makes them usable in SA:MP's Latin font.
    static const std::vector<LangOption> opts = {
        {1033, "English"},
        {1034, "Spanish"},
        {1046, "Portuguese (Brazil)"},
        {1036, "French"},
        {1031, "German"},
        {1040, "Italian"},
        {1049, "Russian"},
        {1058, "Ukrainian"},
        {1045, "Polish"},
        {1048, "Romanian"},
        {1055, "Turkish"},
        {1057, "Indonesian"},
        {1066, "Vietnamese"},
        {1025, "Arabic"},
        {1056, "Urdu"},
        {1081, "Hindi"},
        {2052, "Chinese (Simplified)"},
        {1041, "Japanese"},
        {1042, "Korean"},
        {1032, "Greek"},
        {1043, "Dutch"},
        {1029, "Czech"},
        {1038, "Hungarian"},
        {1037, "Hebrew"},
        {1054, "Thai"},
    };
    return opts;
}

int display_codepage(int targetLang, bool roman) {
    if (roman) return 1252;              // Latin romanization
    switch (targetLang) {
        case 1049: // Russian
        case 1058: // Ukrainian
        case 1026: // Bulgarian
        case 1059: // Belarusian
        case 1071: // Macedonian
            return 1251;                 // Cyrillic
        default:
            return 1252;                 // Western/Latin (best effort)
    }
}

std::string build_system_prompt(int targetLang, bool roman) {
    std::string langName = "the configured language";
    for (const auto& o : language_options()) {
        if (o.code == targetLang) { langName = o.name; break; }
    }

    // Native single-message translation instruction -- what dedicated MT models
    // (Hy-MT2) expect, and fine for general chat models too. The provider places
    // this as the USER message prefix (no system role), with the text to
    // translate after a blank line. Kept prose (no JSON): MT models don't follow
    // a "emit a JSON array" instruction and just parrot the example.
    std::string p =
        "Translate the following SA-MP (San Andreas Multiplayer) chat message into " + langName + ". "
        "Only output the translated text -- no explanation, no quotes, no notes. "
        "Do NOT translate, and keep EXACTLY as they are: player names and nicknames, player IDs, "
        "server commands (like /help or /car), colour codes, GTA:SA vehicle names (Infernus, Sultan, "
        "NRG-500, ...), and San Andreas place names (Grove Street, Idlewood, Los Santos, ...). "
        "Keep numbers, money amounts and times exact. "
        "Keep any placeholder markers like \\x01..\\x02 exactly where they appear. "
        "Translate naturally, matching the casual tone of game chat.";

    if (roman) {
        p += " Write the translation in " + langName + " but using ONLY Latin/Roman letters "
             "(romanized / transliterated the way people type online, e.g. Roman Urdu \"aap kaise ho\", "
             "Hinglish, Arabizi) -- do NOT use the native script.";
    }
    return p;
}

}
