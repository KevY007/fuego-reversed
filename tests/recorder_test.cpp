// Host test for rec::Timeline -- the packet recorder's decoding + noise control.
//
// Build/run: tests\run_recorder_test.cmd

#include "recorder/timeline.h"
#include "i18n/huffman.h"
#include "samp/structs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

// Writes the way RakNet's BitStream does: bits MSB-first in each byte, values
// little-endian (also after a 1-bit bool), bools are ONE bit.
struct W {
    std::vector<uint8_t> b;
    size_t bits = 0;
    W& bit(bool v) {
        if (bits % 8 == 0) b.push_back(0);
        if (v) b.back() |= (uint8_t)(0x80 >> (bits % 8));
        ++bits;
        return *this;
    }
    W& raw(const void* p, size_t n) {
        const uint8_t* s = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i)
            for (int k = 7; k >= 0; --k) bit(((s[i] >> k) & 1) != 0);
        return *this;
    }
    template <class T> W& put(T v) { return raw(&v, sizeof v); }
    W& s8(const std::string& s)  { put<uint8_t>((uint8_t)s.size());   return raw(s.data(), s.size()); }
    W& s16(const std::string& s) { put<uint16_t>((uint16_t)s.size()); return raw(s.data(), s.size()); }
    W& s32(const std::string& s) { put<uint32_t>((uint32_t)s.size()); return raw(s.data(), s.size()); }
};

using Lines = std::vector<std::string>;
static bool has(const Lines& lines, const char* needle) {
    for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}
static std::string dump(const Lines& lines) {
    std::string s;
    for (const auto& l : lines) { s += "      | "; s += l; s += "\n"; }
    return s;
}
// check + print what came out when it fails
static void expect(const Lines& lines, const char* needle, const char* what) {
    const bool ok = has(lines, needle);
    check(ok, what);
    if (!ok) std::printf("      wanted: %s\n%s", needle, dump(lines).c_str());
}

static Lines in(rec::Timeline& t, uint32_t now, int id, const W& w) {
    Lines out; t.incoming_rpc(now, id, w.b.data(), w.bits, out); return out;
}
static Lines out_rpc(rec::Timeline& t, uint32_t now, int id, const W& w, bool injected = false) {
    Lines out; t.outgoing_rpc(now, id, w.b.data(), w.bits, injected, out); return out;
}
static Lines packet(rec::Timeline& t, uint32_t now, uint8_t id, const void* data, size_t n, bool injected = false) {
    W w; w.put<uint8_t>(id); w.raw(data, n);
    Lines out; t.outgoing_packet(now, w.b.data(), w.bits, injected, out); return out;
}

static samp::stOnFootData foot(uint16_t keys, uint8_t weapon = 0, int16_t anim = 1189) {
    samp::stOnFootData d{};
    d.sKeys = keys;
    d.fPosition[0] = 100.0f; d.fPosition[1] = -200.0f; d.fPosition[2] = 3.0f;
    d.fQuaternion[0] = 1.0f;
    d.byteHealth = 100;
    d.byteCurrentWeapon = weapon;
    d.sCurrentAnimationID = anim;
    return d;
}

// ShowTextDraw (134) laid out as the server sends it.
static W textdraw(uint16_t id, float sizeX, const std::string& text, uint8_t flags = 1) {
    W w;
    w.put<uint16_t>(id).put<uint8_t>(flags)
     .put<float>(0.0f).put<float>(4.5f)          // letter size
     .put<uint32_t>(0xFFFFFFFFu)                  // letter colour
     .put<float>(sizeX).put<float>(0.0f)          // text size (box right edge for a bar)
     .put<uint32_t>(0x00FF00FFu)                  // box colour
     .put<uint8_t>(0).put<uint8_t>(1)             // shadow, outline
     .put<uint32_t>(0x000000FFu)                  // background
     .put<uint8_t>(1).put<uint8_t>(0)             // font, selectable
     .put<float>(500.0f).put<float>(360.0f)       // position
     .put<uint16_t>(0)                            // model
     .put<float>(0).put<float>(0).put<float>(0).put<float>(1.0f) // rot + zoom
     .put<int16_t>(-1).put<int16_t>(-1)           // model colours
     .s16(text);
    return w;
}

