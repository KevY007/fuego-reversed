// Host test for i18n::hudtext -- line-by-line HUD / 3D-label translation with
// numbers replaced by stand-ins. The texts are real captures from the user's
// server (Mike's C&R, 2026-09-28 log): the textdraws that stayed in English.
//
// Build/run: tests\run_hudtext_test.cmd

#include "i18n/hudtext.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace i18n::hudtext;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

static std::string join(const Plan& p) {
    std::string s;
    for (size_t i = 0; i < p.lines.size(); ++i) {
        s += p.lines[i].prefix + p.lines[i].core + p.lines[i].suffix;
        if (i < p.seps.size()) s += p.seps[i];
    }
    return s;
}

// A translation memory for resolve(): key -> translation, plus keys to Keep.
struct FakeTm {
    std::map<std::string, std::string> hits;
    std::vector<std::string>           keep, later, retry; // keys answered Keep / Later / Retry
    std::vector<std::string>           asked;
    Lookup look() {
        return [this](const std::string& key, std::string& tr, bool) {
            asked.push_back(key);
            for (const std::string& k : keep)
                if (k == key) return Look::Keep;
            for (const std::string& k : later)
                if (k == key) return Look::Later;
            for (const std::string& k : retry)
                if (k == key) return Look::Retry;
            auto it = hits.find(key);
            if (it == hits.end()) return Look::Miss;
            tr = it->second;
            return Look::Hit;
        };
    }
};

