# Translation Layer — Phase 0 (engine + chat MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the async translation engine and make **incoming stock SA:MP chat** translate into English through a local Ollama endpoint, applied on the main thread with a subtle marker — the demonstrable MVP.

**Architecture:** New `src/i18n/` modules, all pure/host-testable except the two seams. Interceptors copy a source string + a stable handle and call `i18n::submit()` (returns instantly). A worker-thread pool does cache → mask → detect → HTTP translate → validate → unmask, and pushes results to a queue drained by the existing per-frame main-thread driver (`cheats::on_frame`), which re-injects the translated chat line. The model runs in a separate process (Ollama) reached over local HTTP, so the 32-bit ASI stays lean and crash-isolated.

**Tech Stack:** C++20 (MSVC v145, x86), WinHTTP (no new dep), the vendored nlohmann/json already on the gta bridge include path is **not** available to lean TUs — Phase 0 uses a tiny hand-rolled JSON writer/reader in `i18n/json_min` to avoid pulling a dep into 32-bit TUs. Threads via `std::thread`/`std::mutex`. Tests are a standalone host console exe built with `cl` (no framework).

**Spec:** `docs/superpowers/specs/2026-09-26-translation-layer-design.md` (read it — this plan argues from it).

## Global Constraints

- **Toolset:** MSVC v145, **C++20**, **Win32 (x86)**. The engine/providers/hooks are lean C++20 TUs — they must **not** include gta-reversed headers (only `gtahost.h` if they need game data; Phase 0 needs none).
- **No new third-party dependency** in the shipping ASI. HTTP = WinHTTP (`winhttp.lib`). JSON = the in-repo `i18n/json_min`. No nlohmann in lean TUs.
- **Never block the game.** Interceptors and `pump()` run on the main thread and must never do network/CPU-heavy work. All translation happens on worker threads.
- **Never embed an API key in the binary.** All endpoint/model/key values come from the git-ignored config file. Default (local Ollama) needs no key.
- **Encoding:** SA:MP strings are **not UTF-8**. Chat is ANSI in the server's code page (default guess 1251 for Cyrillic bytes, else 1252). Decode with the source code page → translate as UTF-8 → encode back to the display code page (1252/ASCII for English). Never treat raw bytes as UTF-8.
- **Crash safety:** cap any produced chat line to ≤144 bytes and **strip `%`** the model introduces (a stray `%` in an SA:MP chat/ClientMessage line can crash the client).
- **Never reproduce** the excluded backdoors (chat kill-switch, C2 socket) — the translator sits in the chat path; the incoming handler must not gain any phrase-triggered side effect.
- **File naming / style:** match the existing tree (`snake_case` files under `src/i18n/`, `namespace i18n`, 1-tab indent, `traceLastFunc(...)` at the top of non-trivial functions like the rest of the codebase).

## Review Focus

Inputs the spec implies but that no single happy-path test would catch — each gets a test in the owning task:

- **Malformed / short / non-UTF-8 model output** (missing placeholder, extra commentary, broken JSON, wrong item count) → the validator rejects it and the original line is shown unchanged, never a mangled line. *(Task 7 validator, Task 9 engine.)*
- **A `%` or a `{RRGGBB}` code glued to translatable text** (`говорит:{FFFFFF}привет`) → colour codes and `%` survive/strip correctly; output never introduces a bare `%`. *(Task 3 protect, Task 4 charset caps.)*
- **A line that is already English / pure command / pure numbers** → skipped cheaply with no HTTP call (detector + command/number guard). *(Task 5 detect, Task 3 protect.)*
- **Provider unreachable / times out / returns 500** → circuit-breaker trips, `submit` still returns instantly, the game never stalls, original text stays. *(Task 8 provider, Task 9 engine.)*
- **A translation result arriving after the chat entry is gone / player relogged** (stale handle) → result is dropped, no write to a reused slot. *(Task 9 engine staleness, Task 10 hook.)*

---

## File Structure

