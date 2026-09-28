#pragma once
// i18n/hudtext.h -- PURE line-by-line translation of HUD text (textdraws, game
// text, chat bubbles) and 3D text labels. No game or engine types, so it is
// host-tested (tests/hudtext_test.cpp); surfaces.cpp is the glue (translation
// memory lookups, the background fills, putting the result on screen).
//
// A text is translated LINE BY LINE -- split at "~n~"/"~N~", or at '\n' for a 3D
// label -- never as one piece. Given a multi-line HUD text, a model drops or
// repeats the inner line breaks (live: "~W~~R~hotwire failed~N~retrying..." came
// back without its "~N~", so it was never shown translated) or translates only
// the first line ("Selected spawn~N~Civilian (8)~N~Police (0)~N~..." came back
// with just "Selected spawn" translated). One short line at a time, it doesn't.
//
// Each line's leading and trailing colour codes stay out of what is translated,
// so a server's black "shadow" textdraw ("- Big Ear") and its coloured twin on
// top ("~W~- Big Ear") share ONE translation and still line up. And a line's
// numbers are replaced by stand-ins: "Next city: ~P~Los Santos (32m 27s)" is
// translated ONCE, as "Next city: ~P~Los Santos (2m 3s)", and every later value
// reuses that. A timer, a price or a player count would otherwise make a new
// text -- a new request, and English on screen -- every time it changed.

#include <functional>
#include <string>
#include <vector>

namespace i18n::hudtext {

struct Line {
    std::string              prefix;            // leading spaces + colour codes (~w~, ~R~~h~, {RRGGBB}): verbatim
    std::string              core;              // the text in between
    std::string              suffix;            // trailing spaces + colour codes: verbatim
    bool                     translate = false; // false: shown as it is (empty, "_", a sprite name, mostly numbers)
    std::string              key;               // core with its numbers replaced by stand-ins: what is translated + cached
    std::vector<std::string> nums;              // the numbers the stand-ins replaced, in order
};

struct Plan {
    std::vector<Line>        lines;
    std::vector<std::string> seps; // seps[i] joins lines[i] and lines[i + 1]: "~n~"/"~N~", or "\n" for a label
};

// Split `text` at every line break: "~n~"/"~N~" (HUD), or '\n' (label = true).
// Each line's prefix + core + suffix, joined by the separators, is `text` again.
Plan plan_text(const std::string& text, bool label);

// A textdraw sprite name ("hud:radar_centre", "LD_SPAC:white"): the textdraw
// draws that texture, so it must never be translated.
bool is_sprite_name(const std::string& core);

// The most numbers one line can have replaced by stand-ins ("2" .. "9"). A line
// with more keeps its numbers (key == core, no nums).
constexpr size_t kMaxNumbers = 8;

// Put a line's numbers back into its translated key. Every stand-in must appear
// exactly once as a whole number (not inside a word: the "2" of "T2" isn't one);
// any other number the translation has is kept as it is. False if a stand-in is
// missing or repeated -- the caller then keeps the original line.
bool put_numbers(const std::string& translated, const std::vector<std::string>& nums, std::string& out);

// What the translation memory says about a key.
enum class Look {
    Hit,  // translated: `tr` holds the translation
    Keep, // show this line as it is (nothing to translate there, or its translation failed)
    Miss, // not translated yet
};
using Lookup = std::function<Look(const std::string& key, std::string& tr)>;

// Resolve every translatable line of `p` through `look`. When no line is a Miss:
// true, and `out` is the text with every Hit line translated (its numbers put
// back) and every other line as it was -- possibly the original text exactly.
// When some line is a Miss: false, and `missing` lists those keys (each once, in
// order). A translated line that would break the text -- a different number of
// '~' than the original line (an unmatched tilde makes SA:MP show "Error:
// unmatched tilde"), or a stand-in lost or repeated -- keeps the original line.
// Line breaks inside a translated line become spaces.
bool resolve(const Plan& p, const Lookup& look, std::string& out, std::vector<std::string>& missing);

} // namespace i18n::hudtext
