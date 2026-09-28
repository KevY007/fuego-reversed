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
    std::vector<std::string>           keep;
    std::vector<std::string>           asked;
    Lookup look() {
        return [this](const std::string& key, std::string& tr) {
            asked.push_back(key);
            for (const std::string& k : keep)
                if (k == key) return Look::Keep;
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
    {   // Later: shown as it is for now -- complete, but flagged to look again.
        Plan p = plan_text("~W~~R~hotwire failed~N~retrying...", false);
        std::string out;
        std::vector<std::string> missing;
        bool later = false;
        Lookup look = [](const std::string& key, std::string& tr) {
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
        check(resolve(p, tm.look(), out, missing) && out == "Vehicle locked~N~Press ~y~Y",
              "a tilde added or lost: the original line");
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