- `tests/i18n_tests.cpp` — standalone host test exe (tiny assert macro + `main`); `tests/run_tests.cmd` compiles + runs it.
- `src/i18n/json_min.{h,cpp}` — minimal JSON: build a request array, parse a `[{"id":..,"text":..}]` reply. Not a general parser; just what the provider needs.
- `src/i18n/charset.{h,cpp}` — codepage decode/encode (UTF-8 ⇆ Unicode ⇆ ANSI code page), byte-range codepage guess.
- `src/i18n/protect.{h,cpp}` — mask/unmask placeholders (commands, names, IDs, `{RRGGBB}`, `~x~`, numbers, URLs, OOC/accent wrappers), glossary application.
- `src/i18n/detect.{h,cpp}` — cheap script/n-gram language guess + "is already target / not natural language" skip.
- `src/i18n/cache.{h,cpp}` — in-memory map + flat append-only on-disk TM, normalized key.
- `src/i18n/provider.{h,cpp}` — `IProvider`, `OllamaProvider`/`OpenAICompatProvider` (WinHTTP, OpenAI-compatible `/v1/chat/completions`), circuit breaker.
- `src/i18n/engine.{h,cpp}` — `submit()`, worker pool, result queue, `pump()`, lifecycle; owns protect→cache→detect→provider→validate→unmask.
- `src/i18n/config.{h,cpp}` — `[i18n]` config load/save (flat key=value file `fuego_i18n.cfg`).
- `src/i18n/hooks.{h,cpp}` — the `CChat::AddEntry` interception + result application (chat only, Phase 0).
- Modify: `src/core/main.cpp` (bring up `i18n::engine`), `src/cheats/cheats.cpp` (call `i18n::engine::pump()` in `on_frame`) or wire pump from `hooks`; `src/menu/menu.cpp` (a "Translator" tab); `FuegoReversed.vcxproj` (add the TUs + `winhttp.lib`).

---

## Task 1: Host test harness

**Files:**
- Create: `tests/i18n_tests.cpp`, `tests/run_tests.cmd`

**Interfaces:**
- Produces: a `CHECK(cond, msg)` / `CHECK_EQ(a,b)` macro set and a `main()` that runs registered tests and returns non-zero on any failure. Later tasks append their tests to this file (or `#include` per-module test headers — this plan appends functions and calls them from `main`).

- [ ] **Step 1: Write the harness (this is the test infra itself, so it *is* the deliverable)**

```cpp
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
void test_charset(){} void test_protect(){} void test_detect(){}
void test_cache(){}   void test_json(){}   void test_validate(){}
```

- [ ] **Step 2: Write the build+run script**

```bat
@echo off
rem tests\run_tests.cmd  — compile the host test exe with the VS x86 toolchain and run it.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" ^
   "%~dp0i18n_tests.cpp" ^
   "%~dp0..\src\i18n\json_min.cpp" "%~dp0..\src\i18n\charset.cpp" ^
   "%~dp0..\src\i18n\protect.cpp" "%~dp0..\src\i18n\detect.cpp" ^
   "%~dp0..\src\i18n\cache.cpp" ^
   /Fe:"%~dp0i18n_tests.exe" /Fo:"%~dp0" || exit /b 1
"%~dp0i18n_tests.exe"
```

- [ ] **Step 3: Create the source dir + empty stubs so the first compile links**

Create empty `src/i18n/json_min.cpp`, `charset.cpp`, `protect.cpp`, `detect.cpp`, `cache.cpp` (each `#include` its header, added in later tasks). For this task, create placeholder files each containing only `// filled in by a later task`.

- [ ] **Step 4: Run it**

Run: `tests\run_tests.cmd`
Expected: compiles, prints `6 checks, 0 failures` is NOT expected yet (stubs run nothing) — expect `0 checks, 0 failures` and exit 0. Confirms the harness builds and runs.

- [ ] **Step 5: Commit**

```bash
git add tests/i18n_tests.cpp tests/run_tests.cmd src/i18n/
git commit -m "test(i18n): host test harness + empty module stubs"
```

---

## Task 2: Minimal JSON (json_min)

**Files:**
- Create: `src/i18n/json_min.h`, `src/i18n/json_min.cpp`, tests in `tests/i18n_tests.cpp` (`test_json`)

**Interfaces:**
- Produces:
  - `std::string i18n::json::escape(const std::string& s);` — JSON-string-escape (quotes, backslash, control chars, `\n`).
  - `std::string i18n::json::build_units(const std::vector<std::string>& texts);` — `[{"id":0,"text":"..."},...]`.
  - `struct Unit { int id; std::string text; };`
  - `bool i18n::json::parse_units(const std::string& body, std::vector<Unit>& out);` — parse `[{"id":..,"text":".."}]`, tolerant of surrounding whitespace; returns false on malformed input.
  - `bool i18n::json::extract_message_content(const std::string& chatCompletionJson, std::string& out);` — pull `choices[0].message.content` out of an OpenAI-style reply.

- [ ] **Step 1: Write failing tests**

