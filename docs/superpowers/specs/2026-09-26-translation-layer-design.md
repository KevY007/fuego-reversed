# Fuego Reversed — In-Game Translation Layer (design)

Date: 2026-09-26 (reconciled after a context reset the same day)
Status: **DRAFT for review** (design/brainstorming output; no implementation yet)
Author: session `session_01KTvtoDS1boVi45zSnM5tiC`

> **Decisions so far.** Two tiers: what the user **stated** (answered directly),
> and **proposed defaults** I'm recommending that still need the user's yes.
>
> **User-stated (4 answers given this session):**
> - **Backend = pluggable, must work local (Ollama) OR free, keeping online-API
>   capability** — one configurable OpenAI-compatible endpoint (base URL + key +
>   model) pointing at local Ollama or any cloud provider; multi-provider fallback
>   chain (§5.1) is the advanced option. Modest-hardware floor (GTX 1060-class).
> - **Languages both ways** — auto-detect source → target, and a pinnable
>   source→target pair.
> - **Inbound chat = replace the line with a small marker** (original recoverable).
> - **Outbound = Ctrl+Shift toggles a "translate my messages" mode.**
>
> **Confirmed 2026-09-26 (second answer pass):**
> - **Target language = English by default, but configurable to any language** the
>   chosen model supports (English is the primary/priority target for glossary +
>   font coverage).
> - **Inbound timing = show the original instantly, then swap in place** when the
>   translation lands (never a blank wait).
> - **Marker = subtle leading glyph + faint colour tint** (reads like normal chat).
> - **Local model = user delegated the pick to "whatever is best + easy to run +
>   accurate + understands and does it all."** Decision: default to **Tencent
>   Hy-MT2**, sized to the GPU — **7B Q4 (~4.6 GB)** where it fits, **1.8B (~1.9 GB)**
>   on the GTX 1060 floor — because it is purpose-built for translation with
>   first-class glossary / placeholder-preserving / background-context / JSON
>   templates (exactly our §6 needs), Apache-2.0, free and offline. The provider
>   layer stays **pluggable**, so a small general LLM (better slang) or an online
>   model swaps in via config with no code change.
>
> **Still-proposed defaults (change on review):**
> - Scope = **everything incl. textdraws**, gated by change-detection (§6.8).
> - Failure = **best-effort**: original always shows; a failed/slow/keyless call
>   leaves the original with a quiet status indicator (§10).
> - One `.asi`, **zero install**: local mode = the user runs their own
>   Ollama/llama.cpp server; Fuego ships only the client and installs nothing.
> - On-disk translation memory = **flat append-only file + in-memory map** (v1).

---

## 1. What we're building (understanding brief)

A toggleable, real-time translation layer for SA:MP 0.3.DL that lets the player
read and play on foreign-language servers (Russian, Indonesian, Spanish,
Portuguese, Turkish, …) as if they were in the player's own language, and
communicate back.

Two directions:

- **Inbound (read):** translate *every* server-originated visible string —
  public/team/PM chat, server announcements, the kill feed, dialogs (title +
  body + buttons + list rows), 3D text labels, and textdraws/gametext — into the
  player's language, **in place**, with a small marker so translated text is
  recognisable.
- **Outbound (write):** an optional mode (toggled by **Ctrl+Shift**) that
  translates what the player types in chat into the *server's* language before it
  is sent, so the player can talk to locals.

Hard requirements the user set:

- **Must run locally or for free**, while **keeping online-API capability** —
  the backend is pluggable (local Ollama / a free online model / a paid online
  model), selected in config.
- **Modest-hardware floor:** must be usable for an end user on a **GTX 1060-class
  GPU** (the game itself is light; a small model must fit alongside it). The dev
  box (RTX 5080, Ollama already installed) is *not* the floor.
- **Non-blocking:** translation goes over HTTP to an LLM/MT and is slow
  (0.1 s MT … 1.8 s LLM per batch). It must never stall the network or render
  thread.
