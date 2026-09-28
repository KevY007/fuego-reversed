#pragma once
#include <string>
#include <vector>

namespace i18n::protect {

// Result of mask(): text with every non-translatable span replaced by a
// placeholder, the original spans (in placeholder-index order) in tokens,
// and whether any natural-language content actually survived.
struct Masked {
    std::string text;
    std::vector<std::string> tokens;
    bool translatable;
};

// Placeholder shape: byte 0x01, decimal index, byte 0x02 — e.g. index 12 is
// "\x01" "12" "\x02". Those control bytes never occur in ordinary chat text,
// can't collide with it, and a translation model has no reason to alter them.

// Scans src left-to-right and replaces non-translatable spans with
// placeholders: a "/command" at a word boundary together with its arguments (up
// to three typed words, numbers or "[names]" after it: "/gps atm", "/pay [id]
// [amount]"; a little word like "to" ends them; a message command -- /me, /do,
// /pm, /report, ... -- keeps only itself, its words are prose), "{RRGGBB}"/
// "{AARRGGBB}" colour embeds, "~x~"/"~k~~KEY~" format/key
// embeds, player-name shapes ("Name_Surname", "Name[ID]", "Name (ID)",
// "(ID) Name", and a leading "Name:"/"Name says:" chat prefix), standalone
// numbers ("$1234", plain integers, simple signed coordinates), short
// ALLCAPS+digit codes (plates), "http(s)://" URLs, and the OOC/emote wrapper
// glyphs "((", "))", "[", "]", "(radio)", "**". Matched spans are pushed into
// tokens in emission order; everything else is copied through verbatim.
// translatable is true iff at least one non-whitespace, non-placeholder
// character survived in text (i.e. there is something worth translating).
//
// labels = the text is a UI label -- a HUD/3D-label line or a dialog cell -- not a
// chat line. There "Word (N)" / "(N) Word" is a count ("Civilian (8)", "Police (0)"
// of a spawn menu), not a player, so those two name shapes are NOT masked (live
// names, Name_Surname and Name[ID] still are); and a number glued to its unit
// ("2m", "999hp") is masked like a standalone one, so a reply can't drop or change
// it -- the HUD puts its real numbers back into exactly those places.
Masked mask(const std::string& src, bool labels = false);

// A typed chat command whose words are a MESSAGE (/me, /do, /pm, /b, /w, /report,
// ...), split for outgoing translation: `head` = the command, its target when it
// takes one -- /pm and /report always ("/pm 12", "/pm Kevin_Smith"); /w, /whisper,
// /sms, /text when the first word is a number or a Name_Surname -- and the spaces
// after it; `message` = the rest, the only part to translate. False for any other
// command (its words are arguments: sent as typed), or with no message ("/pm 12").
bool split_message_command(const std::string& line, std::string& head, std::string& message);

// True iff every placeholder "\x01k\x02" for k in [0, n) appears in
// translated exactly once (no drops, no duplicates, no corruption).
bool placeholders_intact(const std::string& translated, size_t n);

// Replaces each placeholder in translated with tokens[k]. If the placeholders
// aren't intact per placeholders_intact, returns "" — the signal for callers
// to reject the translation and keep the original text instead.
std::string unmask(const std::string& translated, const std::vector<std::string>& tokens);

// Post-translation canonicalisation against a per-target-language glossary
// (e.g. normalising a term a model translated inconsistently). Phase 0 stub:
// returns text unchanged. The real glossary table lands in Phase 3+.
std::string apply_glossary(const std::string& text, int tgtLang);

// ---- Live player-name protection ---------------------------------------------
// set_names() publishes the protected terms (see name_variants()); mask() then
// masks every whole-word, case-insensitive occurrence, longest first, so the model
// never sees a player's name and can't translate or transliterate it
// ("Khalid" -> "Халид"). The ORIGINAL span is the token, so unmask() restores its
// exact casing ("khalid" stays "khalid"). Thread-safe: workers read an immutable
// snapshot while the main thread swaps in a new one.
void set_names(std::vector<std::string> terms);

// Protected terms for connected players' names: each exact name (>= 3 chars), its
// RP display form with '_' -> ' ' ("Khalid Ahmed"), and each underscore-separated
// part of >= 3 letters that isn't a common word ("Khalid", "Ahmed" -- but not
// "Black" from "John_Black" or "The" from "The_Man"). Deduped case-insensitively,
// sorted longest first.
std::vector<std::string> name_variants(const std::vector<std::string>& names);

} // namespace i18n::protect
