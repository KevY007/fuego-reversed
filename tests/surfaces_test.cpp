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

    // ---- Translated body: rebuilt szInfo must decode back to the translation ----
    {
        std::vector<uint8_t> orig = hex(kDialogLogin);
        const std::string kTitle = "{9e47b5}Iniciar sesion";
        const std::string kBody  = "{ffffff}Introduce la contrasena de esta cuenta.";
        Translator es = [&](const std::string& src, std::string& out) {
            // First call is the title, then buttons, then body (rebuild order).
            // Just map the long one (body) specially; others echo.
            if (src.size() > 40) out = kBody;      // the body is the long piece
            else if (src.find("Log in") != std::string::npos && src.size() > 8) out = kTitle;
            else out = src;
            return true;
        };
        std::vector<uint8_t> out;
        bool ok = rebuild_dialog(orig.data(), orig.size(), es, out);
        check(ok, "rebuild_dialog(translate) returns true");
        check(out != orig, "translated rebuild differs from original");

        // Re-parse the rebuilt packet: u16 id, u8 style, u8+title, u8+b1, u8+b2, CSTRING.
        size_t off = 2 + 1;
        auto skip = [&](std::string& s) { size_t l = out[off++]; s.assign((char*)&out[off], l); off += l; };
        std::string t, b1, b2; skip(t); skip(b1); skip(b2);
        std::string body;
        bool dok = i18n::huffman::decode_string(out.data() + off, out.size() - off, body);
        check(dok, "rebuilt szInfo decodes");
        check(body == kBody, "rebuilt szInfo decodes back to the translated body");
        check(t == kTitle, "rebuilt title is the translated title");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
