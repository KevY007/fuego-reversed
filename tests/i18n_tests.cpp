// tests/i18n_tests.cpp
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
int g_fail = 0, g_ran = 0; // external linkage: engine_test.cpp (a separate TU) shares these counters
#define CHECK(cond, msg) do { ++g_ran; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)
#define CHECK_EQ(a, b) do { ++g_ran; auto _a=(a); auto _b=(b); if(!(_a==_b)){ ++g_fail; \
    std::printf("FAIL %s:%d  expected equal\n", __FILE__, __LINE__); } } while(0)
// Forward-declare per-module test entry points; each later task defines one.
void test_charset();  void test_protect();  void test_detect();
void test_cache();    void test_json();     void test_validate();
void test_provider_breaker();
void test_join_url();
void test_batch();
void test_engine();
void test_config();
void test_names();
int main() {
    test_json(); test_charset(); test_protect(); test_names(); test_detect(); test_cache(); test_validate();
    test_provider_breaker();
    test_join_url();
    test_batch();
    test_engine();
    test_config();
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
#include <fstream>
#include <iterator>
static std::string slurp(const char* path) { // raw bytes, "" if missing
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void write_raw(const char* path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << bytes;
}
static std::string first_line(const char* path) { // text mode, like cache::load reads it
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}
static bool exists(const char* path) { std::ifstream f(path); return f.good(); }

void test_cache() {
    using namespace i18n::cache;
    save_path("test_tm.txt"); // isolate
    // Start from a fresh TM: load() of a missing file creates it holding just the
    // v2 header, so every put() below appends to a valid v2 file.
    std::remove("test_tm.txt");
    clear_for_test();
    load("test_tm.txt");
    CHECK(exists("test_tm.txt") && first_line("test_tm.txt") == "#fuego-tm 2",
          "load() of a missing TM creates it with the '#fuego-tm 2' header");
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

    // ---- I1: master wrote EMPTY / truncated values into the TM ----
    // An empty value is never stored (an empty hit would blank a dialog unit).
    uint64_t k_empty = key("pusto", 1251, 1033, "test:provider");
    put(k_empty, "");
    CHECK(!get(k_empty, out), "an empty put is not returned by get");
    put(k_special, "");
    CHECK(get(k_special, out) && out == special_value, "an empty put never overwrites a good entry");

    // A header-less (v1, master) TM is renamed to <path>.v1.bak -- replacing an
    // older .bak -- and the cache starts empty with a fresh v2 file.
    const std::string v1 = "123\thello\n456\t\n";
    std::remove("test_tm_v1.txt");
    write_raw("test_tm_v1.txt", v1);
    write_raw("test_tm_v1.txt.v1.bak", "an older backup\n");
    clear_for_test();
    load("test_tm_v1.txt");
    CHECK(!get(123, out) && !get(456, out), "a header-less TM loads nothing (empty map)");
    CHECK(slurp("test_tm_v1.txt.v1.bak") == v1, "the header-less TM is kept as <path>.v1.bak (old .bak replaced)");
    CHECK(first_line("test_tm_v1.txt") == "#fuego-tm 2", "a fresh v2 TM replaces the header-less one");
    load("test_tm_v1.txt"); // the fresh v2 file: nothing to rename, nothing to load
    CHECK(slurp("test_tm_v1.txt.v1.bak") == v1, "loading the fresh v2 TM again keeps the backup intact");

    // A v2 TM loads its entries; an empty value in it is skipped.
    std::remove("test_tm_v2.txt");
    std::remove("test_tm_v2.txt.v1.bak");
    write_raw("test_tm_v2.txt", "#fuego-tm 2\n123\thello\n456\t\n");
    clear_for_test();
    load("test_tm_v2.txt");
    CHECK(get(123, out) && out == "hello", "a v2 TM loads its entries");
    CHECK(!get(456, out), "an empty value in the TM is skipped on load");
    CHECK(!exists("test_tm_v2.txt.v1.bak"), "a v2 TM is never renamed");
    clear_for_test();
    std::remove("test_tm_v1.txt");
    std::remove("test_tm_v1.txt.v1.bak");
    std::remove("test_tm_v2.txt");

    // ---- migrate_legacy: the TM file was renamed (fuego_i18n_tm.txt ->
    // fuego_reversed_memory.txt). One file, no leftovers. ----
    const char* OLDP = "test_mig_old.txt";
    const char* NEWP = "test_mig_new.txt";
    const std::string OLDBAK = std::string(OLDP) + ".v1.bak";
    auto reset_mig = [&] { std::remove(OLDP); std::remove(NEWP); std::remove(OLDBAK.c_str()); };

    // Nothing there: nothing happens.
    reset_mig();
    Migration m0 = migrate_legacy(OLDP, NEWP);
    CHECK(!m0.moved && !m0.removedOld && !m0.removedBak && !exists(NEWP), "migrate: no old files -> no-op");

    // A v2 old file holds GOOD entries: it is moved to the new name, byte for byte.
    reset_mig();
    const std::string good = "#fuego-tm 2\n123\thello\n";
    write_raw(OLDP, good);
    Migration m1 = migrate_legacy(OLDP, NEWP);
    CHECK(m1.moved && !exists(OLDP) && slurp(NEWP) == good, "migrate: a v2 old TM is moved to the new name");

    // A pre-v2 old file (master's, damaged) and its .v1.bak are deleted, not moved.
    reset_mig();
    write_raw(OLDP, "123\thello\n");
    write_raw(OLDBAK.c_str(), "456\tworld\n");
    Migration m2 = migrate_legacy(OLDP, NEWP);
    CHECK(m2.removedOld && m2.removedBak && !m2.moved, "migrate: a pre-v2 old TM and its .v1.bak are removed");
    CHECK(!exists(OLDP) && !exists(OLDBAK.c_str()) && !exists(NEWP), "migrate: nothing old is left, nothing new is made");

    // Both exist (e.g. an earlier move failed and load() then created a fresh new
    // file): the old v2 entries are MERGED in, never lost; the new file's entries win
    // on a clash; the old file is removed only after the merge is safely written.
    reset_mig();
    write_raw(NEWP, "#fuego-tm 2\n789\tkept\n123\tnewer\n");
    write_raw(OLDP, "#fuego-tm 2\n123\thello\n456\tworld\n");
    Migration m3 = migrate_legacy(OLDP, NEWP);
    CHECK(m3.merged && !m3.moved && !exists(OLDP), "migrate: an old v2 TM is merged into an existing new one");
    clear_for_test();
    load(NEWP);
    std::string v;
    CHECK(get(456, v) && v == "world", "migrate: an old-only entry survives the merge");
    CHECK(get(789, v) && v == "kept", "migrate: a new-only entry survives the merge");
    CHECK(get(123, v) && v == "newer", "migrate: on a clash the NEW file's entry wins");

    // The failed-move scenario end to end: the new file holds only the header.
    reset_mig();
    write_raw(NEWP, "#fuego-tm 2\n");
    write_raw(OLDP, good);
    Migration m4 = migrate_legacy(OLDP, NEWP);
    clear_for_test();
    load(NEWP);
    CHECK(m4.merged && get(123, v) && v == "hello", "migrate: a header-only new file never costs the old entries");
    clear_for_test();
    reset_mig();
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

    // Short English UI labels (buttons, titles, list cells) have no stopwords, so the
    // stopword test misses them; the UI-word list catches them (target English only).
    CHECK(is_english_ui_text(L"Select"), "ui: Select");
    CHECK(is_english_ui_text(L"Login"), "ui: Login");
    CHECK(is_english_ui_text(L"Withdraw"), "ui: Withdraw");
    CHECK(is_english_ui_text(L"Bank of San Andreas"), "ui: Bank of San Andreas");
    CHECK(is_english_ui_text(L"Select spawn zone"), "ui: Select spawn zone");
    CHECK(is_english_ui_text(L"Balance: {00FF00}$500"), "ui: inline colour codes and numbers are not words");
    CHECK(!worth_translating(L"Close", true), "an English button is not re-translated into English");
    CHECK(worth_translating(L"Close", false), "an English button IS translated for a non-English target");
    // Foreign words stay translated -- every word must be on the list.
    CHECK(!is_english_ui_text(L"Retirar"), "ui: Spanish Retirar is not English");
    CHECK(!is_english_ui_text(L"Salir"), "ui: Spanish Salir is not English");
    CHECK(!is_english_ui_text(L"Beenden"), "ui: German Beenden is not English");
    CHECK(!is_english_ui_text(L"Sat"), "ui: Turkish Sat (sell) is not English");
    CHECK(!is_english_ui_text(L"main theek hoon"), "ui: Roman Urdu is not English");
    CHECK(!is_english_ui_text(L"Gift"), "ui: false friends (German Gift = poison) are not on the list");
    // False friends in languages the app supports: each means something else there,
    // so a label or one-word chat line made of it must still be translated.
    const wchar_t* falseFriends[] = {
        L"Log",      // Roman Urdu/Hindi: people
        L"Chat",     // French: cat
        L"Or",       // French: gold
        L"Ban",      // Indonesian: tyre
        L"List",     // Polish: letter
        L"An",       // Romanian: year
        L"Gang",     // German: gear
        L"No",       // Polish/Czech (casual): yeah
        L"Fine",     // Italian: end
        L"Location", // French: rental
        L"Last",     // German: load
        L"Ty",       // Polish/Czech: you
        L"Store",    // French: window blind
        L"Fee",      // German: fairy
        L"Resume",   // French (unaccented): summary
        L"At",       // Turkish: horse
        L"to",       // Roman Urdu: so / then
        L"Rent",     // Norwegian: clean
        L"Map",      // Dutch: folder
        L"Car",      // French: coach / because
        L"Los",      // Polish: fate; Spanish: the
        L"On",       // Russian translit: he
        L"Bail",     // French: lease
    };
    for (const wchar_t* w : falseFriends)
        CHECK(!is_english_ui_text(w), "ui: a false friend is still translated");
    CHECK(!is_english_ui_text(L"\x0417\x0430\x043A\x0440\x044B\x0442\x044C"), "ui: Cyrillic is never English UI text");
    CHECK(!is_english_ui_text(L"Buy the best car in the whole city now"), "ui: long text is left to the stopword test");
    CHECK(!is_english_ui_text(L"12345"), "ui: no words -> not UI text");
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
    // cap_display: '%' strip + plain byte-truncate.
    CHECK(cap_display("100% sure", 64).find('%') == std::string::npos, "% should be removed");
    CHECK_EQ(cap_display(std::string(200,'x'), 144).size(), (size_t)144);

    // GREEN (fix round 1, controller-found): display text is single-byte-per-char
    // cp1251/cp1252 (config::display_codepage never returns anything else) -- a
    // plain high byte must never be treated as an incomplete UTF-8 lead/
    // continuation byte and deleted (master's old display sanitizer did exactly
    // that; see task-6-report.md's "Fix round 1" for the RED run against it).
    std::string privet = "\xEF\xF0\xE8\xE2\xE5\xF2";                  // "привет" (cp1251)
    std::string privet_mir = "\xEF\xF0\xE8\xE2\xE5\xF2 \xEC\xE8\xF0"; // "привет мир" (cp1251)
    std::string cafe = "caf\xE9";                                    // "café" (cp1252)
    CHECK_EQ(cap_display(privet, 64), privet);
    CHECK_EQ(cap_display(privet_mir, 64), privet_mir);
    CHECK_EQ(cap_display(cafe, 64), cafe);
    CHECK_EQ(cap_display(privet, 3), std::string("\xEF\xF0\xE8"));

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

#include "i18n/url_util.h"
void test_join_url() {
    using namespace i18n;
    // A baseUrl with a stray trailing slash (common config typo) must not
    // produce a doubled '/' at the seam -- many servers 404 on "//".
    CHECK_EQ(join_url("http://x/v1/", "/chat/completions"), std::string("http://x/v1/chat/completions"));
    CHECK_EQ(join_url("http://x/v1", "/chat/completions"), std::string("http://x/v1/chat/completions"));
}

#include "i18n/batch.h"
void test_batch() {
    using namespace i18n::batch;
    CHECK_EQ(numbered_block({ "Withdraw", "Deposit", "Balance" }),
             std::string("1. Withdraw\n2. Deposit\n3. Balance\n"));

    std::vector<std::string> out;
    // The exact layout comes back: each item on its own line, in order.
    CHECK(parse_numbered("1. Retirar\n2. Depositar\n3. Saldo", 3, out) && out.size() == 3 &&
          out[0] == "Retirar" && out[1] == "Depositar" && out[2] == "Saldo", "batch: exact layout parses");
    // Tolerated: blank lines, CRLF, leading spaces, "1)" / "1:" / "[1]" markers.
    CHECK(parse_numbered("\r\n 1) One\r\n\r\n2: Two\r\n[3] Three\r\n", 3, out) &&
          out[0] == "One" && out[1] == "Two" && out[2] == "Three", "batch: tolerant markers + blank lines");
    // An item the model left empty stays empty (the engine retries just that one).
    CHECK(parse_numbered("1. One\n2.\n3. Three", 3, out) && out[1].empty() && out[2] == "Three",
          "batch: an empty item is kept empty, not shifted");
    // A number inside the item text is part of the text.
    CHECK(parse_numbered("1. 2 players online\n2. Top 10", 2, out) && out[0] == "2 players online" &&
          out[1] == "Top 10", "batch: digits after the marker are item text");
    // STRICT: anything that could shift one item's translation onto another row fails.
    CHECK(!parse_numbered("1. One\n2. Two", 3, out) && out.empty(), "batch: a missing item fails (merged lines)");
    CHECK(!parse_numbered("1. One\n2. Two\n3. Three\n4. Four", 3, out), "batch: an extra item fails (split line)");
    CHECK(!parse_numbered("1. One\n3. Three\n2. Two", 3, out), "batch: out of order fails");
    CHECK(!parse_numbered("1. One\n1. Uno\n2. Two", 2, out), "batch: a repeated number fails");
    CHECK(!parse_numbered("1. One\ncontinued here\n2. Two", 2, out), "batch: an unnumbered line fails");
    CHECK(!parse_numbered("Here you go:\n1. One\n2. Two", 2, out), "batch: a chatty preamble fails");
    CHECK(!parse_numbered("", 2, out), "batch: an empty reply fails");
    CHECK(!batch_instruction(3).empty(), "batch: the instruction is not empty");
}

#include "i18n/config.h"
void test_config() {
    using namespace i18n::config;

    // Round-trip test: save and load via temp path
    Settings& s = get();

    // Set some fields to non-default values
    s.inbound_chat = true;
    s.target_lang = 1049;  // Russian
    s.model = "custom-model";
    s.api_key = "test-key-123";
    s.provider_kind = 2;   // Anthropic
    s.request_min_gap_ms = 1500;
    s.source_lang = 1049;  // Russian (source hint)
    s.hotkey_mods = 5; s.hotkey_vk = 0x74; s.show_translate_indicators = false;

    // Save to temp file
    save("test_cfg.cfg");

    // Mutate get() to different values
    s.inbound_chat = false;
    s.target_lang = 1031;  // German
    s.model = "different-model";
    s.api_key = "different-key";
    s.provider_kind = 0;   // Custom
    s.request_min_gap_ms = 0;
    s.source_lang = 0;     // Auto
    s.hotkey_mods = 3; s.hotkey_vk = 0; s.show_translate_indicators = true;

    // Load from temp file - should restore original values
    load("test_cfg.cfg");

    CHECK_EQ(s.inbound_chat, true);
    CHECK_EQ(s.target_lang, 1049);
    CHECK_EQ(s.model, std::string("custom-model"));
    CHECK_EQ(s.api_key, std::string("test-key-123"));
    CHECK_EQ(s.provider_kind, 2);
    CHECK_EQ(s.request_min_gap_ms, 1500);
    CHECK_EQ(s.source_lang, 1049);
    CHECK_EQ(s.hotkey_mods, 5);
    CHECK_EQ(s.hotkey_vk, 0x74);
    CHECK_EQ(s.show_translate_indicators, false);

    // Test build_system_prompt(1033) contains "English" and "placeholder"
    std::string prompt = build_system_prompt(1033);
    CHECK(prompt.find("English") != std::string::npos, "build_system_prompt(1033) must contain 'English'");
    CHECK(prompt.find("placeholder") != std::string::npos, "build_system_prompt must mention 'placeholder'");

    // Auto source (sourceLang 0): no "source is written in" hint.
    CHECK(prompt.find("written in") == std::string::npos, "Auto source must NOT add a source hint");
    // Explicit source language adds a hint naming that language.
    std::string srcPrompt = build_system_prompt(1033, false, 1049); // target EN, source Russian
    CHECK(srcPrompt.find("written in Russian") != std::string::npos, "explicit sourceLang must hint 'Russian'");
}

void test_names() {
    using namespace i18n::protect;
    auto v = name_variants({ "Khalid_Ahmed", "John_Black", "The_Man", "KevY007", "Al" });
    auto has = [&](const char* s) { return std::find(v.begin(), v.end(), std::string(s)) != v.end(); };
    CHECK(has("Khalid_Ahmed") && has("Khalid Ahmed") && has("Khalid") && has("Ahmed"), "RP name: exact, spaced, parts");
    CHECK(has("John_Black") && has("John Black") && has("John") && !has("Black"), "common-word part not protected alone");
    CHECK(has("The_Man") && has("The Man") && !has("The") && !has("Man"), "'The'/'Man' parts not protected alone");
    CHECK(has("KevY007"), "non-RP name protected exactly");
    CHECK(!has("Al"), "names shorter than 3 chars skipped");
    CHECK(v.front().size() >= v.back().size(), "sorted longest first");

    set_names(name_variants({ "Khalid_Ahmed" }));
    Masked m = mask("tell khalid to come to the bank");
    CHECK(std::find(m.tokens.begin(), m.tokens.end(), std::string("khalid")) != m.tokens.end(), "lowercase first name masked");
    CHECK(m.text.find("khalid") == std::string::npos, "name hidden from the model");
    CHECK_EQ(unmask(m.text, m.tokens), std::string("tell khalid to come to the bank"));

    Masked m2 = mask("Khalid Ahmed wants his car back");
    CHECK(std::find(m2.tokens.begin(), m2.tokens.end(), std::string("Khalid Ahmed")) != m2.tokens.end(),
          "spaced full name masked as one token");

    Masked m3 = mask("khalidx is a different word");
    CHECK(m3.text.find("khalidx") != std::string::npos, "word boundary: 'khalidx' untouched");

    set_names({}); // reset global state for the tests that follow
}
