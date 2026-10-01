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

// The provider kind a saved profile's id ("google", "anthropic", ...) names; -1 if none.
static int profile_kind(const std::string& id) {
    for (const auto& p : provider_presets())
        if (id == p.id && p.kind >= 0 && p.kind < kProviderKinds) return p.kind;
    return -1;
}

// The provider a config with no provider_kind line (one from before the Provider
// dropdown) was set up for, told from its URL: a preset's URL -> that provider; the
// default (no URL) -> Ollama / Local; any other URL -> Custom.
static int kind_for_url(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    for (const auto& p : provider_presets())
        if (p.url && p.url[0] && url == p.url) return p.kind;
    return url.empty() ? 4 : 0;
}

void from_text(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    bool sawKind = false;
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
            } else if (key == "source_lang") {
                g_settings.source_lang = std::stoi(value);
            } else if (key == "source_cp") {
                g_settings.source_cp = std::stoi(value);
            } else if (key == "provider_kind") {
                const int kind = std::stoi(value);
                // An unknown kind (hand edit, a newer build) is treated as Custom: its
                // URL/key/model stay in plain view and are kept on a switch.
                g_settings.provider_kind = (kind >= 0 && kind < kProviderKinds) ? kind : 0;
                sawKind = true;
            } else if (key == "base_url") {
                g_settings.base_url = unescape(value);
            } else if (key == "api_key") {
                g_settings.api_key = unescape(value);
            } else if (key == "model") {
                g_settings.model = unescape(value);
            } else if (key == "read_timeout_ms") {
                g_settings.read_timeout_ms = std::stoi(value);
            } else if (key == "translate_temp") {
                g_settings.translate_temp = std::stoi(value);
            } else if (key == "request_min_gap_ms") {
                g_settings.request_min_gap_ms = std::stoi(value);
            } else if (key == "roman_output") {
                g_settings.roman_output = (value == "true" || value == "1");
            } else if (key == "outbound_enabled") {
                g_settings.outbound_enabled = (value == "true" || value == "1");
            } else if (key == "outbound_target_lang") {
                g_settings.outbound_target_lang = std::stoi(value);
            } else if (key == "hotkey_mods") {
                g_settings.hotkey_mods = std::stoi(value);
            } else if (key == "hotkey_vk") {
                g_settings.hotkey_vk = std::stoi(value);
            } else if (key == "show_translate_indicators") {
                g_settings.show_translate_indicators = (value == "true" || value == "1");
            } else if (key == "translate_dialogs") {
                g_settings.translate_dialogs = (value == "true" || value == "1");
            } else if (key == "translate_hud") {
                g_settings.translate_hud = (value == "true" || value == "1");
            } else if (key.compare(0, 8, "profile.") == 0) {
                // profile.<provider id>.<field>: another provider's remembered values
                const size_t dot = key.find('.', 8);
                const int kind = dot == std::string::npos ? -1 : profile_kind(key.substr(8, dot - 8));
                if (kind >= 0) {
                    ProviderProfile& p = g_settings.profiles[kind];
                    const std::string field = key.substr(dot + 1);
                    if (field == "base_url") {
                        p.base_url = unescape(value); p.saved = true;
                    } else if (field == "api_key") {
                        p.api_key = unescape(value); p.saved = true;
                    } else if (field == "model") {
                        p.model = unescape(value); p.saved = true;
                    } else if (field == "request_min_gap_ms") {
                        p.request_min_gap_ms = std::stoi(value); p.saved = true;
                    }
                }
            }
            // unknown keys are silently ignored -- like the retired marker_style, and
            // debug_capture / debug_lab, which are runtime-only now (off at every start)
        } catch (...) {
            // a malformed integer value leaves that field at its default
        }
    }
    // A config from before the Provider dropdown has no provider_kind: tell the
    // provider from the URL, so its key shows (and stays) under the right one.
    if (!sawKind) g_settings.provider_kind = kind_for_url(g_settings.base_url);
    // An empty URL on a provider that has a preset one (its URL field is folded away
    // under Advanced) is the preset's; Custom's stays empty -- it's typed in plain view.
    if (g_settings.base_url.empty())
        for (const auto& p : provider_presets())
            if (p.kind == g_settings.provider_kind && p.url && p.url[0]) g_settings.base_url = p.url;
}

std::string to_text() {
    std::ostringstream out;
    out << "inbound_chat=" << (g_settings.inbound_chat ? "true" : "false") << "\n";
    out << "target_lang=" << g_settings.target_lang << "\n";
    out << "source_lang=" << g_settings.source_lang << "\n";
    out << "source_cp=" << g_settings.source_cp << "\n";
    out << "provider_kind=" << g_settings.provider_kind << "\n";
    out << "base_url=" << escape(g_settings.base_url) << "\n";
    out << "api_key=" << escape(g_settings.api_key) << "\n";
    out << "model=" << escape(g_settings.model) << "\n";
    out << "read_timeout_ms=" << g_settings.read_timeout_ms << "\n";
    out << "translate_temp=" << g_settings.translate_temp << "\n";
    out << "request_min_gap_ms=" << g_settings.request_min_gap_ms << "\n";
    out << "roman_output=" << (g_settings.roman_output ? "true" : "false") << "\n";
    out << "outbound_enabled=" << (g_settings.outbound_enabled ? "true" : "false") << "\n";
    out << "outbound_target_lang=" << g_settings.outbound_target_lang << "\n";
    out << "hotkey_mods=" << g_settings.hotkey_mods << "\n";
    out << "hotkey_vk=" << g_settings.hotkey_vk << "\n";
    out << "show_translate_indicators=" << (g_settings.show_translate_indicators ? "true" : "false") << "\n";
    out << "translate_dialogs=" << (g_settings.translate_dialogs ? "true" : "false") << "\n";
    out << "translate_hud=" << (g_settings.translate_hud ? "true" : "false") << "\n";
    // The other providers' remembered values (the active one's are the keys above).
    for (const auto& preset : provider_presets()) {
        if (preset.kind < 0 || preset.kind >= kProviderKinds || preset.kind == g_settings.provider_kind) continue;
        const ProviderProfile& p = g_settings.profiles[preset.kind];
        if (!p.saved) continue;
        const std::string head = std::string("profile.") + preset.id + ".";
        out << head << "base_url=" << escape(p.base_url) << "\n";
        out << head << "api_key=" << escape(p.api_key) << "\n";
        out << head << "model=" << escape(p.model) << "\n";
        out << head << "request_min_gap_ms=" << p.request_min_gap_ms << "\n";
    }
    return out.str();
}

