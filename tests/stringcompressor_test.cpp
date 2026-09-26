// tests/stringcompressor_test.cpp
//
// Standalone host test for i18n::huffman::decode_string()/encode_string()
// (src/i18n/huffman.h / .cpp) -- the RakNet StringCompressor wire-framing
// layer built on top of the already-verified bare Huffman codec
// (encode()/decode(), covered separately by tests/huffman_test.cpp).
//
// This decodes real captured bytes from a live SA:MP server (Mike's Cops
// & Robbers) -- two "Log in"/"Reset Tutorials" dialog szInfo blobs, one
// long "Medical Services" list-dialog szInfo blob, and two 3D text label
// blobs -- and asserts they come out as non-empty, printable English
// text. It also asserts encode_string(decode_string(blob)) round-trips,
// byte-for-byte if possible, or at least text-for-text otherwise, for
// the two short dialogs.
//
// Builds and runs independently of the game and of our net/BitStream --
// see tests/run_stringcompressor_test.cmd. Has its own main() and its
// own CHECK/CHECK_EQ counters (not shared with the other test files).

#include "i18n/huffman.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_ran = 0;
int g_fail = 0;

} // namespace

#define CHECK(cond, msg) do { \
        ++g_ran; \
        if (!(cond)) { \
            ++g_fail; \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

#define CHECK_EQ(a, b, msg) do { \
        ++g_ran; \
        const auto _a = (a); \
        const auto _b = (b); \
        if (!(_a == _b)) { \
            ++g_fail; \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

namespace {

// ---------------------------------------------------------------------
// Ground-truth captures: real SA:MP server bytes (Mike's Cops & Robbers).
// Each is the exact CSTRING blob for a dialog szInfo or 3D label text --
// i.e. exactly what decode_string() takes -- starting at bit 0, byte
// aligned. See src/i18n/capture.cpp (i18n::capture::on_incoming_rpc) for
// how these were extracted from live ShowDialog (RPC 61) / Create3DTextLabel
// (RPC 36) packets.
// ---------------------------------------------------------------------

constexpr const char* kDialogLogIn =
    "5C013E521C71C71C73E53CEC906D8E0AC67DE0751B7BA632EE7467DE137DA8A21657DF5A9"
    "8CC60661F4E789452F1A6EA3BD70BAA9E39D199A2EC17AE179EA36F74C65DAFE46C5F4BED"
    "7F447E374165EA2349C51B7BA632E79580";

constexpr const char* kDialogResetTutorials =
    "3C013E521C71C71C73E5399A887FBF247E3D70BAAC8E429EAFFC27BC0C8574D355C85C9DE"
    "B85F923F1EE3E46C5FB5DC651A5BF71F20EC8DE3D70BCFD06919AFC1799B41F414618928D"
    "F62773835AF2366500";

constexpr const char* kDialogMedicalServices =
    "78083E52513BD13BD13BBE53EAB1A4E923E52513BD13BD13BBE5399C6E8CA2969053A48F9"
    "4944EF44EF44EEF94FA8A782B8CD73A48F94944EF44EF44EEF94EA93D924C7A492EB9C9D9"
    "94A077D5566A30FA1AF948F97F3AAAE87399F29DF7558CFAA76AA19AF562AC6455ED53B55"
    "6AB0D7CA455F55F55F55F55F55F7CA77C0694E923E522AFAAFAAFAAFAAFAAFBE53F9D247C"
    "A455F55F55F55F55F55F7CA7F3A48F948ABEABEABEABEABEABEF94E635241DE33D64D3D92"
    "4C7C81AF948ABEABEABEABEABEABEF94F3B5791DC9B0DB0FAEDF49D247CA455F55F55F55F"
    "55F55F7CA7F3A48F948ABEABEABEABEABEABEF94FE7491F29157D57D57D57D57D57DF29CC"
    "6A483BC67AC9A7B2498F9035F70E7E973A48FB873F4BDFA18BE46EE323E034AF1D89DF227"
    "A4F1EB85FA700BBA2920E9D24A8A715E13A48F948ABEAD99CCF51BE5FBE53BE594EBAEA35"
    "5D2A234CE9B0DB1CC82480EF3B12529D249DAC5CF1ED9F82F71F6B53237BF56548CF38E4D"
    "86D8E0E38517BE7467CEB517D1097637DF528BF42F21F49D24A8A715E13A48F948ABEAD99"
    "CCF51BE5FBE53BE593E5FAAE9511B3B609D1A4E924ECCC58A2D20B9D98D3D595233EE7492"
    "A29C9D6A29D247CA455F56CCE67A8DF2FDF29DF2C9575AAFBE5F9A6696789D245636EC67B"
    "5169053A49514E4EB514E923E522AFAB66733D46F97EF94EF964ABA2AEB55D0D379AF948F"
    "97F3AAAE87399F29DF7558CFAA769B50144683E81C944E9279D24F3A4866CCD99B00";

constexpr const char* kLabel1024 =
    "04012B803BCEC9315F7CB3A8DF2FCD7CA4562B158AC562B1F29E76498AC3B9F5FC84B19EF"
    "AC1C749083E86AB598A6F061EA27AA2A89F21A6CEDCC2E8C7B8E85632D45A80";

constexpr const char* kLabel1025 =
    "00012B803BCEC9315F7CB3A8E7735F29158AC562B158AC7CA79D9262B0EE7D7F212C67BEB"
    "071D2420FA1AAD6629BC187A89EA8AA27C869B3B730BA31EE3A158CB51680";

std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

// SA:MP dialog/label text is ordinary printable ASCII, plus embedded
// newlines (list dialogs, multi-line info text) and SA:MP color codes
// like `{9E47B5}`, which are themselves just printable ASCII (braces and
// hex digits) -- no separate allowance needed for those.
bool is_samp_text_char(unsigned char c) {
    if (c == '\n' || c == '\r' || c == '\t') {
        return true;
    }
    return c >= 0x20 && c < 0x7F;
}

bool all_printable(const std::string& s) {
    for (const unsigned char c : s) {
        if (!is_samp_text_char(c)) {
            return false;
        }
    }
    return true;
}

// Decodes one captured blob, checks it came out non-empty/printable, and
// prints the decoded text (this is the actual pass/fail evidence this
// test exists to produce). Returns {bytes, decoded} for callers that
// also want to round-trip it.
std::pair<std::vector<uint8_t>, std::string> check_capture(
    const char* hex, size_t expectedBytes, const char* label) {
    const std::vector<uint8_t> bytes = hex_to_bytes(hex);
    CHECK_EQ(bytes.size(), expectedBytes, label);

    std::string decoded;
    const bool ok = i18n::huffman::decode_string(bytes.data(), bytes.size(), decoded);
    CHECK(ok, label);
    CHECK(!decoded.empty(), label);
    CHECK(all_printable(decoded), label);

    std::printf("---- %s (%zu bytes) ----\n%s\n----\n\n", label, bytes.size(), decoded.c_str());

    return {bytes, decoded};
}

// Asserts encode_string(decodedOriginal) round-trips back to
// originalBytes byte-for-byte if RakNet's encoding turns out fully
// deterministic for this input; if not, falls back to asserting that
// decode_string() of the re-encoded bytes still yields the identical
// text (i.e. the two encodings are equivalent StringCompressor frames
// even if not bitwise identical).
void check_roundtrip(
    const std::vector<uint8_t>& originalBytes, const std::string& decodedOriginal, const char* label) {
    const std::vector<uint8_t> reencoded = i18n::huffman::encode_string(decodedOriginal);

    if (reencoded == originalBytes) {
        std::printf("  [%s] encode_string(decode_string(blob)) is BYTE-EXACT with the original blob (%zu bytes)\n\n",
                     label, reencoded.size());
        CHECK(true, label); // record the pass explicitly, matching CHECK-per-assertion convention
        return;
    }

    std::printf(
        "  [%s] encode_string(decode_string(blob)) is NOT byte-exact (original %zu bytes, re-encoded %zu bytes) "
        "-- falling back to a text round-trip check\n",
        label, originalBytes.size(), reencoded.size());

    std::string decodedReencoded;
    const bool ok = i18n::huffman::decode_string(reencoded.data(), reencoded.size(), decodedReencoded);
    CHECK(ok, label);
    CHECK_EQ(decodedReencoded, decodedOriginal, label);
    std::printf("  [%s] decode_string(encode_string(text)) == original text: %s\n\n",
                label, (ok && decodedReencoded == decodedOriginal) ? "yes" : "NO -- MISMATCH");
}

void test_empty_string_frame() {
    // in == "" must produce exactly {0xC0} (see huffman.h), and that
    // must decode back to "".
    const std::vector<uint8_t> encoded = i18n::huffman::encode_string("");
    CHECK_EQ(encoded.size(), static_cast<size_t>(1), "empty string encodes to exactly 1 byte");
    if (!encoded.empty()) {
        CHECK_EQ(static_cast<int>(encoded[0]), 0xC0, "empty string encodes to byte 0xC0");
    }

    std::string decoded = "not empty to start";
    const bool ok = i18n::huffman::decode_string(encoded.data(), encoded.size(), decoded);
    CHECK(ok, "decode_string of empty-string frame succeeds");
    CHECK(decoded.empty(), "decode_string of empty-string frame yields empty string");
}

void test_malformed_input_rejected() {
    std::string out = "unchanged";
    CHECK(!i18n::huffman::decode_string(nullptr, 0, out), "null data is rejected");
    CHECK(out.empty(), "decode_string clears out even when rejecting null data");

    const uint8_t oneByte[1] = {0x00};
    out = "unchanged";
    CHECK(!i18n::huffman::decode_string(oneByte, 0, out), "numBytes==0 is rejected even with non-null data");

    // A single 0x00 byte: its very first bit is 0, which
    // read_compressed_uint16() reads as "the upper byte of the 16-bit
    // length does NOT match zero" -- i.e. "16 more raw bits follow" -- but
    // only 7 bits remain in an 8-bit buffer after that first bit, so this
    // exercises decode_string() being asked to read more bits than are
    // available and failing cleanly rather than reading out of bounds
    // (this is also run under a debugger-free release-ish build, so an
    // OOB read would likely just corrupt output silently instead of
    // crashing -- the real check is that it returns false and clears
    // `out`, not that it doesn't crash).
    out = "unchanged";
    const bool ok = i18n::huffman::decode_string(oneByte, 1, out);
    CHECK(!ok, "a single 0x00 byte is 8 bits, too few to hold a 16-bit-header frame -- rejected");
    if (!ok) {
        CHECK(out.empty(), "decode_string clears out on failure");
    }
}

} // namespace

int main() {
    std::printf("=== i18n::huffman::decode_string() on real SA:MP captures ===\n\n");

    const auto login = check_capture(kDialogLogIn, 90, "DIALOG \"Log in\" szInfo");
    const auto resetTutorials = check_capture(kDialogResetTutorials, 82, "DIALOG \"Reset Tutorials\" szInfo");
    const auto medicalServices = check_capture(kDialogMedicalServices, 545, "DIALOG \"Medical Services\" szInfo");
    const auto label1024 = check_capture(kLabel1024, 68, "3D LABEL id=1024");
    const auto label1025 = check_capture(kLabel1025, 67, "3D LABEL id=1025");

    // Sanity check called out in the task: the two short dialogs should
    // share their opening bytes (same leading text/color code).
    CHECK(login.first.size() >= 9 && resetTutorials.first.size() >= 9 &&
              std::equal(login.first.begin() + 2, login.first.begin() + 9, resetTutorials.first.begin() + 2),
          "Log in / Reset Tutorials dialogs share opening bytes [2..9)");

    std::printf("=== encode_string()/decode_string() round-trip checks ===\n\n");
    check_roundtrip(login.first, login.second, "Log in");
    check_roundtrip(resetTutorials.first, resetTutorials.second, "Reset Tutorials");

    test_empty_string_frame();
    test_malformed_input_rejected();

    (void)medicalServices;
    (void)label1025;

    std::printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
