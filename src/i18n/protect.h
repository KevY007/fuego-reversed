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
// placeholders: leading "/command" (plus one following argument-looking
// token), "{RRGGBB}"/"{AARRGGBB}" colour embeds, "~x~"/"~k~~KEY~" format/key
// embeds, player-name shapes ("Name_Surname", "Name[ID]", "Name (ID)",
// "(ID) Name", and a leading "Name:"/"Name says:" chat prefix), standalone
// numbers ("$1234", plain integers, simple signed coordinates), short
// ALLCAPS+digit codes (plates), "http(s)://" URLs, and the OOC/emote wrapper
// glyphs "((", "))", "[", "]", "(radio)", "**". Matched spans are pushed into
// tokens in emission order; everything else is copied through verbatim.
// translatable is true iff at least one non-whitespace, non-placeholder
// character survived in text (i.e. there is something worth translating).
Masked mask(const std::string& src);

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

} // namespace i18n::protect
