// tests/i18n_tests.cpp
#include <cstdio>
#include <string>
#include <vector>
static int g_fail = 0, g_ran = 0;
#define CHECK(cond, msg) do { ++g_ran; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)
#define CHECK_EQ(a, b) do { ++g_ran; auto _a=(a); auto _b=(b); if(!(_a==_b)){ ++g_fail; \
    std::printf("FAIL %s:%d  expected equal\n", __FILE__, __LINE__); } } while(0)
// Forward-declare per-module test entry points; each later task defines one.
void test_charset();  void test_protect();  void test_detect();
void test_cache();    void test_json();     void test_validate();
void test_provider_breaker();
int main() {
    test_json(); test_charset(); test_protect(); test_detect(); test_cache(); test_validate();
    test_provider_breaker();
    std::printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
// Stubs so it links before later tasks fill them in (each task replaces its stub).
#include "i18n/validate.h"
#include "i18n/protect.h"
void test_validate() {
    using namespace i18n::validate;
    i18n::protect::Masked m = i18n::protect::mask("Kevin says: hola");
    CHECK(result_ok(m.text, m.text, m.tokens.size()), "unchanged placeholders ok");
    CHECK(!result_ok(m.text, "I think this means hello", m.tokens.size()), "lost placeholder");
    CHECK(!result_ok(m.text, "", m.tokens.size()), "empty rejected");
}

#include "i18n/cache.h"
void test_cache() {
    using namespace i18n::cache;
    save_path("test_tm.txt"); // isolate
    uint64_t k = key("привет", 1251, 1033, "ollama:hy-mt2");
    std::string out;
    CHECK(!get(k, out), "cache miss on first query");
    put(k, "hello");
    CHECK(get(k, out), "cache hit after put");
    CHECK_EQ(out, std::string("hello"));
    uint64_t k2 = key("привет", 1251, 1033, "openai:gpt");
    CHECK(!get(k2, out), "different provider profile => different key");
    // persistence: reload from disk into a fresh map
    clear_for_test();
    load("test_tm.txt");
    CHECK(get(k, out), "cache hit after reload");
    CHECK_EQ(out, std::string("hello"));

    // Test escaping and concurrent-ish put: special chars round-trip correctly
    std::string special_value = "line1\nline2\twith tab\\and backslash";
    uint64_t k_special = key("test", 1033, 1251, "test:provider");
    clear_for_test();
    put(k_special, special_value);
    // Reload from disk to verify escaping round-trip
    clear_for_test();
    load("test_tm.txt");
    CHECK(get(k_special, out), "cache hit for special char value after reload");
    CHECK_EQ(out, special_value);
}

#include "i18n/detect.h"
void test_detect() {
    using namespace i18n::detect;
    CHECK(script_of(L"\x043F\x0440\x0438\x0432\x0435\x0442") == Script::Cyrillic, "script_of Cyrillic");
    CHECK(script_of(L"hello") == Script::Latin, "script_of Latin");
    CHECK(is_probably_english(L"you need to go to the store"), "is_probably_english true");
    CHECK(!is_probably_english(L"hola como estas amigo"), "is_probably_english false");
    CHECK(!worth_translating(L"the door is open", true), "skip English when target EN");
    CHECK(worth_translating(L"\x043F\x0440\x0438\x0432\x0435\x0442", true), "translate Cyrillic");
    CHECK(!worth_translating(L"12345", true), "skip pure digits");
}

#include "i18n/json_min.h"
void test_json() {
    using namespace i18n::json;
    // Original tests
    CHECK_EQ(escape("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));
    CHECK_EQ(build_units({"hi","yo"}), std::string("[{\"id\":0,\"text\":\"hi\"},{\"id\":1,\"text\":\"yo\"}]"));
    std::vector<Unit> u;
    CHECK(parse_units("[{\"id\":0,\"text\":\"hola\"},{\"id\":1,\"text\":\"que\"}]", u), "parse_units should succeed");
    CHECK_EQ((int)u.size(), 2);
    CHECK_EQ(u[1].text, std::string("que"));
    CHECK(!parse_units("not json", u), "parse_units should fail on invalid input");
    std::string content;
    CHECK(extract_message_content("{\"choices\":[{\"message\":{\"content\":\"[{\\\"id\\\":0,\\\"text\\\":\\\"hi\\\"}]\"}}]}", content), "extract_message_content should succeed");
    CHECK(content.find("hi") != std::string::npos, "extracted content should contain 'hi'");

    // UTF-8 encoding tests: \uXXXX should decode to proper UTF-8 bytes
    // Cyrillic 'п' (U+043F) should encode as 2 UTF-8 bytes: 0xD0 0xBF
    std::string cyrillic_text;
    CHECK(extract_message_content("{\"choices\":[{\"message\":{\"content\":\"\\u043f\"}}]}", cyrillic_text), "extract Cyrillic \\u escape");
    CHECK_EQ((int)cyrillic_text.size(), 2);
    CHECK_EQ((unsigned char)cyrillic_text[0], (unsigned char)0xd0);
    CHECK_EQ((unsigned char)cyrillic_text[1], (unsigned char)0xbf);

    // Emoji surrogate pair: U+1F600 (😀) is \ud83d\ude00 in JSON
    std::string emoji_text;
    CHECK(extract_message_content("{\"choices\":[{\"message\":{\"content\":\"\\ud83d\\ude00\"}}]}", emoji_text), "extract emoji surrogate pair");
    CHECK_EQ((int)emoji_text.size(), 4);
    CHECK_EQ((unsigned char)emoji_text[0], (unsigned char)0xf0);
    CHECK_EQ((unsigned char)emoji_text[1], (unsigned char)0x9f);
    CHECK_EQ((unsigned char)emoji_text[2], (unsigned char)0x98);
    CHECK_EQ((unsigned char)emoji_text[3], (unsigned char)0x80);

    // Parse_units with unknown first field should succeed
    std::vector<Unit> u2;
    CHECK(parse_units("[{\"x\":1,\"id\":0,\"text\":\"y\"}]", u2), "parse_units with unknown first field");
    CHECK_EQ((int)u2.size(), 1);
    CHECK_EQ(u2[0].text, std::string("y"));

    // Parse_units with reordered fields
    std::vector<Unit> u3;
    CHECK(parse_units("[{\"text\":\"z\",\"id\":0}]", u3), "parse_units with reordered fields");
    CHECK_EQ((int)u3.size(), 1);
    CHECK_EQ(u3[0].text, std::string("z"));
}

#include "i18n/protect.h"
void test_protect() {
    using namespace i18n::protect;
    Masked m = mask("Kevin says: type /car alpha {FF0000}now");
    CHECK(m.translatable, "message with surviving prose should be translatable");
    CHECK(m.text.find("/car") == std::string::npos, "command masked out");    // command masked out
    CHECK(m.text.find("{FF0000}") == std::string::npos, "colour masked out"); // colour masked out
    CHECK(m.text.find("Kevin") == std::string::npos, "name masked out");      // name masked out
    // round trip: a faithful translation keeps the placeholders
    std::string translated = m.text; // pretend the model returned it unchanged
    CHECK(placeholders_intact(translated, m.tokens.size()), "faithful echo keeps placeholders intact");
    std::string back = unmask(translated, m.tokens);
    CHECK(back.find("/car") != std::string::npos, "unmask restores command");
    CHECK(back.find("{FF0000}") != std::string::npos, "unmask restores colour");
    // a dropped placeholder is rejected
    CHECK_EQ(unmask("no placeholders here", m.tokens), std::string(""));
    // pure command / numbers => not translatable (skip, no API call)
    CHECK(!mask("/quit").translatable, "pure command is not translatable");
    CHECK(!mask("$5000").translatable, "pure money amount is not translatable");

    // --- Review fix regressions ---------------------------------------

    // CRITICAL 1: a roleplay command's narrative remainder must stay
    // translatable -- only the command word itself is data.
    Masked meMsg = mask("/me smiles warmly");
    CHECK(meMsg.text.find("/me") == std::string::npos, "/me command is masked out");
    CHECK(meMsg.text.find("smiles") != std::string::npos, "narrative word 'smiles' survives translatable");
    CHECK(meMsg.text.find("warmly") != std::string::npos, "narrative word 'warmly' survives translatable");
    CHECK(meMsg.translatable, "roleplay narration after /me is translatable");

    // CRITICAL 2a: a speech-verb prefix ("Name says:") still masks the name
    // and still round-trips.
    Masked saysMsg = mask("Kevin says: hola");
    CHECK(saysMsg.text.find("Kevin") == std::string::npos, "speech-verb prefix still masks the name");
    CHECK(placeholders_intact(saysMsg.text, saysMsg.tokens.size()), "speech-verb prefix message round trips");
    CHECK(unmask(saysMsg.text, saysMsg.tokens).find("Kevin") != std::string::npos,
          "unmask restores the speaker name");

    // CRITICAL 2b: an ordinary lead word followed by ':' but with NO speech
    // verb (e.g. a system/log-style message) must NOT be masked as a name.
    Masked warnMsg = mask("Warning: server restarting in 5 minutes");
    CHECK(warnMsg.text.find("Warning") != std::string::npos,
          "plain lead word without a speech verb is not masked as a name");

    // IMPORTANT 3: an out-of-range/hallucinated placeholder (index equal to
    // tokens.size(), i.e. never handed out by mask()) must be rejected, not
    // silently copied through as raw control bytes.
    std::string outOfRange;
    outOfRange.push_back('\x01');
    outOfRange += std::to_string(m.tokens.size());
    outOfRange.push_back('\x02');
    std::string hallucinated = m.text + outOfRange;
    CHECK(!placeholders_intact(hallucinated, m.tokens.size()),
          "an out-of-range placeholder fails placeholders_intact");
    CHECK_EQ(unmask(hallucinated, m.tokens), std::string(""));
}

#include "i18n/charset.h"
void test_charset() {
    using namespace i18n::charset;
    // 1251 round trip for a Cyrillic byte (0xEF = 'п' in cp1251)
    std::string ru = "\xEF\xF0\xE8\xE2\xE5\xF2"; // "привет" in cp1251
    std::wstring w = decode(ru, 1251);
    CHECK(!w.empty(), "decode should not be empty");
    CHECK_EQ(encode(w, 1251), ru);
    // utf8 round trip
    CHECK_EQ(from_utf8(to_utf8(w)), w);
    CHECK_EQ(guess_codepage(ru), 1251);
    CHECK_EQ(guess_codepage("hello world"), 1252);
    // cap + % strip
    CHECK(cap_and_sanitize("100% sure", 64).find('%') == std::string::npos, "% should be removed");
    CHECK(cap_and_sanitize(std::string(200,'x'), 144).size() <= 144, "should truncate to maxBytes");

    // Regression tests for bug fixes
    // IMPORTANT 1: cap_and_sanitize must handle incomplete UTF-8 sequences
    // Euro sign (€) is U+20AC = UTF-8 {0xE2, 0x82, 0xAC} (3 bytes)
    // Test: "x" (0x78) + € capped to 2 bytes should drop the incomplete Euro
    std::string x_euro = "x\xE2\x82\xAC";
    std::string capped_x = cap_and_sanitize(x_euro, 2);
    CHECK_EQ(capped_x, std::string("x"));
    CHECK(capped_x.find('\xE2') == std::string::npos, "lead byte of incomplete sequence must be gone");

    // Test: "a" + é (U+00E9 = UTF-8 {0xC3, 0xA9}) capped to 2 should drop incomplete é
    std::string a_e_acute = "a\xC3\xA9";
    std::string capped_ae = cap_and_sanitize(a_e_acute, 2);
    CHECK_EQ(capped_ae, std::string("a"));

    // Test: emoji (U+1F600 = {0xF0, 0x9F, 0x98, 0x80}) capped to 1/2/3 should have nothing left
    std::string emoji = "\xF0\x9F\x98\x80";
    CHECK_EQ(cap_and_sanitize(emoji, 1), std::string(""));
    CHECK_EQ(cap_and_sanitize(emoji, 2), std::string(""));
    CHECK_EQ(cap_and_sanitize(emoji, 3), std::string(""));
    CHECK_EQ(cap_and_sanitize(emoji, 4), emoji);

    // IMPORTANT 2: guess_codepage must use proportion of high bytes over total
    // "café" in cp1252 is {0x63, 0x61, 0x66, 0xE9} (4 bytes, 1 high => 25% > 33% threshold is false)
    std::string cafe_cp1252 = "\x63\x61\x66\xE9";
    CHECK_EQ(guess_codepage(cafe_cp1252), 1252);

    // "привет" (hello in Russian) has many high bytes, should return 1251
    CHECK_EQ(guess_codepage(ru), 1251);
}

#include "i18n/breaker.h"
void test_provider_breaker() {
    using namespace i18n;
    // K=3 consecutive failures trips the breaker for T=15000ms, using a fake
    // clock (no real sleeping, no network) so this drives entirely in-process.
    Breaker b; // defaults: K=3, T=15000
    unsigned long now = 1000;

    CHECK(b.allow(now), "breaker starts closed");
    b.on_failure(now); // failure 1
    CHECK(b.allow(now), "still allowed after 1 failure");
    b.on_failure(now); // failure 2
    CHECK(b.allow(now), "still allowed after 2 failures");
    b.on_failure(now); // failure 3 -> trips the breaker
    CHECK(!b.allow(now), "4th allow() short-circuits after 3 consecutive failures");

    // Still open just before T ms have elapsed.
    CHECK(!b.allow(now + 14999), "still open just under T ms later");

    // T ms elapse -> half-open, allow() returns true again.
    CHECK(b.allow(now + 15000), "allow() true again after T ms elapse");

    // A success resets the failure counter and closes the circuit.
    b.on_success();
    CHECK(b.allow(now), "closed immediately after a success");
    b.on_failure(now);
    b.on_failure(now);
    CHECK(b.allow(now), "only 2 failures after reset -- still below K, stays closed");
}
