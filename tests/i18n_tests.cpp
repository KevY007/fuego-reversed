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
int main() {
    test_json(); test_charset(); test_protect(); test_detect(); test_cache(); test_validate();
    std::printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
// Stubs so it links before later tasks fill them in (each task replaces its stub).
void test_charset(){} void test_detect(){}
void test_cache(){}   void test_validate(){}

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
}