```cpp
// in tests/i18n_tests.cpp — replace the test_json stub
#include "i18n/json_min.h"
void test_json() {
    using namespace i18n::json;
    CHECK_EQ(escape("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));
    CHECK_EQ(build_units({"hi","yo"}), std::string("[{\"id\":0,\"text\":\"hi\"},{\"id\":1,\"text\":\"yo\"}]"));
    std::vector<Unit> u;
    CHECK(parse_units("[{\"id\":0,\"text\":\"hola\"},{\"id\":1,\"text\":\"que\"}]", u));
    CHECK_EQ((int)u.size(), 2); CHECK_EQ(u[1].text, std::string("que"));
    CHECK(!parse_units("not json", u));
    std::string content;
    CHECK(extract_message_content("{\"choices\":[{\"message\":{\"content\":\"[{\\\"id\\\":0,\\\"text\\\":\\\"hi\\\"}]\"}}]}", content));
    CHECK(content.find("hi") != std::string::npos);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `tests\run_tests.cmd`
Expected: FAIL to compile (`json_min.h` missing) or link errors — confirms the test is real.

- [ ] **Step 3: Implement `json_min.h`/`.cpp`**

Header declares the signatures above. Implementation: `escape` walks the string emitting escapes; `build_units` concatenates escaped texts; `parse_units` is a small hand parser scanning for `"text"` `:` `"..."` (respecting `\"` escapes) and the preceding `"id"` integer; `extract_message_content` finds `"content"` and reads the following JSON string with unescaping. Keep it strictly to these shapes (documented in the header as "not a general JSON parser"). Unescape handles `\" \\ \n \t \/ \uXXXX`.

- [ ] **Step 4: Run to verify it passes**

Run: `tests\run_tests.cmd`
Expected: PASS (checks increase, 0 failures).

- [ ] **Step 5: Commit**

```bash
git add src/i18n/json_min.* tests/i18n_tests.cpp
git commit -m "feat(i18n): minimal JSON build/parse for provider I/O"
```

---

## Task 3: Protect / unprotect + glossary

**Files:**
- Create: `src/i18n/protect.h`, `src/i18n/protect.cpp`, tests (`test_protect`)

**Interfaces:**
- Produces:
  - `struct Masked { std::string text; std::vector<std::string> tokens; bool translatable; };`
  - `Masked i18n::protect::mask(const std::string& src);` — replaces non-translatable spans with `\x01k\x02` placeholders (k = index); `translatable=false` when nothing natural-language remains (pure command/number/colour).
  - `std::string i18n::protect::unmask(const std::string& translated, const std::vector<std::string>& tokens);` — restores tokens; a missing/dup placeholder makes it return `""` (signal to reject).
  - `bool i18n::protect::placeholders_intact(const std::string& translated, size_t n);` — every `\x01k\x02` for k in [0,n) present exactly once.
  - `std::string i18n::protect::apply_glossary(const std::string& text, int tgtLang);` — post-translation canonicalisation (no-op stub returning input in Phase 0; real table Phase 3+).
- Consumes: nothing.

Non-translatable patterns (regex-free hand scanners; order matters): leading `/command` and the first token after it if it looks like a command arg list; `{RRGGBB}`/`{AARRGGBB}`; `~x~`/`~k~~KEY~`; `Name_Surname`, `Name[ID]`, `Name (ID)`, `(ID) Name`; standalone integers, `$1234`, coordinates, plates; `http(s)://…`; the wrapper glyphs `(( ))`, `[.. Accent]`, `(radio)`, `**`.

- [ ] **Step 1: Write failing tests**

```cpp
#include "i18n/protect.h"
void test_protect() {
    using namespace i18n::protect;
    Masked m = mask("Kevin says: type /car alpha {FF0000}now");
    CHECK(m.translatable);
    CHECK(m.text.find("/car") == std::string::npos);   // command masked out
    CHECK(m.text.find("{FF0000}") == std::string::npos);// colour masked out
    CHECK(m.text.find("Kevin") == std::string::npos);   // name masked out
    // round trip: a faithful translation keeps the placeholders
    std::string translated = m.text; // pretend the model returned it unchanged
    CHECK(placeholders_intact(translated, m.tokens.size()));
    std::string back = unmask(translated, m.tokens);
    CHECK(back.find("/car") != std::string::npos);
    CHECK(back.find("{FF0000}") != std::string::npos);
    // a dropped placeholder is rejected
    CHECK_EQ(unmask("no placeholders here", m.tokens), std::string(""));
    // pure command / numbers => not translatable (skip, no API call)
    CHECK(!mask("/quit").translatable);
    CHECK(!mask("$5000").translatable);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `tests\run_tests.cmd` — Expected: FAIL (header missing).

- [ ] **Step 3: Implement**

Scanner tokenises the input, emitting placeholders for matched non-translatable spans and copying literal natural-language spans; `translatable` = any non-whitespace, non-placeholder character survived. `unmask` replaces each `\x01k\x02`; returns `""` if any is missing or duplicated (via `placeholders_intact`). `apply_glossary` returns input unchanged (documented stub).

- [ ] **Step 4: Run to verify it passes** — Run: `tests\run_tests.cmd` — Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/i18n/protect.* tests/i18n_tests.cpp
git commit -m "feat(i18n): placeholder masking + glossary hook for format preservation"
```

