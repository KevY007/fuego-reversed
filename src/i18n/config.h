#pragma once
#include <string>
#include <vector>

namespace i18n::config {

// The provider kinds (Settings::provider_kind): 0 Custom, 1 Google, 2 Anthropic,
// 3 OpenAI, 4 Ollama / Local. See provider_presets().
constexpr int kProviderKinds = 5;

// What each provider remembers on its own: switching Google -> Anthropic -> back
// to Google brings the Google key, model, URL and request gap back instead of
// losing them (see switch_provider()).
struct ProviderProfile {
    bool saved = false;          // false = never used: switching to it starts from its preset
    std::string base_url;
    std::string api_key;
    std::string model;
    int request_min_gap_ms = 0;
};

struct Settings {
    bool inbound_chat = false;
    int target_lang = 1033;  // LANG_ENGLISH default (see language_options())
    // Source (incoming) language. 0 = Auto: the model detects it per line, which
    // is the right default for SA:MP (servers mix English + a local language).
    // Set a specific LCID (from language_options()) only to HINT the model when a
    // server is reliably one language and short/ambiguous lines get mis-detected.
    int source_lang = 0;     // 0 = Auto (detect)
    int source_cp = 0;       // 0 = auto-detect (byte codepage, not language)
    // Provider preset the endpoint is configured for. Selecting one in the menu
    // (switch_provider()) brings back that provider's own URL/key/model/gap -- its
    // preset URL + default model the first time -- and drives per-provider request
    // quirks in provider.cpp (see provider_presets() / ProviderConfig::providerKind):
    //   0 = Custom (edit URL freely; permissive request, like local)
    //   1 = Google (Gemini)     -- permissive
    //   2 = Anthropic (Claude)  -- MUST omit top_p (400 if temperature+top_p both
    //                              set) and needs an "anthropic-version" header
    //   3 = OpenAI              -- MUST omit reasoning_effort (400 "unrecognized")
    //   4 = Ollama / Local      -- permissive (the offline default)
    // Default 4 matches the local base_url/model below. A config with no
    // provider_kind line (from before the dropdown) gets it from its URL; an
    // unknown value loads as Custom (see from_text()).
    int provider_kind = 4;
    std::string base_url = "http://127.0.0.1:11434/v1";
    std::string api_key;     // empty = no key (local Ollama)
    // Default: Tencent Hy-MT2-7B (purpose-built MT LLM, Apache-2.0), Q4_K_M ~4.6 GB.
    // Great for native-script languages (Spanish/Russian/etc.). NOTE: Hy-MT2 emits a
    // language's NATIVE script, so romanized output (Roman Urdu etc.) is unreliable
    // with it -- use a general LLM (e.g. qwen3:4b) for romanized targets.
    std::string model = "hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M";
    // Read timeout (ms) for a translation request. Local LLMs cold-load the model
    // on the first call (seconds to tens of seconds), so this needs headroom;
    // 30s is a safe default. Menu exposes it as "Response timeout".
    int read_timeout_ms = 30000;
    // Sampling temperature as a percent (30 = 0.30). 0.3 works well for both the
    // Hy-MT2 MT models and general models (gemma3/qwen). Menu: "Temperature".
    int translate_temp = 30;
    // Minimum gap (ms) between outgoing translation requests, enforced globally
    // across all worker threads (a request pacer). 0 = off (default -- local/paid
    // endpoints have no meaningful RPM limit). Set ~1000-4000ms for a free-tier
    // cloud key (e.g. Gemini free) to stay under its requests-per-minute cap and
    // avoid HTTP 429/503 throttling. Independent of the automatic 429/503 retry
    // (which always backs off + retries a few times). Menu: "Min request gap".
    // Kept per provider, like the URL/key/model (a free-tier gap never slows the
    // local model down).
    int request_min_gap_ms = 0;

    // The OTHER providers' remembered URL/key/model/gap, indexed by provider kind.
    // The ACTIVE provider's values are always the fields above (what the engine and
    // the menu use); switch_provider() moves values between the two.
    ProviderProfile profiles[kProviderKinds];
    // Write the target language phonetically in the Latin/Roman alphabet
    // (Roman Urdu, Hinglish, Arabizi, ...) instead of its native script. This
    // is REQUIRED for languages whose script SA:MP's cp1251/cp1252 font can't
    // render at all (Urdu, Arabic, Hindi, ...); harmless for Latin languages.
    bool roman_output = false;