static std::string g_savedText; // to_text() as of the last mark_saved()
static bool        g_savedKnown = false;

void mark_saved() {
    g_savedText  = to_text();
    g_savedKnown = true;
}

bool has_unsaved_changes() {
    return g_savedKnown && to_text() != g_savedText;
}

void switch_provider(int kind) {
    Settings& s = g_settings;
    if (kind < 0 || kind >= kProviderKinds || kind == s.provider_kind) return;
    const ProviderPreset* preset = nullptr;
    for (const auto& p : provider_presets())
        if (p.kind == kind) { preset = &p; break; }
    if (!preset) return;

    if (s.provider_kind >= 0 && s.provider_kind < kProviderKinds) {
        ProviderProfile& cur = s.profiles[s.provider_kind];
        cur.saved              = true;
        cur.base_url           = s.base_url;
        cur.api_key            = s.api_key;
        cur.model              = s.model;
        cur.request_min_gap_ms = s.request_min_gap_ms;
    }
    s.provider_kind = kind;

    // Its own values -- or, the first time, its preset URL + default model with no key
    // or gap: nothing of the previous provider's carries over (Custom, with no preset,
    // starts empty, so a key typed there never goes to the previous provider's server).
    // A URL/model missing from a partial profile (hand edit) also comes from the preset.
    const ProviderProfile& next = s.profiles[kind];
    const std::string presetUrl   = preset->url ? preset->url : "";
    const std::string presetModel = preset->model ? preset->model : "";
    s.base_url           = next.saved && !next.base_url.empty() ? next.base_url : presetUrl;
    s.model              = next.saved && !next.model.empty() ? next.model : presetModel;
    s.api_key            = next.saved ? next.api_key : std::string();
    s.request_min_gap_ms = next.saved ? next.request_min_gap_ms : 0;
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

const std::vector<ProviderPreset>& provider_presets() {
    // Order = menu dropdown order. Custom first (no url/model: the first time it is
    // picked it starts empty). Cloud defaults chosen from live
    // testing (2026-09): Gemini 2.0 and 2.5 flash are retired, so
    // "gemini-flash-latest" tracks the current flash; claude-haiku-4-5 and
    // gpt-4o-mini are the cheap, strong translation models.
    // The last field names each one's saved profile in the config -- never rename it.
    static const std::vector<ProviderPreset> presets = {
        {0, "Custom (your own URL)", "", "", "custom"},
        {1, "Google (Gemini)", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-flash-latest", "google"},
        {2, "Anthropic (Claude)", "https://api.anthropic.com/v1", "claude-haiku-4-5", "anthropic"},
        {3, "OpenAI", "https://api.openai.com/v1", "gpt-4o-mini", "openai"},
        {4, "Ollama / Local", "http://127.0.0.1:11434/v1", "hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M", "local"},
    };
    return presets;
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

std::string build_system_prompt(int targetLang, bool roman, int sourceLang) {
    std::string langName = "the configured language";
    for (const auto& o : language_options()) {
        if (o.code == targetLang) { langName = o.name; break; }
    }

    // Optional source-language hint. Default (0 = Auto) adds nothing, so the model
    // detects the source per line -- best for mixed-language servers. A specific
    // sourceLang (and only when it differs from the target) nudges the model for
    // servers that are reliably one language.
    std::string srcHint;
    if (sourceLang != 0 && sourceLang != targetLang) {
        for (const auto& o : language_options())
            if (o.code == sourceLang) { srcHint = "The source message is written in " + std::string(o.name) + ". "; break; }
    }

    // Native single-message translation instruction -- what dedicated MT models
    // (Hy-MT2) expect, and fine for general chat models too. The provider places
    // this as the USER message prefix (no system role), with the text to
    // translate after a blank line. Kept prose (no JSON): MT models don't follow
    // a "emit a JSON array" instruction and just parrot the example.
    std::string p =
        "Translate the following SA-MP (San Andreas Multiplayer) chat message into " + langName + ". "
        + srcHint +
        "Only output the translated text -- no explanation, no quotes, no notes. "
        "Do NOT translate, and keep EXACTLY as they are: player names and nicknames, player IDs, "
        "server commands (like /help or /car), colour codes, GTA:SA vehicle names (Infernus, Sultan, "
        "NRG-500, ...), and San Andreas place names (Grove Street, Idlewood, Los Santos, ...). "
        "Keep numbers, money amounts and times exact. "
        "Keep any placeholder markers like \\x01..\\x02 exactly where they appear. "
        "Translate naturally, matching the casual tone of game chat.";

    if (roman) {
        p += " IMPORTANT: translate the MEANING into " + langName + ", then write that translation using "
             "ONLY Latin letters a-z, spaces and punctuation -- a phonetic romanization (Roman Urdu / "
             "Hinglish / Arabizi). Do NOT keep the source words untranslated, and NEVER output any "
             "native-script character.";
    }
    return p;
}

}