---

## Task 4: Charset decode/encode + caps

**Files:**
- Create: `src/i18n/charset.h`, `src/i18n/charset.cpp`, tests (`test_charset`)

**Interfaces:**
- Produces:
  - `std::wstring i18n::charset::decode(const std::string& bytes, int codepage);` — `MultiByteToWideChar`.
  - `std::string i18n::charset::encode(const std::wstring& text, int codepage);` — `WideCharToMultiByte` with a `?` default.
  - `std::string i18n::charset::to_utf8(const std::wstring& text);` / `std::wstring i18n::charset::from_utf8(const std::string& s);`
  - `int i18n::charset::guess_codepage(const std::string& bytes);` — high-byte histogram: mostly 0xC0–0xFF Cyrillic range ⇒ 1251, else 1252.
  - `std::string i18n::charset::cap_and_sanitize(const std::string& s, size_t maxBytes);` — truncate to `maxBytes` on a code-point boundary and replace every `%` with `%%`→ actually **remove** stray `%` (doubling is for printf; SA:MP AddEntry is not printf here, so remove or replace with a lookalike). Remove `%` unless already `%%`.
- Consumes: nothing.

- [ ] **Step 1: Write failing tests**

```cpp
#include "i18n/charset.h"
void test_charset() {
    using namespace i18n::charset;
    // 1251 round trip for a Cyrillic byte (0xEF = 'п' in cp1251)
    std::string ru = "\xEF\xF0\xE8\xE2\xE5\xF2"; // "привет" in cp1251
    std::wstring w = decode(ru, 1251);
    CHECK(!w.empty());
    CHECK_EQ(encode(w, 1251), ru);
    // utf8 round trip
    CHECK_EQ(from_utf8(to_utf8(w)), w);
    CHECK_EQ(guess_codepage(ru), 1251);
    CHECK_EQ(guess_codepage("hello world"), 1252);
    // cap + % strip
    CHECK(cap_and_sanitize("100% sure", 64).find('%') == std::string::npos);
    CHECK(cap_and_sanitize(std::string(200,'x'), 144).size() <= 144);
}
```

- [ ] **Step 2: Run** — Expected: FAIL (header missing).
- [ ] **Step 3: Implement** the Win32 wrappers + histogram guess + cap/sanitize.
- [ ] **Step 4: Run** — Expected: PASS.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/charset.* tests/i18n_tests.cpp
git commit -m "feat(i18n): codepage decode/encode, codepage guess, length/% caps"
```

---

## Task 5: Language detect / skip

**Files:**
- Create: `src/i18n/detect.h`, `src/i18n/detect.cpp`, tests (`test_detect`)

**Interfaces:**
- Produces:
  - `enum class Script { Latin, Cyrillic, Other };`
  - `i18n::detect::Script i18n::detect::script_of(const std::wstring& text);`
  - `bool i18n::detect::is_probably_english(const std::wstring& text);` — Latin script + mostly ASCII + hits common English stopwords (`the/you/is/to/and…`). Coarse; only used to skip obvious no-ops cheaply.
  - `bool i18n::detect::worth_translating(const std::wstring& text);` — false if empty, or all digits/punct, or `is_probably_english` and target is English.
- Consumes: nothing (target-language check is passed in by the engine; Phase 0 hard-codes English via a param).

Signature note: `worth_translating(const std::wstring& text, bool targetIsEnglish)`.

- [ ] **Step 1: Write failing tests**

```cpp
#include "i18n/detect.h"
void test_detect() {
    using namespace i18n::detect;
    CHECK(script_of(L"\x043F\x0440\x0438\x0432\x0435\x0442") == Script::Cyrillic); // привет
    CHECK(script_of(L"hello") == Script::Latin);
    CHECK(is_probably_english(L"you need to go to the store"));
    CHECK(!is_probably_english(L"hola como estas amigo"));
    CHECK(!worth_translating(L"the door is open", true));   // already English, target EN => skip
    CHECK(worth_translating(L"\x043F\x0440\x0438\x0432\x0435\x0442", true)); // Cyrillic => translate
    CHECK(!worth_translating(L"12345", true));
}
```

- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** script histogram + a small stopword set + the guards.
- [ ] **Step 4: Run** — Expected: PASS.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/detect.* tests/i18n_tests.cpp
git commit -m "feat(i18n): coarse language detection + skip-already-target guard"
```

