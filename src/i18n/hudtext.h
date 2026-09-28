#pragma once
// i18n/hudtext.h -- PURE line-by-line translation of HUD text (textdraws, game
// text, chat bubbles) and 3D text labels. No game or engine types, so it is
// host-tested (tests/hudtext_test.cpp); surfaces.cpp is the glue (translation
// memory lookups, the background fills, putting the result on screen).
//
// A text is translated LINE BY LINE -- split at "~n~"/"~N~", or at '\n' for a 3D
// label -- never as one piece. Given a multi-line HUD text, a model drops or
// repeats the inner line breaks (live: "~W~~R~hotwire failed~N~retrying..." came
// back without its "~N~", so it was never shown translated). One short line at a
// time, it doesn't -- and a line that fails costs only that line.
//
// Each line's leading and trailing colour codes stay out of what is translated,
// so a server's black "shadow" textdraw ("- Big Ear") and its coloured twin on
// top ("~W~- Big Ear") share one translation for every line that differs only in
// those edge codes (a line with an inner code of its own -- "Players nearby:
// ~Y~0" vs "Players nearby: 0" -- is a different text). And a line's numbers are
// replaced by stand-ins: "Next city: ~P~Los Santos (32m 27s)" is translated
// ONCE, as "Next city: ~P~Los Santos (2m 3s)", and every later value reuses that.
// A timer, a price or a player count would otherwise make a new text -- a new
// request, and English on screen -- every time it changed. (The fills are UI-label
// jobs, engine::Job::labels: the stand-ins reach the model as placeholders.)

#include <functional>
#include <string>
#include <vector>

namespace i18n::hudtext {

struct Line {
    std::string              prefix;            // leading spaces + colour codes (~w~, ~R~~h~, {RRGGBB}): verbatim
    std::string              core;              // the text in between
    std::string              suffix;            // trailing spaces + colour codes: verbatim
    bool                     translate = false; // false: shown as it is (empty, "_", mostly numbers)
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

// Looks like a textdraw sprite name ("hud:radar_centre", "LD_SPAC:white"): a
// sprite textdraw draws that texture, so its text must never be translated. The
// caller asks this only for a textdraw whose font it doesn't know (a sprite is
// font 4, a model preview font 5): "Job:Trucker" in a text font is text.
bool is_sprite_name(const std::string& text);

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
    Hit,   // translated: `tr` holds the translation
    Keep,  // show this line as it is: there's nothing to translate
    Later, // show it as it is for now, and ask again later (its translation failed; a retry is due)
    Miss,  // not translated yet
};
using Lookup = std::function<Look(const std::string& key, std::string& tr)>;

// Resolve every translatable line of `p` through `look`. `out` is always the text
// with every Hit line translated (its numbers put back) and every other line as it
// was -- possibly the original text exactly. Returns true when no line is a Miss;
// otherwise false, with `missing` listing those keys (each once, in order).
// `*later` (if given) is set when some line was Later. A translated line that
// would break the text -- a different number of '~' than the original line (an
// unmatched tilde makes SA:MP show "Error: unmatched tilde"), or a stand-in lost
// or repeated -- keeps the original line. Line breaks inside a translated line
// become spaces.
bool resolve(const Plan& p, const Lookup& look, std::string& out, std::vector<std::string>& missing,
             bool* later = nullptr);

} // namespace i18n::hudtext
