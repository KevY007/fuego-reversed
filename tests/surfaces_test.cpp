// Host test for i18n::surfaces packet rebuild (dialogs / 3D labels), verified
// against REAL captured packets. With an identity translator the rebuilt packet
// MUST equal the original byte-for-byte -- that is the proof the rebuild + Huffman
// re-encode can never hand SA:MP a malformed packet. Also checks that a genuinely
// translated body re-encodes to something that decodes back to the translation.
//
// Build/run: tests\run_surfaces_test.cmd

#include "i18n/surfaces.h"
#include "i18n/huffman.h"

#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (const char* p = s; p[0] && p[1]; p += 2)
        v.push_back((uint8_t)((nib(p[0]) << 4) | nib(p[1])));
    return v;
}

// Real captures (Mike's C&R server), full RPC payloads.
static const char* kDialogLogin =
    "0600030E7B3965343762357D4C6F6720696E064C6F6720696E0643616E63656C5C013E521C71C71C73E5"
    "3CEC906D8E0AC67DE0751B7BA632EE7467DE137DA8A21657DF5A98CC60661F4E789452F1A6EA3BD70BAA9"
    "E39D199A2EC17AE179EA36F74C65DAFE46C5F4BED7F447E374165EA2349C51B7BA632E79580";
// Real captures from the user's C&R server (2026-09-27), full RPC 61 payloads.
// id 863, TABLIST_HEADERS: "{c8c8c8}Topic\t{c8c8c8}Shortcode\nHelp System Overview\tintro"
static const char* kDlgHelpTopics =
    "5F03052A7B3965343762357D48656C7020546F70696373202D2048656C702053797374656D204F766572766965770653656C656374044261636B4C00BE52513BD13BD13BBE53D4A144A27491F29289DE89DE89DDF29EACC466E887035F024A7AB2BDD8D398D3C667A0A674910AF30A80";
// id 291, LIST: "{ffffff}Account Balance\nWithdraw\nDeposit\nRecent Transactions\nSet up ATM PIN"
static const char* kDlgBankList =
    "2301021B7B3965343762357D42616E6B206F662053616E20416E64726561730653656C65637405436C6F73655800BE521C71C71C73E53AA51442CAFD41A4C540355C4BC3B2D49A670A46D2CD320A015FA9658B6D45A416CDAB05F5A9D52A4FBF9D9A8C4A80";
// id 416, TABLIST_HEADERS: "{c8c8c8}Destination\t{c8c8c8}Distance\nLas Venturas Bank\t12m\n{666666}Palomino Creek Bank\t{666666}1.15km\n..."
static const char* kDlgGps =
    "A001052F7B3965343762357D4750533A2053656C6563742064657374696E6174696F6E2028636C6F73657374206669727374290653656C656374044261636B2804BE52513BD13BD13BBE5399C6ED09AD20A7491F29289DE89DE89DDF29CCE9BB62A01A669B7D57815AF2DBF50627F3A493AAB7A1AF948ABEABEABEABEABEABEF94F3B1A51A21479DB901FFA8313F9D247CA455F55F55F55F55F55F7CA79D5E9D55747FA1AAC8CDF9DAD9DC17A8313F9D2456DE55FA89FE86BE522AFAAFAAFAAFAAFAAFBE5399A8DFAB18AE37EA0C4FE7491F29157D57D57D57D57D57DF29D5B79F2FE777FA1A669B7EA195D5DB7EA0C4FE74915B79F2FDF2FDFE8699A6DFA83672C5436FD4189FCE922BC1E56D57DFE86BE522AFAAFAAFAAFAAFAAFBE53D58C5D5883398F50627F3A48F948ABEABEABEABEABEABEF94EABA3CADAB6FF435F29157D57D57D57D57D57DF29D50A612F3B1087A8313F9D247CA455F55F55F55F55F55F7CA755D1E55D751BFD100";
