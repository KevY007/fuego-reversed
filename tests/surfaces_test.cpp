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

// A synthetic RPC 61 payload (u16 id, u8 style, 3 x u8-length strings, Huffman body).
static std::vector<uint8_t> make_dialog(uint16_t id, uint8_t style, const std::string& title,
                                        const std::string& b1, const std::string& b2, const std::string& body) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(id & 0xFF));
    v.push_back((uint8_t)(id >> 8));
    v.push_back(style);
    for (const std::string* s : { &title, &b1, &b2 }) {
        v.push_back((uint8_t)s->size());
        v.insert(v.end(), s->begin(), s->end());
    }
    std::vector<uint8_t> enc = i18n::huffman::encode_string(body);
    v.insert(v.end(), enc.begin(), enc.end());
    return v;
}

// Every unit the translator knows gets its translation; the rest keep their core.
static std::vector<std::string> translate_cores(const i18n::surfaces::DialogPlan& d,
                                                const i18n::surfaces::Translator& tr) {
    std::vector<std::string> c = i18n::surfaces::original_cores(d);
    for (size_t k = 0; k < d.units.size(); ++k) {
        std::string t;
        if (d.units[k].translate && tr(d.units[k].core, t)) c[k] = t;
    }
    return c;
}

static std::vector<std::string> split_on(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    return out;
}