- **Context- and format-preserving, not literal:** must keep the *structure*
  of RP text and never mangle things that aren't natural language — `/commands`,
  player names, player IDs, area/zone names, vehicle/weapon names, numbers,
  `{RRGGBB}` colour codes, OOC `(( ))` / `[Accent]` / `(radio)` wrappers. Use
  short rolling context (previous few lines) + a game glossary so it translates
  *meaning*, not words.
- **Languages configurable both ways:** default = auto-detect source → fixed
  target (the player's language); optionally pin a fixed source→target pair.
- **Inbound display:** the line appears immediately as received (original), then
  its text is **replaced in place** with the translation + a subtle marker when it
  lands (never a blank wait); original recoverable on demand.

Non-goals (v1): translating the game's own singleplayer UI/menus; OCR of
image-baked text; voice; translating the trainer's own menu.

---

## 2. Why this is tractable here

The trainer already owns every seam this needs, so the layer is mostly *new code
in existing choke points*, not new infrastructure:

- **Incoming RPC dispatch hook** already exists (`src/compat/compat.cpp`
  `HandleRPCPacketFunc`, the two naked trampolines on SA:MP's RPC dispatch). It
  already decodes/rewrites RPC payloads (the 0.3.7 skin rewrites) — the same
  technique reads/replaces dialog/label/textdraw strings.
- **Net seam** (`src/net/net.h` `on_recv_packet` / `on_send_rpc`) gives a clean
  place to intercept chat packets and outgoing say/command RPCs.
- **`CChat::AddEntry`** (`samp.dll+0x67650`, already wrapped as `samp::chat`) is
  the single display choke point for all chat.
- **Per-frame main-thread driver** already exists (`cheats::on_frame`, called
  from the D3D9 EndScene hook) — the place to safely apply async results (SA:MP
  and D3D are single-threaded; results computed on worker threads must be applied
  here).
- **Pools** (`src/samp/pools.*`, `samp_sdk.h`) already expose the textdraw / 3D
  text-label / dialog structures whose string buffers we rewrite in place.

---

## 3. The visible-text surface (what to hook, and where)

Wire IDs verified against the open.mp SDK NetCode structs (authoritative 0.3.x
layout). "Compressed" = RakNet `StringCompressor` (Huffman) on the wire.

| Surface | Wire / API | String(s) | Chosen interception |
|---|---|---|---|
| **Chat** (player, team, PM, server msgs) | server→client chat RPC → `CChat::AddEntry` (`+0x67650`) | assembled line (prefix + text) | **Hook `AddEntry`** — string already decompressed & assembled; one point covers all chat |
| **Dialog** | `ShowDialog` RPC **61**: `int16 id, u8 style, dynstr8 title, dynstr8 btn1, dynstr8 btn2, compressed body` | title, body, buttons, list rows (body is `\n`-split for list styles) | **RPC-dispatch hook** (compat seam): decode → translate → (async) update `stDialogInfo` + re-layout |
| **3D text label** | `PlayerShowTextLabel` RPC **36**: `u16 id, u32 colour, vec3 pos, float dist, u8 LOS, u16 attachPlayer, u16 attachVeh, compressed text` | label text | RPC-dispatch hook → translate → rewrite `stTextLabel.pText` in the pool (re-rendered from the pool each frame) |
| **Textdraw** | `PlayerShowTextDraw` RPC **134** (+ `ShowTextDraws`/updates) | `szString` | RPC-dispatch hook → translate → rewrite `stTextdraw.szString` in the pool |
| **GameText** | `DisplayGameText` RPC | the string | RPC-dispatch hook (same pattern) |
| **Kill feed** | kill RPC → `stKillInfo` | killer/victim are names (skip); only weapon/reason text if any | usually names only → mostly skip |
| **Outbound chat** | `CInput::Say` (`+0x5860`, `this`=local player) for chat; `SendCommand` (`+0x69340`) for `/cmds` | typed line | Hook the say path (already centralised in `rpc::say`): if outbound-mode on and it's *not* a command, translate→send |

**Interception strategy differs by surface because of async** (see §7):

- **Chat**: swallow the original `AddEntry` call, kick off async translation,
  and re-inject the translated line (with marker) from the per-frame driver when
  it lands. Short strings + cache make this feel instant for repeats.
- **Dialogs**: a dialog stays on screen, so we can **show it immediately with
  the original text, then live-patch** `stDialogInfo` (title/body/buttons) and
  force a re-layout when the translation arrives (typically <1 s). No "hold".
- **Labels / textdraws**: persistent pool entries re-rendered every frame —
  translate once (async), then overwrite the pool string buffer in place; it
  updates on the next frame and stays cached.

Additional text-bearing RPCs (same RPC-dispatch-seam pattern): **ClientMessage 93**
(server info lines, `DynStr32`, max 144), **ChatBubble 59** (`DynStr8`), **GameText
73** (`DynStr32`, GTA bitmap font), **Menu 76** (fixed `char[32]` rows — safe to
translate, selected by index), **SetObjectMaterialText 84 / CreateObject 44**
(in-world signs/billboards, Huffman text — high value on streamer maps).

### 3.1 Huffman string codec — **must be built/borrowed (work item + risk)**
Dialog **body (61)**, **3D-label text (36)**, and **object-material text (84/44)**
are RakNet Huffman-compressed on the wire (`WriteCompressed(u16 bitLen)` +
Huffman bits over `englishCharacterFrequencies[256]`, zero weights→1 so all 256
bytes — incl. cp1251 0xC0–0xFF — are encodable). The compressed-**int** part is
already in `src/net/BitStream.cpp`, but **neither Fuego nor fuego-reversed has a
working Huffman tree** (Fuego's `DecodeString` is dead code). To read/replace
those strings we must either (a) port RakNet's `HuffmanEncodingTree` +
frequency table, or (b) call samp.dll's own `StringCompressor` at its address.
Recommend (a) (self-contained, testable offline). Chat/buttons/titles/textdraws
use plain length-prefixed bytes (no codec needed).

### 3.2 Outbound dialog responses & passwords
For **LIST/TABLIST** dialogs, the client sends the **selected row's text** back in
`DialogResponse (62).inputText`. If rows were shown translated, we must keep the
original↔translated row map and **rewrite the outgoing response back to the
original row text**, or the server won't recognise the choice (open.mp validates
`listItem` against the *original* body's line count — so translation must also
**preserve exact `\n` line count and `\t` columns**). **Never translate
PASSWORD dialogs (style 3)** or their responses.

### 3.3 Known limitation — custom chat `.asi`
Some large servers (e.g. Arizona-family) ship their **own chat renderer**
(`_chat.asi`, ImGui/mimgui based) that bypasses `CChat::AddEntry` entirely, and
may use their own emoji/token encoding (`:u1f603:`, `:item%d:`). On those, the
chat hook won't fire. Mitigation path (later phase): fall back to **wire-level**
chat interception in the recv seam (translate the chat RPC payload before the
custom plugin sees it), or detect the known plugin and hook it specifically. v1
targets stock SA:MP chat; this is documented as a known gap.

---

## 4. Text encoding / charset (easy to get wrong — must be explicit)

SA:MP does **not** use UTF-8. There are **two distinct decode regimes** — getting
this wrong is the single biggest correctness trap:

**Regime A — ANSI codepage** (chat, dialogs, chat bubbles, 3D labels, object
material text): raw single-byte bytes rendered via `D3DXCreateFontA`/`DrawTextA`,
interpreted through the **system ANSI code page (CP_ACP)**.
- Russian/Ukrainian → **Windows-1251**, Turkish → 1254, Central-EU/Polish →
  1250, Western (ES/PT/ID/most Latin) → 1252, Thai → 874, etc.
- Contract: **decode with the *server's* code page → translate → re-encode for
  display in the *user's* ACP** (or add a `DrawTextW` shim), and **encode outgoing
  text in the *server's* code page**. The bytes are meaningless as UTF-8.

**Regime B — GTA bitmap-glyph codes** (textdraws, GameText, native menus): **not
ANSI at all** — ASCII plus glyph slots 128–175, filled per-server by a
**FixChars/ConvertToGameText** table (Latin accents, e.g. `á`→152) or a
**russifier** table (Cyrillic in the same slots, e.g. SanLtd `б`→151, `в`→162,
and Cyrillic look-alikes mapped to Latin). **Byte 151 = `à` on a Spanish server
but `б` on a Russian one** → decoding requires the **per-server table + its
inverse**, not a codepage. (Inverse tables for the common russifiers + FixChars
ship with the layer.)

Source language/regime is a **per-server setting** (auto-guessed by byte-range
profiling + detected language, manually overridable), stored so it's set once.
For a Latin/English target, display re-encoding is always safe (ASCII/1252).

**Pipeline contract for every string:**

1. **Decode** source bytes → Unicode using the configured/detected source
   codepage (`MultiByteToWideChar(cp, …)`).
2. Detect language (on the Unicode text) if in auto mode.
3. Translate (UTF-8 over HTTP).
4. **Encode** the translated Unicode → the display codepage (Regime A) or GTA
   glyph table (Regime B). For an English/Latin target Regime A is 1252/ASCII and
   always safe; for non-Latin targets, ensure the client font's charset supports
   it (a font-charset note, à la the `samp-font` mod, in docs).

---

## 5. Architecture

```
                 ┌────────────────────────────────────────────────────────┐
  incoming RPC   │  interceptors (main thread)                              │
  (recv seam / ──┼─▶ chat / dialog / label / textdraw / gametext           │
   RPC dispatch) │        │  extract string(s) + a stable target handle     │
                 │        ▼                                                  │
                 │   i18n::submit(job)  ── enqueue, return immediately ─────┼──┐
                 └────────────────────────────────────────────────────────┘  │
                                                                              ▼
   CChat::AddEntry hook ─▶ (swallow) ─▶ submit(job) ; original cached for toggle
                                                                              │
                 ┌──────────────────────────── worker pool (N threads) ◀─────┘
                 │  1. cache lookup (normalized key)  → hit: done            │
                 │  2. protect: mask names/ids/cmds/colours/wrappers → ⟦k⟧   │
                 │  3. detect language (skip if already target)              │
                 │  4. micro-batch (≤ ~8 units / 300–500 ms window)          │
                 │  5. provider.translate(batch, glossary, rolling-context)  │
                 │  6. validate: placeholders round-trip? JSON ok? else retry│
                 │     / fall back down the provider chain                   │
                 │  7. unmask, cache, push result → main-thread result queue │
                 └──────────────────────────────────────────────────────────┘
                                                                              │
   per-frame driver (cheats::on_frame, main thread) ◀── drain result queue ──┘
        chat  → AddEntry(translated + marker)
        dialog→ patch stDialogInfo + re-layout
        label → rewrite stTextLabel.pText
        textdraw→ rewrite stTextdraw.szString
```

### 5.1 New modules (`src/i18n/`)
- `engine.{h,cpp}` — `submit(job)`, worker pool, result queue, `pump()` (drained
  by `on_frame`), lifecycle.
- `providers/` — `IProvider` interface + `OllamaProvider` (local, OpenAI-compatible
  `/v1/chat/completions` on 127.0.0.1), `OpenAICompatProvider` (any online LLM by
  base-URL + key + model id: GPT/Gemini-via-OpenRouter/DeepSeek/Mistral/local
  llama-server), `MTProvider` (Azure/Tencent/LibreTranslate as fast fallback).
  Fallback **chain** configurable (e.g. local → online-LLM → MT).
- `cache.{h,cpp}` — in-memory LRU + on-disk persistent translation memory
  (`fuego_tm.db`/flat file), keyed by `hash(normalizedText + srcLang + tgtLang + providerProfile)`.
- `protect.{h,cpp}` — mask/unmask (placeholders), glossary application,
  colour-code/wrapper/command/name/id detection.
- `detect.{h,cpp}` — lightweight offline language detection (n-gram/script based;
  e.g. a compact CLD3-style or trigram model) to skip already-target lines
  cheaply without an API call.
- `charset.{h,cpp}` — codepage decode/encode helpers + per-server codepage.
- `hooks.{h,cpp}` — the AddEntry hook + the dialog/label/textdraw handlers that
  plug into the compat RPC-dispatch seam and the net recv seam.
- `config` — extends `core/settings.h` + `core/config` with an `[i18n]` section.

Nothing here touches the 32-bit game address space with a model — the model
always lives in a separate process (Ollama/llama-server) reached over local
HTTP, keeping the ASI lean and crash-isolated.

### 5.2 HTTP client
WinHTTP (ships with Windows, no new dependency) on the worker threads, with tight
connect/read timeouts (e.g. 300 ms connect, 2.5 s read) and a circuit-breaker
(after K consecutive failures, stop hammering for T seconds and show a status
line). Keep-alive to a local Ollama is essentially free.

---

## 6. Format & context preservation (the "don't translate /car" problem)

Real RP chat/dialog corpus (gathered from live gamemodes — SA-RP, VX-RP, PT/ES
servers) shows the exact shapes we must respect:

```
Kevin says: <message>            Kevin shouts: <message>!
Kevin whispers: <message>        %s says (radio) %s
%s says [in-game]: %s            %s says quietly [Russian Accent]: %s
%s says: (( <OOC message> ))     ** [S: 5] Name: <radio> **
"type /car alpha to spawn"       {C2A2DA}/me toma el arma{FFFFFF}
```

Rules, enforced by **placeholder masking before translation** and a
**round-trip validator after**:

1. **Never translate:** leading/inline `/command` tokens; player names in their
   many shapes (`First_Last`, `First Last`, `Name[ID]`, `Name (ID)`, `(ID) Name`,
   masked `Stranger_123`); numeric IDs; `{RRGGBB}` colour codes (may be glued to
   text: `говорит:{FFFFFF}text`); **GTA tokens** `~r~ ~n~ ~k~~PED_DUCK~` (appear
   even inside 3D labels/gametext); custom-chat emoji tokens (`:u1f603:`,
   `:item%d:`); the wrapper tokens themselves (`(( ))`, `[.. Accent]`, `(radio)`,
   `[S: n]`, `**`); pure numbers, coordinates, plates, money; URLs. Each is
   replaced with an opaque placeholder `⟦1⟧ … ⟦n⟧` and restored verbatim after
   translation.
2. **Translate only the natural-language spans** between placeholders.
3. **Glossary** (in the cached prompt prefix): GTA:SA area/zone names (Grove
   Street, Los Santos, Idlewood…), vehicle names, weapon names, common RP jargon
   (/me, /do, IC, OOC, PD, EMS, mask, frisk) → force canonical target-language
   forms so "Grove Street" never becomes "Улица Рощи". The glossary doubles as
   cache-prefix padding to reach providers' minimum cacheable prefix (§8).
4. **Rolling context:** keep the last ~3–5 translated lines (and, for chat, the
   speaker) as a short context block so pronouns/ellipsis resolve. Small, and
   deliberately *not* part of the cache key (it's advisory).
5. **Validator:** if any placeholder is missing/duplicated in the output, or the
   model added commentary / broke JSON, reject → retry once with a stricter
   prompt → then fall back to the next provider → then show original + a
   "translation failed" marker. Never display a mangled line.
6. **Names:** resolve the speaker's name from the player pool by ID and mask it,
   rather than trusting the model to leave arbitrary names alone.

7. **Length & crash safety** (client limits — translations often grow):
   ClientMessage ≤144 (longer not delivered), chat input ≤128, dialog caption ≤64
   visible, **list item ≤~128 (>130 crashes the client)**, tablist col 128 / row
   256. Cap and, where the surface allows, split long results (RP servers already
   use ZMsg `" ..."`/`"... "` continuation). **Strip/neutralise any `%`** the
   model introduces — a stray `%` in ClientMessage can crash the client.
8. **Skip non-language surfaces:** textdraw **style 4 (sprite `LIB:NAME`)** and
   **5 (model preview)** carry no text — never translate. HUD textdraws
   (speedometers, hunger/thirst bars) update many times/second → cache by a
   **number-masked template** (mask digits before the cache key) so they hit cache
   instead of re-translating every tick.

LLM output is requested as **schema-constrained JSON** (`[{id, text}]`) so
batched units don't bleed into each other and the validator is simple. Reasoning
/ "thinking" is **disabled** on every provider (latency).

Client-generated English strings ("Connected to %s", "Server closed the
connection.", "Audio stream: %s", …) come from samp.dll itself, not the server —
handle these with a small **static translation table**, no LLM/MT call needed.

---

## 7. Async & threading

- `submit(job)` copies the source string(s) + a **stable target handle** (chat:
  a synthesized entry slot; dialog: the dialog id; label/textdraw: pool index +
  a version stamp) and returns instantly. Interceptors never block.
- A small **worker pool** (2–4 threads) does cache/detect/protect/HTTP/validate.
- Completed results are pushed to a lock-free/mutex'd **result queue** and applied
  **only** from `on_frame` (main thread) — the one place it's safe to call
  `AddEntry`, touch `stDialogInfo`, or write pool buffers.
- **Micro-batching:** a 300–500 ms coalescing window groups up to ~8 units into
  one request (chat bursts, a dialog's list rows, several labels). Batching is
  the biggest cost lever (§8) and hides latency.
- **Staleness:** each async result carries the version stamp; if the pool slot /
  dialog id was reused or the player left, drop the result.
- **Ordering (chat):** to preserve readability, either (a) buffer a line briefly
  and emit in order, or (b) emit immediately with the marker and accept mild
  reordering. Default (a) with a small cap (e.g. hold ≤700 ms, else emit
  original then patch) — configurable.

---

## 8. Cost analysis (researched 2026-09-26, live sources)

Workload model (one heavy player, 4 h/day × 30 d = 120 h/mo): ~900 chat lines/h
(70 ch), 20 dialogs/h (400 ch), 60 unique labels/h, 40 unique textdraws/h, 100
outgoing lines/h. After filtering (already-target-lang, commands, numbers) and
cache hits ⇒ **~625 translated units/h ≈ 46 k chars/h ≈ 5.5 M chars/month.**

**Key findings:**

- Small **LLM APIs are 10–40× cheaper than per-character MT APIs** for this
  workload *and* can do context + "don't-translate" rules MT can't.
- The LLM bill is dominated by re-sending the ~800-token system prompt (~72 % of
  input tokens) → **prompt caching + batching** matter more than sticker price.
  Pad the cacheable prefix (with the glossary) to each provider's minimum
  (OpenAI 1024, Gemini 3.x 4096, Claude 4096; DeepSeek auto).

| Tier | Option | ~$/month | Notes |
|---|---|---:|---|
| **Local / free** | **Ollama + Tencent Hy-MT2** (Apache-2.0, purpose-built MT; 1.8B Q8 = 1.9 GB fits a 1060 ~0.12–0.15 s/line GPU; 7B Q4 = 4.6 GB) | **$0** | private, offline, no key; official templates for glossary/"don't touch delimiters"/context. No Romanian. |
| Local / free | Ollama + small general LLM (Qwen3.x-3–9B, Gemma) | $0 | better slang/rolling-context; needs more VRAM |
| Free online | Gemma-4 on Gemini API (free), Mistral Free ($10/mo credits > bill), Cerebras free (1 M tok/day), OpenRouter `:free` | ~$0 | rate-limited; data-use caveats |
| **Cheap online LLM** | **DeepSeek V4.1 Flash** (works from RU), **Gemini 2.5 Flash-Lite** (fastest, 0.3 s TTFT, via OpenRouter), **GPT-6 Luna** effort=none | **~$2.5–6** | with caching+batching; thinking OFF |
| Premium online | Claude Haiku 4.5 | ~$33 | best small-model instruction-following (verify current pricing before relying on it) |
| MT API (fast, weaker context) | Azure Translator S1 ($10/M; F0 2 M/mo free), Tencent TMT (5 M/mo free ≈ covers it) | ~$0–55 | ~0.1–0.3 s; use as fast fallback / static-UI tier |
| MT API (avoid) | Google NMT / DeepL | ~$100–150 | expensive here, no conversation context |

**Recommendation (matches "local or free, keep online capability"):**
default profile = **local Hy-MT2 via Ollama** (free, private, offline, no key,
fits the 1060 floor with the 1.8B build). Ship an **online profile** the user
fills in with a base-URL + model id + key (BYOK) for higher quality / weak-CPU
users — recommend **DeepSeek Flash** (region-proof incl. RU) or **Gemini
Flash-Lite** (lowest latency). Optional **MT fallback** (Azure/Tencent free
tiers) for sub-second static UI. All selectable + chainable in config.

### 8.1 Latency reality
Every LLM path is ~0.6–1.8 s/batch; local Hy-MT2 ~0.15–0.3 s/line; MT APIs
~0.1–0.3 s. This is exactly why the async "show-then-patch / short-hold" model
(§7) is mandatory, and why the **cache** (repeated labels, menus, command help,
recurring phrases) does most of the heavy lifting on a busy server.

---

## 9. Security / privacy / ToS

- **Never embed an API key in the distributed `.asi`.** Keys are **BYOK** from a
  local, git-ignored config the user fills in. Default (local Ollama) needs no
  key and sends nothing off-machine.
- **Regions:** OpenAI/Gemini/Anthropic are blocked in RU/CN; DeepSeek & Yandex
  work from RU. The provider layer is swappable per user → no hardcoded provider.
- **Other players' chat is third-party personal content.** Provide an
  "incoming translation off / local-only" switch; the local model keeps
  everything on-device.
- **Unofficial endpoints** (scraped `translate.googleapis.com` gtx, Lingva)
  violate ToS and are being actively blocked — **not** used.
- Model ids + base URLs live in **config, not code** (providers churn fast).
- This is a client-side reading aid; it does not send anything to the game
  server. (Anti-cheat footprint is unchanged from the existing trainer; the
  layer adds only local hooks + outbound HTTP to the chosen translator.)
- **Exclude the chat-triggered kill-switch.** Project Fuego's chat handler
  (`cheat_samp.cpp:1392,1452`) has a deliberate client kill-switch (infinite
  `delete g_SAMP`) that fires when an incoming message contains magic phrases
  (`kevysaidso`/`obeykevy`/…). Since the translator sits directly in the incoming
  chat path, this must be a conscious decision — **recommend NOT porting it**
  (it's a remote-crash backdoor; anyone who says the phrase crashes the user).
  It is already absent from fuego-reversed; keep it that way.

---

## 10. UX

- **Inbound chat:** replace the line with the translation + a subtle marker (a
  small leading glyph / faint colour tint) so it reads like normal chat but is
  identifiably translated. A hotkey (configurable) toggles "show originals"
  (shows the cached source line beneath, dimmed) globally or for the last N.
- **Dialogs/labels/textdraws:** translated in place; a tiny marker in the dialog
  title bar.
- **Outbound:** **Ctrl+Shift toggles** "translate my messages" mode; while on, a
  small on-screen indicator shows it's active and the target (server) language;
  each chat line you send is translated first. Commands (`/…`) always pass
  through untranslated. (Must respect the chat-input guard — see §12.)
- **Config** (menu section + `[i18n]` in the cfg): enable inbound/outbound,
  target language, source mode (auto / pinned + which), per-server source
  codepage, provider profile + fallback chain, endpoint/model/key, marker style,
  hold-time, per-surface toggles (chat/dialogs/labels/textdraws), and a "local
  only / privacy" master switch.
- **Status/health:** reuse the existing `status::` overlay for "translator:
  local Hy-MT2 ✓ / online ✓ / offline — retrying", and a one-time chat notice if
  the provider is unreachable.

---

## 11. Phased rollout

- **Phase 0 — engine skeleton:** `i18n::engine` (submit/worker/result-queue/pump),
  `OllamaProvider` + `OpenAICompatProvider`, cache, protect/glossary, detect,
  charset, config. Prove end-to-end on **chat only** (AddEntry hook) with a local
  Ollama + Hy-MT2. Marker display. This is the MVP and the demo.
- **Phase 1 — outbound chat:** Ctrl+Shift mode on the say path (command-safe),
  indicator.
- **Phase 2 — dialogs:** RPC 61 intercept + show-then-patch `stDialogInfo`
  (title/body/buttons/list rows).
- **Phase 3 — 3D labels + textdraws + gametext:** RPC 36 / 134 / gametext, pool
  buffer rewrite + cache (biggest cache-hit win).
- **Phase 4 — hardening:** MT fallback provider, circuit breaker, per-server
  codepage auto-guess, "show originals" toggle, config UI polish.
- **Phase 5 (stretch):** custom-chat-`.asi` servers via wire-level chat
  interception; per-language glossaries; optional on-disk TM sharing.

---

## 12. Interaction with existing trainer bugs (must-fix prerequisites)

The menu/cheat audit found the trainer reads hotkeys with raw
`GetAsyncKeyState` **without** guarding on `stInputInfo::iInputEnabled` or window
focus (audit A §0.1). That means **typing in chat currently triggers cheats**
(BACKSPACE = fake-lag backtrack, I = iTroll, WASD = fly, etc.). The translator's
**outbound typing and Ctrl+Shift toggle will collide with this** unless fixed.
→ **Prerequisite:** a shared key-input helper gated on `iInputEnabled` +
`GetForegroundWindow()`; the translator and the existing cheats both use it. (See
the porting-gap list; this one is required for the translator, not just fidelity.)

---

## 13. Decisions (resolved 2026-09-26)

1. **Target language** = **English** default, configurable to any model-supported
   language. Initial source glossary/detection priority: **RU, ID, ES, PT-BR, TR**
   (then VI, PL, RO, UK, AR).
2. **Marker** = **subtle leading glyph + faint colour tint**.
3. **Local model** = **Tencent Hy-MT2**, GPU-sized (7B Q4 where it fits, 1.8B on
   the 1060 floor); provider layer pluggable for general-LLM / online swaps.
4. **On-disk TM** = **flat append-only file + in-memory map** for v1 (SQLite later
   if it grows).
5. **Chat ordering / timing** = **show original instantly, swap in place** when the
   translation lands (not hold-in-order).

---

## 14. Testing

- **Offline unit tests** (host build, no game): protect/unprotect round-trips on
  the real corpus shapes (§6); charset decode/encode for 1251/1254/1250/1252;
  language detection precision/recall; JSON/validator on adversarial model
  output; cache key stability.
- **Provider contract tests:** a mock HTTP server returning canned/broken
  responses → exercises retry, fallback chain, circuit breaker, timeouts.
- **Golden translations:** a fixed set of RP lines → snapshot expected
  masking + a human spot-check per provider.
- **In-game smoke:** local Ollama, join an RU/ID test server (or replay captured
  RPCs), verify chat/dialog/label/textdraw translate, markers show, no stalls,
  no crash on disconnect/relogin (staleness handling).
- **Perf:** frame-time budget in `on_frame` for the result drain (cap work per
  frame); worker pool never blocks the main thread.

---

## Appendix A — provider/cost sources
Full researched tables (MT vs LLM pricing, latency, region/ToS, WMT quality
evidence, and local-model benchmarks incl. Hy-MT2 sizing) are in the session
research notes (`research_F_hosted.md`, `research_H_local.md`). Prices are
2026-09-26; keep model ids/prices in config as they change frequently.