int main() {
    using rec::Timeline;

    {   // 1. Bit reader: 1-bit bools shift what follows; values stay little-endian.
        W w; w.bit(true).bit(false).bit(true).put<uint32_t>(0x11223344u).put<uint16_t>(0xBEEF);
        rec::BitReader r(w.b.data(), w.bits);
        bool a = false, b = true, c = false; uint32_t v = 0; uint16_t h = 0;
        check(r.bit(a) && r.bit(b) && r.bit(c) && a && !b && c, "bits come MSB-first");
        check(r.get(v) && v == 0x11223344u, "a u32 after 3 bits reads back");
        check(r.get(h) && h == 0xBEEF, "then a u16");
        check(!r.get(h), "nothing left -> a read fails, never runs past the end");
        rec::BitReader none(nullptr, 64);
        check(!none.bit(a), "no payload -> nothing to read");
    }
    {   // 2. Key names + quoting.
        check(rec::key_names(0) == "none", "no keys -> none");
        check(rec::key_names(4 | 128) == "FIRE(attack)+HANDBRAKE(aim)", "fire + aim, named as a script tests them");
        check(rec::key_names(8) == "SPRINT(space)", "space = KEY_SPRINT on foot");
        check(rec::key_names(8, false) == "SPRINT", "in a vehicle: no on-foot key name");
        check(rec::key_names(65536) == "YES(Y)", "KEY_YES");
        check(rec::quote("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"", "quotes, backslashes, newlines escaped");
        check(rec::quote(std::string("\x01\xE9", 2)) == "\"\\x01\\xE9\"", "other bytes as \\xNN");
        check(rec::quote(std::string(10, 'x'), 4) == "\"xxxx...(+6)\"", "long text is cut with a count");
    }

    Timeline t;
    t.reset(0);
    t.set_local_id(7);

    {   // 3. Game text, chat, dialogs.
        W gt; gt.put<uint32_t>(3).put<uint32_t>(2000).s32("~g~Something is biting!");
        expect(in(t, 1500, 73, gt), "IN  GameText style=3 time=2000ms \"~g~Something is biting!\"", "game text: style, time, text");
        expect(in(t, 1500, 73, gt), "1.500 ", "lines are stamped with seconds since the start");

        W cm; cm.put<uint32_t>(0x33AA33FFu).s32("You caught a Bass (3.2 kg)!");
        expect(in(t, 2000, 93, cm), "IN  Chat color=33AA33FF \"You caught a Bass (3.2 kg)!\"", "client message");

        W dl; dl.put<int16_t>(12).put<uint8_t>(2).s8("Fish shop").s8("Sell").s8("Close");
        const std::vector<uint8_t> body = i18n::huffman::encode_string("Bass\tx3\nTrout\tx1");
        dl.raw(body.data(), body.size());
        Lines d = in(t, 2500, 61, dl);
        expect(d, "IN  Dialog id=12 style=2(list) title=\"Fish shop\" buttons=\"Sell\",\"Close\"", "dialog header");
        expect(d, "body=\"Bass\\tx3\\nTrout\\tx1\"", "dialog body (compressed on the wire) decoded");

        W pw; pw.put<int16_t>(1).put<uint8_t>(3).s8("Login").s8("OK").s8("Quit");
        const std::vector<uint8_t> pbody = i18n::huffman::encode_string("Enter your password");
        pw.raw(pbody.data(), pbody.size());
        Lines p = in(t, 2600, 61, pw);
        expect(p, "style=3(password)", "password dialog shown");
        expect(p, "body=\"Enter your password\"", "a password dialog is written in full (nothing hidden by default)");
        Timeline h; h.reset(0); rec::Options o; o.hideSecrets = true; h.set_options(o);
        Lines hp = in(h, 2600, 61, pw);
        check(!has(hp, "Enter your password") && has(hp, "body hidden"), "hide-secrets mode: its body isn't written");
    }
    {   // 4. Textdraws: a progress bar is a box re-shown with a new size.
        Lines a = in(t, 3000, 134, textdraw(2049, 500.0f, "_"));
        expect(a, "IN  TD show #2049(player 1) font=1 box", "player textdraw shown");
        expect(a, "size=(500.0,0.0)", "its box size");
        Lines b = in(t, 3100, 134, textdraw(2049, 530.0f, "_"));
        expect(b, "changed: size.x 500.0->530.0", "re-shown bigger: the change is spelled out");
        check(in(t, 3200, 134, textdraw(2049, 530.0f, "_")).empty(),
              "re-shown unchanged: not written (counted in the summary)");

        W s; s.put<uint16_t>(2050).s16("~r~Tension: 80%");
        expect(in(t, 3300, 105, s), "IN  TD set #2050(player 2) \"~r~Tension: 80%\"", "textdraw text set");
        check(in(t, 3350, 105, s).empty(), "the same text again isn't written (counted in the summary)");
        W s2; s2.put<uint16_t>(2050).s16("~r~Tension: 85%");
        expect(in(t, 3400, 105, s2), "\"~r~Tension: 85%\"", "a new text is written");

        W h; h.put<uint16_t>(2049);
        expect(in(t, 3500, 135, h), "IN  TD hide #2049(player 1)", "textdraw hidden");
        // The next attempt shows the very same bar again: that must be written.
        Lines again = in(t, 3600, 134, textdraw(2049, 530.0f, "_"));
        expect(again, "IN  TD show #2049(player 1)", "shown again after a hide: written");
        expect(again, "(shown again, same as last show)", "... and said to be the same as before");
        check(in(t, 3700, 134, textdraw(2049, 530.0f, "_")).empty(), "while it stays up, an unchanged re-show is counted only");
    }
    {   // 5. Animations + attached objects: only ours; the bools are single bits.
        W an; an.put<uint16_t>(7).s8("SWORD").s8("sword_4").put<float>(4.1f)
                .bit(false).bit(true).bit(true).bit(false).put<uint32_t>(1500);
        expect(in(t, 4000, 86, an), "IN  Anim me \"SWORD\" \"sword_4\" delta=4.10 loop=0 lock=1,1 freeze=0 time=1500",
               "our animation, fields after the 1-bit bools read right");
        W other; other.put<uint16_t>(9).s8("PED").s8("WALK_civi").put<float>(4.1f)
                .bit(true).bit(true).bit(true).bit(false).put<uint32_t>(0);
        check(in(t, 4000, 86, other).empty(), "another player's animation isn't written");

        W att; att.put<uint16_t>(7).put<uint32_t>(3).bit(true).put<int32_t>(18632).put<uint32_t>(6);
        for (int i = 0; i < 9; ++i) att.put<float>(i < 6 ? 0.5f : 1.0f);
        att.put<uint32_t>(0).put<uint32_t>(0);
        expect(in(t, 4100, 113, att), "IN  AttachedObject me slot=3 model=18632 bone=6", "fishing-rod-like attached object");
        W det; det.put<uint16_t>(7).put<uint32_t>(3).bit(false);
        expect(in(t, 4200, 113, det), "IN  AttachedObject me slot=3 removed", "attached object removed");

        W cl; cl.put<uint16_t>(7);
        expect(in(t, 4300, 87, cl), "IN  ClearAnim me", "our animation cleared");

        // A server that keeps resetting our chat bubble to blank (Mike's CnR: ~2 a second).
        W bb; bb.put<uint16_t>(7).put<uint32_t>(0).put<float>(0.0f).put<uint32_t>(0).s8(" ");
        check(in(t, 4310, 59, bb).empty(), "a blank bubble on us isn't written");
        W bt; bt.put<uint16_t>(7).put<uint32_t>(0xFFFFFFFFu).put<float>(20.0f).put<uint32_t>(3000).s8("*reels in*");
        expect(in(t, 4320, 59, bt), "IN  Bubble me", "a bubble with text on us is written");
    }
    {   // 6. Weapons, freeze, sounds, health.
        W gw; gw.put<uint32_t>(15).put<uint32_t>(1);
        expect(in(t, 5000, 22, gw), "IN  GiveWeapon 15(cane) ammo=1", "weapon given");
        W tc; tc.put<uint8_t>(0);
        expect(in(t, 5100, 15, tc), "IN  Controllable 0(frozen)", "frozen");
        W snd; snd.put<uint32_t>(1056).put<float>(0).put<float>(0).put<float>(0);
        expect(in(t, 5200, 16, snd), "IN  Sound 1056", "sound");
        W hp; hp.put<float>(85.0f);
        expect(in(t, 5300, 14, hp), "IN  Health 85.0", "health set");
    }
    {   // 7. Our commands and dialog replies: everything written by default -- the full
        //    command, what was typed (passwords too), the row picked in a list.
        W c; c.s32("/fish");
        expect(out_rpc(t, 6000, 50, c), "OUT cmd \"/fish\"", "command");
        W lg; lg.s32("/login hunter2");
        expect(out_rpc(t, 6100, 50, lg), "OUT cmd \"/login hunter2\"", "a login command, in full");
        W dr; dr.put<int16_t>(12).put<uint8_t>(1).put<int16_t>(1).s8("Trout\tx1");
        Lines r = out_rpc(t, 6200, 62, dr);
        expect(r, "OUT DialogResponse id=12 button=1(left) list=1", "dialog response");
        expect(r, "row \"Trout\\tx1\"", "the row picked in the list, from the dialog's body");
        expect(r, "input=\"Trout\\tx1\"", "what came back as input");
        W pr; pr.put<int16_t>(1).put<uint8_t>(1).put<int16_t>(-1).s8("hunter2");
        expect(out_rpc(t, 6210, 62, pr), "input=\"hunter2\"", "a password dialog reply, in full");
        W ch; ch.s8("anyone selling bait?");
        expect(out_rpc(t, 6300, 101, ch), "OUT chat \"anyone selling bait?\"", "chat");
        W cc; cc.put<uint8_t>(2).put<uint32_t>(0x00401000u).put<uint8_t>(0x7F);
        expect(out_rpc(t, 6400, 103, cc), "OUT ClientCheck type=2 address=00401000 answer=7F", "anti-cheat answer decoded");
        W cj; cj.put<int32_t>(4057).put<uint8_t>(1).s8("KevY007").put<uint32_t>(0x1234u).s8("ABCDEF0123").s8("0.3.7-R5");
        expect(out_rpc(t, 6410, 25, cj), "OUT ClientJoin version=4057 mod=1 name=\"KevY007\"", "the join packet decoded");
        // A cut-off command is shown as far as it goes.
        W cut; cut.put<uint32_t>(40); cut.raw("/login hunter2", 14);
        expect(out_rpc(t, 6510, 50, cut), "cut off?", "a cut-off command is marked");
    }
    {   // 7h. Hide-secrets mode (a log to share): login-type arguments, typed dialog input,
        //     RCON, the join packet and anti-cheat answers stay out -- never even hex-dumped.
        Timeline h;
        h.reset(0);
        rec::Options o; o.hideSecrets = true; h.set_options(o);
        W dl; dl.put<int16_t>(12).put<uint8_t>(2).s8("Fish shop").s8("Sell").s8("Close");
        const std::vector<uint8_t> body = i18n::huffman::encode_string("Bass\tx3\nTrout\tx1");
        dl.raw(body.data(), body.size());
        Lines ignore1 = in(h, 10, 61, dl);
        W pw; pw.put<int16_t>(1).put<uint8_t>(3).s8("Login").s8("OK").s8("Quit");
        const std::vector<uint8_t> pbody = i18n::huffman::encode_string("Enter your password");
        pw.raw(pbody.data(), pbody.size());
        Lines ignore2 = in(h, 20, 61, pw);

        W lg; lg.s32("/login hunter2");
        Lines l = out_rpc(h, 6100, 50, lg);
        check(has(l, "\"/login\"") && !has(l, "hunter2"), "login arguments hidden");
        W lg2; lg2.s32("/CHANGEPASS old new");
        check(!has(out_rpc(h, 6100, 50, lg2), "old"), "any case of a password command hidden");
        W dr; dr.put<int16_t>(12).put<uint8_t>(1).put<int16_t>(3).s8("secret");
        Lines r = out_rpc(h, 6200, 62, dr);
        expect(r, "OUT DialogResponse id=12 button=1(left) list=3 input=<6 chars hidden>", "dialog response");
        check(!has(r, "secret"), "dialog input not written");
        W rc; rc.s32("login pass");
        check(!has(out_rpc(h, 6400, 103, rc), "pass"), "client-check answer hidden");
        const char* logins[] = { "/alogin hunter2", "/l hunter2", "/AdminLogin hunter2", "/rcon login hunter2",
                                 "/pw hunter2", "/setpin hunter2", "/2fa hunter2", "/pw:hunter2", "/login\thunter2" };
        for (const char* cmd : logins) {
            W w; w.s32(cmd);
            Lines out = out_rpc(h, 6500, 50, w);
            check(has(out, "<arguments hidden>") && !has(out, "hunter2"), cmd);
        }
        W cut; cut.put<uint32_t>(40); cut.raw("/login hunter2", 14);
        Lines cutOut = out_rpc(h, 6510, 50, cut);
        check(has(cutOut, "not shown") && !has(cutOut, "68 75 6E") && !has(cutOut, "hunter2"),
              "a cut-off command is not dumped");
        W cutChat; cutChat.put<uint8_t>(30).raw("my pass is", 10);
        check(!has(out_rpc(h, 6520, 101, cutChat), "70 61 73"), "a cut-off chat line is not dumped");
        W pr; pr.put<int16_t>(1).put<uint8_t>(1).put<int16_t>(-1).s8("hunter2");
        Lines prOut = out_rpc(h, 6600, 62, pr);
        check(has(prOut, "input=<hidden>") && !has(prOut, "7 chars"), "password dialog: not even the length");
        W ur; ur.put<int16_t>(77).put<uint8_t>(1).put<int16_t>(-1).s8("abc");
        check(has(out_rpc(h, 6610, 62, ur), "input=<hidden>"), "a dialog never seen: no length either");
        uint8_t rcon[1 + 4 + 12] = { 201, 12, 0, 0, 0 };
        std::memcpy(rcon + 5, "login secret", 12);
        Lines rco;
        h.outgoing_packet(6700, rcon, sizeof rcon * 8, false, rco);
        check(has(rco, "RCON") && !has(rco, "secret"), "RCON hidden");
        W cj; cj.put<int32_t>(4057).put<uint8_t>(1).s8("KevY007").put<uint32_t>(0x1234u).s8("ABCDEF0123").s8("0.3.7-R5");
        check(!has(out_rpc(h, 6710, 25, cj), "ABCDEF"), "the join packet's auth key hidden");
        rec::Options both; both.hideSecrets = true; both.everything = true; h.set_options(both);
        W lg3; lg3.s32("/login hunter2");
        Lines l3 = out_rpc(h, 6800, 50, lg3);
        check(has(l3, "\"/login\"") && !has(l3, "raw") && !has(l3, "68 75 6E"), "everything + hide-secrets: no raw bytes of what was typed");
    }
    {   // 7b. Everyone's chat is written; other players' cars only counted; ours written.
        W pc; pc.put<uint16_t>(9).s8("hi all");
        expect(in(t, 6700, 101, pc), "IN  PlayerChat player 9 \"hi all\"", "another player's chat is written (it's in the chat box)");
        W mine; mine.put<uint16_t>(7).s8("selling fish");
        expect(in(t, 6710, 101, mine), "IN  PlayerChat me \"selling fish\"", "our own chat line as the server relays it");
        W ev; ev.put<uint16_t>(9).put<uint16_t>(33).put<uint8_t>(0);
        check(in(t, 6720, 26, ev).empty(), "another player entering a car isn't written");
        W xv; xv.put<uint16_t>(9).put<uint16_t>(33);
        check(in(t, 6730, 154, xv).empty(), "another player leaving a car isn't written");

        W st; st.bit(true).put<uint32_t>(0xFF0000FFu);
        expect(in(t, 6740, 83, st), "IN  SelectTextDraw on hover=FF0000FF", "textdraw selection: a 1-bit switch, then the colour");
        W skDL; skDL.put<uint16_t>(7).put<uint32_t>(101).put<uint32_t>(0);
        expect(in(t, 6750, 153, skDL), "IN  SetSkin 101", "our skin (0.3.DL layout)");
        W sk37; sk37.put<uint32_t>(7).put<uint32_t>(102);
        expect(in(t, 6760, 153, sk37), "IN  SetSkin 102", "our skin (0.3.7 layout, in compat mode)");
    }
    {   // 8. Our keys: press / release with the held time; changes only.
        Timeline k;
        k.reset(1000);
        auto f0 = foot(0);
        Lines a = packet(k, 1000, 207, &f0, sizeof f0);
        expect(a, "OUT foot (start)", "first on-foot sync: the starting state");
        expect(a, "wpn=0(fists)", "weapon");
        check(packet(k, 1030, 207, &f0, sizeof f0).empty(), "an unchanged sync isn't written");

        auto f1 = foot(128, 15);
        Lines b = packet(k, 1100, 207, &f1, sizeof f1);
        expect(b, "+HANDBRAKE(aim)", "aim pressed");
        expect(b, "wpn 0->15(cane)", "weapon change spelled out");
        auto f2 = foot(128 | 4, 15);
        expect(packet(k, 1200, 207, &f2, sizeof f2), "+FIRE(attack)", "attack pressed");
        auto f3 = foot(128, 15);
        expect(packet(k, 1712, 207, &f3, sizeof f3), "-FIRE(attack) held 512ms", "attack released: how long it was held");
        expect(packet(k, 1800, 207, &f0, sizeof f0), "-HANDBRAKE(aim) held 700ms", "aim released");
        check(k.keys_now() == "none", "overlay: no keys now");

        auto f4 = foot(8, 15 | (1 << 6)); // weapon byte: weapon | KEY_YES in the top bits
        Lines c = packet(k, 1900, 207, &f4, sizeof f4);
        expect(c, "+SPRINT(space)", "space pressed");
        expect(c, "+YES(Y)", "KEY_YES from the weapon byte's top bits");
        expect(c, "wpn=15(cane)", "the weapon itself is the low 6 bits");
        check(k.keys_now() == "SPRINT(space)+YES(Y)", "overlay: keys now");

        auto f5 = foot(8, 15, 1167);
        expect(packet(k, 2000, 207, &f5, sizeof f5), "anim 1189->1167", "animation change");

        Lines inj = packet(k, 2100, 207, &f0, sizeof f0, true);
        expect(inj, "OUT [fuego] foot", "a packet a cheat sent itself is tagged");

        samp::stAimData aim{};
        aim.byteCamMode = 53;
        aim.byteWeaponState = 2;
        expect(packet(k, 2200, 203, &aim, sizeof aim), "OUT aim cam=53(aiming) wstate=2", "aim sync camera mode");
        check(packet(k, 2230, 203, &aim, sizeof aim).empty(), "unchanged aim sync not written");

        uint8_t shortFoot[10] = {};
        check(packet(k, 2300, 207, shortFoot, sizeof shortFoot).empty(), "a cut-off sync isn't decoded (and doesn't crash)");
    }
    {   // 9. Objects, labels and pickups near us only; changes only for those written.
        Timeline o;
        o.reset(0);
        o.set_local_id(7);
        auto f = foot(0);
        Lines ignore = packet(o, 10, 207, &f, sizeof f); // we're at (100, -200, 3)
        W nearObj; nearObj.put<uint16_t>(40).put<int32_t>(1600).put<float>(110.0f).put<float>(-205.0f).put<float>(1.0f);
        for (int i = 0; i < 4; ++i) nearObj.put<float>(0.0f);
        nearObj.put<uint8_t>(0).put<uint16_t>(0xFFFF).put<uint16_t>(0xFFFF);
        expect(in(o, 100, 44, nearObj), "IN  Object #40 create model=1600 at (110.0,-205.0,1.0)", "an object near us");
        W farObj; farObj.put<uint16_t>(41).put<int32_t>(1600).put<float>(900.0f).put<float>(900.0f).put<float>(1.0f);
        for (int i = 0; i < 4; ++i) farObj.put<float>(0.0f);
        farObj.put<uint8_t>(0).put<uint16_t>(0xFFFF).put<uint16_t>(0xFFFF);
        check(in(o, 110, 44, farObj).empty(), "an object far away isn't written");
        W d40; d40.put<uint16_t>(40);
        expect(in(o, 200, 47, d40), "IN  Object #40 destroyed", "the near object's removal");
        W d41; d41.put<uint16_t>(41);
        check(in(o, 210, 47, d41).empty(), "the far one's removal isn't");

        W lbl; lbl.put<uint16_t>(5).put<uint32_t>(0xFFFF00FFu).put<float>(101.0f).put<float>(-201.0f).put<float>(3.0f)
                .put<float>(20.0f).put<uint8_t>(0).put<uint16_t>(0xFFFF).put<uint16_t>(0xFFFF);
        const std::vector<uint8_t> txt = i18n::huffman::encode_string("Fishing spot\nType /fish");
        lbl.raw(txt.data(), txt.size());
        expect(in(o, 300, 36, lbl), "IN  Label #5 at (101.0,-201.0,3.0) color=FFFF00FF \"Fishing spot\\nType /fish\"", "a 3D label near us, text decoded");

        W wpa; wpa.put<uint16_t>(9);
        check(in(o, 400, 32, wpa).empty(), "another player streaming in: only counted");
        Lines sum;
        o.tick(5000, sum);
        check(sum.empty(), "no summary before 10 s");
        o.tick(10000, sum);
        expect(sum, "--- 10 s:", "summary after 10 s");
        expect(sum, "foot 1", "it counts what we sent");
        expect(sum, "WorldPlayerAdd x1", "and what was skipped");
        Lines again;
        o.tick(20000, again);
        check(again.empty(), "a quiet window has no summary");
        W gw; gw.put<uint32_t>(15).put<uint32_t>(1);
        Lines given = in(o, 21000, 22, gw);
        Lines last;
        o.tick(23500, last, true);
        expect(last, "--- 4 s:", "stopping: the part of a window left is summarised at once");
    }
    {   // 10. A new session forgets textdraws/objects; marks are numbered.
        Timeline s;
        s.reset(0);
        Lines a = in(s, 10, 134, textdraw(10, 300.0f, "Hi"));
        W ig; ig.put<uint8_t>(0);
        expect(in(s, 20, 139, ig), "IN  InitGame", "a new session is written");
        Lines b = in(s, 30, 134, textdraw(10, 300.0f, "Hi"));
        check(!has(b, "same as last show"), "after a new session a textdraw is new again");
        Lines m;
        s.mark(40, "line broke here", m);
        expect(m, "MARK 1 \"line broke here\"", "numbered mark");
        s.mark(50, "", m);
        expect(m, "MARK 2", "a mark without a note");
        check(s.marks() == 2, "marks counted");
    }
    {   // 11. Cut-off and unknown payloads never crash; unknown ones still show up.
        Timeline u;
        u.reset(0);
        u.set_local_id(7);
        const uint8_t junk[3] = { 0xFF, 0x01, 0x02 };
        size_t lines = 0;
        for (int id = 0; id < 256; ++id) {
            Lines a; u.incoming_rpc(5, id, junk, 20, a);
            Lines b; u.outgoing_rpc(5, id, junk, 20, false, b);
            Lines c; u.incoming_rpc(5, id, nullptr, 0, c);
            lines += a.size() + b.size() + c.size();
            if (a.size() > 1 || b.size() > 1 || c.size() > 1) check(false, "one RPC -> at most one line");
        }
        check(lines > 0 && lines <= 3 * 256, "every RPC id with a cut-off payload: at most one line each, no crash");
        W x; x.put<uint32_t>(1).put<uint32_t>(2);
        expect(in(u, 6, 250, x), "IN  rpc(250) 8B", "an unknown RPC still shows its id and size");
    }

    {   // 12. On foot -> driving -> on foot: each kind starts fresh, nothing held carries over.
        Timeline v;
        v.reset(0);
        v.set_local_id(7);
        auto f0 = foot(0);
        Lines a = packet(v, 0, 207, &f0, sizeof f0);
        auto fe = foot(16);
        expect(packet(v, 100, 207, &fe, sizeof fe), "+SECONDARY_ATTACK(enter)", "enter pressed on foot");

        samp::stInCarData c{};
        c.sVehicleID = 33;
        c.fPosition[0] = 100.0f; c.fPosition[1] = -200.0f; c.fPosition[2] = 3.0f;
        c.fVehicleHealth = 1000.0f;
        c.bytePlayerHealth = 100;
        expect(packet(v, 500, 200, &c, sizeof c), "OUT car (in vehicle 33)", "driving: starts fresh");
        c.sKeys = 8;
        expect(packet(v, 600, 200, &c, sizeof c), "OUT car +SPRINT |", "accelerate pressed (no on-foot key name)");
        c.sKeys = 0;
        expect(packet(v, 900, 200, &c, sizeof c), "-SPRINT held 300ms", "accelerate released");

        // An object attached to our car: the vehicle id comes first after the flag byte.
        W ob; ob.put<uint16_t>(50).put<int32_t>(19000).put<float>(0.0f).put<float>(0.0f).put<float>(1.0f);
        for (int i = 0; i < 4; ++i) ob.put<float>(0.0f);
        ob.put<uint8_t>(0).put<uint16_t>(33).put<uint16_t>(0xFFFF);
        expect(in(v, 950, 44, ob), "IN  Object #50 create model=19000", "an object on our car is written");
        expect(in(v, 950, 44, ob), "on vehicle 33", "... as on our vehicle");
        W lb; lb.put<uint16_t>(9).put<uint32_t>(0xFFFFFFFFu).put<float>(0.0f).put<float>(0.0f).put<float>(1.0f)
               .put<float>(20.0f).put<uint8_t>(0).put<uint16_t>(0xFFFF).put<uint16_t>(33);
        const std::vector<uint8_t> lt = i18n::huffman::encode_string("Fish: 3");
        lb.raw(lt.data(), lt.size());
        expect(in(v, 960, 36, lb), "IN  Label #9 on my vehicle", "a label on our car is written though its position is an offset");

        auto f1 = foot(0);
        Lines back = packet(v, 2000, 207, &f1, sizeof f1);
        expect(back, "OUT foot (start)", "on foot again: starts fresh");
        check(!has(back, "held"), "no stale press from before the car");

        samp::stPassengerData q{};
        q.sVehicleID = 40;
        q.byteSeatID = 0x41; // drive-by flag + seat 1
        expect(packet(v, 3000, 211, &q, sizeof q), "OUT passenger (vehicle 40 seat 1)", "seat without the drive-by bits");
        q.byteSeatID = 0x01;
        Lines db = packet(v, 3100, 211, &q, sizeof q);
        expect(db, "driveby 1->0", "drive-by flag change");
        check(!has(db, "seat 1)"), "not taken for a seat change");

        samp::stBulletData b{};
        b.byteType = 1; b.sTargetID = 9; b.byteWeaponID = 31;
        expect(packet(v, 3200, 206, &b, sizeof b), "OUT bullet hit=1(player) target=9 wpn=31(m4)", "bullet");

        int32_t stats[2] = { 100, 0 };
        expect(packet(v, 3300, 205, stats, sizeof stats), "OUT stats money=100 drunk=0", "money + drunk level");
        check(packet(v, 3400, 205, stats, sizeof stats).empty(), "unchanged stats not written");
        stats[0] = 150;
        expect(packet(v, 3500, 205, stats, sizeof stats), "money=150", "changed money written");
        stats[1] = 2000;
        expect(packet(v, 3510, 205, stats, sizeof stats), "drunk=2000", "getting drunk is written");
        bool decay = true;
        for (int d = 1999; d > 1900; --d) { stats[1] = d; decay = decay && packet(v, 3520, 205, stats, sizeof stats).empty(); }
        check(decay, "the drunk level wearing off by itself (every frame) isn't written");
        // As the client sends it: the player / actor aimed at (none), then slot, weapon, ammo.
        const uint8_t wp[] = { 0xFF, 0xFF, 0xFF, 0xFF, 2, 24, 70, 0, 0, 0, 1, 0 };
        Lines ws = packet(v, 3600, 204, wp, sizeof wp);
        expect(ws, "OUT weapons target=none [slot 2: 24(desert eagle) x70, slot 0: 0(fists) x1]", "weapon slots decoded");
        check(packet(v, 3700, 204, wp, sizeof wp).empty(), "unchanged weapon slots not written");
        const uint8_t aimAt[] = { 9, 0, 0xFF, 0xFF, 2, 24, 69, 0 };
        expect(packet(v, 3800, 204, aimAt, sizeof aimAt), "target=player 9 [slot 2: 24(desert eagle) x69]", "the player aimed at");
    }
    {   // 13. Actors (an NPC buyer) near us; far ones counted.
        Timeline a;
        a.reset(0);
        auto f = foot(0);
        Lines ig = packet(a, 0, 207, &f, sizeof f); // at (100, -200, 3)
        W sa; sa.put<uint16_t>(3).put<uint32_t>(155).put<uint32_t>(0).put<float>(102.0f).put<float>(-199.0f).put<float>(3.0f)
              .put<float>(90.0f).put<float>(100.0f).put<uint8_t>(1);
        expect(in(a, 100, 171, sa), "IN  Actor #3 show skin=155 at (102.0,-199.0,3.0)", "an actor near us");
        W far; far.put<uint16_t>(4).put<uint32_t>(155).put<uint32_t>(0).put<float>(900.0f).put<float>(900.0f).put<float>(3.0f)
               .put<float>(90.0f).put<float>(100.0f).put<uint8_t>(1);
        check(in(a, 110, 171, far).empty(), "a far actor isn't written");
        W aa; aa.put<uint16_t>(3).s8("DEALER").s8("DEALER_IDLE").put<float>(4.1f).bit(true).bit(false).bit(false).bit(false).put<uint32_t>(0);
        expect(in(a, 200, 173, aa), "IN  Actor #3 anim \"DEALER\" \"DEALER_IDLE\"", "the near actor's animation");
        W aa4; aa4.put<uint16_t>(4).s8("DEALER").s8("DEALER_IDLE").put<float>(4.1f).bit(true).bit(false).bit(false).bit(false).put<uint32_t>(0);
        check(in(a, 210, 173, aa4).empty(), "the far one's animation isn't");
        W ha; ha.put<uint16_t>(3);
        expect(in(a, 300, 172, ha), "IN  Actor #3 hidden", "the near actor hidden");
    }
    {   // 14. A textdraw re-shown unchanged (a HUD clock) is only counted; the summary names the busy ones.
        Timeline h;
        h.reset(0);
        Lines first = in(h, 10, 134, textdraw(120, 50.0f, "12:00"));
        bool quiet = true;
        for (int i = 0; i < 5; ++i) quiet = quiet && in(h, 100 + i, 134, textdraw(120, 50.0f, "12:00")).empty();
        check(quiet, "five unchanged re-shows: nothing written");
        Lines sum;
        h.tick(10000, sum);
        expect(sum, "TD show unchanged x5", "counted in the summary");
        expect(sum, "busiest textdraws: #120 x6", "the busiest textdraws named");
    }
    {   // 16. A cheat's own syncs (another kind, interleaved) never break the game's press/hold record.
        Timeline j;
        j.reset(0);
        samp::stInCarData ic{};
        ic.sVehicleID = 55;
        auto g0 = foot(0);
        auto g1 = foot(8);
        Lines first = packet(j, 0, 207, &g0, sizeof g0);
        expect(packet(j, 10, 200, &ic, sizeof ic, true), "OUT [fuego] car (in vehicle 55)", "the cheat's first in-car sync");
        expect(packet(j, 30, 207, &g1, sizeof g1), "OUT foot +SPRINT(space)", "space pressed, a cheat's sync in between");
        check(packet(j, 40, 200, &ic, sizeof ic, true).empty(), "the cheat's unchanged sync isn't written again");
        expect(packet(j, 530, 207, &g0, sizeof g0), "-SPRINT(space) held 500ms", "released: the held time survives it");
        check(j.keys_now() == "none", "the overlay shows the game's keys, not the cheat's");
    }
    {   // 15. Random payloads of every id and length: never more than one line per call, never a crash.
        Timeline z;
        z.reset(0);
        z.set_local_id(7);
        uint32_t seed = 12345;
        auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
        bool ok = true;
        for (int n = 0; n < 20000; ++n) {
            std::vector<uint8_t> buf(rnd() % 80);
            for (auto& x : buf) x = (uint8_t)rnd();
            const size_t bits = buf.empty() ? 0 : buf.size() * 8 - rnd() % 8;
            const int id = (int)(rnd() % 256);
            const uint8_t* p = buf.empty() ? nullptr : buf.data();
            Lines a, b, c;
            z.incoming_rpc(n, id, p, bits, a);
            z.outgoing_rpc(n, id, p, bits, false, b);
            if (!buf.empty()) buf[0] = (uint8_t)(200 + rnd() % 13); // the sync packet ids
            z.outgoing_packet(n, p, bits, false, c);
            if (a.size() > 1 || b.size() > 1 || c.size() > 1) ok = false;
        }
        check(ok, "20000 random payloads: at most one line each, no crash");
    }

    {   // 17. Other players (detail "others"): their RPCs and their syncs, written on change.
        Timeline o;
        o.reset(0);
        o.set_local_id(7);
        rec::Options opt; opt.others = true; o.set_options(opt);
        W an; an.put<uint16_t>(9).s8("PED").s8("WALK_civi").put<float>(4.1f).bit(true).bit(true).bit(true).bit(false).put<uint32_t>(0);
        expect(in(o, 10, 86, an), "IN  Anim player 9 \"PED\" \"WALK_civi\"", "another player's animation is written");
        W pc; pc.put<uint16_t>(9).s8("hi all");
        expect(in(o, 20, 101, pc), "IN  PlayerChat player 9 \"hi all\"", "another player's chat is written");
        W wpa; wpa.put<uint16_t>(9);
        expect(in(o, 30, 32, wpa), "IN  WorldPlayerAdd", "streaming traffic is written too");

        // A player's on-foot sync as the server relays it: id, [lr], [ud], keys, position,
        // compressed rotation, health/armour, weapon, special action.
        auto relay = [](uint16_t pid, uint16_t keys, float x, uint8_t weapon) {
            W w;
            w.put<uint8_t>(207).put<uint16_t>(pid).bit(false).bit(true).put<uint16_t>(0xFF80).put<uint16_t>(keys);
            w.put<float>(x).put<float>(2.0f).put<float>(3.0f);
            w.bit(false).bit(false).bit(false).bit(false).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(0);
            w.put<uint8_t>(0x99).put<uint8_t>(weapon).put<uint8_t>(0);
            return w;
        };
        auto sync_in = [&](Timeline& tl, uint32_t now, const W& w) { Lines l; tl.incoming_packet(now, w.b.data(), w.bits, l); return l; };
        Lines s1 = sync_in(o, 100, relay(9, 4, 1.0f, 24));
        expect(s1, "IN  sync player 9 foot (start) | keys=FIRE(attack) ud=W", "a player's first sync");
        expect(s1, "wpn=24(desert eagle)", "with the weapon");
        expect(s1, "pos=(1.0,2.0,3.0)", "and the position");
        check(sync_in(o, 130, relay(9, 4, 1.5f, 24)).empty(), "only moved: not written");
        expect(sync_in(o, 160, relay(9, 4 | 128, 1.5f, 24)), "IN  sync player 9 foot +HANDBRAKE(aim)", "a key change is written");
        // Their car: the vehicle named, key presses with how long they were held.
        auto car_in = [](uint16_t pid, uint16_t vid, uint16_t keys) {
            W w;
            w.put<uint8_t>(200).put<uint16_t>(pid).put<uint16_t>(vid).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(keys);
            w.bit(false).bit(false).bit(false).bit(false).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(0);
            w.put<float>(5.0f).put<float>(6.0f).put<float>(7.0f);
            return w;
        };
        expect(sync_in(o, 200, car_in(9, 33, 0)), "IN  sync player 9 car (in vehicle 33) | keys=none", "their car");
        expect(sync_in(o, 300, car_in(9, 33, 8)), "IN  sync player 9 car +SPRINT |", "accelerating");
        expect(sync_in(o, 500, car_in(9, 33, 0)), "-SPRINT held 200ms", "released, with the held time");
        check(sync_in(o, 530, car_in(9, 33, 0)).empty(), "unchanged: not written");
        W bs; bs.put<uint8_t>(206).put<uint16_t>(9);
        samp::stBulletData bd{};
        bd.byteType = 1; bd.sTargetID = 7; bd.byteWeaponID = 24;
        bs.raw(&bd, sizeof bd);
        expect(sync_in(o, 600, bs), "IN  sync player 9 bullet hit=1(player) target=7 (me) wpn=24(desert eagle)", "a shot at us");
        uint8_t cut[2] = { 207, 9 };
        Lines lc; o.incoming_packet(700, cut, 16, lc);
        check(lc.size() == 1, "a cut-off relayed sync: one line, no crash");

        Timeline n; // normal detail: other players' syncs only counted
        n.reset(0);
        n.set_local_id(7);
        check(sync_in(n, 100, relay(9, 4, 1.0f, 24)).empty(), "normal detail: another player's sync isn't written");
        uint8_t lost[1] = { 33 };
        Lines l2; n.incoming_packet(200, lost, 8, l2);
        expect(l2, "IN  CONNECTION_LOST", "connection events always written");
    }
    {   // 18. Everything: every packet, unchanged ones too, each with its raw bytes.
        Timeline e;
        e.reset(0);
        rec::Options opt; opt.everything = true; e.set_options(opt);
        auto f0 = foot(0);
        Lines a = packet(e, 0, 207, &f0, sizeof f0);
        Lines b = packet(e, 30, 207, &f0, sizeof f0);
        expect(b, "OUT foot (no change)", "an unchanged sync is written");
        expect(b, "| raw CF 00 00", "with its raw bytes");
        Lines c = in(e, 40, 134, textdraw(10, 300.0f, "Hi"));
        Lines d = in(e, 50, 134, textdraw(10, 300.0f, "Hi"));
        expect(d, "(same as last show)", "an unchanged re-show is written");
    }
    {   // 19. The chat box (local lines), real key / mouse input, our state.
        Timeline k;
        k.reset(0);
        Lines cl; k.chat_line(5, 8, "{FF6347}[rec] {FFFFFF}Recording.", cl);
        expect(cl, "CHATBOX debug \"{FF6347}[rec] {FFFFFF}Recording.\"", "a local chat-box line");

        Lines i1; k.input(100, 'W', true, i1);
        expect(i1, "KEY  W down", "a key press");
        Lines i2; k.input(150, 'W', true, i2);
        check(i2.empty(), "a held key's repeats aren't written");
        Lines i3; k.input(330, 'W', false, i3);
        expect(i3, "KEY  W up (held 230ms)", "the release, with how long it was held");
        Lines i4; k.input(400, 0x01, true, i4);
        expect(i4, "KEY  LMB down", "a mouse button");
        check(rec::vk_name(0x20) == "SPACE" && rec::vk_name(0x71) == "F2", "key names");

        rec::LocalState st;
        st.gameState = 14; st.id = 7; st.spawned = true; st.health = 100; st.armor = 50; st.money = 1234;
        st.pos[0] = 10; st.pos[1] = 20; st.pos[2] = 3; st.weapon = 24; st.weapons[2] = 24; st.ammo[2] = 70;
        Lines s1; k.state(1000, st, s1);
        expect(s1, "STATE connected id=7 spawned", "a full snapshot at the start");
        expect(s1, "hp=100 ar=50 money=1234", "with health, armour, money");
        expect(s1, "weapons=[24(desert eagle) x70]", "and the weapons");
        Lines s2; k.state(1100, st, s2);
        check(s2.empty(), "nothing changed: nothing written");
        st.interior = 3; st.dialogOpen = true;
        Lines s3; k.state(1200, st, s3);
        expect(s3, "STATE interior 0->3", "a change is written");
        expect(s3, "dialog open", "with each thing that changed");
        st.pos[0] = 500;
        Lines s4; k.state(1300, st, s4);
        expect(s4, "STATE teleported", "a position jump is written");
        st.pos[0] = 501;
        Lines s5; k.state(1400, st, s5);
        check(s5.empty(), "a small move isn't");
        Lines s6; k.state(1000 + rec::kStateEveryMs, st, s6);
        expect(s6, "STATE connected", "a full snapshot again every 30 s");
    }
    {   // 20. Server-assigned ids are named: players (pool lookup, joins, renames, the
        //     roster), vehicles (pool lookup, streamed-in models).
        Timeline nm;
        nm.reset(0);
        nm.set_local_id(7);
        nm.set_name_lookups([](int id) { return id == 3 ? std::string("Panzer_") : std::string(); },
                            [](int id) { return id == 412 ? 412 : -1; });
        W pc; pc.put<uint16_t>(3).s8("yo");
        expect(in(nm, 10, 101, pc), "IN  PlayerChat player 3 (Panzer_) \"yo\"", "a player id with the name from the pool");
        W sj; sj.put<uint16_t>(12).put<uint32_t>(0xFF0000FFu).put<uint8_t>(0).s8("New_Guy");
        expect(in(nm, 20, 137, sj), "IN  ServerJoin player 12 \"New_Guy\"", "a join is written (normal detail)");
        W pc12; pc12.put<uint16_t>(12).s8("hello");
        expect(in(nm, 30, 101, pc12), "player 12 (New_Guy)", "the joined player's name known from then on");
        W rn; rn.put<uint16_t>(12).s8("Renamed").put<uint8_t>(1);
        expect(in(nm, 40, 11, rn), "IN  SetPlayerName player 12 -> \"Renamed\"", "a name change");
        expect(in(nm, 50, 101, pc12), "player 12 (Renamed)", "the new name used");
        W kd; kd.put<uint16_t>(3).put<uint16_t>(12).put<uint8_t>(24);
        expect(in(nm, 60, 55, kd), "IN  Kill player 3 (Panzer_) killed player 12 (Renamed) with 24(desert eagle)", "the kill feed, named");
        W sq; sq.put<uint16_t>(12).put<uint8_t>(1);
        expect(in(nm, 70, 138, sq), "IN  ServerQuit player 12 (Renamed) (quit)", "a quit, with the name");

        W ev; ev.put<uint16_t>(7).put<uint16_t>(412).put<uint8_t>(0);
        expect(in(nm, 80, 26, ev), "IN  PlayerEnterVehicle me vehicle 412 (Voodoo)", "a vehicle named by its model");
        W va; va.put<uint16_t>(500).put<int32_t>(411).put<float>(1.0f).put<float>(2.0f).put<float>(3.0f);
        check(in(nm, 90, 164, va).empty(), "a vehicle streaming in: counted only...");
        samp::stInCarData c{};
        c.sVehicleID = 500;
        expect(packet(nm, 100, 200, &c, sizeof c), "OUT car (in vehicle 500 (Infernus))", "... but its model is learned");
        check(rec::vehicle_model_name(400) == std::string("Landstalker") && rec::vehicle_model_name(611) == std::string("Utility Trailer") &&
              rec::vehicle_model_name(399) == nullptr, "vehicle model names 400..611");

        W vh; vh.put<uint16_t>(500).put<float>(650.0f);
        expect(in(nm, 110, 147, vh), "IN  SetVehicleHealth vehicle 500 (Infernus) 650.0", "streaming RPCs about our car are written");
        W vo; vo.put<uint16_t>(33).put<float>(650.0f);
        check(in(nm, 120, 147, vo).empty(), "the same for another car: counted");
        W tm; tm.put<uint16_t>(7).put<uint8_t>(2);
        expect(in(nm, 130, 69, tm), "IN  SetPlayerTeam me 2", "... and about us");

        Lines ro;
        nm.roster(140, { {0, "Alpha"}, {7, "KevY007"}, {12, "Renamed"} }, ro);
        expect(ro, "PLAYERS 3: 0 Alpha, 7 KevY007 (me), 12 Renamed", "the roster");
        samp::stBulletData bl{};
        bl.byteType = 1; bl.sTargetID = 0; bl.byteWeaponID = 24;
        expect(packet(nm, 150, 206, &bl, sizeof bl), "target=0 (Alpha) wpn=24", "a bullet's target named (from the roster)");
        W bad; bad.put<uint16_t>(40).put<uint32_t>(0).put<uint8_t>(0).s8("Evil\nName");
        Lines ignoreBad = in(nm, 160, 137, bad);
        W pc40; pc40.put<uint16_t>(40).s8("x");
        expect(in(nm, 170, 101, pc40), "player 40 (Evil?Name)", "a name stays on one line");
        std::vector<std::pair<int, std::string>> many;
        for (int i = 0; i < 90; ++i) many.push_back({ i, "P" + std::to_string(i) });
        Lines ro2;
        nm.roster(180, many, ro2);
        check(ro2.size() == 3 && has(ro2, "PLAYERS 90 (3/3): 80 P80"), "a big roster over several lines");
    }
    {   // 21. A tablist dialog with headers (the header isn't a row); focus; the wheel.
        Timeline d;
        d.reset(0);
        W dl; dl.put<int16_t>(30).put<uint8_t>(5).s8("Shop").s8("Buy").s8("Close");
        const std::vector<uint8_t> body = i18n::huffman::encode_string("Item\tPrice\nBait\t$5\nRod\t$50");
        dl.raw(body.data(), body.size());
        expect(in(d, 10, 61, dl), "style=5(tablist+headers)", "a tablist with headers shown");
        W dr; dr.put<int16_t>(30).put<uint8_t>(1).put<int16_t>(1).s8("Rod");
        expect(out_rpc(d, 20, 62, dr), "list=1 row \"Rod\\t$50\" input=\"Rod\"", "row 1 is the line after the header");
        W dr9; dr9.put<int16_t>(30).put<uint8_t>(1).put<int16_t>(9).s8("");
        check(!has(out_rpc(d, 25, 62, dr9), "row"), "no row past the end");

        Lines f;
        d.input(30, 'W', true, f);
        d.focus(40, false, f);
        expect(f, "FOCUS lost (alt-tab / minimized) -- released W", "losing the focus releases held keys");
        Lines f2; d.focus(50, false, f2);
        check(f2.empty(), "focus: changes only");
        Lines f3; d.input(60, 'W', false, f3);
        check(f3.empty(), "a release whose press wasn't written isn't either");
        Lines f4; d.focus(70, true, f4);
        expect(f4, "FOCUS back in the game", "focus back");
        Lines wh; d.wheel(80, -2, wh);
        expect(wh, "KEY  WHEEL down x2", "the mouse wheel");
    }
    {   // 22. RCON answers hidden in a log to share; a player streaming out starts fresh;
        //     other players' car syncs read to the weapon byte; spectating; a hitch isn't a teleport.
        Timeline h;
        h.reset(0);
        rec::Options ho; ho.hideSecrets = true; h.set_options(ho);
        uint8_t rr[1 + 4 + 14] = { 202, 14, 0, 0, 0 };
        std::memcpy(rr + 5, "pass is s3cret", 14);
        Lines hr; h.incoming_packet(10, rr, sizeof rr * 8, hr);
        check(has(hr, "RCON_RESPONSE (hidden)") && !has(hr, "s3cret"), "hide secrets: an RCON answer isn't written");
        Timeline n;
        n.reset(0);
        Lines nr; n.incoming_packet(10, rr, sizeof rr * 8, nr);
        expect(nr, "IN  RCON_RESPONSE \"pass is s3cret\"", "normal: the RCON answer is written");

        Timeline o;
        o.reset(0);
        o.set_local_id(7);
        rec::Options oo; oo.others = true; o.set_options(oo);
        auto foot_in = [](uint16_t pid, uint16_t keys) {
            W w;
            w.put<uint8_t>(207).put<uint16_t>(pid).bit(false).bit(false).put<uint16_t>(keys);
            w.put<float>(1.0f).put<float>(2.0f).put<float>(3.0f);
            w.bit(false).bit(false).bit(false).bit(false).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(0);
            w.put<uint8_t>(0xFF).put<uint8_t>(0).put<uint8_t>(0);
            return w;
        };
        auto sync_in = [&](Timeline& tl, uint32_t now, const W& w) { Lines l; tl.incoming_packet(now, w.b.data(), w.bits, l); return l; };
        Lines f1 = sync_in(o, 100, foot_in(9, 8));
        W so; so.put<uint16_t>(9);
        Lines out1 = in(o, 200, 163, so); // streamed out while holding SPRINT
        Lines f2 = sync_in(o, 300000, foot_in(9, 0));
        expect(f2, "IN  sync player 9 foot (start)", "streamed back in: starts fresh");
        check(!has(f2, "held"), "no key 'held' across the stream-out");

        auto car_full = [](uint16_t pid, uint16_t keys, float speed, uint8_t weaponByte) {
            W w;
            w.put<uint8_t>(200).put<uint16_t>(pid).put<uint16_t>(33).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(keys);
            w.bit(false).bit(false).bit(false).bit(false).put<uint16_t>(0).put<uint16_t>(0).put<uint16_t>(0);
            w.put<float>(5.0f).put<float>(6.0f).put<float>(7.0f);
            w.put<float>(speed);
            if (speed != 0.0f) w.put<uint16_t>(40000).put<uint16_t>(30000).put<uint16_t>(32767);
            w.put<uint16_t>(1000).put<uint8_t>(0xFF).put<uint8_t>(weaponByte);
            return w;
        };
        expect(sync_in(o, 400000, car_full(12, 0, 0.0f, 29)), "wpn=29(mp5) carhp=1000", "their car sync read to the weapon (standing)");
        expect(sync_in(o, 400100, car_full(12, 0, 1.5f, 29 | (1 << 6))), "IN  sync player 12 car +YES(Y)", "moving: the speed's axes skipped, KEY_YES read");

        W sp; sp.put<uint32_t>(1);
        expect(in(o, 500000, 124, sp), "IN  Spectating on", "the server making us spectate");

        Timeline k;
        k.reset(0);
        rec::LocalState st;
        st.gameState = 14; st.id = 7; st.spawned = true;
        Lines s0; k.state(1000, st, s0);
        st.pos[0] = 200;
        Lines s1; k.state(3000, st, s1); // a 2 s hitch while flying fast: 200 m
        check(!has(s1, "teleported"), "a long frame isn't a teleport");
        st.pos[0] = 1200;
        Lines s2; k.state(3016, st, s2);
        expect(s2, "STATE teleported", "a real jump still is");
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
