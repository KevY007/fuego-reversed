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

// A coloured stretch inside a line: the `open` codes, its text, the `close` codes
// right after it ("" when it runs to the end of the line).
struct Mark {
    std::string open, text, close;
};

struct Line {
    std::string              prefix;            // leading spaces + colour codes (~w~, ~R~~h~, {RRGGBB}): verbatim
    std::string              core;              // the text in between
    std::string              suffix;            // trailing spaces + colour codes: verbatim
    bool                     translate = false; // false: shown as it is (empty, "_", mostly numbers)
    std::string              key;               // core with its numbers replaced by stand-ins: what is translated + cached
    std::vector<std::string> nums;              // the numbers the stand-ins replaced, in order
    // The key WITHOUT its inner colour codes ("" when there are none). A model loses
    // the codes around words it translates (live: "Use ~Y~ARROW KEYS~W~ to select a
    // spawn location" failed every time), so once the coloured key failed this one
    // is translated instead -- and while the coloured one is out, its translation is
    // shown if already in memory (a shadow textdraw has exactly this text).
    std::string              plainKey;
    std::vector<std::string> plainNums;         // the numbers its stand-ins replaced (a code removed can join two)
    std::vector<Mark>        marks;             // the coloured stretches: coloured again in a plain translation where
                                                // their text survives verbatim (a key name, a command, a name)
    // Its colours are decoration -- a code inside a word (a gradient: "/{FF1300}h
    // {FF1D00}e{FF2700}l..."), or more than six stretches: the coloured key could
    // never come back intact, so only plainKey is ever translated.
    bool                     plainOnly = false;
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
// with more keeps its numbers (key == core, no nums) -- so does a line with an
// ordinal ("2nd", "21st", "5th"): that is a word the model should localize, and
// left in the key it could collide with a stand-in.
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
    Later, // its translation failed; show it as it is for now and ask again later (a retry isn't due yet)
    Retry, // its translation failed and a retry is due now: fetch it again
    Miss,  // never translated yet: fetch it
};
// hitOnly: only a Hit is of any use here (a plain key while its coloured one is out
// for the first time) -- the lookup may answer Miss for anything else, without the
// costlier checks.
using Lookup = std::function<Look(const std::string& key, std::string& tr, bool hitOnly)>;

// Resolve every translatable line of `p` through `look`. `out` is always the text
// with every Hit line translated (its numbers put back) and every other line as it
// was -- possibly the original text exactly. A line with inner colour codes whose
// coloured key isn't a Hit shows its plain key's translation when that is a Hit,
// recoloured where its stretches survive verbatim; its plain key is fetched only
// once the coloured one has failed (Later / Retry). Returns true when nothing is
// to be fetched; otherwise false, with `missing` listing the keys to fetch (each
// once, in order: a Miss or Retry). `*later` (if given) is set when some key was
// Later -- the caller should ask again later. A translated line that
// would break the text -- a different number of '~' than the original line (an
// unmatched tilde makes SA:MP show "Error: unmatched tilde"), or a stand-in lost
// or repeated -- keeps the original line. Line breaks inside a translated line
// become spaces. `limit`: the most bytes the surface takes -- translated lines are
// taken in order while the text stays within it, and one that would push it over
// keeps its original line (never cut: a cut can split a ~x~ code), so a text only
// ever gains translated lines as more of them land.
bool resolve(const Plan& p, const Lookup& look, std::string& out, std::vector<std::string>& missing,
             bool* later = nullptr, size_t limit = (size_t)-1);

} // namespace i18n::hudtext
