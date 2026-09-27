# Translate-In-Place (Chat + Dialogs) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show every incoming chat line and server dialog in the player's language without the current "untranslated first / translated on the 2nd sight" behaviour, keep list/tablist dialogs structurally intact, never mistranslate player names, and translate the player's outgoing dialog input — all toggled and signposted from the Translator tab.

**Architecture:** Chat lines are shown immediately and, when their translation lands, rewritten *in place* inside SA:MP's chat entry buffer (`szText` + the redraw flag). Server dialogs that aren't in the translation memory are *held* (their ShowDialog RPC is swallowed), translated unit-by-unit (title, buttons, and every body row/cell separately, so a model can never break the `\n`/`\t` skeleton), then released through SA:MP's own `Show` routine in arrival order with the original inter-dialog timing. Player names from SA:MP's live player pool are masked before any text reaches the model. Outgoing dialog responses are remapped (list picks → the original row) or translated (input dialogs), never touching passwords.

**Tech Stack:** C++20, MSVC v145, Win32 **x86** (size_t is 32-bit), MSBuild via `scripts\build.cmd`, host tests compiled with `cl` via `tests\run_*.cmd`, Dear ImGui, WinHTTP-backed `i18n::engine`.

**Spec:** The design below was agreed with the user in-session on 2026-09-27 (also recorded in project memory `translation-layer.md`). The "Design" section is the binding spec.

## Global Constraints

