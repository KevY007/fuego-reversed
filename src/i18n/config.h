#pragma once
#include <string>
#include <vector>

namespace i18n::config {

struct Settings {
    bool inbound_chat = false;
    int target_lang = 1033;  // LANG_ENGLISH default (see language_options())
    int source_cp = 0;       // 0 = auto-detect
    std::string base_url = "http://127.0.0.1:11434/v1";
    std::string api_key;     // empty = no key (local Ollama)
    // Default: Tencent Hy-MT2-1.8B (purpose-built MT LLM, Apache-2.0). Q4_K_M is
    // ~1.1 GB -- runs on a 2-3 GB GPU beside the game, or on CPU. Bigger card?
    // switch to hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M via the menu's model dropdown.
    std::string model = "hf.co/tencent/Hy-MT2-1.8B-GGUF:Q4_K_M";
    int marker_style = 0;
    // Write the target language phonetically in the Latin/Roman alphabet
    // (Roman Urdu, Hinglish, Arabizi, ...) instead of its native script. This
    // is REQUIRED for languages whose script SA:MP's cp1251/cp1252 font can't
    // render at all (Urdu, Arabic, Hindi, ...); harmless for Latin languages.
    bool roman_output = false;
};

Settings& get();

// --- persistence -----------------------------------------------------------
// In normal use the translator settings live in the UNIFIED cheat config
// (fuego_reversed.cfg) -- core cfg::save()/load() call to_text()/from_text()
// to embed this block there, so there is one file and one Save button. The
// path-based save()/load() below remain for tests and any standalone use.
void load();                       // legacy standalone file (fuego_i18n.cfg)
void load(const char* path);
void save();                       // legacy standalone file (fuego_i18n.cfg)
void save(const char* path);
std::string to_text();             // serialize this block to key=value lines
void from_text(const std::string& text); // parse key=value lines into get()

// --- helpers shared by the engine + the menu -------------------------------
struct LangOption { int code; const char* name; };
// The languages offered in the menu's target-language dropdown, in menu order.
// build_system_prompt() resolves a code to its name from this same table, so
// the two never drift.
const std::vector<LangOption>& language_options();

// The Windows codepage to encode finished text in for SA:MP's chat font.
// Romanized output is Latin -> 1252; Cyrillic targets -> 1251; everything else
// -> 1252. (Scripts SA:MP can't render at all should use roman_output.)
int display_codepage(int targetLang, bool roman);

// The cacheable system prompt: target language + the do-not-translate rules
// (names, IDs, commands, vehicle names, place names, placeholders) + the
// rolling-context note, and -- when roman is true -- the romanization rule.
std::string build_system_prompt(int targetLang, bool roman = false);

}