// SA:MP's LIST/TABLIST builders, per line of the body: an EMPTY line makes no item;
// a line longer than the reader's 255-byte copy is cut and the rest parsed as
// ANOTHER item. Returns the items each '\n' line becomes (the index structure).
static std::vector<size_t> native_line_items(const std::string& body) {
    std::vector<size_t> v;
    for (const std::string& line : split_on(body, '\n')) v.push_back((line.size() + 254) / 255);
    return v;
}
static size_t native_items(const std::string& body) {
    size_t n = 0;
    for (size_t k : native_line_items(body)) n += k;
    return n;
}
static size_t longest(const std::vector<std::string>& v) {
    size_t m = 0;
    for (const std::string& s : v) if (s.size() > m) m = s.size();
    return m;
}

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

    // ---- DialogResponse remap: translated row -> original row, by index ----
    {
        std::vector<std::string> orig  = { "{ffffff}Account Balance", "Withdraw", "Deposit" };
        std::vector<std::string> trans = { "{ffffff}Saldo de cuenta", "Retirar", "Depositar" };
        std::string out;
        check(remap_list_response(2, orig, trans, 1, "Retirar", out) && out == "Withdraw", "LIST: picked row maps back");
        check(remap_list_response(2, orig, trans, 0, "Saldo de cuenta", out) && out == "Account Balance",
              "LIST: colour-stripped response -> colour-stripped original");
        check(!remap_list_response(2, orig, trans, 5, "x", out), "out-of-range row -> no remap");
        check(!remap_list_response(2, orig, trans, 1, "Something else", out), "unrecognised response -> no remap");
    }
    {
        std::vector<std::string> orig  = { "{c8c8c8}Destination\t{c8c8c8}Distance", "Las Venturas Bank\t12m",
                                           "{666666}Palomino Creek Bank\t{666666}1.15km" };
        std::vector<std::string> trans = { "{c8c8c8}Destino\t{c8c8c8}Distancia", "Banco de Las Venturas\t12m",
                                           "{666666}Banco de Palomino Creek\t{666666}1.15km" };
        std::string out;
        check(remap_list_response(5, orig, trans, 0, "Banco de Las Venturas", out) && out == "Las Venturas Bank",
              "TABLIST_HEADERS: listItem 0 = first data row, first column");
        check(remap_list_response(5, orig, trans, 1, "Banco de Palomino Creek\t1.15km", out) &&
              out == "Palomino Creek Bank\t1.15km", "whole colour-stripped row variant");
        std::vector<std::string> dupO = { "{c8c8c8}H", "Bank A", "Bank B" };
        std::vector<std::string> dupT = { "{c8c8c8}H", "Banco", "Banco" };
        check(remap_list_response(5, dupO, dupT, 1, "Banco", out) && out == "Bank B",
              "two rows with the same translation: the index picks the right one");
    }
    check(strip_colors("{666666}Palomino{ffffff} Bank") == "Palomino Bank", "strip_colors");

    // ---- DialogResponse remap: fix-round-1 hardening ----
    {
        // style 4 = TABLIST (no header row): row == listItem, no +1 offset.
        std::vector<std::string> orig  = { "Alpha\tOne", "Beta\tTwo" };
        std::vector<std::string> trans = { "Alfa\tUno", "Beta2\tDos" };
        std::string out;
        check(remap_list_response(4, orig, trans, 1, "Beta2", out) && out == "Beta",
              "TABLIST (no header): row == listItem, first-column candidate");
    }
    {
        std::vector<std::string> orig  = { "A", "B" };
        std::vector<std::string> trans = { "A" };
        std::string out;
        check(!remap_list_response(2, orig, trans, 0, "A", out), "orig/trans size mismatch -> no remap");
        check(!remap_list_response(2, orig, orig, -1, "A", out), "negative listItem -> no remap");
    }
    {
        // style 5 upper bound: 3 rows total (1 header + 2 data) -> valid listItem is 0..1.
        std::vector<std::string> orig  = { "{c8c8c8}H", "Row0", "Row1" };
        std::vector<std::string> trans = { "{c8c8c8}H", "Fila0", "Fila1" };
        std::string out;
        check(remap_list_response(5, orig, trans, 1, "Fila1", out) && out == "Row1",
              "TABLIST_HEADERS upper bound: last data row (listItem = rows-2) remaps");
        check(!remap_list_response(5, orig, trans, 2, "x", out),
              "TABLIST_HEADERS upper bound: one past the last row (listItem = rows-1) -> no remap");
    }
    {
        // SA:MP didn't strip colour from this particular response -> the raw (uncoloured-stripped) original.
        std::vector<std::string> orig  = { "{ffffff}Account Balance" };
        std::vector<std::string> trans = { "{ffffff}Saldo de cuenta" };
        std::string out;
        check(remap_list_response(2, orig, trans, 0, "{ffffff}Saldo de cuenta", out) &&
              out == "{ffffff}Account Balance", "raw-colour response -> raw original");
    }
    {
        // Blank-row guard: unverified whether SA:MP counts a blank line as a list item,
        // so a blank row before the picked one must block the remap entirely.
        std::vector<std::string> orig  = { "A", "", "B" };
        std::vector<std::string> trans = { "X", "", "Y" };
        std::string out;
        check(!remap_list_response(2, orig, trans, 2, "Y", out), "blank row before the picked one -> no remap");
    }

    // ---- should_translate_input: single tokens (passwords/emails/amounts) never leave ----
    check(!should_translate_input(""), "should_translate_input: empty -> false");
    check(!should_translate_input("/help me"), "should_translate_input: command -> false");
    check(!should_translate_input("12345"), "should_translate_input: digits-only -> false");
    check(!should_translate_input("qwerty123"), "should_translate_input: single token -> false");
    check(!should_translate_input("john@mail.com"), "should_translate_input: email (single token) -> false");
    check(should_translate_input("hello there"), "should_translate_input: a real phrase -> true");
    check(should_translate_input("deposit\tmoney"), "should_translate_input: tab-separated phrase -> true");

    // ---- C1: a translation must never change a list's ITEM COUNT / row indices ----
    {   // Real LIST (291): one row translates to 300 bytes, another to "".
        std::vector<uint8_t> orig = hex(kDlgBankList);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        Translator tr = [](const std::string& src, std::string& out) {
            if (src == "Account Balance") out = "Saldo de cuenta";
            else if (src == "Withdraw") out = std::string(300, 'W');   // longer than the 255-byte line reader
            else if (src == "Deposit") out = "";                        // an empty line is no item at all
            else if (src == "Recent Transactions") out = "Transacciones recientes";
            else return false;
            return true;
        };
        const std::vector<std::string> cores = translate_cores(d, tr);
        const std::string origBody = assemble_body(d, original_cores(d));
        check(native_line_items(assemble_body(d, cores)) != native_line_items(origBody),
              "C1 fixture: unfitted, the translation really shifts SA:MP's items");
        const std::vector<std::string> fit = fit_list_budgets(d, cores);
        check(fit.size() == cores.size(), "C1 list: same number of cores");
        check(fit[4] == "Withdraw", "C1 list: the 300-byte row is reverted to its original");
        check(fit[5] == "Deposit", "C1 list: the empty row is reverted to its original");
        bool othersKept = true;
        for (size_t k = 0; k < fit.size(); ++k)
            if (k != 4 && k != 5 && fit[k] != cores[k]) othersKept = false;
        check(othersKept, "C1 list: every other translated row is kept");
        const std::string body = assemble_body(d, fit);
        check(native_items(body) == native_items(origBody), "C1 list: same item count as the original, the SA:MP way");
        check(native_line_items(body) == native_line_items(origBody), "C1 list: every row keeps its native index");
        check(longest(split_on(body, '\n')) <= 255, "C1 list: every line fits the 255-byte line reader");
    }
    {   // Real TABLIST_HEADERS (416): a 200-byte cell fits the line but not the 127-byte cell reader.
        std::vector<uint8_t> orig = hex(kDlgGps);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        Translator tr = [](const std::string& src, std::string& out) {
            if (src == "Destination") out = "Destino";
            else if (src == "Las Venturas Bank") out = std::string(200, 'L');
            else if (src == "Palomino Creek Bank") out = "Banco de Palomino Creek";
            else return false;
            return true;
        };
        const std::vector<std::string> cores = translate_cores(d, tr);
        const std::vector<std::string> fit = fit_list_budgets(d, cores);
        check(fit[5] == "Las Venturas Bank" && fit[6] == "12m", "C1 tablist: the line with a 200-byte cell is reverted");
        check(fit[3] == "Destino", "C1 tablist: the header's translated cell is kept");
        check(fit[7] == "Banco de Palomino Creek", "C1 tablist: the next row's translation is kept");
        const std::string body = assemble_body(d, fit);
        const std::string origBody = assemble_body(d, original_cores(d));
        check(native_line_items(body) == native_line_items(origBody), "C1 tablist: every row keeps its native index");
        size_t widest = 0;
        for (const std::string& line : split_on(body, '\n'))
            if (longest(split_on(line, '\t')) > widest) widest = longest(split_on(line, '\t'));
        check(widest <= 127, "C1 tablist: every cell fits the 127-byte cell reader");
    }
    {   // Real TABLIST_HEADERS (1006): the style-5 HEADER line is budgeted too.
        std::vector<uint8_t> orig = hex(kDlgSpawn);
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        check(d.units.size() > 4 && d.units[3].core == "Zone", "1006 header cell is 'Zone'");
        Translator tr = [](const std::string& src, std::string& out) {
            if (src == "Zone") out = std::string(300, 'Z');
            else if (src == "Las Venturas Airport") out = "Aeropuerto de Las Venturas";
            else return false;
            return true;
        };
        const std::vector<std::string> cores = translate_cores(d, tr);
        const std::vector<std::string> fit = fit_list_budgets(d, cores);
        check(fit[3] == "Zone", "C1 header: an oversized header line is reverted");
        check(fit[4] == "Aeropuerto de Las Venturas", "C1 header: the first data row's translation is kept");
    }
    {   // An ORIGINAL line already longer than 255 bytes is not reverted just for being long.
        std::string longRow;
        while (longRow.size() < 300) longRow += "Please read the server rules ";
        const size_t origLen = longRow.size();                                  // 319: two native items
        std::vector<uint8_t> pkt = make_dialog(7, 2, "Rules", "OK", "", longRow + "\nWithdraw");
        DialogPlan d;
        check(plan_dialog(pkt.data(), pkt.size(), d) && d.units.size() == 5, "long-row fixture parses");
        std::string sameItems(origLen + 10, 'r');                              // 329: still two items
        std::vector<std::string> cores = original_cores(d);
        cores[3] = sameItems;
        cores[4] = "Retirar";
        std::vector<std::string> fit = fit_list_budgets(d, cores);
        check(fit[3] == sameItems && fit[4] == "Retirar",
              "C1 long original: a translation cut into the same number of items is kept");
        check(native_line_items(assemble_body(d, fit)) == native_line_items(assemble_body(d, original_cores(d))),
              "C1 long original: row indices stay native");
        cores[3] = std::string(600, 'r');                                       // three items
        fit = fit_list_budgets(d, cores);
        check(fit[3] == d.units[3].core && fit[4] == "Retirar",
              "C1 long original: a translation cut into MORE items is reverted");
        cores[3] = std::string(100, 'r');                                       // one item
        fit = fit_list_budgets(d, cores);
        check(fit[3] == d.units[3].core, "C1 long original: a translation cut into FEWER items is reverted");
    }
    {   // Whitespace-only translation of a non-blank row -> reverted; other styles are untouched.
        std::vector<uint8_t> pkt = make_dialog(8, 2, "Menu", "OK", "", "Withdraw\nDeposit");
        DialogPlan d;
        plan_dialog(pkt.data(), pkt.size(), d);
        std::vector<std::string> cores = original_cores(d);
        cores[3] = "   ";
        check(fit_list_budgets(d, cores)[3] == "Withdraw", "C1: a whitespace-only translation is reverted");

        std::vector<uint8_t> msg = make_dialog(9, 0, "Info", "OK", "", "Hello there friend\nSecond line here");
        DialogPlan m;
        plan_dialog(msg.data(), msg.size(), m);
        std::vector<std::string> mc = original_cores(m);
        mc[3] = std::string(300, 'h');
        mc[4] = "";
        check(fit_list_budgets(m, mc) == mc, "C1: a MSGBOX (style 0) is never touched");

        std::vector<uint8_t> tab = make_dialog(10, 4, "Shop", "Buy", "Close", "Big Burger\t$5\nSmall Fries\t$2");
        DialogPlan t;
        plan_dialog(tab.data(), tab.size(), t);
        std::vector<std::string> tc = original_cores(t);
        tc[3] = std::string(150, 'b');
        tc[5] = "Patatas";
        std::vector<std::string> tf = fit_list_budgets(t, tc);
        check(tf[3] == "Big Burger" && tf[5] == "Patatas", "C1: a TABLIST (style 4) cell over 127 bytes is reverted");
    }
    {   // I5: missing units with an IDENTICAL core share ONE translation job.
        std::vector<uint8_t> pkt = make_dialog(11, 2, "Shop", "Buy", "Close", "Buy\nSell\nBuy\n{ffffff}Buy");
        DialogPlan d;
        plan_dialog(pkt.data(), pkt.size(), d);
        const std::vector<size_t> missing = { 1, 2, 3, 4, 5, 6 };  // "Buy" "Close" "Buy" "Sell" "Buy" "Buy"
        const std::vector<std::vector<size_t>> g = group_by_core(d, missing);
        check(g.size() == 3, "I5: 6 missing units with 3 distinct cores -> 3 jobs");
        check(g.size() == 3 && g[0] == std::vector<size_t>({ 1, 3, 5, 6 }) && g[1] == std::vector<size_t>({ 2 }) &&
              g[2] == std::vector<size_t>({ 4 }),
              "I5: groups in first-seen order; a colour prefix doesn't split an identical core");
        check(group_by_core(d, {}).empty(), "I5: nothing missing -> no jobs");
    }
    {   // M9: a translated BUTTON label lands in its button field (units[1]/[2]).
        std::vector<uint8_t> orig = hex(kDlgBankList); // buttons "Select" / "Close"
        DialogPlan d;
        plan_dialog(orig.data(), orig.size(), d);
        check(d.units[1].core == "Select" && d.units[2].core == "Close", "291 buttons are Select / Close");
        Translator tr = [](const std::string& src, std::string& out) {
            if (src == "Select") out = "Seleccionar";
            else if (src == "Close") out = "Cerrar";
            else out = src;
            return true;
        };
        std::vector<uint8_t> pkt;
        check(rebuild_dialog(orig.data(), orig.size(), tr, pkt), "M9: rebuild with translated buttons");
        size_t off = 3; // u16 id, u8 style, then u8-length title, button 1, button 2
        auto field = [&]() {
            const size_t l = pkt[off++];
            std::string s((const char*)&pkt[off], l);
            off += l;
            return s;
        };
        const std::string title = field(), b1 = field(), b2 = field();
        check(b1 == "Seleccionar", "M9: the translated button 1 is in the button-1 field");
        check(b2 == "Cerrar", "M9: the translated button 2 is in the button-2 field");
        check(title == unit_text(d.units[0], d.units[0].core), "M9: the title field is untouched");
        std::string body;
        check(i18n::huffman::decode_string(pkt.data() + off, pkt.size() - off, body) &&
              body == assemble_body(d, original_cores(d)), "M9: the body is byte-identical");
    }
    {   // M3: SA:MP's response text is the picked row (LIST) / its first cell (TABLIST)
        // copied with strncpy(.., 0x80), then colour-stripped. At that cap the text was CUT.
        const std::string longT(200, 't');
        std::vector<std::string> orig  = { "Withdraw", std::string(200, 'o') };
        std::vector<std::string> trans = { longT, std::string(180, 'u') };
        std::string out;
        check(remap_list_response(2, orig, trans, 0, longT.substr(0, 127), out) && out == "Withdraw",
              "M3: a response cut at the native cap matches its row by prefix");
        check(remap_list_response(2, orig, trans, 1, std::string(127, 'u'), out) && out == std::string(127, 'o'),
              "M3: the chosen original is cut to the response's length");
        check(remap_list_response(2, orig, trans, 0, longT.substr(0, 128), out) && out == "Withdraw",
              "M3: a 128-byte response (the other reading of the 0x80 copy) matches too");
        check(!remap_list_response(2, orig, trans, 0, std::string(127, 'x'), out),
              "M3: a cut response that isn't a prefix of the row -> no remap");
        // Below the cap: exact matching only.
        std::vector<std::string> o2 = { "Withdraw money" };
        std::vector<std::string> t2 = { "Retirar dinero" };
        check(!remap_list_response(2, o2, t2, 0, "Retirar", out), "M3: below the cap a mere prefix is NOT a match");
        check(remap_list_response(2, o2, t2, 0, "Retirar dinero", out) && out == "Withdraw money",
              "M3: below the cap exact matching still works");
        // TABLIST: the response is the FIRST CELL only, so the original's first cell is sent.
        std::vector<std::string> o3 = { "{c8c8c8}H1\t{c8c8c8}H2", "Las Venturas Bank\t12m" };
        std::vector<std::string> t3 = { "{c8c8c8}H1\t{c8c8c8}H2", std::string(150, 'L') + "\t12m" };
        check(remap_list_response(5, o3, t3, 0, std::string(127, 'L'), out) && out == "Las Venturas Bank",
              "M3: a tablist first cell at the cap maps back to the original's first cell only");
        // SA:MP cuts BEFORE it strips colours: the original is cut the same way.
        std::vector<std::string> o4 = { "{ffffff}" + std::string(200, 'o') };
        std::vector<std::string> t4 = { longT };
        check(remap_list_response(2, o4, t4, 0, longT.substr(0, 127), out) && out == std::string(119, 'o'),
              "M3: the original is cut at the response length, then colour-stripped (native order)");
    }
    {   // remap: a data row before the picked one that isn't exactly ONE native item
        // (longer than the 255-byte reader) shifts SA:MP's indices -> never remap.
        std::vector<std::string> orig  = { std::string(300, 'a'), "Bank B", "Bank C" };
        std::vector<std::string> trans = { std::string(300, 'x'), "Banco", "Banco" };
        std::string out;
        check(!remap_list_response(2, orig, trans, 2, "Banco", out),
              "remap: a >255-byte row before the pick shifts the index -> no remap (never a wrong row)");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
