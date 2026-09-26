// tests/huffman_test.cpp
//
// Standalone host test for i18n::huffman (src/i18n/huffman.h / .cpp).
// Builds and runs independently of the game and of our net/BitStream --
// see tests/run_huffman_test.cmd. Has its own main() and its own
// CHECK/CHECK_EQ counters (not shared with tests/i18n_tests.cpp).

#include "i18n/huffman.h"

#include <cstdio>
#include <string>
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

// Encodes `s`, decodes it back with charCount == s.size(), and checks
// (via CHECK/CHECK_EQ, so one bad string doesn't abort the whole suite)
// that it comes back exactly.
void expect_round_trip(const std::string& s, const char* label) {
    size_t bitLength = 0;
    const std::vector<uint8_t> encoded = i18n::huffman::encode(s, bitLength);

    if (s.empty()) {
        CHECK(encoded.empty(), label);
        CHECK(bitLength == 0, label);
    } else {
        CHECK(bitLength > 0, label);
        CHECK(encoded.size() == (bitLength + 7) / 8, label);
    }

    std::string decoded;
    const bool ok = i18n::huffman::decode(encoded.data(), bitLength, s.size(), decoded);
    CHECK(ok, label);
    CHECK_EQ(decoded, s, label);
}

void test_round_trip_basic_strings() {
    expect_round_trip("", "empty string");
    expect_round_trip("hello", "hello");
    expect_round_trip("Hello, world!", "Hello, world!");
    expect_round_trip(" ", "single space");
    expect_round_trip("digits 0123456789 and punct: .,;:!?()[]{}-_'\"", "digits/punct/spaces");
}

void test_round_trip_lorem_200() {
    std::string lorem =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do "
        "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut "
        "enim ad minim veniam, quis nostrud exercitation ullamco laboris "
        "nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor.";
    // Force exactly 200 characters regardless of the literal's length above.
    lorem.resize(200, '.');
    CHECK(lorem.size() == 200, "lorem test string is exactly 200 chars");
    expect_round_trip(lorem, "200-char lorem");
}

void test_round_trip_all_bytes() {
    std::string allBytes;
    allBytes.reserve(256);
    for (int i = 0; i < 256; ++i) {
        allBytes.push_back(static_cast<char>(static_cast<unsigned char>(i)));
    }
    CHECK(allBytes.size() == 256, "all-bytes string is 256 bytes");
    expect_round_trip(allBytes, "all bytes 0..255 once");
}

void test_determinism() {
    const std::string s = "Determinism check: the quick brown fox jumps over the lazy dog.";
    size_t bitLength1 = 0;
    size_t bitLength2 = 0;
    const std::vector<uint8_t> a = i18n::huffman::encode(s, bitLength1);
    const std::vector<uint8_t> b = i18n::huffman::encode(s, bitLength2);
    CHECK_EQ(bitLength1, bitLength2, "determinism: same bitLength both times");
    CHECK(a == b, "determinism: identical encoded bytes both times");
}

void test_compression_sanity() {
    const std::string s =
        "The quick brown fox jumps over the lazy dog. Pack my box with "
        "five dozen liquor jugs. This is ordinary English text, the sort "
        "of thing a SA:MP dialog or 3D text label would actually contain.";
    size_t bitLength = 0;
    const std::vector<uint8_t> encoded = i18n::huffman::encode(s, bitLength);
    (void)encoded;
    CHECK(bitLength < 8 * s.size(), "ordinary English text compresses to fewer than 8 bits/char");
    std::printf(
        "  compression sanity: %zu chars -> %zu bits (%.2f bits/char, vs 8 uncompressed)\n",
        s.size(), bitLength, s.empty() ? 0.0 : static_cast<double>(bitLength) / static_cast<double>(s.size()));
}

void test_truncated_input_fails_safely() {
    const std::string s =
        "This string is long enough that aggressively truncating its encoded "
        "bit stream should reliably prevent decode() from finishing.";
    size_t bitLength = 0;
    const std::vector<uint8_t> encoded = i18n::huffman::encode(s, bitLength);
    CHECK(bitLength > 32, "sanity: encoded to a non-trivial number of bits");

    // Keep only a quarter of the bits (and only the bytes needed to back
    // them) -- nowhere near enough to decode all of s.size() characters.
    const size_t truncatedBitLength = bitLength / 4;
    const size_t truncatedByteLength = (truncatedBitLength + 7) / 8;
    const std::vector<uint8_t> truncated(encoded.begin(), encoded.begin() + static_cast<ptrdiff_t>(truncatedByteLength));

    std::string decoded = "unchanged";
    const bool ok = i18n::huffman::decode(
        truncated.empty() ? nullptr : truncated.data(), truncatedBitLength, s.size(), decoded);
    CHECK(!ok, "truncated bit stream is rejected (false), not crashed on");
    CHECK(decoded.empty(), "decode() clears out on failure");

    // The untruncated encoding must still decode correctly (control case,
    // proves the truncation above -- not some other bug -- caused the
    // failure).
    std::string decodedFull;
    const bool okFull = i18n::huffman::decode(encoded.data(), bitLength, s.size(), decodedFull);
    CHECK(okFull, "control: non-truncated input decodes fine");
    CHECK_EQ(decodedFull, s, "control: non-truncated decode matches original");
}

void test_charcount_zero() {
    std::string out = "not empty to start";
    const bool ok = i18n::huffman::decode(nullptr, 0, 0, out);
    CHECK(ok, "charCount==0 always succeeds, even with null data");
    CHECK(out.empty(), "charCount==0 yields empty output");
}

} // namespace

int main() {
    test_round_trip_basic_strings();
    test_round_trip_lorem_200();
    test_round_trip_all_bytes();
    test_determinism();
    test_compression_sanity();
    test_truncated_input_fails_safely();
    test_charcount_zero();

    std::printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