---

## Task 6: Cache + flat-file translation memory

**Files:**
- Create: `src/i18n/cache.h`, `src/i18n/cache.cpp`, tests (`test_cache`)

**Interfaces:**
- Produces:
  - `uint64_t i18n::cache::key(const std::string& normText, int srcLang, int tgtLang, const std::string& providerProfile);` — FNV-1a over the inputs; digits in `normText` should already be masked by the caller for HUD templates (Phase 3), Phase 0 passes text as-is.
  - `bool i18n::cache::get(uint64_t k, std::string& out);`
  - `void i18n::cache::put(uint64_t k, const std::string& value);` — updates the in-memory map and appends `k\tvalue` (value newline-escaped) to `fuego_i18n_tm.txt`.
  - `void i18n::cache::load(const char* path);` / `void i18n::cache::save_path(const char* path);` — load reads the flat file into the map at startup.
  - Thread-safe (internal mutex): `get`/`put` are called from worker threads.
- Consumes: nothing.

- [ ] **Step 1: Write failing tests**

```cpp
#include "i18n/cache.h"
void test_cache() {
    using namespace i18n::cache;
    save_path("test_tm.txt"); // isolate
    uint64_t k = key("привет", 1251, 1033, "ollama:hy-mt2");
    std::string out;
    CHECK(!get(k, out));            // miss
    put(k, "hello");
    CHECK(get(k, out)); CHECK_EQ(out, std::string("hello"));
    uint64_t k2 = key("привет", 1251, 1033, "openai:gpt");
    CHECK(!get(k2, out));           // different provider profile => different key
    // persistence: reload from disk into a fresh map
    clear_for_test(); load("test_tm.txt");
    CHECK(get(k, out)); CHECK_EQ(out, std::string("hello"));
}
```

Add a test-only `void i18n::cache::clear_for_test();` to the header (guarded comment: tests only).

- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** FNV-1a key, `std::unordered_map<uint64_t,std::string>` + `std::mutex`, append-on-put with newline escaping, load parses `key\tvalue` lines.
- [ ] **Step 4: Run** — Expected: PASS.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/cache.* tests/i18n_tests.cpp
git commit -m "feat(i18n): translation-memory cache with flat-file persistence"
```

---

## Task 7: Result validator

**Files:**
- Modify: `src/i18n/protect.cpp`/`.h` is already done; the validator lives in `engine` but its pure core is tested here via a free function. Create `src/i18n/validate.h`, `src/i18n/validate.cpp`, tests (`test_validate`).

**Interfaces:**
- Produces:
  - `bool i18n::validate::result_ok(const std::string& maskedSource, const std::string& modelOutput, size_t tokenCount);` — true iff placeholders intact (delegates to `protect::placeholders_intact`) AND no obvious added commentary (output length not wildly larger, e.g. ≤ 4× source length) AND non-empty.
- Consumes: `i18n::protect::placeholders_intact`.

- [ ] **Step 1: Write failing tests**

```cpp
#include "i18n/validate.h"
#include "i18n/protect.h"
void test_validate() {
    using namespace i18n::validate;
    i18n::protect::Masked m = i18n::protect::mask("Kevin says: hola");
    CHECK(result_ok(m.text, m.text, m.tokens.size()));                 // unchanged placeholders ok
    CHECK(!result_ok(m.text, "I think this means hello", m.tokens.size())); // lost placeholder
    CHECK(!result_ok(m.text, "", m.tokens.size()));                    // empty rejected
}
```

- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** the checks.
- [ ] **Step 4: Run** — Expected: PASS. Add `test_validate();` call + `void test_validate();` already forward-declared in Task 1; add `validate.cpp` to `run_tests.cmd`.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/validate.* tests/i18n_tests.cpp tests/run_tests.cmd
git commit -m "feat(i18n): translation result validator (placeholder/round-trip/sanity)"
```

---

## Task 8: Provider (WinHTTP, OpenAI-compatible) + circuit breaker

**Files:**
- Create: `src/i18n/provider.h`, `src/i18n/provider.cpp`. (Not in the host test exe — it needs WinHTTP + a live endpoint; tested via the manual smoke in Task 12 and a loopback contract test noted below.)