- Work on branch `feat/i18n-translate-in-place`. Never commit to `master` directly. Never stage `samp.dll`, `fuego_reversed.cfg`, `fuego_reversed.log`, `fuego_i18n.cfg`, `fuego_i18n_tm.txt` (they hold API keys / runtime data).
- Every commit message ends with the line `Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC`.
- Build (from repo root, in Bash): `cmd //c "scripts\build.cmd > scratch_build.log 2>&1"; grep -iE "error C|error LNK|error MSB|fatal" scratch_build.log; tail -n 1 scratch_build.log; rm -f scratch_build.log`. Success = no error lines and the last line is `  FuegoReversed.vcxproj -> D:\hax0r\fuego-reversed\bin\FuegoReversed.asi`. The `LNK4088 /FORCE` warning is pre-existing and expected. If you see `"CL.exe" exited with code -1073741819` that is a known flaky MSVC internal compiler error — rerun the build once before investigating.
- Host tests (from repo root, in Bash): `cmd //c "call D:\hax0r\fuego-reversed\tests\<runner>.cmd 2>&1" | tail -n 1` — success line is `<N> checks, 0 failures`. Runners: `run_tests` (i18n), `run_surfaces_test`, `run_huffman_test`, `run_stringcompressor_test`, plus new `run_chatbuf_test`, `run_dlgq_test`.
- Every new `.cpp` under `src/` is added to `FuegoReversed.vcxproj` next to the other `src\i18n\*.cpp` entries with a unique object name: `<ClCompile Include="src\i18n\NAME.cpp"><ObjectFileName>$(IntDir)i18n_NAME.obj</ObjectFileName></ClCompile>`.
- NEVER call `dev->BeginScene()` / `dev->EndScene()` from inside `hkEndScene` (EndScene is a vtable hook — that recursed and froze the game in 2f2177a).
- All reads/writes of SA:MP memory and all calls into samp.dll happen on the game's main thread (RPC dispatch, `hooked_add_entry`, `engine::pump()` / `hooks::pump_tick`). Calls into samp.dll are wrapped in SEH (`__try/__except`) in a function with no C++ objects.
- No C++ exception may cross a game C-ABI seam: RPC handlers, detours and send-seam callbacks keep their `try { } catch (...) { }`.
- **Password dialogs (style 3) are never translated, never held, and their responses are never touched or logged.**
- A dialog id with `(int16_t)id < 0` (0x8000–0xFFFF) is the server's CLOSE signal: never translated; `Show(-1)` hides the dialog.
- SA:MP `Show` must be called with `bServerside = 1` and the server's original id; the title must be ≤ 255 bytes (Show copies it unbounded into a 256-byte field); the body ≤ 4095 bytes; skip the call if the title or body is empty (the RPC handler does the same).
- Confirmed samp.dll 0.3.DL offsets (disassembly + in-game test): chat `this` = `*(base+0x2ACA10)`; `chatEntry[0]` at `+0x132`, stride `0xFC`, `SystemTime` `+0x00`, `szText[144]` `+0x20`; newest entry `[99]`; the redraw flag AddEntry sets is `this+0x63DA` (NOT the header's `m_iRedraw`). Dialog `this` = `*(base+0x2AC9E0)`; `iIsActive` `+0x28`, `iType` `+0x2C`, `DialogID` (int16) `+0x30`; `Show` = `base+0x6FA50`, `__thiscall(this, int id, int style, char* caption, char* body, char* btn1, char* btn2, int bServerside)`.
- Never re-add the two excluded Project Fuego backdoors (the C2/remote-shell socket and the chat-triggered kill-switch phrases) — this plan edits the incoming chat/dialog path they abused.

## Design (agreed with the user — the spec)

1. **Chat (Option A, user-chosen):** a translatable CHAT/INFO line on a translation-memory hit is shown already translated (SA:MP wraps it correctly). On a miss it is shown immediately in the original (exact order + timing), and when the translation lands the rows that line became are rewritten in place — lines above/below never move. Failure/decline → the original stays. Long lines that wrapped differently may overflow; accepted.
2. **Dialogs = hold + timed sequential replay (wait-then-show):** a ShowDialog that is not fully in the translation memory is swallowed, translated, and released via SA:MP's `Show` strictly in arrival order, each release no sooner than the previous release + the ORIGINAL gap between the two arrivals (spammed "pages" keep their reading time; must work for 3–4+ pages). Not translated within 10 s → released as the original. Nothing held and fully cached → shown immediately (rewritten in place). Close signals and password dialogs are never translated but keep their place in the order when something is held.
3. **Cell-safe list/tablist translation:** rows split on `\n`, cells on `\t`; every separator is re-emitted byte-for-byte and stray `\t`/`\n` in a translation are turned into spaces; each unit's leading `{RRGGBB}` codes are re-attached verbatim; shortcode/number cells (`intro`, `/help`, `12m`, `1.15km`) are never translated; capitalised labels (`Withdraw`) are.
4. **Everything visible is translated; what's sent matches what the server expects:** button presses are indices (unaffected); a picked list/tablist row sends the ORIGINAL row's text (by `listItem` index); input-dialog text is translated into the server's language; password responses are untouched.
5. **Player names never translated:** names of connected players (by id, from SA:MP's player pool) — exact, RP `First Last`, and each `First`/`Last` part that isn't a common word — are masked before the model sees the text and restored with their original casing (`khalid` stays `khalid`).
6. **Outgoing translation is unified:** one toggle (`outbound_enabled`) and ONE settable hotkey (default Ctrl+Shift) cover outgoing chat and outgoing dialog input; one "your message will be translated" indicator; a "translating dialog…" indicator while a dialog is held; a master **Show translation indicators** setting, default ON.

## Review Focus

1. A translation that contains `\t` or `\n` for a list/tablist cell → the dialog must keep exactly the same row/column skeleton (user's explicit warning). Test: Task 1, "hostile translator" check.
2. Shortcode/number cells (`intro`, `12m`, `1.15km`) and each cell's own `{RRGGBB}` must come back verbatim. Test: Task 1 fixture checks on real captures 863/416.
3. Three or more dialogs spammed within a second, followed by a close → released in order, each after its original gap, close last; a late-translated earlier page never overtaken. Test: Task 5 checks 2, 3, 5.
4. A player's name in lowercase mid-sentence (`tell khalid to come`) or in `First Last` form must survive translation verbatim. Test: Task 2 `test_names`.
5. A PASSWORD dialog must never be translated, held, or have its response touched or logged, even with outgoing translation on. Display side pinned by Task 1's `never_translated` test (the real login capture is style 3); response side guarded in Task 7 (`answered_style(...) == 3` returns before anything else) and checked in the Task 9 in-game list.

## File Structure

| File | Responsibility |
|---|---|
| `src/i18n/surfaces.h` / `surfaces_rebuild.cpp` (modify) | PURE dialog decomposition (`plan_dialog`, units, `assemble_dialog`), verbatim-cell rule, response remap, Cyrillic-aware `is_dynamic_text`. Host-tested. |
| `src/i18n/protect.h` / `protect.cpp` (modify) | Live player-name masking (`set_names`, `name_variants`). Host-tested. |
| `src/i18n/names.h` / `names.cpp` (create) | Main-thread refresher: SA:MP player pool → `protect::set_names`. |
| `src/i18n/chatbuf.h` / `chatbuf.cpp` (create) | PURE in-place chat entry rewrite on the raw CChat buffer. Host-tested. |
| `src/i18n/dlgq.h` / `dlgq.cpp` (create) | PURE ordered, timing-faithful release queue for held dialogs. Host-tested. |
| `src/i18n/hooks.cpp` (modify) | Chat Option A (cache-hit instant / show-then-flip), result routing, per-frame ticks. |
| `src/i18n/surfaces.cpp` (modify) | Dialog hold/translate/release glue, response remap + outgoing input translation. |
| `src/i18n/engine.h` / `engine.cpp` (modify) | `Job::capBytes` so dialog lines aren't cut to 144 bytes. |
| `src/compat/compat.cpp` (modify) | Let `surfaces::on_incoming_rpc` swallow a held dialog. |
| `src/i18n/config.h` / `config.cpp` (modify) | `hotkey_mods`, `hotkey_vk`, `show_translate_indicators`; drop `debug_lab_replace`. |
| `src/i18n/indicator.h` / `indicator.cpp` (create) | On-screen translation hints. |
| `src/menu/menu.cpp` (modify) | Settable hotkey, indicator toggle, draw call, lab cleanup. |
| `src/i18n/lab.*` (modify) | Remove the LIVE replace experiment; keep read-only logging. |
| `tests/surfaces_test.cpp`, `tests/i18n_tests.cpp` (modify); `tests/chatbuf_test.cpp`, `tests/dlgq_test.cpp`, `tests/run_chatbuf_test.cmd`, `tests/run_dlgq_test.cmd` (create) | Tests. |

---

### Task 0: Branch + baseline

**Files:**
- Modify: `.gitignore`
- Commit (already in the working tree, verified in-game): `src/i18n/lab.h`, `src/i18n/lab.cpp`, `src/compat/compat.cpp`, `src/i18n/hooks.cpp`, `src/i18n/config.h`, `src/i18n/config.cpp`, `src/menu/menu.cpp`, `FuegoReversed.vcxproj`

**Interfaces:** Consumes nothing. Produces the branch every later task commits to.

- [ ] **Step 1: Create the branch**

Run: `cd /d/hax0r/fuego-reversed && git checkout -b feat/i18n-translate-in-place && git status --short`
Expected: `Switched to a new branch 'feat/i18n-translate-in-place'`; status lists the lab files as modified/untracked plus `?? samp.dll`.

- [ ] **Step 2: Ignore the reference samp.dll**

Append this line to `.gitignore`:

```
/samp.dll
```

- [ ] **Step 3: Verify the baseline builds and passes**

Run the Build command from Global Constraints, then `run_tests`, `run_surfaces_test`, `run_huffman_test`, `run_stringcompressor_test`.
Expected: clean build; `0 failures` on all four.

- [ ] **Step 4: Commit the baseline**

```bash
git add .gitignore FuegoReversed.vcxproj src/compat/compat.cpp src/i18n/config.cpp src/i18n/config.h src/i18n/hooks.cpp src/menu/menu.cpp src/i18n/lab.cpp src/i18n/lab.h docs/superpowers/plans/2026-09-27-translate-in-place.md
git commit -m "chore(i18n): debug lab that verified in-place chat/dialog replacement in-game

Read-only chat/dialog internals logging plus a LIVE experiment that rewrote a
shown chat line (szText + redraw flag @+0x63DA) and re-showed an open dialog
through SA:MP's Show (0x6FA50). Both confirmed working in-game (38 dialogs,
29 chat flips, 11 re-shows, 0 failures). Ignore the reference samp.dll.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 1: Cell-safe dialog units (pure) + Cyrillic-aware dynamic-text rule

**Files:**
- Modify: `src/i18n/surfaces.h`
- Modify: `src/i18n/surfaces_rebuild.cpp`
- Test: `tests/surfaces_test.cpp`

**Interfaces:**
- Consumes: `huffman::decode_string(const uint8_t*, size_t, std::string&)`, `huffman::encode_string(const std::string&) -> std::vector<uint8_t>`.
- Produces (namespace `i18n::surfaces`): `struct Unit { std::string prefix, core, suffix; bool translate; }`, `struct DialogPlan { uint16_t id; uint8_t style; bool haveBody; std::vector<uint8_t> rawBody; std::vector<Unit> units; std::vector<char> bodySeps; }`, `bool is_verbatim_cell(const std::string&)`, `Unit make_unit(const std::string&, bool cell)`, `bool plan_dialog(const uint8_t*, size_t, DialogPlan&)`, `std::vector<std::string> original_cores(const DialogPlan&)`, `std::string clean_translation(const std::string&)`, `std::string unit_text(const Unit&, const std::string& core)`, `std::string assemble_body(const DialogPlan&, const std::vector<std::string>&)`, `std::vector<uint8_t> assemble_dialog(const DialogPlan&, const std::vector<std::string>&)`, `bool never_translated(const DialogPlan&)` (password style 3 or `(int16_t)id < 0`). `units[0]` = title, `[1]` = button 1, `[2]` = button 2, `[3..]` = body units; `bodySeps[k]` follows body unit `k`.

- [ ] **Step 1: Write the failing tests**

In `tests/surfaces_test.cpp`, add `#include <algorithm>` below `#include <cstdint>`, then add these fixtures directly below the existing `kDialogLogin` definition:

```cpp
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
```

Replace the whole existing `// ---- Translated body: rebuilt szInfo must decode back to the translation ----` block (from that comment through its closing `}`) with:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_surfaces_test.cmd 2>&1" | tail -n 5`
Expected: compile errors — `DialogPlan`, `plan_dialog`, `is_verbatim_cell` etc. are undeclared.

- [ ] **Step 3: Declare the new API**

In `src/i18n/surfaces.h`, insert directly after the line `using Translator = std::function<bool(const std::string& src, std::string& out)>;`:

```cpp

// --- cell-safe dialog translation (pure) ------------------------------------
// A dialog is translated UNIT BY UNIT, never as one blob: units[0] = title,
// units[1] = button 1, units[2] = button 2, units[3..] = the body split on EVERY
// '\n' and '\t' (bodySeps[k] = the separator that followed body unit k). A model
// can therefore never add, drop or move a row/column separator -- rejoining is
// byte-exact by construction. Each unit's leading spaces + {RRGGBB} codes
// (prefix) and trailing spaces (suffix) are re-attached verbatim; only `core` is
// ever translated.
struct Unit {
    std::string prefix;
    std::string core;
    std::string suffix;
    bool        translate = false; // false -> core kept verbatim
};

struct DialogPlan {
    uint16_t             id = 0;
    uint8_t              style = 0;      // 0 msgbox 1 input 2 list 3 password 4 tablist 5 tablist+headers
    bool                 haveBody = false;
    std::vector<uint8_t> rawBody;        // undecodable body bytes, kept verbatim
    std::vector<Unit>    units;
    std::vector<char>    bodySeps;
};

// A LIST/TABLIST cell that must stay verbatim: empty, numbers/units/timers
// ("12m", "1.15km", "$500"), or a single ASCII token that is all-lowercase or
// carries a digit, '/' or '_' -- shortcodes/commands the player types ("intro",
// "/help", "cmd_2"). Capitalised words ("Withdraw", "No") and phrases are labels
// and DO get translated; so is any non-Latin (e.g. cp1251) word.
bool is_verbatim_cell(const std::string& core);

// Split a raw piece into prefix/core/suffix. `cell` selects the list-cell rule;
// otherwise a unit is translated unless empty or mostly numeric.
Unit make_unit(const std::string& raw, bool cell);

// Parse a ShowDialog (RPC 61) payload into units. False if malformed.
bool plan_dialog(const uint8_t* pkt, size_t n, DialogPlan& out);

// The cores that reproduce the packet unchanged (unit.core for every unit).
std::vector<std::string> original_cores(const DialogPlan& plan);

// A translated core with '\t' / '\n' turned into spaces and '\r' dropped, so a
// model's stray separator can never break the dialog's row/column structure.
std::string clean_translation(const std::string& s);

// prefix + core + suffix, using the ORIGINAL core when the unit isn't
// translatable or `core` equals it (keeps identity byte-exact), else the
// cleaned translated core.
std::string unit_text(const Unit& u, const std::string& core);

// The body text from units[3..] + bodySeps with the given cores.
std::string assemble_body(const DialogPlan& plan, const std::vector<std::string>& cores);

// Re-encode a full RPC 61 payload. `cores` is indexed like plan.units (a missing
// entry falls back to the original core).
std::vector<uint8_t> assemble_dialog(const DialogPlan& plan, const std::vector<std::string>& cores);

// A dialog that must never be translated or held: a PASSWORD dialog (style 3) or
// the server's CLOSE signal (dialog id < 0 as int16, i.e. 0x8000..0xFFFF).
bool never_translated(const DialogPlan& d);
```

Also replace the comment above `rebuild_dialog` (the three lines starting `// Rebuild a ShowDialog (RPC 61) packet with translated title/buttons/body.`) with:

```cpp
// Rebuild a ShowDialog (RPC 61) packet translating it UNIT BY UNIT (plan_dialog):
// `tr` receives each translatable unit's bare core. Returns true and fills `out`
// only if EVERY translatable unit resolved via `tr` (so the caller shows the
// original until all units are cached). Returns false otherwise.
```

- [ ] **Step 4: Implement**

In `src/i18n/surfaces_rebuild.cpp`, add inside the anonymous namespace, directly after `put_u32`:

```cpp

// "{RRGGBB}" at s[i].
bool is_color_code(const std::string& s, size_t i) {
    if (i + 8 > s.size() || s[i] != '{' || s[i + 7] != '}') return false;
    for (size_t k = i + 1; k < i + 7; ++k) {
        char c = s[k];
        bool hx = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hx) return false;
    }
    return true;
}

// Letters vs digits, skipping ~x~ and {RRGGBB} codes. Bytes >= 0x80 are letters of
// a non-Latin codepage (cp1251 Cyrillic etc.): counting only ASCII made every
// Cyrillic label look "numeric" and it was never translated.
void count_letters_digits(const std::string& t, int& letters, int& digits) {
    letters = digits = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = (unsigned char)t[i];
        if (c == '~') { size_t j = i + 1; while (j < t.size() && t[j] != '~') ++j; i = j; continue; }
        if (c == '{') { size_t j = i + 1; while (j < t.size() && t[j] != '}') ++j; i = j; continue; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) ++letters;
        else if (c >= '0' && c <= '9') ++digits;
    }
}

// Dialog units: nothing worth translating ("12m", "1.15km", "$500", "7").
bool mostly_numeric(const std::string& t) {
    int letters, digits;
    count_letters_digits(t, letters, digits);
    return letters < 2 || digits >= letters;
}
```

Replace the whole existing `is_dynamic_text` function with:

```cpp
bool is_dynamic_text(const std::string& t) {
    int letters, digits;
    count_letters_digits(t, letters, digits);
    return letters < 3 || digits >= letters;
}

bool is_verbatim_cell(const std::string& core) {
    if (core.empty() || mostly_numeric(core)) return true;
    if (core.find(' ') != std::string::npos) return false;      // a phrase -> translate
    bool anyUpper = false;
    for (unsigned char c : core) {
        if (c >= 0x80) return false;                             // non-Latin word -> translate
        if (c >= 'A' && c <= 'Z') anyUpper = true;
        if ((c >= '0' && c <= '9') || c == '/' || c == '_') return true;
    }
    return !anyUpper;                                            // all-lowercase token = shortcode
}

Unit make_unit(const std::string& raw, bool cell) {
    Unit u;
    size_t a = 0, b = raw.size();
    while (a < b) {                                  // leading spaces + colour codes
        if (raw[a] == ' ') { ++a; continue; }
        if (is_color_code(raw, a)) { a += 8; continue; }
        break;
    }
    while (b > a && raw[b - 1] == ' ') --b;          // trailing spaces
    u.prefix = raw.substr(0, a);
    u.core   = raw.substr(a, b - a);
    u.suffix = raw.substr(b);
    u.translate = cell ? !is_verbatim_cell(u.core) : !(u.core.empty() || mostly_numeric(u.core));
    return u;
}

bool plan_dialog(const uint8_t* p, size_t n, DialogPlan& d) {
    d = DialogPlan{};
    if (!p || n < 6) return false;
    d.id = rd16(p);
    size_t off = 2;
    d.style = p[off++];
    std::string title, b1, b2;
    if (!read_u8(p, n, off, title) || !read_u8(p, n, off, b1) || !read_u8(p, n, off, b2)) return false;
    d.units.push_back(make_unit(title, false));
    d.units.push_back(make_unit(b1, false));
    d.units.push_back(make_unit(b2, false));
    if (off >= n) return true;                                    // no body bytes at all
    std::string body;
    d.haveBody = huffman::decode_string(p + off, n - off, body);
    if (!d.haveBody) { d.rawBody.assign(p + off, p + n); return true; }
    const bool list = (d.style == 2 || d.style == 4 || d.style == 5);
    std::string cur;
    for (char c : body) {
        if (c == '\n' || c == '\t') {
            d.units.push_back(make_unit(cur, list));
            d.bodySeps.push_back(c);
            cur.clear();
        } else {
            cur += c;
        }
    }
    d.units.push_back(make_unit(cur, list));
    return true;
}

std::vector<std::string> original_cores(const DialogPlan& d) {
    std::vector<std::string> c;
    c.reserve(d.units.size());
    for (const Unit& u : d.units) c.push_back(u.core);
    return c;
}

std::string clean_translation(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\r') continue;
        o += (c == '\t' || c == '\n') ? ' ' : c;
    }
    return o;
}

std::string unit_text(const Unit& u, const std::string& core) {
    if (!u.translate || core == u.core) return u.prefix + u.core + u.suffix;
    return u.prefix + clean_translation(core) + u.suffix;
}

std::string assemble_body(const DialogPlan& d, const std::vector<std::string>& cores) {
    std::string body;
    for (size_t k = 3; k < d.units.size(); ++k) {
        body += unit_text(d.units[k], k < cores.size() ? cores[k] : d.units[k].core);
        if (k - 3 < d.bodySeps.size()) body += d.bodySeps[k - 3];
    }
    return body;
}

std::vector<uint8_t> assemble_dialog(const DialogPlan& d, const std::vector<std::string>& cores) {
    auto core = [&](size_t k) -> const std::string& { return k < cores.size() ? cores[k] : d.units[k].core; };
    std::vector<uint8_t> out;
    out.push_back((uint8_t)(d.id & 0xFF));
    out.push_back((uint8_t)(d.id >> 8));
    out.push_back(d.style);
    put_u8(out, unit_text(d.units[0], core(0)));
    put_u8(out, unit_text(d.units[1], core(1)));
    put_u8(out, unit_text(d.units[2], core(2)));
    if (d.haveBody) {
        std::vector<uint8_t> enc = huffman::encode_string(assemble_body(d, cores));
        out.insert(out.end(), enc.begin(), enc.end());
    } else {
        out.insert(out.end(), d.rawBody.begin(), d.rawBody.end());
    }
    return out;
}

bool never_translated(const DialogPlan& d) {
    return d.style == 3 || (int16_t)d.id < 0;
}
```

Replace the whole existing `rebuild_dialog` function with:

```cpp
bool rebuild_dialog(const uint8_t* p, size_t n, const Translator& tr, std::vector<uint8_t>& out) {
    DialogPlan d;
    if (!plan_dialog(p, n, d)) return false;
    std::vector<std::string> cores = original_cores(d);
    // Every translatable unit must resolve, or the caller passes the original through.
    for (size_t k = 0; k < d.units.size(); ++k)
        if (d.units[k].translate && !tr(d.units[k].core, cores[k])) return false;
    out = assemble_dialog(d, cores);
    return true;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_surfaces_test.cmd 2>&1" | tail -n 3`
Expected: `64 checks, 0 failures`.

- [ ] **Step 6: Build + regression**

Run the Build command, then `run_tests`. Expected: clean build; `0 failures`.

- [ ] **Step 7: Commit**

```bash
git add src/i18n/surfaces.h src/i18n/surfaces_rebuild.cpp tests/surfaces_test.cpp
git commit -m "feat(i18n): translate dialogs unit by unit so list/tablist structure can't break

Split the dialog body on every \\n and \\t into units, re-attach each unit's
leading {RRGGBB} and the separators byte-for-byte, and keep shortcode/number
cells (intro, /help, 12m, 1.15km) verbatim. Stray tabs/newlines in a
translation become spaces. Byte-exact against four real captures (help
topics, bank list, GPS, spawn zones). Also count non-ASCII bytes as letters
so Cyrillic labels are no longer mistaken for dynamic numbers.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 2: Live player-name protection

**Files:**
- Modify: `src/i18n/protect.h`, `src/i18n/protect.cpp`
- Create: `src/i18n/names.h`, `src/i18n/names.cpp`
- Modify: `src/i18n/hooks.cpp`, `FuegoReversed.vcxproj`
- Test: `tests/i18n_tests.cpp`

**Interfaces:**
- Consumes: `samp::player_name(int id) -> std::string` ("" for an empty slot; handles the local player), `samp::SAMP_MAX_PLAYERS` (1004) — both from `src/samp/pools.h`.
- Produces: `i18n::protect::set_names(std::vector<std::string>)`, `i18n::protect::name_variants(const std::vector<std::string>&) -> std::vector<std::string>`, `i18n::names::tick(uint32_t nowMs)`.

- [ ] **Step 1: Write the failing test**

In `tests/i18n_tests.cpp`, add `#include <algorithm>` near the other includes if absent, add `void test_names();` to the forward declarations, call `test_names();` in `main()` right after `test_protect();` (on the same line: `test_protect(); test_names();`), and append this function at the end of the file:

```cpp
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_tests.cmd 2>&1" | tail -n 5`
Expected: compile error — `name_variants` / `set_names` undeclared.

- [ ] **Step 3: Declare**

In `src/i18n/protect.h`, insert before the closing `} // namespace i18n::protect`:

```cpp

// ---- Live player-name protection ---------------------------------------------
// set_names() publishes the protected terms (see name_variants()); mask() then
// masks every whole-word, case-insensitive occurrence, longest first, so the model
// never sees a player's name and can't translate or transliterate it
// ("Khalid" -> "Халид"). The ORIGINAL span is the token, so unmask() restores its
// exact casing ("khalid" stays "khalid"). Thread-safe: workers read an immutable
// snapshot while the main thread swaps in a new one.
void set_names(std::vector<std::string> terms);

// Protected terms for connected players' names: each exact name (>= 3 chars), its
// RP display form with '_' -> ' ' ("Khalid Ahmed"), and each underscore-separated
// part of >= 3 letters that isn't a common word ("Khalid", "Ahmed" -- but not
// "Black" from "John_Black" or "The" from "The_Man"). Deduped case-insensitively,
// sorted longest first.
std::vector<std::string> name_variants(const std::vector<std::string>& names);
```

- [ ] **Step 4: Implement masking**

In `src/i18n/protect.cpp`, add these includes next to the existing ones: `#include <algorithm>`, `#include <memory>`, `#include <mutex>`, `#include <unordered_set>`. Inside the file's anonymous namespace (after `is_word_char`), add:

```cpp

std::mutex g_namesMtx;
std::shared_ptr<const std::vector<std::string>> g_names =
    std::make_shared<const std::vector<std::string>>();

std::shared_ptr<const std::vector<std::string>> names_snapshot() {
    std::lock_guard<std::mutex> lk(g_namesMtx);
    return g_names;
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (is_upper(c)) c = char(c - 'A' + 'a');
    return s;
}
```

In `mask()`, directly after the line `size_t i = 0;`, add:

```cpp
    const auto names = names_snapshot();
```

In `mask()`, insert this block immediately BEFORE the line `        // {RRGGBB} / {AARRGGBB} colour embed.`:

```cpp
        // Live player names (set_names): whole word, case-insensitive, longest first.
        if (!matched && !names->empty() && (i == 0 || !is_word_char(src[i - 1]))) {
            for (const std::string& nm : *names) {
                size_t len = match_ci_literal(src, i, nm.c_str());
                if (len == 0) continue;
                if (i + len < n && is_word_char(src[i + len])) continue; // "Khalidx" isn't "Khalid"
                emit(i, i + len);
                i += len;
                matched = true;
                break;
            }
        }

```

Append at the end of the file, before the closing `} // namespace i18n::protect`:

```cpp

void set_names(std::vector<std::string> terms) {
    std::stable_sort(terms.begin(), terms.end(),
                     [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
    auto snap = std::make_shared<const std::vector<std::string>>(std::move(terms));
    std::lock_guard<std::mutex> lk(g_namesMtx);
    g_names = std::move(snap);
}

std::vector<std::string> name_variants(const std::vector<std::string>& names) {
    // Common words that also show up as parts of RP names ("John_Black", "The_Man"):
    // protecting them alone would freeze ordinary chat words, so such a part is only
    // protected inside its full name.
    static const std::unordered_set<std::string> kCommon = {
        "the", "and", "you", "are", "for", "not", "but", "all", "any", "can", "had", "her", "was",
        "one", "our", "out", "day", "get", "has", "him", "his", "how", "man", "men", "new", "now",
        "old", "see", "two", "way", "who", "boy", "did", "its", "let", "put", "say", "she", "too",
        "use", "big", "bad", "car", "cop", "gun", "god", "sir", "king", "lord", "lady", "miss",
        "mister", "black", "white", "brown", "green", "gray", "grey", "red", "blue", "gold",
        "silver", "young", "long", "short", "little", "hunter", "baker", "cook", "smith", "will",
        "may", "june", "mark", "hope", "rose", "grace", "faith", "joy", "sky", "star", "stone",
        "wood", "woods", "hill", "hills", "lake", "rich", "love", "dead", "death", "ghost", "snake",
        "wolf", "fox", "bear", "lion", "hawk", "eagle", "bird", "fish", "ice", "fire", "storm",
        "rain", "snow", "frost", "night", "dark", "light", "shadow", "angel", "devil", "bishop",
        "knight", "cross", "church", "street", "park", "west", "east", "north", "south", "money",
        "cash", "boss", "chief", "doctor", "officer", "police"
    };
    std::vector<std::string> out;
    std::unordered_set<std::string> seen; // lower-cased, for case-insensitive dedupe
    auto add = [&](const std::string& s) {
        if (s.size() < 3) return;
        if (seen.insert(lower_ascii(s)).second) out.push_back(s);
    };
    auto all_letters = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s)
            if (!is_alpha(c)) return false;
        return true;
    };
    for (const std::string& nm : names) {
        add(nm);
        if (nm.find('_') == std::string::npos) continue;
        std::string spaced = nm;
        std::replace(spaced.begin(), spaced.end(), '_', ' ');
        add(spaced);
        size_t a = 0;
        while (a <= nm.size()) {
            size_t b = nm.find('_', a);
            if (b == std::string::npos) b = nm.size();
            std::string part = nm.substr(a, b - a);
            if (all_letters(part) && !kCommon.count(lower_ascii(part))) add(part);
            a = b + 1;
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
    return out;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_tests.cmd 2>&1" | tail -n 3`
Expected: `<N> checks, 0 failures` (N = previous count + 11).

- [ ] **Step 6: Create the live refresher**

Create `src/i18n/names.h`:

```cpp
#pragma once
#include <cstdint>

namespace i18n::names {

// Main thread, every frame (cheap: rescans every 2s). Collects every connected
// player's name from SA:MP's player pool (by id, so they are real names -- incl.
// the local player), keeps names seen in the last 10 minutes (so "X left the
// server" still protects X), and republishes protect::set_names(name_variants())
// when the set changes. Names are then masked before any text reaches the model.
void tick(uint32_t nowMs);

} // namespace i18n::names
```

Create `src/i18n/names.cpp`:

```cpp
#include "names.h"

#include "protect.h"
#include "../samp/pools.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace i18n::names {

namespace {
constexpr uint32_t kScanMs   = 2000;
constexpr uint32_t kForgetMs = 10 * 60 * 1000;
std::unordered_map<std::string, uint32_t> g_seen; // name -> last seen tick
uint32_t g_lastScan = 0;
bool     g_scanned  = false;
} // namespace

void tick(uint32_t nowMs) {
    if (g_scanned && nowMs - g_lastScan < kScanMs) return;
    g_scanned  = true;
    g_lastScan = nowMs;

    bool changed = false;
    for (int id = 0; id < samp::SAMP_MAX_PLAYERS; ++id) {
        std::string nm = samp::player_name(id);
        if (nm.empty()) continue;
        auto it = g_seen.find(nm);
        if (it == g_seen.end()) { g_seen.emplace(std::move(nm), nowMs); changed = true; }
        else it->second = nowMs;
    }
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        if (nowMs - it->second > kForgetMs) { it = g_seen.erase(it); changed = true; }
        else ++it;
    }
    if (g_seen.size() > 3000) { g_seen.clear(); changed = true; }
    if (!changed) return;

    std::vector<std::string> names;
    names.reserve(g_seen.size());
    for (const auto& kv : g_seen) names.push_back(kv.first);
    i18n::protect::set_names(i18n::protect::name_variants(names));
}

} // namespace i18n::names
```

- [ ] **Step 7: Wire it**

In `src/i18n/hooks.cpp`: add `#include "names.h"` after `#include "lab.h"`, and replace the `pump_tick` function with:

```cpp
void pump_tick( IDirect3DDevice9 * ) {
	i18n::engine::pump();
	i18n::names::tick( GetTickCount() ); // live player names -> masked, never translated
	i18n::lab::on_frame();               // debug lab (read-only logging)
}
```

In `FuegoReversed.vcxproj`, after the `src\i18n\lab.cpp` entry, add:

```xml
    <ClCompile Include="src\i18n\names.cpp">
      <ObjectFileName>$(IntDir)i18n_names.obj</ObjectFileName>
    </ClCompile>
```

- [ ] **Step 8: Build + regression**

Run the Build command, then `run_tests` and `run_surfaces_test`. Expected: clean build; `0 failures`.

- [ ] **Step 9: Commit**

```bash
git add src/i18n/protect.h src/i18n/protect.cpp src/i18n/names.h src/i18n/names.cpp src/i18n/hooks.cpp FuegoReversed.vcxproj tests/i18n_tests.cpp
git commit -m "feat(i18n): never translate player names -- mask the live player list

Every connected player's name (from SA:MP's player pool, by id) is masked
before text reaches the model: the exact name, the RP 'First Last' form and
each First/Last part that isn't a common word. Whole-word, case-insensitive,
longest first; unmask restores the original casing, so 'khalid' stays
'khalid' instead of being transliterated. Names seen in the last 10 minutes
stay protected.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 3: Chat buffer helpers (pure)

**Files:**
- Create: `src/i18n/chatbuf.h`, `src/i18n/chatbuf.cpp`
- Create: `tests/chatbuf_test.cpp`, `tests/run_chatbuf_test.cmd`

**Interfaces:**
- Consumes: nothing (raw memory).
- Produces (namespace `i18n::chatbuf`): constants `kEntry0 0x132`, `kStride 0xFC`, `kTime 0x00`, `kText 0x20`, `kTextMax 144`, `kEntries 100`, `kDirty 0x63DA`, `kMinBytes`; `struct Piece { uint32_t time; std::string text; }`; `snapshot_newest(const uint8_t* chat) -> std::vector<uint8_t>`; `added_since(const uint8_t* chat, const std::vector<uint8_t>& snap) -> int`; `read_newest(const uint8_t* chat, int n) -> std::vector<Piece>` (oldest piece first); `find_block(const uint8_t* chat, const std::vector<Piece>&) -> int` (first index or -1); `wrap_to(const std::string&, int n, size_t width) -> std::vector<std::string>` (exactly n rows); `write_block(uint8_t* chat, int first, const std::vector<std::string>& rows)` (also sets the redraw flag).

- [ ] **Step 1: Write the failing test**

Create `tests/chatbuf_test.cpp`:

```cpp
// Host test for i18n::chatbuf -- the in-place chat entry rewrite -- against a fake
// CChat buffer laid out like SA:MP 0.3.DL's (offsets confirmed by disassembly).
//
// Build/run: tests\run_chatbuf_test.cmd

#include "i18n/chatbuf.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace i18n::chatbuf;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

static uint8_t* entry(std::vector<uint8_t>& b, int i) { return b.data() + kEntry0 + (size_t)i * kStride; }

// Exactly what CChat::AddEntry does per appended entry: shift [1..99] -> [0..98],
// then write the new entry into [99].
static void sim_add(std::vector<uint8_t>& b, uint32_t t, const char* text) {
    std::memmove(entry(b, 0), entry(b, 1), (size_t)(kEntries - 1) * kStride);
    std::memset(entry(b, 99), 0, kStride);
    std::memcpy(entry(b, 99) + kTime, &t, 4);
    std::strncpy((char*)entry(b, 99) + kText, text, kTextMax - 1);
}
static std::string text_at(std::vector<uint8_t>& b, int i) { return std::string((const char*)entry(b, i) + kText); }
static int dirty(std::vector<uint8_t>& b) { int d; std::memcpy(&d, b.data() + kDirty, 4); return d; }

int main() {
    std::vector<uint8_t> b(kMinBytes, 0);

    // One line into an empty chat.
    std::vector<uint8_t> snap = snapshot_newest(b.data());
    sim_add(b, 1000, "Privet brat");
    check(added_since(b.data(), snap) == 1, "one appended entry detected on an empty buffer");

    // A long line that SA:MP wrapped into two entries.
    snap = snapshot_newest(b.data());
    sim_add(b, 2000, "part one of a long line");
    sim_add(b, 2000, "and its second half");
    check(added_since(b.data(), snap) == 2, "two appended entries detected");
    std::vector<Piece> pieces = read_newest(b.data(), 2);
    check(pieces.size() == 2 && pieces[0].text == "part one of a long line" &&
          pieces[1].text == "and its second half" && pieces[0].time == 2000,
          "read_newest: oldest piece first, with times");

    // Newer lines push the block up; it is still found by content + time.
    sim_add(b, 3000, "x");
    sim_add(b, 3001, "y");
    int first = find_block(b.data(), pieces);
    check(first == 96, "block found after shifting two slots");

    // Same text, different time: not confused with our block.
    std::vector<Piece> other = { { 9999, "part one of a long line" }, { 9999, "and its second half" } };
    check(find_block(b.data(), other) == -1, "same text with a different time is not a match");

    // Rewrite in place: our two rows change, neighbours don't, redraw flag set.
    write_block(b.data(), first, wrap_to("una parte larga y su segunda mitad", 2, 20));
    check(text_at(b, 96) == "una parte larga y su" && text_at(b, 97) == "segunda mitad",
          "translation wrapped across the same two rows");
    check(text_at(b, 95) == "Privet brat" && text_at(b, 98) == "x" && text_at(b, 99) == "y",
          "neighbouring lines untouched");
    check(dirty(b) == 1, "redraw flag (+0x63DA) set");

    // Scrolled off: after 100 more lines the block is gone.
    for (int k = 0; k < 100; ++k) sim_add(b, 5000 + k, "spam");
    check(find_block(b.data(), pieces) == -1, "block that scrolled off is not found");

    // wrap_to edge cases.
    std::vector<std::string> one = wrap_to("short", 1, 72);
    check(one.size() == 1 && one[0] == "short", "short text -> one row");
    std::vector<std::string> pad = wrap_to("short", 3, 72);
    check(pad.size() == 3 && pad[0] == "short" && pad[1].empty() && pad[2].empty(), "spare rows become empty");
    std::vector<std::string> cut = wrap_to(std::string(300, 'a'), 1, 72);
    check(cut.size() == 1 && cut[0].size() == kTextMax - 1 && cut[0].substr(cut[0].size() - 3) == "...",
          "overflow truncated to 143 bytes ending in '...'");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
```

Create `tests/run_chatbuf_test.cmd`:

```bat
@echo off
rem Host test for i18n::chatbuf (in-place chat entry rewrite) vs a fake CChat buffer.
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0chatbuf_test.cpp" "%~dp0..\src\i18n\chatbuf.cpp" /Fe:"%~dp0chatbuf_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0chatbuf_test.exe"
exit /b %errorlevel%
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_chatbuf_test.cmd 2>&1" | tail -n 5`
Expected: compile error — `i18n/chatbuf.h` not found.

- [ ] **Step 3: Implement**

Create `src/i18n/chatbuf.h`:

```cpp
#pragma once
// In-place edits of SA:MP's chat entry buffer (CChat, 0.3.DL). PURE: every
// function takes the raw CChat pointer (the AddEntry `this`), so it is
// host-tested against a fake buffer laid out identically (tests/chatbuf_test.cpp).
// Confirmed by disassembly of the user's samp.dll + an in-game test (2026-09-27):
// newest entry = [99]; AddEntry shifts older entries toward [0] and writes the new
// one at [99]; a long line becomes several entries (wrapped at insert time, never
// at render); setting the redraw flag makes CChat rebuild its cached texture from
// the entries' szText on the next frame.

#include <cstdint>
#include <string>
#include <vector>

namespace i18n::chatbuf {

constexpr uint32_t kEntry0   = 0x132;  // chatEntry[0] within CChat
constexpr uint32_t kStride   = 0xFC;   // sizeof(stChatEntry) = 252
constexpr uint32_t kTime     = 0x00;   // entry.SystemTime (u32)
constexpr uint32_t kText     = 0x20;   // entry.szText[144]
constexpr uint32_t kTextMax  = 144;
constexpr int      kEntries  = 100;
constexpr uint32_t kDirty    = 0x63DA; // the redraw flag AddEntry itself sets (NOT the header's m_iRedraw)
constexpr uint32_t kMinBytes = kDirty + 4;

struct Piece {
    uint32_t    time;
    std::string text;
};

// Copy of entry [99] -- take it BEFORE calling AddEntry.
std::vector<uint8_t> snapshot_newest(const uint8_t* chat);

// After AddEntry: how many entries it appended (it shifted older entries down one
// per appended entry, so the pre-call newest entry now sits at 99 - n). 0 if the
// snapshot can't be found.
int added_since(const uint8_t* chat, const std::vector<uint8_t>& snap);

// The newest n entries as one message's pieces, oldest piece first.
std::vector<Piece> read_newest(const uint8_t* chat, int n);

// Index of the first entry of the consecutive block matching `pieces` exactly
// (time + text), newest match first; -1 if gone (scrolled off / cleared).
int find_block(const uint8_t* chat, const std::vector<Piece>& pieces);

// Exactly n rows for n entries: greedy word wrap at `width` for all but the last
// row, the last row takes the rest; rows are capped at 143 bytes (overflow ends in
// "..."); spare rows are "".
std::vector<std::string> wrap_to(const std::string& text, int n, size_t width);

// Overwrite szText of entries [first, first + rows.size()) and set the redraw flag.
void write_block(uint8_t* chat, int first, const std::vector<std::string>& rows);

} // namespace i18n::chatbuf
```

Create `src/i18n/chatbuf.cpp`:

```cpp
#include "chatbuf.h"

#include <cstring>

namespace i18n::chatbuf {

namespace {

const uint8_t* entry(const uint8_t* chat, int i) { return chat + kEntry0 + (uint32_t)i * kStride; }
uint8_t* entry(uint8_t* chat, int i) { return chat + kEntry0 + (uint32_t)i * kStride; }

uint32_t time_of(const uint8_t* e) {
    uint32_t t;
    std::memcpy(&t, e + kTime, 4);
    return t;
}

std::string text_of(const uint8_t* e) {
    const char* t = (const char*)(e + kText);
    size_t len = 0;
    while (len < kTextMax && t[len]) ++len;
    return std::string(t, len);
}

} // namespace

std::vector<uint8_t> snapshot_newest(const uint8_t* chat) {
    const uint8_t* e = entry(chat, kEntries - 1);
    return std::vector<uint8_t>(e, e + kStride);
}

int added_since(const uint8_t* chat, const std::vector<uint8_t>& snap) {
    if (snap.size() != kStride) return 0;
    for (int i = kEntries - 2; i >= 0; --i)
        if (std::memcmp(entry(chat, i), snap.data(), kStride) == 0) return (kEntries - 1) - i;
    return 0;
}

std::vector<Piece> read_newest(const uint8_t* chat, int n) {
    std::vector<Piece> out;
    if (n <= 0 || n > kEntries) return out;
    for (int i = kEntries - n; i < kEntries; ++i) {
        const uint8_t* e = entry(chat, i);
        out.push_back({ time_of(e), text_of(e) });
    }
    return out;
}

int find_block(const uint8_t* chat, const std::vector<Piece>& pieces) {
    const int n = (int)pieces.size();
    if (n <= 0 || n > kEntries) return -1;
    for (int first = kEntries - n; first >= 0; --first) {
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            const uint8_t* e = entry(chat, first + k);
            ok = time_of(e) == pieces[k].time && text_of(e) == pieces[k].text;
        }
        if (ok) return first;
    }
    return -1;
}

std::vector<std::string> wrap_to(const std::string& text, int n, size_t width) {
    std::vector<std::string> out;
    if (n <= 0) return out;
    const size_t hard = kTextMax - 1;                          // 143 + NUL
    if (width == 0 || width > hard) width = hard;
    size_t pos = 0;
    for (int k = 0; k < n; ++k) {
        while (pos < text.size() && text[pos] == ' ') ++pos;   // no leading space on a row
        const size_t remaining = text.size() - pos;
        if (k == n - 1) {                                       // the last row takes the rest
            if (remaining <= hard) out.push_back(text.substr(pos));
            else out.push_back(text.substr(pos, hard - 3) + "...");
            pos = text.size();
            continue;
        }
        if (remaining <= width) { out.push_back(text.substr(pos)); pos = text.size(); continue; }
        size_t cut = text.rfind(' ', pos + width);             // last space within the width
        if (cut == std::string::npos || cut <= pos) cut = pos + width; // one long word: hard split
        out.push_back(text.substr(pos, cut - pos));
        pos = cut;
    }
    return out;
}

void write_block(uint8_t* chat, int first, const std::vector<std::string>& rows) {
    for (size_t k = 0; k < rows.size(); ++k) {
        const int i = first + (int)k;
        if (i < 0 || i >= kEntries) break;
        char* t = (char*)(entry(chat, i) + kText);
        const size_t len = rows[k].size() > kTextMax - 1 ? kTextMax - 1 : rows[k].size();
        std::memcpy(t, rows[k].data(), len);
        t[len] = '\0';
    }
    const int one = 1;
    std::memcpy(chat + kDirty, &one, 4);
}

} // namespace i18n::chatbuf
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_chatbuf_test.cmd 2>&1" | tail -n 3`
Expected: `12 checks, 0 failures`.

- [ ] **Step 5: Commit**

```bash
git add src/i18n/chatbuf.h src/i18n/chatbuf.cpp tests/chatbuf_test.cpp tests/run_chatbuf_test.cmd
git commit -m "feat(i18n): pure helpers to rewrite SA:MP chat entries in place

Snapshot/diff the CChat entry ring to learn which rows an AddEntry produced,
find those rows again later by SystemTime + text (they shift as new lines
arrive), wrap a translation into exactly that many rows, and write it back
with the redraw flag AddEntry itself uses (+0x63DA). Host-tested against a
fake buffer laid out like 0.3.DL's.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 4: Inbound chat — instant cache hits + show-then-flip in place

**Files:**
- Modify: `src/i18n/hooks.cpp`, `FuegoReversed.vcxproj`

**Interfaces:**
- Consumes: `i18n::chatbuf::*` (Task 3), `i18n::engine::cache_lookup(const std::string&, int cp, int targetLang, std::string& out)`, `samp::chat_ptr()`.
- Produces: new inbound-chat behaviour; `apply_result` routes surface 0 to the in-place flip. Later tasks add surface 3/4 lines to `apply_result`.

- [ ] **Step 1: Add the project entry**

In `FuegoReversed.vcxproj`, after the `src\i18n\names.cpp` entry, add:

```xml
    <ClCompile Include="src\i18n\chatbuf.cpp">
      <ObjectFileName>$(IntDir)i18n_chatbuf.obj</ObjectFileName>
    </ClCompile>
```

- [ ] **Step 2: Replace the pending-line state**

In `src/i18n/hooks.cpp`: add `#include "chatbuf.h"` after `#include "names.h"`; add `#include <vector>` next to `<unordered_map>`. Replace the whole `PendingLine` comment + struct + `g_pendingLines` definition (from the comment starting `// The original AddEntry call's formatting, kept while its translation is in` through `std::unordered_map<uint64_t, PendingLine> g_pendingLines;`) with:

```cpp
// A line shown in its ORIGINAL form while its translation is in flight: the exact
// chat entries it became (AddEntry may have wrapped it into several), identified by
// SystemTime + text, so the translation can be written back over THOSE rows in
// place when it lands -- lines above/below never move (Option A). Main thread only
// (SA:MP calls AddEntry and the pump both run on the game thread).
struct PendingFlip {
	std::vector<i18n::chatbuf::Piece> pieces;
};
std::unordered_map<uint64_t, PendingFlip> g_pendingFlips;
```

- [ ] **Step 3: Replace `apply_result`**

Replace the whole `apply_result` function (its leading comment block starting `// Called from i18n::engine::pump() on the main thread once a translation` through the function's closing `}`) with:

```cpp
// Called from i18n::engine::pump() on the main thread once a translation lands.
// Routes each result by surface. Nothing here may call the patched AddEntry.
void apply_result( const i18n::engine::Result &r ) {
	// Outbound chat (surface 1) is SENT to the server, not shown.
	if ( r.surface == 1 ) { i18n::outbound::on_result( r ); return; }
	// Surface 2: translation-memory fill for labels/HUD -- already cached by
	// process_one(); nothing to show.
	if ( r.surface != 0 ) return;

	// Inbound chat (surface 0): overwrite the rows the original line became with the
	// translation, in place. Declined/failed -> the original simply stays. Guarded:
	// this runs inside SA:MP/D3D's C call stack, where an escaping exception is UB.
	try {
		auto it = g_pendingFlips.find( r.handle );
		if ( it == g_pendingFlips.end() ) return;
		PendingFlip pf = std::move( it->second );
		g_pendingFlips.erase( it );
		if ( !r.ok || r.displayBytes.empty() ) return;

		uint8_t *chat = static_cast<uint8_t *>( samp::chat_ptr() );
		if ( !chat ) return; // left the server before the result landed
		int first = i18n::chatbuf::find_block( chat, pf.pieces );
		if ( first < 0 ) return; // scrolled off (or cleared) before the translation came back

		size_t width = 72; // SA:MP's own wrap width -- wider if the original rows were
		for ( const auto &p : pf.pieces )
			if ( p.text.size() > width ) width = p.text.size(); // (no std::max: windows.h defines max)
		i18n::chatbuf::write_block( chat, first,
		                            i18n::chatbuf::wrap_to( r.displayBytes, (int) pf.pieces.size(), width ) );
	} catch ( ... ) {
	}
}
```

- [ ] **Step 4: Replace `hooked_add_entry`**

Replace the whole `hooked_add_entry` function (keep its leading comment block about `__fastcall` and re-entrancy) with:

```cpp
void __fastcall hooked_add_entry( void *self, void * /*edx*/, int type, const char *msg,
                                   const char *prefix, uint32_t textColor, uint32_t prefixColor ) {
	bool doTranslate = false;
	try {
		if ( i18n::config::get().inbound_chat && msg && msg[0] &&
		     ( type == samp::CHAT_TYPE_CHAT || type == samp::CHAT_TYPE_INFO ) )
			doTranslate = true;
	} catch ( ... ) {}

	if ( !doTranslate ) {
		if ( g_origAddEntry ) g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
		return;
	}

	// Option A: the line ALWAYS shows immediately (live chat, exact order + timing).
	// `shown` guards the catch below so a line is never lost and never shown twice.
	bool shown = false;
	try {
		auto &s = i18n::config::get();

		// Translation-memory hit: show it ALREADY translated -- SA:MP wraps it with its
		// own rules, and there is nothing to flip later.
		std::string cached;
		if ( i18n::engine::cache_lookup( msg, s.source_cp, s.target_lang, cached ) && !cached.empty() ) {
			g_origAddEntry( self, type, cached.c_str(), prefix, textColor, prefixColor );
			shown = true;
			return;
		}

		// Miss: show the original now and remember exactly which rows it became.
		const uint8_t *chat = static_cast<const uint8_t *>( self );
		std::vector<uint8_t> snap = i18n::chatbuf::snapshot_newest( chat );
		g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
		shown = true;

		int n = i18n::chatbuf::added_since( chat, snap );
		if ( n <= 0 || n > 4 ) return; // can't tell which rows it became -> leave it as is

		if ( g_pendingFlips.size() > 256 ) g_pendingFlips.clear(); // results that never came back
		uint64_t h = g_seq.fetch_add( 1, std::memory_order_relaxed );
		g_pendingFlips[h].pieces = i18n::chatbuf::read_newest( chat, n );

		i18n::engine::Job job;
		job.handle      = h;
		job.surface     = 0; // inbound chat -> apply_result flips the rows in place
		job.sourceBytes = msg;
		job.srcCodepage = s.source_cp; // 0 = auto-detect
		job.noDedup     = true;        // every shown line gets its own flip
		i18n::engine::submit( job );
	} catch ( ... ) {
		if ( !shown && g_origAddEntry ) g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
	}
}
```

- [ ] **Step 5: Build + regression**

Run the Build command, then `run_tests`, `run_surfaces_test`, `run_chatbuf_test`. Expected: clean build; `0 failures` on all.

- [ ] **Step 6: Commit**

```bash
git add src/i18n/hooks.cpp FuegoReversed.vcxproj
git commit -m "feat(i18n): show chat instantly and flip it to the translation in place

A translation-memory hit is shown already translated (SA:MP wraps it). A miss
is shown immediately in the original -- exact order and timing -- and the rows
it became are rewritten with the translation when it lands, without moving any
other line. Replaces the swallow-and-reshow design that delayed lines and could
reorder them.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 5: Dialog replay queue (pure)

**Files:**
- Create: `src/i18n/dlgq.h`, `src/i18n/dlgq.cpp`
- Create: `tests/dlgq_test.cpp`, `tests/run_dlgq_test.cmd`

**Interfaces:**
- Consumes: nothing (injected clock).
- Produces (namespace `i18n::dlgq`): `struct Release { uint64_t seq; std::vector<uint8_t> payload, original; bool translated; }`; `class Queue { Queue(uint32_t maxHoldMs = 10000, size_t maxItems = 16); bool empty() const; size_t size() const; bool push(uint64_t seq, uint32_t nowMs, std::vector<uint8_t> original); void resolve(uint64_t seq, std::vector<uint8_t> translated); std::vector<Release> tick(uint32_t nowMs); std::vector<Release> flush(); }`. `resolve` with an empty vector means "show the original".

- [ ] **Step 1: Write the failing test**

Create `tests/dlgq_test.cpp`:

```cpp
// Host test for i18n::dlgq -- ordered, timing-faithful release of held dialogs.
//
// Build/run: tests\run_dlgq_test.cmd

#include "i18n/dlgq.h"

#include <cstdio>
#include <vector>

using namespace i18n::dlgq;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}
static std::vector<uint8_t> P(uint8_t tag) { return std::vector<uint8_t>{ tag }; }

int main() {
    {   // 1. Held until translated, then released translated.
        Queue q;
        q.push(1, 0, P('a'));
        check(q.tick(100).empty(), "nothing released while translating");
        q.resolve(1, P('A'));
        auto r = q.tick(150);
        check(r.size() == 1 && r[0].translated && r[0].payload == P('A') && r[0].original == P('a'), "released translated");
        check(q.empty(), "queue drained");
    }
    {   // 2. Faithful gaps: A@0, B@300, C@1000, all translated late (t=2000).
        Queue q;
        q.push(1, 0, P('a')); q.push(2, 300, P('b')); q.push(3, 1000, P('c'));
        q.resolve(1, P('A')); q.resolve(2, P('B')); q.resolve(3, P('C'));
        auto r = q.tick(2000);
        check(r.size() == 1 && r[0].seq == 1, "A released first");
        check(q.tick(2299).empty(), "B waits for its original 300ms gap");
        r = q.tick(2300);
        check(r.size() == 1 && r[0].seq == 2, "B released 300ms after A");
        check(q.tick(2999).empty(), "C waits for its original 700ms gap");
        r = q.tick(3000);
        check(r.size() == 1 && r[0].seq == 3, "C released 700ms after B");
    }
    {   // 3. A later page translated first never overtakes an earlier one.
        Queue q;
        q.push(1, 0, P('a')); q.push(2, 100, P('b'));
        q.resolve(2, P('B'));
        check(q.tick(50).empty(), "B ready but A is not -> nothing");
        q.resolve(1, P('A'));
        auto r = q.tick(500);
        check(r.size() == 1 && r[0].seq == 1, "A first");
        r = q.tick(600);
        check(r.size() == 1 && r[0].seq == 2, "then B, keeping the 100ms gap");
    }
    {   // 4. Translation never arrives -> original at maxHold.
        Queue q(1000);
        q.push(1, 0, P('a'));
        check(q.tick(999).empty(), "held until maxHold");
        auto r = q.tick(1000);
        check(r.size() == 1 && !r[0].translated && r[0].payload == P('a'), "released as the ORIGINAL at maxHold");
    }
    {   // 5. A close keeps its place: it cannot jump ahead of the page before it.
        Queue q;
        q.push(1, 0, P('a'));                        // page, translating
        q.push(2, 200, P('x')); q.resolve(2, {});    // close signal: ready, shown as original
        check(q.tick(500).empty(), "close waits behind the untranslated page");
        q.resolve(1, P('A'));
        auto r = q.tick(1000);
        check(r.size() == 1 && r[0].seq == 1, "page first");
        r = q.tick(1200);
        check(r.size() == 1 && r[0].seq == 2 && !r[0].translated && r[0].payload == P('x'),
              "close 200ms later, as the original");
    }
    {   // 6. A new burst on an empty queue does not inherit the previous gap chain.
        Queue q;
        q.push(1, 0, P('a')); q.resolve(1, P('A'));
        check(q.tick(1000).size() == 1, "A released (1s translation delay)");
        q.push(2, 5000, P('b')); q.resolve(2, P('B'));
        auto r = q.tick(5010);
        check(r.size() == 1 && r[0].seq == 2, "new burst released as soon as ready");
    }
    {   // 7. Full queue refuses; flush releases originals in order.
        Queue q(10000, 2);
        check(q.push(1, 0, P('a')) && q.push(2, 10, P('b')), "two fit");
        check(!q.push(3, 20, P('c')), "third refused when full");
        q.resolve(1, P('A'));
        auto r = q.flush();
        check(r.size() == 2 && r[0].seq == 1 && r[1].seq == 2 && !r[0].translated && r[0].payload == P('a'),
              "flush: originals, in order");
        check(q.empty(), "flushed");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
```

Create `tests/run_dlgq_test.cmd`:

```bat
@echo off
rem Host test for i18n::dlgq (ordered, timing-faithful release of held dialogs).
setlocal enabledelayedexpansion
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /std:c++20 /I"%~dp0..\src" "%~dp0dlgq_test.cpp" "%~dp0..\src\i18n\dlgq.cpp" /Fe:"%~dp0dlgq_test.exe" /Fo:"%~dp0\" || exit /b 1
"%~dp0dlgq_test.exe"
exit /b %errorlevel%
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_dlgq_test.cmd 2>&1" | tail -n 5`
Expected: compile error — `i18n/dlgq.h` not found.

- [ ] **Step 3: Implement**

Create `src/i18n/dlgq.h`:

```cpp
#pragma once
// Ordered, timing-faithful release queue for HELD server dialogs.
//
// PURE (no game types, injected clock) so the ordering/timing rules are
// host-tested (tests/dlgq_test.cpp). surfaces.cpp holds a ShowDialog while it is
// translated and releases it through this queue:
//  * strictly in arrival order -- a fast translation never overtakes a slow one;
//  * each release no sooner than the previous release + the ORIGINAL gap between
//    the two arrivals, so spammed "pages" keep the reading time the server gave;
//  * an item still untranslated after maxHoldMs is released as its original;
//  * a push onto an empty queue starts a fresh burst (no inherited gap).
// Payloads are opaque bytes; the caller decides how to show them.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace i18n::dlgq {

struct Release {
    uint64_t             seq = 0;
    std::vector<uint8_t> payload;    // what to show: the translation, or the original
    std::vector<uint8_t> original;   // always the original payload
    bool                 translated = false;
};

class Queue {
public:
    explicit Queue(uint32_t maxHoldMs = 10000, size_t maxItems = 16)
        : maxHoldMs_(maxHoldMs), maxItems_(maxItems) {}

    bool   empty() const { return q_.empty(); }
    size_t size() const { return q_.size(); }

    // Hold a dialog (its original payload) that arrived at nowMs. False if full.
    bool push(uint64_t seq, uint32_t nowMs, std::vector<uint8_t> original);

    // Translation finished for `seq`: show `translated` (empty = show the original).
    void resolve(uint64_t seq, std::vector<uint8_t> translated);

    // Call every frame: everything due now, in order.
    std::vector<Release> tick(uint32_t nowMs);

    // Release everything immediately as originals, in order (feature switched off).
    std::vector<Release> flush();

private:
    struct Item {
        uint64_t             seq = 0;
        uint32_t             arrivalMs = 0;
        std::vector<uint8_t> original;
        std::vector<uint8_t> translated;
        bool                 ready = false;
    };
    std::deque<Item> q_;
    uint32_t maxHoldMs_;
    size_t   maxItems_;
    bool     chain_ = false;       // a previous release in the current burst exists
    uint32_t lastReleaseMs_ = 0;
    uint32_t lastArrivalMs_ = 0;
};

} // namespace i18n::dlgq
```

Create `src/i18n/dlgq.cpp`:

```cpp
#include "dlgq.h"

#include <utility>

namespace i18n::dlgq {

namespace {
// Wrap-safe "a is at or after b" for GetTickCount()-style clocks.
bool at_or_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }
} // namespace

bool Queue::push(uint64_t seq, uint32_t nowMs, std::vector<uint8_t> original) {
    if (q_.size() >= maxItems_) return false;
    if (q_.empty()) chain_ = false; // a new burst: no gap to honour yet
    Item it;
    it.seq       = seq;
    it.arrivalMs = nowMs;
    it.original  = std::move(original);
    q_.push_back(std::move(it));
    return true;
}

void Queue::resolve(uint64_t seq, std::vector<uint8_t> translated) {
    for (Item& it : q_) {
        if (it.seq != seq) continue;
        it.translated = std::move(translated);
        it.ready = true;
        return;
    }
}

std::vector<Release> Queue::tick(uint32_t nowMs) {
    std::vector<Release> out;
    while (!q_.empty()) {
        Item& h = q_.front();
        const bool expired = at_or_after(nowMs, h.arrivalMs + maxHoldMs_);
        if (!h.ready && !expired) break;                          // still translating
        if (chain_) {
            const int32_t  d   = (int32_t)(h.arrivalMs - lastArrivalMs_);
            const uint32_t gap = d > 0 ? (uint32_t)d : 0u;
            if (!at_or_after(nowMs, lastReleaseMs_ + gap)) break;  // not its turn yet
        }
        const bool useT = h.ready && !h.translated.empty();
        Release r;
        r.seq        = h.seq;
        r.translated = useT;
        r.payload    = useT ? h.translated : h.original;
        r.original   = std::move(h.original);
        out.push_back(std::move(r));
        chain_         = true;
        lastReleaseMs_ = nowMs;
        lastArrivalMs_ = h.arrivalMs;
        q_.pop_front();
    }
    return out;
}

std::vector<Release> Queue::flush() {
    std::vector<Release> out;
    for (Item& h : q_) {
        Release r;
        r.seq        = h.seq;
        r.translated = false;
        r.payload    = h.original;
        r.original   = std::move(h.original);
        out.push_back(std::move(r));
    }
    q_.clear();
    chain_ = false;
    return out;
}

} // namespace i18n::dlgq
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_dlgq_test.cmd 2>&1" | tail -n 3`
Expected: `22 checks, 0 failures`.

- [ ] **Step 5: Commit**

```bash
git add src/i18n/dlgq.h src/i18n/dlgq.cpp tests/dlgq_test.cpp tests/run_dlgq_test.cmd
git commit -m "feat(i18n): ordered, timing-faithful release queue for held dialogs

Releases held dialogs strictly in arrival order, each no sooner than the
previous release plus the original gap between their arrivals, so spammed
pages keep their reading time; falls back to the original after maxHold;
closes keep their place; a new burst starts a fresh chain. Pure, host-tested
with an injected clock.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 6: Dialog glue — hold, translate unit by unit, faithful replay via Show

**Files:**
- Modify: `src/i18n/engine.h`, `src/i18n/engine.cpp`
- Modify: `src/i18n/surfaces.h`, `src/i18n/surfaces.cpp`
- Modify: `src/compat/compat.cpp`, `src/i18n/hooks.cpp`, `FuegoReversed.vcxproj`

**Interfaces:**
- Consumes: Task 1 (`plan_dialog`, `original_cores`, `unit_text`, `assemble_body`, `assemble_dialog`), Task 5 (`dlgq::Queue`), `engine::cache_lookup`, `engine::submit`, `samp::base()`.
- Produces: `engine::Job::capBytes` (default 144); `process_one(..., int capBytes = 144)`; `bool i18n::surfaces::on_incoming_rpc(unsigned char, RPCParameters*)` (true = swallowed); `void i18n::surfaces::on_part_result(const i18n::engine::Result&)` (surface 3); `void i18n::surfaces::tick(uint32_t nowMs)`; `bool i18n::surfaces::holding()`. Engine surfaces: 0 inbound chat, 1 outbound chat, 2 TM fill, 3 dialog unit, 4 dialog response (Task 7).

- [ ] **Step 1: Let dialog lines exceed a chat line**

In `src/i18n/engine.h`, inside `struct Job`, after `bool noDedup = false;`, add:

```cpp
    // Max bytes of the finished translation. Chat's 144 by default; dialog units
    // (a body line can be longer than one chat line) pass more.
    int capBytes = 144;
```

Also change the `surface` field comment to: `int surface;          // 0 inbound chat, 1 outbound chat, 2 TM fill, 3 dialog unit, 4 dialog response. See hooks::apply_result.`

In `src/i18n/engine.h`, change the `process_one` declaration's last line from `                  const std::string& rollingCtx = "");` to:

```cpp
                  const std::string& rollingCtx = "", int capBytes = 144);
```

In `src/i18n/engine.cpp`, change the `process_one` definition signature's last line from `                  int targetLang, int displayCp, const std::string& rollingCtx) {` to:

```cpp
                  int targetLang, int displayCp, const std::string& rollingCtx, int capBytes) {
```

Replace the cap statement (the two lines starting `    displayOut = charset::cap_and_sanitize(`) with:

```cpp
    displayOut = charset::cap_and_sanitize(
        charset::encode(charset::from_utf8(un), displayCp), (size_t)(capBytes > 0 ? capBytes : 144));
```

In the worker loop, change `tl, dcp, job.rollingCtx);` to `tl, dcp, job.rollingCtx, job.capBytes);`.

Run `run_tests`. Expected: `0 failures` (engine tests use the default).

- [ ] **Step 2: Update the surfaces API**

In `src/i18n/surfaces.h`: after `struct RPCParameters;` add:

```cpp
namespace i18n::engine { struct Result; }
```

Replace the `on_incoming_rpc` declaration and its comment with:

```cpp
// Called from the incoming RPC-dispatch chokepoint (compat::HandleRPCPacketFunc).
// ShowDialog (translate_dialogs): fully cached + nothing held -> rewritten in
// place and shown now (returns false). Otherwise HELD: returns true (the caller
// must NOT pass the RPC to SA:MP); it is translated unit by unit and released
// later through SA:MP's own Show, in arrival order with its original timing.
// Password dialogs and close signals are never translated. Labels/HUD: TM-first
// rewrite in place (returns false). Main thread.
bool on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// A dialog unit's translation finished (engine surface 3). Main thread (pump).
void on_part_result(const i18n::engine::Result& r);

// Release held dialogs that are due (every frame, main thread).
void tick(uint32_t nowMs);

// A dialog is currently held for translation (for the on-screen indicator).
bool holding();
```

- [ ] **Step 3: Implement the hold/release glue**

In `src/i18n/surfaces.cpp`: add `#include "dlgq.h"` after `#include "engine.h"`; add `#include "../samp/samp.h"` after the logger include; add `#include <windows.h>` before `<atomic>`.

Inside the anonymous namespace, directly after the `commit(...)` function, add:

```cpp

// ---- held dialogs: translate, then release in order with the original timing ----
// samp.dll 0.3.DL, confirmed by disassembly + an in-game test (2026-09-27):
//   dialog `this`  *(base+0x2AC9E0)
//   Show           base+0x6FA50  __thiscall(this, int id, int style, char* caption,
//                  char* body, char* btn1, char* btn2, int bServerside), ret 0x1c
// The ShowDialog RPC handler only decodes the packet and calls Show (bServerside=1,
// and only when title AND body are non-empty); the cursor/controls are taken each
// frame from iIsActive, so calling Show ourselves is exactly equivalent. An id < 0
// (0x8000..0xFFFF) is the server's CLOSE: Show(-1) hides the dialog.
constexpr uint32_t kDialogPtr  = 0x2AC9E0;
constexpr uint32_t kDialogShow = 0x6FA50;
using ShowFn = void( __thiscall * )( void* self, int id, int style, const char* caption,
                                     const char* body, const char* btn1, const char* btn2, int serverside );

// SEH-isolated call into SA:MP (no C++ objects in this frame, so __except is legal).
bool safe_show(void* fn, void* self, int id, int style, const char* cap, const char* body,
               const char* b1, const char* b2) {
    __try {
        ((ShowFn)fn)(self, id, style, cap, body, b1, b2, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

i18n::dlgq::Queue g_dlgq(10000, 16); // hold at most 10s, at most 16 dialogs

// A held dialog still waiting for some of its unit translations.
struct PendingDialog {
    DialogPlan               plan;
    std::vector<std::string> cores;
    int                      outstanding = 0;
};
std::unordered_map<uint64_t, PendingDialog>               g_pendingDialogs; // seq -> dialog
std::unordered_map<uint64_t, std::pair<uint64_t, size_t>> g_partJobs;       // job -> (seq, unit)

// List/tablist rows of the translated dialog now on screen, per dialog id, so a
// DialogResponse can be mapped back to the ORIGINAL row (used by Task 7).
struct ShownRows {
    uint8_t                  style = 0;
    std::vector<std::string> orig, trans;
};
std::unordered_map<uint16_t, ShownRows> g_shownRows;

// Remember the rows of a translated LIST/TABLIST dialog that is now on screen.
void remember_rows(const std::vector<uint8_t>& original, const std::vector<uint8_t>& shown) {
    DialogPlan o, t;
    if (!plan_dialog(original.data(), original.size(), o) || !plan_dialog(shown.data(), shown.size(), t)) return;
    if (!(o.style == 2 || o.style == 4 || o.style == 5) || !o.haveBody || !t.haveBody) return;
    ShownRows r;
    r.style = o.style;
    split_lines(assemble_body(o, original_cores(o)), r.orig);
    split_lines(assemble_body(t, original_cores(t)), r.trans);
    if (r.orig.size() != r.trans.size()) return;
    if (g_shownRows.size() > 64) g_shownRows.clear();
    g_shownRows[o.id] = std::move(r);
}

// Show a released payload (translated or original) through SA:MP's own Show.
void show_payload(const std::vector<uint8_t>& pkt) {
    DialogPlan d;
    if (!plan_dialog(pkt.data(), pkt.size(), d) || !d.haveBody) return;
    const std::vector<std::string> cores = original_cores(d);
    std::string title = unit_text(d.units[0], cores[0]);
    std::string b1    = unit_text(d.units[1], cores[1]);
    std::string b2    = unit_text(d.units[2], cores[2]);
    std::string body  = assemble_body(d, cores);
    if (title.empty() || body.empty()) return;        // the RPC handler skips these too
    if (title.size() > 255) title.resize(255);        // Show copies it into a 256-byte field
    if (body.size() > 4095) body.resize(4095);        // the handler's decode limit
    const uint32_t base = samp::base();
    if (!base) return;
    void* dlg = *reinterpret_cast<void**>(base + kDialogPtr);
    if (!dlg) return;
    if (!safe_show((void*)(base + kDialogShow), dlg, (int)(int16_t)d.id, d.style,
                   title.c_str(), body.c_str(), b1.c_str(), b2.c_str()))
        Log("[i18n] dialog release: Show failed (crash-guarded) for id %d.", (int)(int16_t)d.id);
}

// A ShowDialog arrived (translate_dialogs on). True = we SWALLOWED it (it will be
// released later through the queue); false = let SA:MP show it now (possibly
// rewritten in place from the translation memory).
bool handle_dialog(RPCParameters* rpc, const uint8_t* p, size_t n) {
    DialogPlan d;
    if (!plan_dialog(p, n, d) || !d.haveBody) return false;   // can't release it ourselves -> native
    const bool passthru = never_translated(d);                 // CLOSE / PASSWORD: never translated
    auto& s = i18n::config::get();

    // Translation-memory pass: fill every unit we already have.
    std::vector<std::string> cores = original_cores(d);
    std::vector<size_t> missing;
    bool anyTranslated = false;
    if (!passthru) {
        for (size_t k = 0; k < d.units.size(); ++k) {
            if (!d.units[k].translate) continue;
            std::string t;
            if (i18n::engine::cache_lookup(d.units[k].core, s.source_cp, s.target_lang, t)) {
                cores[k] = t;
                anyTranslated = true;
            } else {
                missing.push_back(k);
            }
        }
    }
    const std::vector<uint8_t> original(p, p + n);

    // Nothing to wait for and nothing ahead of it: SA:MP shows it right now.
    if (g_dlgq.empty() && missing.empty()) {
        if (anyTranslated) {
            std::vector<uint8_t> pkt = assemble_dialog(d, cores);
            commit(rpc, pkt);
            remember_rows(original, pkt);
        }
        return false;
    }

    // Hold it: translate what's missing; release in order with the original timing.
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    if (!g_dlgq.push(seq, GetTickCount(), original)) {
        // Queue full (something is stuck): show everything held, then this one natively.
        for (auto& r : g_dlgq.flush()) { g_pendingDialogs.erase(r.seq); show_payload(r.payload); }
        return false;
    }
    if (missing.empty()) { // ready now (cached / close / password) -- waits for its turn
        g_dlgq.resolve(seq, anyTranslated ? assemble_dialog(d, cores) : std::vector<uint8_t>{});
        return true;
    }

    if (g_partJobs.size() > 512) g_partJobs.clear(); // results that never came back
    PendingDialog pd;
    pd.plan        = d;
    pd.cores       = std::move(cores);
    pd.outstanding = (int)missing.size();
    g_pendingDialogs[seq] = std::move(pd);
    for (size_t k : missing) {
        i18n::engine::Job j;
        j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
        j.surface     = 3;          // dialog unit -> hooks::apply_result -> on_part_result
        j.sourceBytes = d.units[k].core;
        j.srcCodepage = s.source_cp;
        j.skipContext = true;
        j.noDedup     = true;       // every held dialog must get its own result back
        j.capBytes    = 1024;       // dialog lines can be longer than a chat line
        g_partJobs[j.handle] = { seq, k };
        i18n::engine::submit(j);
    }
    return true;
}
```

Replace the whole existing `on_incoming_rpc` function (after the anonymous namespace) with:

```cpp
bool on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!rpc || !rpc->input || rpc->numberOfBitsOfData < 8) return false;
    auto& s = i18n::config::get();
    const uint8_t* p = rpc->input;
    size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);

    try {
        if (id == 61) return s.translate_dialogs ? handle_dialog(rpc, p, n) : false;

        std::vector<uint8_t> out;
        bool ok = false;
        if (s.translate_dialogs && id == 36) {
            ok = rebuild_label(p, n, tm_translate, out);
        } else if (s.translate_hud && (id == 73 || id == 59 || id == 105 || id == 134)) {
            switch (id) {
                case 73:  ok = rebuild_gametext(p, n, tm_translate_hud, out); break;
                case 59:  ok = rebuild_chatbubble(p, n, tm_translate_hud, out); break;
                case 105: ok = rebuild_textdraw_setstring(p, n, tm_translate_hud, out); break;
                case 134: ok = rebuild_textdraw_show(p, n, tm_translate_hud, out); break;
            }
        } else {
            return false;
        }
        if (ok && !out.empty()) commit(rpc, out);
    } catch (...) {
        // Never let an exception cross back into SA:MP's RPC dispatch.
    }
    return false;
}

void on_part_result(const i18n::engine::Result& r) {
    auto pj = g_partJobs.find(r.handle);
    if (pj == g_partJobs.end()) return;
    const uint64_t seq  = pj->second.first;
    const size_t   unit = pj->second.second;
    g_partJobs.erase(pj);
    auto it = g_pendingDialogs.find(seq);
    if (it == g_pendingDialogs.end()) return; // already released (timed out)
    PendingDialog& pd = it->second;
    if (r.ok && !r.displayBytes.empty() && unit < pd.cores.size()) pd.cores[unit] = r.displayBytes;
    if (--pd.outstanding > 0) return;
    g_dlgq.resolve(seq, assemble_dialog(pd.plan, pd.cores));
    g_pendingDialogs.erase(it);
}

void tick(uint32_t nowMs) {
    if (g_dlgq.empty()) return;
    std::vector<i18n::dlgq::Release> out =
        i18n::config::get().translate_dialogs ? g_dlgq.tick(nowMs) : g_dlgq.flush();
    for (auto& r : out) {
        g_pendingDialogs.erase(r.seq); // a straggler that timed out
        show_payload(r.payload);
        if (r.translated) remember_rows(r.original, r.payload);
    }
}

bool holding() { return !g_dlgq.empty(); }
```

- [ ] **Step 4: Swallow held dialogs in compat**

In `src/compat/compat.cpp`, replace these lines:

```cpp
	// Translate incoming dialogs / 3D labels IN PLACE before the game parses them
	// (no-op unless config.translate_dialogs is on; TM-first, so it only rewrites
	// on a cache hit). Runs before the rewrites/reads below so they see final text.
	i18n::surfaces::on_incoming_rpc( id, rpc );
```

with:

```cpp
	// Translate incoming dialogs / 3D labels / HUD text. A ShowDialog that still needs
	// translating is HELD (swallowed here) and released later, translated and in order,
	// through SA:MP's own Show. Runs before the rewrites/reads below so they see the
	// final text.
	if ( i18n::surfaces::on_incoming_rpc( id, rpc ) ) return;
```

- [ ] **Step 5: Route results + tick**

In `src/i18n/hooks.cpp`: add `#include "surfaces.h"` after `#include "chatbuf.h"`. In `apply_result`, directly after the surface-1 line, add:

```cpp
	// Dialog unit translations (surface 3) complete a held dialog.
	if ( r.surface == 3 ) { i18n::surfaces::on_part_result( r ); return; }
```

Replace `pump_tick` with:

```cpp
void pump_tick( IDirect3DDevice9 * ) {
	i18n::engine::pump();
	const uint32_t now = GetTickCount();
	i18n::names::tick( now );    // live player names -> masked, never translated
	i18n::surfaces::tick( now ); // release held dialogs that are due
	i18n::lab::on_frame();       // debug lab (read-only logging)
}
```

In `FuegoReversed.vcxproj`, after the `src\i18n\chatbuf.cpp` entry, add:

```xml
    <ClCompile Include="src\i18n\dlgq.cpp">
      <ObjectFileName>$(IntDir)i18n_dlgq.obj</ObjectFileName>
    </ClCompile>
```

- [ ] **Step 6: Build + regression**

Run the Build command, then all six runners (`run_tests`, `run_surfaces_test`, `run_huffman_test`, `run_stringcompressor_test`, `run_chatbuf_test`, `run_dlgq_test`). Expected: clean build; `0 failures` on all.

- [ ] **Step 7: Commit**

```bash
git add src/i18n/engine.h src/i18n/engine.cpp src/i18n/surfaces.h src/i18n/surfaces.cpp src/compat/compat.cpp src/i18n/hooks.cpp FuegoReversed.vcxproj
git commit -m "feat(i18n): hold dialogs until translated, release them in order with original timing

A ShowDialog not fully in the translation memory is swallowed, translated unit
by unit (dialog units may exceed a chat line: Job::capBytes), and released
through SA:MP's own Show (0x6FA50, bServerside=1, title <=255 bytes) in arrival
order, each page no sooner than its original gap after the previous one;
untranslated after 10s -> shown as the original. Cached dialogs with nothing
held still show instantly. Close signals and password dialogs are never
translated but keep their place in the order.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 7: Dialog responses — index remap for lists, outgoing input translation

**Files:**
- Modify: `src/i18n/surfaces.h`, `src/i18n/surfaces_rebuild.cpp`, `src/i18n/surfaces.cpp`, `src/i18n/hooks.cpp`
- Test: `tests/surfaces_test.cpp`

**Interfaces:**
- Consumes: `g_shownRows` (Task 6), `config.outbound_enabled`, `config.outbound_target_lang`, `config::build_system_prompt`, `config::display_codepage`, `g_RakClient`.
- Produces: `std::string i18n::surfaces::strip_colors(const std::string&)`, `bool i18n::surfaces::remap_list_response(int style, const std::vector<std::string>& orig, const std::vector<std::string>& trans, int listItem, const std::string& resp, std::string& out)`, `void i18n::surfaces::on_response_result(const i18n::engine::Result&)` (surface 4).

- [ ] **Step 1: Write the failing test**

In `tests/surfaces_test.cpp`, add before the final `std::printf(...)`/`return`:

```cpp
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_surfaces_test.cmd 2>&1" | tail -n 5`
Expected: compile error — `remap_list_response` / `strip_colors` undeclared.

- [ ] **Step 3: Declare + implement the pure remap**

In `src/i18n/surfaces.h`, after the `assemble_dialog` declaration, add:

```cpp

// Remove {RRGGBB} colour codes (SA:MP strips them from a list row's response text).
std::string strip_colors(const std::string& s);

// A DialogResponse from a TRANSLATED list/tablist dialog carries text SA:MP derived
// from the TRANSLATED row the player picked. Rebuild the text the ORIGINAL dialog
// would have produced for the same row: find which derivation (whole row / first
// column, colour-stripped or raw) turns trans[row] into `resp` and apply it to
// orig[row]. row = listItem (+1 for TABLIST_HEADERS, whose row 0 is the header).
// False if out of range or nothing matches (the caller then sends `resp` unchanged).
bool remap_list_response(int style, const std::vector<std::string>& orig,
                         const std::vector<std::string>& trans, int listItem,
                         const std::string& resp, std::string& out);

// An outgoing dialog-input translation finished (engine surface 4). Main thread.
void on_response_result(const i18n::engine::Result& r);
```

Also replace the comment above `install()` with:

```cpp
// Register the outbound DialogResponse (RPC 62) handler. Call ONCE at startup,
// BEFORE net::install() (like outbound::install). It (a) maps a picked row of a
// TRANSLATED list/tablist back to the ORIGINAL row text by index, and (b) with
// outgoing translation on, translates INPUT-dialog text into the server's
// language. PASSWORD dialog responses are never touched.
```

In `src/i18n/surfaces_rebuild.cpp`, after `assemble_dialog`, add:

```cpp

std::string strip_colors(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (is_color_code(s, i)) { i += 8; continue; }
        o += s[i++];
    }
    return o;
}

bool remap_list_response(int style, const std::vector<std::string>& orig,
                         const std::vector<std::string>& trans, int listItem,
                         const std::string& resp, std::string& out) {
    if (listItem < 0 || orig.size() != trans.size()) return false;
    const size_t row = (size_t)listItem + (style == 5 ? 1u : 0u);
    if (row >= orig.size()) return false;
    auto first_col = [](const std::string& s) {
        size_t t = s.find('\t');
        return t == std::string::npos ? s : s.substr(0, t);
    };
    const std::string& o = orig[row];
    const std::string& t = trans[row];
    const std::string candT[4] = { strip_colors(t), strip_colors(first_col(t)), t, first_col(t) };
    const std::string candO[4] = { strip_colors(o), strip_colors(first_col(o)), o, first_col(o) };
    for (int k = 0; k < 4; ++k)
        if (candT[k] == resp) { out = candO[k]; return true; }
    return false;
}
```

Run: `cmd //c "call D:\hax0r\fuego-reversed\tests\run_surfaces_test.cmd 2>&1" | tail -n 3`
Expected: `0 failures`.

- [ ] **Step 4: Replace the response handler**

In `src/i18n/surfaces.cpp`: delete `g_listRevMap` and the whole `note_list_rows` function, and in `tm_translate` delete the line `note_list_rows(src, out); // remember rows in case this is a list dialog body`. Then replace the whole `on_send_dialogresponse` function (and its comment) with:

```cpp
// DialogResponse wire (client -> server): i16 dialogId, u8 response, i16 listItem,
// u8 textLen, text. Sent via g_RakClient, which wraps the REAL interface and so
// bypasses the send seam -- this can never re-enter on_send_dialogresponse().
void send_dialog_response(int16_t dlgId, uint8_t resp, int16_t listItem, const std::string& text) {
    if (!g_RakClient) return;
    const size_t tl = text.size() > 255 ? 255 : text.size();
    BitStream bs;
    bs.Write(dlgId);
    bs.Write(resp);
    bs.Write(listItem);
    bs.Write((uint8_t)tl);
    if (tl) bs.Write((char*)text.data(), (int)tl);
    g_RakClient->RPC(62, &bs);
}

struct PendingResponse {
    int16_t     dlgId = 0;
    uint8_t     resp = 0;
    int16_t     listItem = 0;
    std::string original;
};
std::unordered_map<uint64_t, PendingResponse> g_pendingResponses;

// The live dialog's style if it's the one being answered (SA:MP's own state, read at
// send time -- the response is sent before the dialog closes), else -1.
int answered_style(int16_t dlgId) {
    const uint32_t base = samp::base();
    if (!base) return -1;
    const uint8_t* dlg = *reinterpret_cast<const uint8_t* const*>(base + kDialogPtr);
    if (!dlg) return -1;
    if (*reinterpret_cast<const int16_t*>(dlg + 0x30) != dlgId) return -1;
    return *reinterpret_cast<const int*>(dlg + 0x2C);
}

bool on_send_dialogresponse(int id, BitStream* params, PacketPriority, PacketReliability, char, bool) {
    if (id != 62 || !params) return true;
    try {
        params->ResetReadPointer();
        int16_t dlgId = 0, listItem = 0;
        uint8_t resp = 0, tlen = 0;
        if (!params->Read(dlgId) || !params->Read(resp) || !params->Read(listItem) || !params->Read(tlen)) {
            params->ResetReadPointer();
            return true;
        }
        char buf[256] = {};
        if (tlen && !params->Read(buf, tlen)) { params->ResetReadPointer(); return true; }
        params->ResetReadPointer();

        const int style = answered_style(dlgId);
        if (style == 3) return true;  // PASSWORD: never touched, never logged
        if (style < 0) return true;   // not the dialog on screen -> leave it alone
        const std::string text(buf, tlen);
        auto& s = i18n::config::get();

        // LIST / TABLIST we translated: send the ORIGINAL row's text for the picked index.
        if (style == 2 || style == 4 || style == 5) {
            auto it = g_shownRows.find((uint16_t)dlgId);
            if (it == g_shownRows.end()) return true;
            std::string orig;
            if (!remap_list_response(style, it->second.orig, it->second.trans, listItem, text, orig) || orig == text)
                return true;
            send_dialog_response(dlgId, resp, listItem, orig);
            return false; // swallow the translated-text response; the original went instead
        }

        // INPUT: translate what I typed into the server's language (shared outgoing toggle).
        // Only for the main button, never commands, never numbers/PINs/amounts.
        if (style == 1 && s.outbound_enabled && resp == 1 && !text.empty() && text[0] != '/' &&
            !is_dynamic_text(text)) {
            if (g_pendingResponses.size() > 64) g_pendingResponses.clear();
            i18n::engine::Job job;
            job.handle             = g_seq.fetch_add(1, std::memory_order_relaxed);
            job.surface            = 4; // -> hooks::apply_result -> on_response_result
            job.sourceBytes        = text;
            job.srcCodepage        = s.source_cp;
            job.skipContext        = true;
            job.noDedup            = true;
            job.promptOverride     = i18n::config::build_system_prompt(s.outbound_target_lang, false);
            job.targetLangOverride = s.outbound_target_lang;
            job.displayCpOverride  = i18n::config::display_codepage(s.outbound_target_lang, false);
            g_pendingResponses[job.handle] = { dlgId, resp, listItem, text };
            i18n::engine::submit(job);
            return false; // swallow; on_response_result sends it (translated, or as typed)
        }
    } catch (...) {
        params->ResetReadPointer();
    }
    return true;
}
```

After the `holding()` function (outside the anonymous namespace), add:

```cpp
void on_response_result(const i18n::engine::Result& r) {
    auto it = g_pendingResponses.find(r.handle);
    if (it == g_pendingResponses.end()) return;
    PendingResponse pr = std::move(it->second);
    g_pendingResponses.erase(it);
    const bool ok = r.ok && !r.displayBytes.empty();
    send_dialog_response(pr.dlgId, pr.resp, pr.listItem, ok ? r.displayBytes : pr.original);
}
```

Update the `install()` log line to: `Log("[i18n] surfaces: DialogResponse handler registered (list remap + input translation).");`

- [ ] **Step 5: Route surface 4**

In `src/i18n/hooks.cpp` `apply_result`, after the surface-3 line, add:

```cpp
	// Outgoing dialog-input translations (surface 4) are sent to the server.
	if ( r.surface == 4 ) { i18n::surfaces::on_response_result( r ); return; }
```

- [ ] **Step 6: Build + regression**

Run the Build command, then all six runners. Expected: clean build; `0 failures` on all.

- [ ] **Step 7: Commit**

```bash
git add src/i18n/surfaces.h src/i18n/surfaces_rebuild.cpp src/i18n/surfaces.cpp src/i18n/hooks.cpp tests/surfaces_test.cpp
git commit -m "feat(i18n): send servers what they expect from translated dialogs; translate input replies

A pick on a translated list/tablist now sends the ORIGINAL row's text for the
picked index (whole row or first column, colour-stripped -- exact even when
two rows translate alike), replacing the whole-row text map that never matched
tablist responses. With outgoing translation on, input-dialog text is
translated into the server's language. Password dialog responses are never
touched.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 8: Shared settable hotkey, indicators, indicator toggle

**Files:**
- Modify: `src/i18n/config.h`, `src/i18n/config.cpp`, `src/i18n/outbound.cpp`, `src/menu/menu.cpp`, `FuegoReversed.vcxproj`
- Create: `src/i18n/indicator.h`, `src/i18n/indicator.cpp`

**Interfaces:**
- Consumes: `i18n::surfaces::holding()`, `input::typing()`, `config::language_options()`, `samp::base()`, `render::overlay_hidden()`, `fx::g_set.cheat_panic`.
- Produces: config fields `hotkey_mods` (int, default 3 = Ctrl|Shift), `hotkey_vk` (int, default 0), `show_translate_indicators` (bool, default true); `void i18n::indicator::draw()`.

- [ ] **Step 1: Config fields**

In `src/i18n/config.h`, after `int outbound_target_lang = 1049;` (and its comment), add:

```cpp

    // Outgoing-translation toggle hotkey -- ONE key shared by outgoing chat AND
    // outgoing dialog-input replies (both use outbound_enabled). hotkey_mods is a
    // bitmask (1 Ctrl, 2 Shift, 4 Alt); hotkey_vk an extra virtual key (0 = the
    // modifiers alone). Default Ctrl+Shift (the original toggle).
    int hotkey_mods = 3;
    int hotkey_vk = 0;

    // Draw the on-screen translation hints ("your message will be sent in ...",
    // "translating dialog..."). Default ON.
    bool show_translate_indicators = true;
```

In `src/i18n/config.cpp` `from_text`, after the `outbound_target_lang` branch, add:

```cpp
            } else if (key == "hotkey_mods") {
                g_settings.hotkey_mods = std::stoi(value);
            } else if (key == "hotkey_vk") {
                g_settings.hotkey_vk = std::stoi(value);
            } else if (key == "show_translate_indicators") {
                g_settings.show_translate_indicators = (value == "true" || value == "1");
```

In `to_text`, after the `outbound_target_lang=` line, add:

```cpp
    out << "hotkey_mods=" << g_settings.hotkey_mods << "\n";
    out << "hotkey_vk=" << g_settings.hotkey_vk << "\n";
    out << "show_translate_indicators=" << (g_settings.show_translate_indicators ? "true" : "false") << "\n";
```

In `tests/i18n_tests.cpp` `test_config`, extend the save/load round-trip:
- after the line `    s.source_lang = 1049;  // Russian (source hint)` add:

```cpp
    s.hotkey_mods = 5; s.hotkey_vk = 0x74; s.show_translate_indicators = false;
```

- after the line `    s.source_lang = 0;     // Auto` add:

```cpp
    s.hotkey_mods = 3; s.hotkey_vk = 0; s.show_translate_indicators = true;
```

- after the line `    CHECK_EQ(s.source_lang, 1049);` add:

```cpp
    CHECK_EQ(s.hotkey_mods, 5);
    CHECK_EQ(s.hotkey_vk, 0x74);
    CHECK_EQ(s.show_translate_indicators, false);
```

Run `run_tests`. Expected: `0 failures`.

- [ ] **Step 2: Indicator module**

Create `src/i18n/indicator.h`:

```cpp
#pragma once

namespace i18n::indicator {

// On-screen translation hints, drawn inside the ImGui frame on the background draw
// list: "your message will be sent in <lang>" while typing in chat or while an INPUT
// dialog is open with outgoing translation on, and "translating dialog..." while a
// dialog is held for translation. No-op unless config.show_translate_indicators.
void draw();

} // namespace i18n::indicator
```

Create `src/i18n/indicator.cpp`:

```cpp
#include "indicator.h"

#include "config.h"
#include "surfaces.h"

#include "../core/input.h"
#include "../samp/samp.h"

#include "imgui.h"

#include <cstdio>

namespace i18n::indicator {

namespace {

// An INPUT-style dialog is open (SA:MP's own state: iIsActive +0x28, style +0x2C).
bool input_dialog_open() {
    const uint32_t base = samp::base();
    if (!base) return false;
    const uint8_t* dlg = *reinterpret_cast<const uint8_t* const*>(base + 0x2AC9E0);
    if (!dlg) return false;
    return *reinterpret_cast<const int*>(dlg + 0x28) != 0 && *reinterpret_cast<const int*>(dlg + 0x2C) == 1;
}

const char* lang_name(int code) {
    for (const auto& o : config::language_options())
        if (o.code == code) return o.name;
    return "the server's language";
}

// A small dark pill, horizontally centred near the top of the screen.
void pill(ImDrawList* dl, float y, ImU32 col, const char* text) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float x = (ImGui::GetIO().DisplaySize.x - sz.x) * 0.5f;
    dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + sz.x + 8, y + sz.y + 4), IM_COL32(0, 0, 0, 170), 5.0f);
    dl->AddText(ImVec2(x, y), col, text);
}

} // namespace

void draw() {
    const auto& s = config::get();
    if (!s.show_translate_indicators) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = ImGui::GetIO().DisplaySize.y * 0.06f;
    char buf[160];
    if (s.outbound_enabled && (input::typing() || input_dialog_open())) {
        std::snprintf(buf, sizeof(buf), "[translate] your message will be sent in %s", lang_name(s.outbound_target_lang));
        pill(dl, y, IM_COL32(120, 230, 120, 255), buf);
        y += ImGui::GetFontSize() + 14;
    }
    if (s.translate_dialogs && surfaces::holding())
        pill(dl, y, IM_COL32(235, 200, 90, 255), "[translate] translating dialog...");
}

} // namespace i18n::indicator
```

In `FuegoReversed.vcxproj`, after the `src\i18n\dlgq.cpp` entry, add:

```xml
    <ClCompile Include="src\i18n\indicator.cpp">
      <ObjectFileName>$(IntDir)i18n_indicator.obj</ObjectFileName>
    </ClCompile>
```

- [ ] **Step 3: Menu — hotkey, toggle, draw call**

In `src/menu/menu.cpp`: add `#include "../i18n/indicator.h"` next to the other `../i18n/` includes.

Replace the whole Ctrl+Shift block in `on_frame` (from the comment `// Ctrl+Shift toggles outgoing chat translation (edge-detected + 400ms cooldown` through the closing `}` of that block) with:

```cpp
	// Outgoing-translation hotkey (config hotkey_mods/hotkey_vk, default Ctrl+Shift),
	// shared by outgoing chat + dialog-input replies. Edge-detected + 400ms cooldown so
	// a held combo fires once; toggle() prints a chat notice so it's never silent.
	{
		static bool  prevCombo  = false;
		static DWORD lastToggle = 0;
		const auto  &hk   = i18n::config::get();
		auto         down = []( int vk ) { return ( GetAsyncKeyState( vk ) & 0x8000 ) != 0; };
		bool combo = ( hk.hotkey_mods || hk.hotkey_vk ) &&
		             ( !( hk.hotkey_mods & 1 ) || down( VK_CONTROL ) ) &&
		             ( !( hk.hotkey_mods & 2 ) || down( VK_SHIFT ) ) &&
		             ( !( hk.hotkey_mods & 4 ) || down( VK_MENU ) ) &&
		             ( !hk.hotkey_vk || down( hk.hotkey_vk ) );
		DWORD now = GetTickCount();
		if ( combo && !prevCombo && ( now - lastToggle ) > 400 ) {
			i18n::outbound::toggle();
			lastToggle = now;
		}
		prevCombo = combo;
	}
```

In `on_frame`, directly after the line `visuals::draw(); // ESP + other on-screen cheats (background draw list)`, add:

```cpp
	if ( !render::overlay_hidden() && !fx::g_set.cheat_panic )
		i18n::indicator::draw(); // "will be translated" / "translating dialog..." hints
```

In the Translator tab, replace the outgoing section header + checkbox + help (the three statements starting `ImGui::SeparatorText( "Outgoing chat (translate MY messages)" );` through the end of that `HelpMarker( ... );`) with:

```cpp
			ImGui::SeparatorText( "Outgoing (translate MY chat + dialog input)" );
			if ( ImGui::Checkbox( "Translate my messages before sending", &s.outbound_enabled ) )
				i18n::hooks::apply_config(); // apply live
			ImGui::SameLine();
			HelpMarker( "Your typed chat AND your text in input dialogs are translated into the server's "
			            "language before sending. Password dialogs are never touched; picking a row in a "
			            "translated list always sends the server's original row. Commands (/...) and "
			            "numbers are never translated. If translation fails, your original text is sent." );
			{
				bool c  = ( s.hotkey_mods & 1 ) != 0;
				bool sh = ( s.hotkey_mods & 2 ) != 0;
				bool a  = ( s.hotkey_mods & 4 ) != 0;
				ImGui::TextUnformatted( "Toggle hotkey:" );
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Ctrl", &c ) ) s.hotkey_mods ^= 1;
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Shift", &sh ) ) s.hotkey_mods ^= 2;
				ImGui::SameLine();
				if ( ImGui::Checkbox( "Alt", &a ) ) s.hotkey_mods ^= 4;
				ImGui::SameLine();
				static const struct { int vk; const char *name; } kKeys[] = {
					{ 0, "(no key)" }, { VK_F3, "F3" }, { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
					{ VK_F9, "F9" }, { VK_F11, "F11" }, { VK_HOME, "Home" }, { VK_END, "End" },
					{ VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" }, { VK_PAUSE, "Pause" } };
				const char *cur = "(custom)";
				for ( const auto &k : kKeys )
					if ( k.vk == s.hotkey_vk ) cur = k.name;
				ImGui::SetNextItemWidth( 120.0f );
				if ( ImGui::BeginCombo( "##hotkey_vk", cur ) ) {
					for ( const auto &k : kKeys )
						if ( ImGui::Selectable( k.name, k.vk == s.hotkey_vk ) ) s.hotkey_vk = k.vk;
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				HelpMarker( "One hotkey for outgoing chat AND dialog input. Default Ctrl+Shift with no key. "
				            "Avoid keys the game uses (F1 help, F2 menu, F7 chat, F8 screenshot, F12). "
				            "Saved with Save config." );
			}
			ImGui::Checkbox( "Show translation indicators", &s.show_translate_indicators );
			ImGui::SameLine();
			HelpMarker( "On-screen hints: 'your message will be sent in <language>' while you type in chat "
			            "or an input dialog with outgoing translation on, and 'translating dialog...' while "
			            "a dialog is held for translation." );
```

- [ ] **Step 4: Toggle notice mentions dialogs**

In `src/i18n/outbound.cpp` `toggle()`, change the ON string to:

```cpp
		? "{33FF33}[translate] {FFFFFF}Outgoing translation ON -- your chat and dialog input go out in the server's language."
```

- [ ] **Step 5: Build + regression**

Run the Build command, then all six runners. Expected: clean build; `0 failures` on all.

- [ ] **Step 6: Commit**

```bash
git add src/i18n/config.h src/i18n/config.cpp src/i18n/outbound.cpp src/i18n/indicator.h src/i18n/indicator.cpp src/menu/menu.cpp FuegoReversed.vcxproj tests/i18n_tests.cpp
git commit -m "feat(i18n): one settable hotkey + indicators for outgoing translation

The outgoing toggle (chat + dialog input) now uses a settable hotkey (Ctrl/
Shift/Alt + optional key, default Ctrl+Shift). New on-screen hints: 'your
message will be sent in <lang>' while typing or in an input dialog, and
'translating dialog...' while one is held -- behind a 'Show translation
indicators' setting that defaults ON. All saved in the unified config.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

---

### Task 9: Lab cleanup, full verification, hand-off

**Files:**
- Modify: `src/i18n/lab.h`, `src/i18n/lab.cpp`, `src/i18n/config.h`, `src/i18n/config.cpp`, `src/menu/menu.cpp`, `src/compat/compat.cpp`

**Interfaces:** Consumes everything above. Produces the final branch.

- [ ] **Step 1: Remove the LIVE replace experiment**

The real feature supersedes it, and while on it tagged every chat line and dialog with `[LAB]`. Keep the read-only logging.

- `src/i18n/config.h`: replace the whole block from `    // Debug lab (developer): verify the in-place chat / dialog replacement` through `    bool debug_lab_replace = false;` with:

```cpp
    // Debug lab (developer, read-only): log the newest chat entry + decoded
    // ShowDialog fields to fuego_reversed.log. Default OFF.
    bool debug_lab = false;
```
- `src/i18n/config.cpp`: delete the `debug_lab_replace` branch in `from_text` and the `debug_lab_replace=` line in `to_text` (an old config line with that key is then silently ignored).
- `src/menu/menu.cpp`: delete the `"Lab: LIVE replace test (experimental)"` checkbox, its `ImGui::SameLine();` and its `HelpMarker(...)`.
- `src/compat/compat.cpp`: change the lab comment to `// Debug lab (read-only): decode + log ShowDialog (61). Never modifies the packet.`

Replace `src/i18n/lab.h` with:

```cpp
#pragma once
// Debug lab (read-only): with config.debug_lab on, log the newest chat entry and
// every decoded ShowDialog (id/style/title/buttons/body) to fuego_reversed.log --
// handy when checking the in-place chat/dialog translation against a live server.

struct RPCParameters; // net/RakClient.h

namespace i18n::lab {

// Incoming-RPC seam (compat::HandleRPCPacketFunc): logs ShowDialog (61) fields.
void on_incoming_rpc(unsigned char id, RPCParameters* rpc);

// Main-thread per-frame tick (hooks::pump_tick): logs each new bottom chat line.
void on_frame();

} // namespace i18n::lab
```

Replace `src/i18n/lab.cpp` with:

```cpp
#include "lab.h"

#include "config.h"
#include "huffman.h"

#include "../samp/samp.h"
#include "../net/RakClient.h"
#include "../core/logger.h"

#include <cstdint>
#include <string>

namespace i18n::lab {

namespace {
// samp.dll 0.3.DL: chat `this` @ *(base+0x2ACA10); chatEntry[99].szText @ this+0x132+99*0xFC+0x20.
constexpr uint32_t kChatPtr = 0x2ACA10;
constexpr uint32_t kNewestText = 0x132 + 99 * 0xFC + 0x20;
std::string g_lastBottom;

bool read_u8str(const uint8_t* p, size_t n, size_t& off, std::string& out) {
    if (off >= n) return false;
    uint8_t len = p[off++];
    if (off + len > n) return false;
    out.assign((const char*)(p + off), len);
    off += len;
    return true;
}
} // namespace

void on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!i18n::config::get().debug_lab) return;
    if (id != 61 || !rpc || !rpc->input || rpc->numberOfBitsOfData < 48) return;
    const uint8_t* p = rpc->input;
    size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);
    size_t off = 2;
    int dlgId = (int)(int16_t)(p[0] | (p[1] << 8));
    int style = p[off++];
    std::string title, b1, b2, body;
    if (!read_u8str(p, n, off, title) || !read_u8str(p, n, off, b1) || !read_u8str(p, n, off, b2)) return;
    if (n > off) huffman::decode_string(p + off, n - off, body);
    if (style == 3) body = "(password dialog body not logged)";
    Log("[lab] dialog RPC61: id=%d style=%d title='%s' btn1='%s' btn2='%s' body='%.120s'",
        dlgId, style, title.c_str(), b1.c_str(), b2.c_str(), body.c_str());
}

void on_frame() {
    if (!i18n::config::get().debug_lab) return;
    const uint32_t base = samp::base();
    if (!base) return;
    const uint8_t* chat = *reinterpret_cast<const uint8_t* const*>(base + kChatPtr);
    if (!chat) return;
    const char* bottom = (const char*)(chat + kNewestText);
    if (!bottom[0]) return;
    std::string cur(bottom);
    if (cur == g_lastBottom) return;
    g_lastBottom = cur;
    Log("[lab] chat[99]: '%s'", cur.c_str());
}

} // namespace i18n::lab
```

- [ ] **Step 2: Full verification**

Run the Build command, then all six runners.
Expected: clean build; `0 failures` on `run_tests`, `run_surfaces_test`, `run_huffman_test`, `run_stringcompressor_test`, `run_chatbuf_test`, `run_dlgq_test`.

Then confirm nothing references removed symbols:
Run: `grep -rn "debug_lab_replace\|g_listRevMap\|note_list_rows\|PendingLine\|g_pendingLines" src/`
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add src/i18n/lab.h src/i18n/lab.cpp src/i18n/config.h src/i18n/config.cpp src/menu/menu.cpp src/compat/compat.cpp
git commit -m "chore(i18n): drop the LIVE replace experiment, keep the read-only lab log

The real in-place translation supersedes the [LAB] experiment. The lab keeps
logging the newest chat line and decoded ShowDialogs (never a password
dialog's body) for checking behaviour against a live server.

Claude-Session: https://claude.ai/code/session_01KTvtoDS1boVi45zSnM5tiC"
```

- [ ] **Step 4: Hand the user the in-game checklist**

Report to the user (in chat), with `bin\FuegoReversed.asi` ready to copy:

1. **Chat:** a new foreign line appears instantly, then turns into the translation in place about a second later without moving other lines; a line seen before appears already translated.
2. **Names:** a line mentioning a player (`khalid`, `Khalid Ahmed`, `Khalid_Ahmed`) keeps the name exactly.
3. **Dialogs:** a dialog seen for the first time appears translated after a short "translating dialog..." hint; `/help` topic pages clicked through quickly replay in order with the same spacing; the `intro`-style shortcode column and distances (`12m`) are unchanged; the login/password dialog appears immediately, untranslated.
4. **Responses:** pick a row in a translated list (bank, GPS) — the server reacts as if you picked the original; with outgoing translation on, text typed in an input dialog reaches the server translated; a password still logs you in.
5. **Hotkey + indicators:** the configured hotkey flips outgoing translation (chat notice appears); the "your message will be sent in …" hint shows while typing; unticking "Show translation indicators" hides the hints.
6. **The optional checks skipped last time:** Provider dropdown + Refresh models, inbound chat with the "Avg response" readout, restart → settings persist.

Then use superpowers:finishing-a-development-branch to integrate `feat/i18n-translate-in-place` (the user has been merging finished work to `master` and pushing).