int main() {
    // Every captured text must split and join back byte-exactly.
    const char* real[] = {
        "~W~~R~Vehicle locked~N~~W~Tap ~y~~k~~VEHICLE_ENTER_EXIT~~w~ to break in",
        "Selected spawn~N~Civilian (8)~N~Police (0)~N~Medic/Fire (0)~N~Taxi (0)~N~_",
        "~W~~R~hotwire failed~N~retrying...",
        "~W~This vehicle can be scrapped for~N~cash at the ~B~~H~blue car icon~N~~W~(~Y~/scrap~W~ for more info)",
        "You do not seem to have the audio plugin.~N~~N~The audio plugin adds sounds to the game and makes the "
        "game more fun and immersive.~N~~N~See ~H~mikescnr.com/audio for more information.~N~~N~_~N~~N~~N~",
        "~N~~N~~N~~N~~N~~N~~N~~W~There is a ~G~~H~$2,000~W~ daily bonus if you use the plugin!",
        "Active city: ~P~Las Venturas~N~~W~Next city: ~P~Los Santos (32m 27s)~N~~W~Time: ~G~~H~Saturday 15:32~N~~W~Players online: ~Y~8",
        "- El Quebrados Sheriff~N~- El Quebrados Medical Center~N~- Gas station~N~~N~Players nearby: 0~N~~N~_",
        "~W~- El Quebrados Sheriff~N~- El Quebrados Medical Center~N~- Gas station~N~~N~Players nearby: ~Y~0",
        "~W~Press ~Y~~k~~TOGGLE_SUBMISSIONS~ ~W~to start ~P~smuggling",
        "hud:radar_gangY", "_", " ", "~W~", "", "Information", "Boxville (999hp, 0kmh)",
    };
    for (const char* t : real) {
        check(join(plan_text(t, false)) == t, t);
        check(join(plan_text(t, true)) == t, t);
    }

    {   // Lines, edge codes, and which lines are translated at all.
        Plan p = plan_text("~W~~R~Vehicle locked~N~~W~Tap ~y~~k~~VEHICLE_ENTER_EXIT~~w~ to break in", false);
        check(p.lines.size() == 2 && p.seps.size() == 1 && p.seps[0] == "~N~", "two lines");
        check(p.lines[0].prefix == "~W~~R~" && p.lines[0].core == "Vehicle locked" && p.lines[0].suffix.empty(),
              "leading colour codes are the prefix");
        check(p.lines[0].translate && p.lines[0].key == "Vehicle locked", "plain line: key == core");
        check(p.lines[1].prefix == "~W~" && p.lines[1].core == "Tap ~y~~k~~VEHICLE_ENTER_EXIT~~w~ to break in",
              "inner codes (and the key code) stay in the core");
    }
    {
        Plan p = plan_text("Selected spawn~N~Civilian (8)~N~Police (0)~N~Medic/Fire (0)~N~Taxi (0)~N~_", false);
        check(p.lines.size() == 6, "six lines");
        check(p.lines[0].key == "Selected spawn", "line 1");
        check(p.lines[1].key == "Civilian (2)" && p.lines[1].nums == std::vector<std::string>{ "8" },
              "a number becomes a stand-in");
        check(p.lines[2].key == "Police (2)" && p.lines[3].key == "Medic/Fire (2)" && p.lines[4].key == "Taxi (2)",
              "each row its own key");
        check(!p.lines[5].translate, "\"_\" is not translated");
    }
    {   // A shadow textdraw and its coloured twin share their keys.
        Plan a = plan_text("- El Quebrados Sheriff~N~- Gas station", false);
        Plan b = plan_text("~W~- El Quebrados Sheriff~N~- Gas station", false);
        check(a.lines[0].key == b.lines[0].key && a.lines[1].key == b.lines[1].key, "shadow + twin: same keys");
    }
    {   // A timer: every value, one key.
        Plan a = plan_text("~W~Next city: ~P~Los Santos (32m 27s)", false);
        Plan b = plan_text("~W~Next city: ~P~Los Santos (9m 3s)", false);
        check(a.lines[0].key == "Next city: ~P~Los Santos (2m 3s)", "timer key");
        check(a.lines[0].key == b.lines[0].key, "a changed timer reuses the key");
        check(a.lines[0].nums == (std::vector<std::string>{ "32", "27" }), "the timer's numbers, in order");
        Plan c = plan_text("~W~Time: ~G~~H~Saturday 15:32", false);
        check(c.lines[0].key == "Time: ~G~~H~Saturday 2:3", "a clock");
        Plan d = plan_text("~W~Players online: ~Y~8", false);
        check(d.lines[0].key == "Players online: ~Y~2", "a number right after a colour code");
        Plan e = plan_text("There is a ~G~~H~$2,000~W~ daily bonus", false);
        check(e.lines[0].key == "There is a ~G~~H~$2,3~W~ daily bonus" &&
              e.lines[0].nums == (std::vector<std::string>{ "2", "000" }), "a price with a thousands separator");
    }
    {   // Numbers glued to a word stay; codes are never touched.
        Plan p = plan_text("AK47 ammo: 30", false);
        check(p.lines[0].key == "AK47 ammo: 2" && p.lines[0].nums == std::vector<std::string>{ "30" },
              "\"AK47\" keeps its digits");
        Plan q = plan_text("{FF0000}Owner: {00FF00}John 12", true);
        check(q.lines[0].prefix == "{FF0000}" && q.lines[0].key == "Owner: {00FF00}John 2",
              "digits inside a {RRGGBB} code are not numbers");
        Plan r = plan_text("Scores: one 1, two 2, three 3, four 4, five 5, six 6, seven 7, eight 8, nine 9", false);
        check(r.lines[0].translate && r.lines[0].key == r.lines[0].core && r.lines[0].nums.empty(),
              "more numbers than stand-ins: kept as they are");
    }
    {   // Not translated: "mostly numbers", nothing but codes. (Sprites are the
        // caller's call -- by the textdraw's font -- so "Job:Trucker" stays text here.)
        check(plan_text("Job:Trucker", false).lines[0].translate, "\"Job:Trucker\" is text to plan_text");
        check(is_sprite_name("hud:radar_centre") && is_sprite_name("LD_SPAC:white") && !is_sprite_name("Time:") &&
              !is_sprite_name("Time: 5") && !is_sprite_name("a:b:c"), "is_sprite_name");
        check(!plan_text("F: 77%", false).lines[0].translate, "a stat");
        check(!plan_text("$00005000", false).lines[0].translate, "money");
        check(!plan_text("~W~", false).lines[0].translate, "codes only");
        check(plan_text("Level 4", false).lines[0].translate, "\"Level 4\" is text");
        check(plan_text("Boxville (999hp, 0kmh)", false).lines[0].key == "Boxville (2hp, 3kmh)", "units after numbers");
    }
    {   // A key code followed by what looks like a colour code is a KEY NAME, not a suffix.
        Plan p = plan_text("Press ~k~~W~", false);
        check(p.lines[0].suffix.empty() && p.lines[0].core == "Press ~k~~W~", "~k~~W~ stays in the core");
        Plan q = plan_text("Press ~k~~VEHICLE_ENTER_EXIT~~w~ ", false);
        check(q.lines[0].suffix == "~w~ " && q.lines[0].core == "Press ~k~~VEHICLE_ENTER_EXIT~", "trailing ~w~ + space");
    }
    {   // Labels split at '\n' only; "~n~" in a label is not a break. "\r\n" keeps its '\r'.
        Plan p = plan_text("{FFFFFF}House for sale\r\nPrice: $5000~n~x", true);
        check(p.lines.size() == 2 && p.seps[0] == "\n", "label: split at \\n");
        check(p.lines[0].suffix == "\r" && p.lines[0].core == "House for sale", "\\r goes to the suffix");
        check(p.lines[1].core == "Price: $5000~n~x", "~n~ is text in a label");
        Plan q = plan_text("a~n~b", false);
        check(q.lines.size() == 2 && q.seps[0] == "~n~", "lowercase ~n~ splits a HUD text");
    }

    // ---- put_numbers ----
    {
        std::string o;
        check(put_numbers("Próxima ciudad: ~P~Los Santos (2m 3s)", { "32", "27" }, o) &&
              o == "Próxima ciudad: ~P~Los Santos (32m 27s)", "numbers back in");
        check(put_numbers("(3s, 2m)", { "32", "27" }, o) && o == "(27s, 32m)", "reordered stand-ins");
        check(!put_numbers("(2m)", { "32", "27" }, o), "a lost stand-in");
        check(!put_numbers("(2m 2m 3s)", { "32", "27" }, o), "a repeated stand-in");
        check(put_numbers("Modelo T2: 2", { "5" }, o) && o == "Modelo T2: 5", "the 2 of T2 is not a stand-in");
        check(put_numbers("24/7: 2", { "5" }, o) && o == "24/7: 5", "the translation's own numbers stay");
        check(put_numbers("{00FF00}2", { "9" }, o) && o == "{00FF00}9", "a colour code's digits stay");
        check(put_numbers("sin números", {}, o) && o == "sin números", "no numbers");
        check(!put_numbers("(02m 3s)", { "32", "27" }, o), "a changed stand-in counts as lost");
        check(put_numbers("2,3 km", { "1", "15" }, o) && o == "1,15 km", "a localized decimal");
    }

    // ---- resolve ----
    {   // Everything in memory: one text, lines + separators + numbers put back.
        Plan p = plan_text("Selected spawn~N~Civilian (8)~N~Police (0)~N~_", false);
        FakeTm tm;
        tm.hits = { { "Selected spawn", "Aparición elegida" }, { "Civilian (2)", "Civil (2)" }, { "Police (2)", "Policía (2)" } };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing) && missing.empty(), "all resolved");
        check(out == "Aparición elegida~N~Civil (8)~N~Policía (0)~N~_", "assembled with the numbers back");
    }
    {   // One line missing: not complete; each missing key listed once; `out` still
        // has every line it could translate (the missing ones as they were).
        Plan p = plan_text("~W~- Big Ear~N~- Big Ear~N~Players nearby: 1", false);
        FakeTm tm;
        tm.hits = { { "Players nearby: 2", "Jugadores cerca: 2" } };
        std::string out;
        std::vector<std::string> missing;
        bool later = true;
        check(!resolve(p, tm.look(), out, missing, &later), "a miss: not complete");
        check(missing == std::vector<std::string>{ "- Big Ear" }, "the missing key, once");
        check(out == "~W~- Big Ear~N~- Big Ear~N~Jugadores cerca: 1", "the translated lines already in out");
        check(!later, "no line was Later");
    }
    {   // A size limit: translated lines are taken in order while the text fits; one
        // that would push it over stays original -- the rest never flips back.
        Plan p = plan_text("Aaaa~N~Bbbb~N~Cccc", false); // 18 bytes
        FakeTm tm;
        tm.hits = { { "Aaaa", "Aaaaaa" }, { "Bbbb", "Bbbbbbbbbb" }, { "Cccc", "Cc" } };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing, nullptr, 21) && out == "Aaaaaa~N~Bbbb~N~Cc",
              "limit 21: A (+2) fits, B (+6) would not, C (-2) does");
        check(out.size() <= 21, "the result fits the limit");
        check(resolve(p, tm.look(), out, missing, nullptr, 100) && out == "Aaaaaa~N~Bbbbbbbbbb~N~Cc",
              "a roomy limit takes every line");
    }
    {   // Ordinals are words, not counts: a line with one keeps all its numbers.
        Plan p = plan_text("You finished 2nd with 300 points", false);
        check(p.lines[0].key == p.lines[0].core && p.lines[0].nums.empty(), "an ordinal: no stand-ins on that line");
        check(plan_text("Level 5th-ish 4", false).lines[0].nums.empty(), "5th- is an ordinal too");
        check(plan_text("Room 4thly 3", false).lines[0].nums.size() == 2, "\"4thly\" is not an ordinal");
    }
    {   // Later: shown as it is for now -- complete, but flagged to look again.
        Plan p = plan_text("~W~~R~hotwire failed~N~retrying...", false);
        std::string out;
        std::vector<std::string> missing;
        bool later = false;
        Lookup look = [](const std::string& key, std::string& tr, bool) {
            if (key == "hotwire failed") return Look::Later;
            tr = "reintentando...";
            return Look::Hit;
        };
        check(resolve(p, look, out, missing, &later) && later && missing.empty() &&
              out == "~W~~R~hotwire failed~N~reintentando...", "Later: original line, complete, flagged");
    }
    {   // Keep = the original line; nothing translatable = the original text.
        Plan p = plan_text("~W~~R~hotwire failed~N~retrying...", false);
        FakeTm tm;
        tm.hits = { { "retrying...", "reintentando..." } };
        tm.keep = { "hotwire failed" };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing) && out == "~W~~R~hotwire failed~N~reintentando...", "keep + hit");
        Plan q = plan_text("_~N~F: 77%", false);
        FakeTm none;
        check(resolve(q, none.look(), out, missing) && out == "_~N~F: 77%" && none.asked.empty(),
              "nothing translatable: never looked up");
    }
    {   // A translation that would break the text keeps the original line.
        Plan p = plan_text("Vehicle locked~N~Press ~y~Y", false);
        FakeTm tm;
        tm.hits = { { "Vehicle locked", "Vehículo ~ bloqueado" }, { "Press ~y~Y", "Pulsa Y" } };
        std::string out;
        std::vector<std::string> missing;
        check(!resolve(p, tm.look(), out, missing) && out == "Vehicle locked~N~Press ~y~Y",
              "a tilde added or lost: the original line");
        check(missing == std::vector<std::string>{ "Press Y" },
              "a coloured translation that breaks the line: the plain key is fetched instead");
        FakeTm lost;
        lost.hits = { { "Civilian (2)", "Civil" } };
        Plan q = plan_text("Civilian (8)", false);
        check(resolve(q, lost.look(), out, missing) && out == "Civilian (8)", "a lost number: the original line");
        FakeTm br;
        br.hits = { { "House for sale", "Casa\nen venta" } };
        Plan r = plan_text("House for sale", true);
        check(resolve(r, br.look(), out, missing) && out == "Casa en venta", "a line break inside becomes a space");
        FakeTm empty;
        empty.hits = { { "House for sale", "" } };
        check(resolve(r, empty.look(), out, missing) && out == "House for sale", "an empty translation: the original");
    }

    // ---- colour codes inside a line: the plain fallback ----
    {   // The plain key drops the inner colour codes (not a key name after ~k~) and
        // keeps words apart; a line without inner codes has none.
        Plan p = plan_text("~W~Use ~Y~ARROW KEYS~W~ to select a spawn location~N~Press ~Y~~k~~W~ now~N~Plain text", false);
        check(p.lines[0].plainKey == "Use ARROW KEYS to select a spawn location", "plain key without the inner codes");
        check(p.lines[0].marks.size() == 2 && p.lines[0].marks[0].open == "~Y~" &&
              p.lines[0].marks[0].text == "ARROW KEYS" && p.lines[0].marks[0].close == "~W~", "the coloured stretch");
        check(p.lines[1].plainKey == "Press ~k~~W~ now", "a key name after ~k~ is not a colour code");
        check(p.lines[2].plainKey.empty() && p.lines[2].marks.empty(), "no inner codes: no plain key");
        check(plan_text("red~W~car", false).lines[0].plainKey == "red car", "words stay apart");
        Plan q = plan_text("Time: ~G~~H~Saturday 15:32", false);
        check(q.lines[0].plainKey == "Time: Saturday 2:3" && q.lines[0].nums.size() == 2,
              "the plain key has the same stand-ins");
    }
    {   // Coloured key in memory: used; the plain key is never asked.
        Plan p = plan_text("Press ~Y~SHIFT~W~ to spawn", false);
        FakeTm tm;
        tm.hits = { { "Press ~Y~SHIFT~W~ to spawn", "Presiona ~Y~SHIFT~W~ para aparecer" },
                    { "Press SHIFT to spawn", "Pulsa SHIFT" } };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing) && out == "Presiona ~Y~SHIFT~W~ para aparecer" &&
              tm.asked.size() == 1, "coloured hit");
    }
    {   // First sight: the coloured key is fetched; a plain translation already in
        // memory (a shadow twin's) shows meanwhile -- recoloured where it survives.
        Plan p = plan_text("Press ~Y~SHIFT~W~ to spawn", false);
        FakeTm tm;
        tm.hits = { { "Press SHIFT to spawn", "Presiona SHIFT para aparecer" } };
        std::string out;
        std::vector<std::string> missing;
        check(!resolve(p, tm.look(), out, missing) && missing == std::vector<std::string>{ "Press ~Y~SHIFT~W~ to spawn" },
              "the coloured key is fetched");
        check(out == "Presiona ~Y~SHIFT~W~ para aparecer", "the plain translation meanwhile, SHIFT coloured again");
        FakeTm none;
        check(!resolve(p, none.look(), out, missing) && missing.size() == 1 && out == "Press ~Y~SHIFT~W~ to spawn",
              "neither in memory: only the coloured key is fetched");
    }
    {   // The coloured key failed: the plain one is fetched (and shown when it lands);
        // a translated coloured stretch simply loses its colour.
        Plan p = plan_text("~W~Use ~Y~ARROW KEYS~W~ to select a spawn location", false);
        FakeTm tm;
        tm.later = { "Use ~Y~ARROW KEYS~W~ to select a spawn location" };
        std::string out;
        std::vector<std::string> missing;
        bool later = false;
        check(!resolve(p, tm.look(), out, missing, &later) && later &&
              missing == std::vector<std::string>{ "Use ARROW KEYS to select a spawn location" },
              "coloured failed: the plain key is fetched");
        tm.hits = { { "Use ARROW KEYS to select a spawn location", "Usa las TECLAS DE FLECHA para elegir dónde aparecer" } };
        check(resolve(p, tm.look(), out, missing, &later) && later &&
              out == "~W~Usa las TECLAS DE FLECHA para elegir dónde aparecer", "the plain translation, uncoloured");
        tm.later.clear();
        tm.retry = { "Use ~Y~ARROW KEYS~W~ to select a spawn location" };
        check(!resolve(p, tm.look(), out, missing) && out == "~W~Usa las TECLAS DE FLECHA para elegir dónde aparecer" &&
              missing == std::vector<std::string>{ "Use ~Y~ARROW KEYS~W~ to select a spawn location" },
              "a due retry of the coloured key keeps the plain one on screen");
    }
    {   // Recolouring: a key name kept verbatim gets its colour back, a stretch found
        // twice doesn't; a plain translation that adds a '~' is not used.
        Plan p = plan_text("Press ~Y~~k~~TOGGLE_SUBMISSIONS~ ~W~to start ~P~smuggling", false);
        FakeTm tm;
        tm.later = { p.lines[0].key };
        tm.hits = { { p.lines[0].plainKey, "Pulsa ~k~~TOGGLE_SUBMISSIONS~ para empezar a contrabandear" } };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing) &&
              out == "Pulsa ~Y~~k~~TOGGLE_SUBMISSIONS~~W~ para empezar a contrabandear", "the key name recoloured");
        Plan q = plan_text("Hi ~Y~Bob~W~ and Bob", false);
        FakeTm twice;
        twice.later = { q.lines[0].key };
        twice.hits = { { "Hi Bob and Bob", "Hola Bob y Bob" } };
        check(resolve(q, twice.look(), out, missing) && out == "Hola Bob y Bob", "found twice: not recoloured");
        FakeTm bad;
        bad.later = { p.lines[0].key };
        bad.hits = { { p.lines[0].plainKey, "Pulsa ~k~ para ~ empezar" } };
        check(resolve(p, bad.look(), out, missing) && out == p.lines[0].core, "a broken plain translation: the original");
    }
    {   // A stretch that ran to the end of the line, moved by the translation: closed
        // with the line's own colour -- or, with none known, not coloured at all.
        Plan p = plan_text("~W~Next city: ~P~Los Santos", false);
        FakeTm tm;
        tm.later = { p.lines[0].key };
        tm.hits = { { "Next city: Los Santos", "Los Santos es la siguiente ciudad" } };
        std::string out;
        std::vector<std::string> missing;
        check(resolve(p, tm.look(), out, missing) && out == "~W~~P~Los Santos~W~ es la siguiente ciudad",
              "closed with the line's colour");
        tm.hits = { { "Next city: Los Santos", "Siguiente ciudad: Los Santos" } };
        check(resolve(p, tm.look(), out, missing) && out == "~W~Siguiente ciudad: ~P~Los Santos", "still at the end: as before");
        Plan q = plan_text("Next city: ~P~Los Santos", false); // no colour of its own
        FakeTm tq;
        tq.later = { q.lines[0].key };
        tq.hits = { { "Next city: Los Santos", "Los Santos es la siguiente ciudad" } };
        check(resolve(q, tq.look(), out, missing) && out == "Los Santos es la siguiente ciudad",
              "no colour of its own to close with: not coloured");
    }
    {   // While the coloured key is out for the first time, the plain key is only
        // looked up in memory (hitOnly) -- never tested for "needs translating".
        Plan p = plan_text("Press ~Y~SHIFT~W~ to spawn", false);
        std::vector<std::pair<std::string, bool>> asked;
        Lookup look = [&](const std::string& key, std::string&, bool hitOnly) {
            asked.push_back({ key, hitOnly });
            return Look::Miss;
        };
        std::string out;
        std::vector<std::string> missing;
        resolve(p, look, out, missing);
        check(asked.size() == 2 && !asked[0].second && asked[1].first == "Press SHIFT to spawn" && asked[1].second,
              "the plain key: memory only");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
