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
    // Default: Tencent Hy-MT2-7B (purpose-built MT LLM, Apache-2.0), Q4_K_M ~4.6 GB.
    // Great for native-script languages (Spanish/Russian/etc.). NOTE: Hy-MT2 emits a
    // language's NATIVE script, so romanized output (Roman Urdu etc.) is unreliable
    // with it -- use a general LLM (e.g. qwen3:4b) for romanized targets.
    std::string model = "hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M";
    int marker_style = 0;
    // Read timeout (ms) for a translation request. Local LLMs cold-load the model
    // on the first call (seconds to tens of seconds), so this needs headroom;
    // 30s is a safe default. Menu exposes it as "Response timeout".
    int read_timeout_ms = 30000;
    // Write the target language phonetically in the Latin/Roman alphabet
    // (Roman Urdu, Hinglish, Arabizi, ...) instead of its native script. This
    // is REQUIRED for languages whose script SA:MP's cp1251/cp1252 font can't
    // render at all (Urdu, Arabic, Hindi, ...); harmless for Latin languages.
    bool roman_output = false;

    // --- Outbound: translate MY typed chat before it is sent (opposite
    // direction: my language -> the server's language). Toggled live with
    // Ctrl+Shift; outbound_enabled persists the last state. outbound_target_lang
    // is the SERVER's language to translate into. ---
    bool outbound_enabled = false;
    int outbound_target_lang = 1049; // default Russian (classic "I speak EN, RU server")

    // Debug: log raw bytes (+ decoded plain fields) of incoming dialog/label/
    // textdraw/gametext RPCs to fuego_reversed.log, so their exact wire format
    // can be verified against a real server before those surfaces are built.
    // Read-only; never modifies packets. Default OFF.
    bool debug_capture = false;

    // Translate incoming DIALOGS + 3D text labels (RPC 61/36 -- Huffman CSTRING
    // bodies). Translation-memory-first: first sight shows the original + fills the
    // cache, the next identical dialog/label shows translated. Default OFF.
    bool translate_dialogs = false;
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