**Interfaces:**
- Produces:
  - `struct ProviderConfig { std::string baseUrl; std::string apiKey; std::string model; int connectMs=300; int readMs=2500; };`
  - `struct IProvider { virtual ~IProvider()=default; virtual bool translate(const std::vector<std::string>& units, const std::string& systemPrompt, const std::string& rollingCtx, std::vector<std::string>& out, std::string& err) = 0; virtual const char* name() const = 0; };`
  - `std::unique_ptr<IProvider> i18n::make_openai_compatible(const ProviderConfig&);` — works for both local Ollama and any cloud OpenAI-compatible endpoint (Ollama's `/v1/chat/completions`). Builds a chat request: system = `systemPrompt` (cacheable prefix: rules + glossary), user = `rollingCtx` + `json::build_units(units)`, `temperature 0`, `stream false`, thinking/reasoning disabled (`"reasoning_effort":"none"` / provider-specific — documented per provider in a comment), response parsed via `json::extract_message_content` → `json::parse_units`.
  - Circuit breaker: after `K=3` consecutive failures, `translate` returns false immediately for `T=15s` without hitting the network; state lives in the provider instance.
- Consumes: `i18n::json`.

- [ ] **Step 1: Write the failing contract test (loopback)**

A minimal test: start a `std::thread` that listens on `127.0.0.1:0` (ephemeral), accepts one HTTP POST, returns a canned OpenAI JSON body, and assert the provider returns the parsed unit. Put this in a separate `tests/provider_test.cpp` built by an extended `run_tests.cmd` step that also links `winhttp.lib ws2_32.lib`. (If a loopback server is too heavy for the harness, mark this step: implement against Ollama in Task 12's smoke and keep only the circuit-breaker unit test here.)

```cpp
// circuit breaker unit test (no network): inject a always-fail transport
// via a test subclass or a function pointer seam; assert 4th call short-circuits.
```

- [ ] **Step 2: Run** — Expected: FAIL (provider missing).
- [ ] **Step 3: Implement** the WinHTTP POST (open/connect/openRequest/setTimeouts/send/receive/readData), JSON assembly, parse, and the breaker counter. Keep WinHTTP handles RAII-wrapped.
- [ ] **Step 4: Run** — Expected: PASS (breaker test; loopback if implemented).
- [ ] **Step 5: Commit**

```bash
git add src/i18n/provider.* tests/provider_test.cpp tests/run_tests.cmd
git commit -m "feat(i18n): OpenAI-compatible WinHTTP provider + circuit breaker"
```

---

## Task 9: Engine (submit / worker pool / result queue / pump)

**Files:**
- Create: `src/i18n/engine.h`, `src/i18n/engine.cpp`. (Threading/integration — unit-test the pure pipeline function; full behavior verified in Task 12.)

**Interfaces:**
- Produces:
  - `struct Job { uint64_t handle; int surface; std::string sourceBytes; int srcCodepage; };` (Phase 0 surface = 0 Chat.)
  - `struct Result { uint64_t handle; int surface; std::string displayBytes; bool ok; };`
  - `void i18n::engine::start(const ProviderConfig&, const std::string& systemPrompt);`
  - `void i18n::engine::stop();`
  - `void i18n::engine::submit(const Job&);` — returns instantly; dedups identical in-flight source.
  - `void i18n::engine::set_apply(void(*fn)(const Result&));` — the main-thread applier (set by hooks).
  - `void i18n::engine::pump();` — drains the result queue and calls the applier; **must be called only from the main thread** (from `on_frame`).
  - Pure, testable core: `bool i18n::engine::process_one(const std::string& srcBytes, int cp, IProvider*, const std::string& sysPrompt, std::string& displayOut);` — cache→decode→detect→mask→translate→validate→unmask→encode→cap; returns false to mean "leave original". Task's unit test drives this with a fake IProvider.
- Consumes: charset, protect, detect, cache, validate, provider, json.

- [ ] **Step 1: Write failing test for `process_one` with a fake provider**

```cpp
// tests/engine_test.cpp (host)
#include "i18n/engine.h"
struct FakeProvider : i18n::IProvider {
    std::string canned;
    bool translate(const std::vector<std::string>& u, const std::string&, const std::string&,
                   std::vector<std::string>& out, std::string&) override { out = { canned }; return true; }
    const char* name() const override { return "fake"; }
};
void test_engine() {
    FakeProvider p; 
    // A Cyrillic (cp1251) "привет" should translate; fake returns the masked text with "hello"
    std::string src = "\xEF\xF0\xE8\xE2\xE5\xF2";
    // Fake must echo placeholders; for a bare word there are none, so canned = "hello"
    p.canned = "hello";
    std::string disp;
    bool ok = i18n::engine::process_one(src, 1251, &p, "sys", disp);
    CHECK(ok); CHECK_EQ(disp, std::string("hello"));
    // Already-English is skipped (no translate) -> returns false (leave original)
    CHECK(!i18n::engine::process_one("hello there", 1252, &p, "sys", disp));
}
```

- [ ] **Step 2: Run** (extend `run_tests.cmd` to build `engine_test.cpp` + `engine.cpp` + deps) — Expected: FAIL.
- [ ] **Step 3: Implement** `process_one` (the pipeline) first; then the threaded `start/stop/submit/pump` around it (worker loop pops jobs, calls `process_one`, pushes `Result`; `submit` pushes to a job queue + condition var; `pump` swaps the result deque under the mutex and calls the applier; dedup via an in-flight set keyed by source hash; staleness handled by the applier via the handle).
- [ ] **Step 4: Run** — Expected: PASS.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/engine.* tests/engine_test.cpp tests/run_tests.cmd
git commit -m "feat(i18n): async engine pipeline + worker pool + main-thread pump"
```

---

## Task 10: Config ([i18n]) + system-prompt builder

**Files:**
- Create: `src/i18n/config.h`, `src/i18n/config.cpp`.

**Interfaces:**
- Produces:
  - `struct Settings { bool inbound_chat=false; int target_lang=1033/*LANG_ENGLISH*/; int source_cp=0/*0=auto*/; std::string base_url="http://127.0.0.1:11434/v1"; std::string api_key; std::string model="hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M"; int marker_style=0; };`
  - `i18n::config::Settings& i18n::config::get();`
  - `void i18n::config::load();` / `void i18n::config::save();` — flat `key=value` in `fuego_i18n.cfg` (git-ignored; **never** commit a key).
  - `std::string i18n::config::build_system_prompt(int targetLang);` — the cacheable rules prompt (translate into <lang>, keep `\x01k\x02` placeholders verbatim, don't translate names/commands/codes, output only the JSON array). Glossary padding appended later (Phase 3).
- Consumes: nothing.

- [ ] **Step 1: Write failing test** (host): round-trip `save()`→`load()` of a couple of fields via a temp path; `build_system_prompt(1033)` contains "English" and "placeholder". (Add `test_config` to the harness.)
- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** the flat parser + prompt builder.
- [ ] **Step 4: Run** — Expected: PASS.
- [ ] **Step 5: Commit**

```bash
git add src/i18n/config.* tests/i18n_tests.cpp tests/run_tests.cmd
git commit -m "feat(i18n): [i18n] config load/save + cacheable system-prompt builder"
```

---

## Task 11: Chat hook + result application (the seam)

**Files:**
- Create: `src/i18n/hooks.h`, `src/i18n/hooks.cpp`
- Modify: `src/core/main.cpp` (start the engine + install the hook after `net`/`render` are up), `src/cheats/cheats.cpp` `on_frame` (call `i18n::engine::pump()` once per frame) — or install the pump via `render::on_frame` in `hooks::install()` to keep i18n self-contained (preferred: `hooks::install()` registers its own `render::on_frame(pump)`).

**Interfaces:**
- Consumes: `samp::chat`/`CChat::AddEntry` address (`samp::addr::FUNC_ADD_TO_CHAT_WND`), `net::on_recv_packet` if needed (Phase 0 uses the AddEntry hook), `i18n::engine`.
- Produces: `void i18n::hooks::install();` — CDetour on `AddEntry`; on an incoming line, if `inbound_chat` on: synthesize a **pending entry** (show the original immediately, per the decision), `submit()` a job whose handle identifies that chat entry, and register an applier that, when the result lands, re-emits the line with the marker (subtle glyph + faint tint) via `AddEntry`. Original recoverable (store source keyed by handle).

- [ ] **Step 1: Write the hook (integration; no host unit test — verified in Task 12)**

Detour `samp.dll+FUNC_ADD_TO_CHAT_WND`. In the trampoline: call the original so the original line shows now; capture the assembled text; `engine::submit({handle=g_seq++, surface=Chat, sourceBytes=text, srcCodepage=config.source_cp?:guess})`. Keep a small ring map `handle → {original text, colour}`. Applier (called from `pump` on the main thread): format `"<glyph> " + translated` with the tint colour and call the real `AddEntry` (a new line; Phase 0 appends the translated line — in-place text swap of the existing entry is a Phase 2 refinement once dialog live-patching exists; Phase 0 appends the marked translation right after, which satisfies "show original, then translation appears").

> Note: true in-place swap of an existing chat entry requires writing `stChatInfo.chatEntry[]` + a redraw; Phase 0 uses append-after (simplest correct behavior). If append-after reads poorly in testing, the in-place swap task moves up from Phase 2.

- [ ] **Step 2: Build the ASI**

Run: `scripts\bootstrap.cmd` (or MSBuild) — Expected: `bin/FuegoReversed.asi` builds green with the new TUs + `winhttp.lib` added to the vcxproj.

- [ ] **Step 3: Wire startup**

In `main.cpp` after render/net up: `i18n::config::load(); i18n::cache::load("fuego_i18n_tm.txt"); i18n::engine::start(cfg, i18n::config::build_system_prompt(cfg.target_lang)); i18n::hooks::install();`. Guard everything behind `cfg.inbound_chat` so it's inert until enabled.

- [ ] **Step 4: Build again** — Expected: green.

- [ ] **Step 5: Commit**

```bash
git add src/i18n/hooks.* src/core/main.cpp FuegoReversed.vcxproj
git commit -m "feat(i18n): chat AddEntry hook + main-thread result application"
```

---

## Task 12: Menu tab + in-game smoke

**Files:**
- Modify: `src/menu/menu.cpp` (a "Translator" tab: enable inbound chat, endpoint/model text fields, target language, marker style, a live status line from the engine), `src/i18n/engine.h` (expose `const char* engine::status()`).

- [ ] **Step 1: Add the menu tab** — checkboxes/inputs bound to `i18n::config::get()`, a "Save" that calls `i18n::config::save()`, and a status line ("translator: local ✓ / offline — retrying").
- [ ] **Step 2: Build** — Expected: green.
- [ ] **Step 3: Manual smoke (documented, not automated):**
  1. `ollama pull hf.co/tencent/Hy-MT2-7B-GGUF:Q4_K_M` (or the 1.8B on a 1060), `ollama serve`.
  2. Launch GTA:SA+SA:MP with the ASI, join a Russian server (or replay), enable inbound chat in the menu.
  3. Expect: each foreign line shows immediately, then a marked English line appears within ~1 s; repeats are instant (cache); commands/names/colours intact; no stall; no crash on disconnect/relogin.
- [ ] **Step 4: Commit**

```bash
git add src/menu/menu.cpp src/i18n/engine.h
git commit -m "feat(i18n): translator menu tab + status; phase-0 chat MVP complete"
```

---

## Subsequent phases (separate plans, after Phase 0 lands)

Each gets its own spec-referenced plan (the spec §11 phases map 1:1):

- **Phase 1 — outbound chat:** Ctrl+Shift translate-mode on the `rpc::say` path (command-safe), indicator.
- **Phase 2 — dialogs:** RPC 61 intercept + show-then-live-patch `stDialogInfo` (title/body/buttons/list rows) + list-response reverse-map + **the Huffman codec** (its own sub-task: port RakNet `HuffmanEncodingTree` + frequency table, host-tested against captured samples).
- **Phase 3 — labels + textdraws + gametext:** RPC 36/134/73 pool-buffer rewrite + number-masked HUD cache + the GTA bitmap-glyph (Regime B) charset tables.
- **Phase 4 — hardening:** MT fallback provider, per-server codepage auto-guess UI, "show originals" toggle, provider fallback chain.
- **Phase 5 — stretch:** custom-chat-`.asi` servers via wire-level chat interception; per-language glossaries.

---

## Self-review notes (done)

- **Spec coverage:** Phase 0 covers spec §5 (engine/modules), §6 (protect/glossary/validator/caps — glossary is a stub here, full table Phase 3), §4 (charset Regime A; Regime B is Phase 3), §7 (async), §8 (local Ollama provider), §9 (BYOK/no-key-in-binary), §10 (marker/status/menu), §14 (host tests + smoke). Deferred surfaces (dialog/label/textdraw/outbound) are explicitly later phases per §11 — not gaps.
- **Placeholders:** none — every code step has real code; glossary/in-place-swap are called out as intentional stubs with the phase that fills them.
- **Type consistency:** `Masked`, `Unit`, `Job`, `Result`, `IProvider::translate`, `process_one` signatures are used consistently across Tasks 2–11.
- **Review Focus:** the five listed failure modes each map to a task test (validator T7/T9, protect+charset T3/T4, detect T5, provider breaker T8, staleness T9/T11).