// id 1006, TABLIST_HEADERS single column: "{c8c8c8}Zone\nLas Venturas Airport\nThe Strip (North)\n..."
static const char* kDlgSpawn =
    "EE0305197B3965343762357D53656C65637420737061776E207A6F6E650653656C65637405436C6F73651C01BE52513BD13BD13BBE53ABCC1034CD36FAAF02B5E5B7D5132A466CDA9607AB2F28A77D4C519BC3E86D4B03D5979453BEB5642EF0FA1AAC8CDF9DAD9DC11A669B7E76356DB6736CD55A5D576B07E59D1B34CD36FD41B3962A1B66D4B03D4126EAB364DA8357B47000";
static const char* kLabel1024 =
    "0004FF18AB31CD7811C5CDCCD440713D0B420000A04000FFFFFFFF04012B803BCEC9315F7CB3A8DF2FCD7C"
    "A4562B158AC562B1F29E76498AC3B9F5FC84B19EFAC1C749083E86AB598A6F061EA27AA2A89F21A6CEDCC2"
    "E8C7B8E85632D45A80";

int main() {
    using namespace i18n::surfaces;

    // Identity translator: every piece "translates" to itself.
    Translator identity = [](const std::string& src, std::string& out) { out = src; return true; };

    // ---- Dialog: identity rebuild must be byte-exact with the original ----
    {
        std::vector<uint8_t> orig = hex(kDialogLogin);
        std::vector<uint8_t> out;
        bool ok = rebuild_dialog(orig.data(), orig.size(), identity, out);
        check(ok, "rebuild_dialog(identity) returns true");
        check(out == orig, "rebuild_dialog(identity) is byte-exact with the original login dialog");
    }

    // ---- Label: identity rebuild must be byte-exact ----
    {
        std::vector<uint8_t> orig = hex(kLabel1024);
        std::vector<uint8_t> out;
        bool ok = rebuild_label(orig.data(), orig.size(), identity, out);
        check(ok, "rebuild_label(identity) returns true");
        check(out == orig, "rebuild_label(identity) is byte-exact with the original label");
    }

    // ---- Translated units: each title/button/body-line core is translated on its
    //      own; colour prefixes and line breaks are re-attached byte-for-byte ----
    {
        std::vector<uint8_t> orig = hex(kDialogLogin);
        Translator es = [&](const std::string& src, std::string& out) {
            // Units arrive as bare cores (the {RRGGBB} prefix is split off).
            if (src == "Log in") out = "Iniciar sesion";
            else if (src.rfind("Please enter", 0) == 0) out = "Introduce la contrasena de esta cuenta (xerohero).";
            else out = src;
            return true;
        };
        std::vector<uint8_t> out;
        bool ok = rebuild_dialog(orig.data(), orig.size(), es, out);
        check(ok, "rebuild_dialog(translate) returns true");
        check(out != orig, "translated rebuild differs from original");

        size_t off = 2 + 1;
        auto skip = [&](std::string& s) { size_t l = out[off++]; s.assign((char*)&out[off], l); off += l; };
        std::string t, b1, b2; skip(t); skip(b1); skip(b2);
        std::string body;
        bool dok = i18n::huffman::decode_string(out.data() + off, out.size() - off, body);
        check(dok, "rebuilt szInfo decodes");
        check(t == "{9e47b5}Iniciar sesion", "title core translated, colour prefix kept");
        check(body == "{ffffff}Introduce la contrasena de esta cuenta (xerohero).\n\n"
                      "If you've forgotten your password, reset it at mikescnr.com/password.",
              "only the translated line changed; prefix, blank line and untouched line byte-identical");
    }

    // ---- Cell-safe dialog units on real captures ----
    {
        const char* fixtures[] = { kDialogLogin, kDlgHelpTopics, kDlgBankList, kDlgGps, kDlgSpawn };
        for (const char* fx : fixtures) {
            std::vector<uint8_t> orig = hex(fx);
            DialogPlan d;
            check(plan_dialog(orig.data(), orig.size(), d), "plan_dialog parses a real capture");
            check(assemble_dialog(d, original_cores(d)) == orig, "plan -> assemble with original cores is byte-exact");
        }
    }
    {   // Help Topics (863): header cells, a translatable topic, a verbatim shortcode
        std::vector<uint8_t> orig = hex(kDlgHelpTopics);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        check(d.style == 5, "863 is TABLIST_HEADERS");
        check(d.units.size() == 7 && d.bodySeps == std::vector<char>{ '\t', '\n', '\t' }, "863 body = 2 rows x 2 cells");
        check(d.units[3].prefix == "{c8c8c8}" && d.units[3].core == "Topic" && d.units[3].translate,
              "header cell: colour split off, label translated");
        check(d.units[5].core == "Help System Overview" && d.units[5].translate, "topic phrase translated");
        check(d.units[6].core == "intro" && !d.units[6].translate, "shortcode 'intro' stays verbatim");
    }
    {   // GPS (416): names translate, distances stay, grey colour per cell kept
        std::vector<uint8_t> orig = hex(kDlgGps);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        check(d.units[5].core == "Las Venturas Bank" && d.units[5].translate, "destination translated");
        check(d.units[6].core == "12m" && !d.units[6].translate, "'12m' verbatim");
        check(d.units[7].prefix == "{666666}" && d.units[7].core == "Palomino Creek Bank", "per-cell grey colour split off");
        check(d.units[8].prefix == "{666666}" && d.units[8].core == "1.15km" && !d.units[8].translate,
              "'1.15km' verbatim, colour kept");
    }
    {   // Bank list (291): single capitalised words ARE translated
        std::vector<uint8_t> orig = hex(kDlgBankList);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        check(d.style == 2, "291 is LIST");
        check(d.units[3].prefix == "{ffffff}" && d.units[3].core == "Account Balance" && d.units[3].translate,
              "first row: colour split off, translated");
        check(d.units[4].core == "Withdraw" && d.units[4].translate, "'Withdraw' (capitalised word) translated");
    }
    {   // A hostile translator that injects separators cannot change the skeleton
        std::vector<uint8_t> orig = hex(kDlgGps);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        std::vector<std::string> cores = original_cores(d);
        for (size_t k = 0; k < d.units.size(); ++k)
            if (d.units[k].translate) cores[k] = "X\tY\nZ\r " + d.units[k].core;
        const std::string before = assemble_body(d, original_cores(d));
        const std::string after  = assemble_body(d, cores);
        auto count = [](const std::string& s, char c) { return (size_t)std::count(s.begin(), s.end(), c); };
        check(count(after, '\t') == count(before, '\t'), "no tab added or lost");
        check(count(after, '\n') == count(before, '\n'), "no row added or lost");
        check(count(after, '\r') == 0, "stray CR dropped");
        check(after.find("{666666}1.15km") != std::string::npos, "verbatim cell and its colour untouched");
        std::vector<uint8_t> pkt = assemble_dialog(d, cores);
        DialogPlan d2;
        check(plan_dialog(pkt.data(), pkt.size(), d2) && d2.units.size() == d.units.size(),
              "rebuilt packet re-parses to the same unit count");
    }

    // ---- never translated: PASSWORD dialogs and CLOSE signals ----
    {
        std::vector<uint8_t> login = hex(kDialogLogin);   // style 3 = password
        DialogPlan pw;
        plan_dialog(login.data(), login.size(), pw);
        check(never_translated(pw), "password dialog is never translated");
        std::vector<uint8_t> help = hex(kDlgHelpTopics);
        DialogPlan normal;
        plan_dialog(help.data(), help.size(), normal);
        check(!never_translated(normal), "a normal tablist is translated");
        DialogPlan close1; close1.id = 0xFFFF;
        DialogPlan close2; close2.id = 0x8000;
        check(never_translated(close1) && never_translated(close2), "ids 0x8000..0xFFFF are CLOSE signals");
    }

    // ---- verbatim-cell rule ----
    check(is_verbatim_cell("intro"), "'intro' shortcode verbatim");
    check(is_verbatim_cell("/help"), "'/help' verbatim");
    check(is_verbatim_cell("cmd_2"), "'cmd_2' verbatim");
    check(is_verbatim_cell("12m") && is_verbatim_cell("1.15km") && is_verbatim_cell("$500"), "numbers/units verbatim");
    check(is_verbatim_cell(""), "empty verbatim");
    check(!is_verbatim_cell("Withdraw"), "'Withdraw' translated");
    check(!is_verbatim_cell("No"), "'No' translated");
    check(!is_verbatim_cell("Las Venturas Bank"), "phrase translated");
    check(!is_verbatim_cell("\xD1\xED\xFF\xF2\xFC"), "cp1251 word translated");
    check(!is_dynamic_text("\xC0\xEA\xF2\xE8\xE2\xED\xFB\xE9 \xE3\xEE\xF0\xEE\xE4"), "cp1251 label is not 'dynamic'");

    // ---- is_dynamic_text: skip timers/counters, translate real labels ----
    check(is_dynamic_text("1h 27m 49s"), "'1h 27m 49s' is dynamic (skip)");
    check(is_dynamic_text("27"), "'27' is dynamic (skip)");
    check(is_dynamic_text("$5000"), "'$5000' is dynamic (skip)");
    check(is_dynamic_text("~r~8:10"), "'~r~8:10' is dynamic (skip)");
    check(is_dynamic_text("_"), "'_' is dynamic (skip)");
    check(!is_dynamic_text("Active city: ~P~San Fierro"), "'Active city: San Fierro' is translatable");
    check(!is_dynamic_text("Players online: ~Y~27"), "'Players online: 27' is translatable");

    // ---- Plain HUD surfaces: identity rebuild is byte-exact ----
    {
        // Real TextDrawSetString (105), id=3 "Active city: ..." (133 bytes).
        std::vector<uint8_t> orig = hex(
            "0300810041637469766520636974793A207E507E53616E2046696572726F7E4E7E7E577E4E657874"
            "20636974793A207E507E4C61732056656E7475726173202831682032376D20343973297E4E7E7E57"
            "7E54696D653A207E477E7E487E546875727364617920383A31307E4E7E7E577E506C6179657273206F"
            "6E6C696E653A207E597E3237");
        std::vector<uint8_t> out;
        check(rebuild_textdraw_setstring(orig.data(), orig.size(), identity, out), "rebuild textdraw105 ok");
        check(out == orig, "textdraw105 identity is byte-exact");
    }
    {
        // Real ShowTextDraw (134), the "_" textdraw (68 bytes).
        std::vector<uint8_t> orig = hex(
            "0008110000000000004842E1E1E1FF0000A0440000A044000000000200000000FF010000000000000000"
            "0000000000000000000000000000000000803FFFFFFFFF01005F");
        std::vector<uint8_t> out;
        check(rebuild_textdraw_show(orig.data(), orig.size(), identity, out), "rebuild textdraw134 ok");
        check(out == orig, "textdraw134 identity is byte-exact");
    }
    {
        // Synthesized gametext (73): style=1, time=5000, "MISSION PASSED".
        std::vector<uint8_t> orig = hex("010000008813" "00000E0000004D495353494F4E20504153534544");
        std::vector<uint8_t> out;
        check(rebuild_gametext(orig.data(), orig.size(), identity, out), "rebuild gametext ok");
        check(out == orig, "gametext identity is byte-exact");
    }
    {
        // Synthesized chat bubble (59): pid=5, "hello".
        std::vector<uint8_t> orig = hex("0500FFFFFFFF00004842B80B00000568656C6C6F");
        std::vector<uint8_t> out;
        check(rebuild_chatbubble(orig.data(), orig.size(), identity, out), "rebuild chatbubble ok");
        check(out == orig, "chatbubble identity is byte-exact");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