    // --- Outbound: translate MY typed chat, the message of my message commands
    // ("/pm 35 <message>", "/me <message>", ...) AND my dialog-input replies before
    // they are sent (opposite direction: my language -> the server's language).
    // Toggled live with a settable hotkey (hotkey_mods/hotkey_vk below, default
    // Ctrl+Shift) or the menu; outbound_enabled persists the last state.
    // outbound_target_lang is the SERVER's language to translate into. ---
    bool outbound_enabled = false;
    int outbound_target_lang = 1049; // default Russian (classic "I speak EN, RU server")

    // Outgoing-translation toggle hotkey -- ONE key shared by outgoing chat AND
    // outgoing dialog-input replies (both use outbound_enabled). hotkey_mods is a
    // bitmask (1 Ctrl, 2 Shift, 4 Alt); hotkey_vk an extra virtual key (0 = the
    // modifiers alone). Default Ctrl+Shift (the original toggle).
    int hotkey_mods = 3;
    int hotkey_vk = 0;

    // Draw the on-screen translation hints ("your message will be sent in ...",
    // "translating dialog..."). Default ON.
    bool show_translate_indicators = true;

    // Translate incoming DIALOGS, 3D text labels and SIGNS (object material text:
    // RPC 61/36/84 -- Huffman CSTRING bodies). A dialog not fully in the translation
    // memory is HELD until it is translated, then shown in arrival order with the
    // server's original timing (after 10 s untranslated: shown in the original -- a
    // text-only one switches to its translation if still open when it's ready); a
    // password dialog's text is translated too, but the password the player types is
    // never touched. 3D labels and signs are translated line by line: one not in the
    // translation memory shows in the original and switches to the translation on
    // screen as soon as it's ready. Default OFF.
    bool translate_dialogs = false;

    // Translate the HUD: textdraws (RPC 105/134), gametext (73), chat bubbles (59).
    // Plain text (no Huffman). Line by line, numbers kept out of it (a timer or a
    // count reuses one translation); lines that are mostly numbers stay as they are.
    // A textdraw not in the translation memory shows in the original and switches
    // to the translation on screen as soon as it's ready (game text and bubbles, gone
    // in seconds: translated from the next time on). NOTE: textdraws/gametext render
    // via GTA's bitmap-glyph font -> best for Latin/romanized targets. Default OFF.
    bool translate_hud = false;
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

// Bookkeeping for the menu's "changes pending" hint: mark_saved() remembers the
// current settings as the saved + applied ones -- cfg::load() calls it after loading
// and the menu's Save after saving AND applying (not every cfg::save(): /compat
// saves the file without applying the translator settings) -- and
// has_unsaved_changes() is true once anything differs from that.
void mark_saved();
bool has_unsaved_changes();

// Switch the active provider: the current provider's URL/key/model/gap are kept
// in its profile, and the new one's come back -- or, the first time, its preset
// URL + default model with no key and no gap (Custom, which has no preset, starts
// empty: a key typed there must never go to the previous provider's server). A
// URL/model missing from a partial profile also comes from the preset. The same
// kind, or an unknown one, is a no-op.
void switch_provider(int kind);

// --- helpers shared by the engine + the menu -------------------------------

// A selectable provider preset for the menu's Provider dropdown. `kind` is the
// value stored in Settings::provider_kind. `url` / `model` are what a provider
// starts with the first time it is chosen; both are empty for Custom, which starts
// empty. `id` names its saved profile in the config
// ("profile.<id>.api_key=..."). The per-provider request quirks are applied in
// provider.cpp based on `kind` (see Settings::provider_kind).
struct ProviderPreset { int kind; const char* name; const char* url; const char* model; const char* id; };
const std::vector<ProviderPreset>& provider_presets();

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
// sourceLang: 0 = Auto (the model detects the source language -- the default and
// best for mixed-language servers); a non-zero LCID adds a "source is <lang>" hint.
std::string build_system_prompt(int targetLang, bool roman = false, int sourceLang = 0);

}
