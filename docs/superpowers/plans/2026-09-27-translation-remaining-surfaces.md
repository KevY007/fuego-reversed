# Translation Layer — Remaining Surfaces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development or
> executing-plans. This continues the shipped chat translator (inbound + outbound). Steps use
> checkbox (`- [ ]`) syntax.

**Goal:** Extend the in-game translation layer beyond chat to the remaining server-sent text
surfaces: **dialogs, 3D text labels, textdraws, gametext** (and material text), plus the
**list-dialog response reverse-map**.

**Status of the layer as of 2026-09-27 (what already ships on `master`):**
- Inbound chat — **working, verified in the model loop.** (`CChat::AddEntry` detour → async engine → pump.)
- Outbound chat (Ctrl+Shift) — **built, direction verified, default OFF.** (net send seam on `RPC_Chat`.)
- RakNet Huffman `StringCompressor` codec — **built, round-trip verified (40/0)**, NOT yet wire-verified
  vs a captured SA:MP packet, NOT yet compiled into the ASI. (`src/i18n/huffman.{h,cpp}`.)
- Engine supports per-job direction overrides + surfaces (`Job.surface`, `promptOverride`, etc.).

**Spec:** `docs/superpowers/specs/2026-09-26-translation-layer-design.md`
**Authoritative packet formats:** `docs/reference/samp-packet-list.wiki/RPC-List.md` (submodule).

---

## Global Constraints
- Default OFF for every new surface (per-surface toggle in `i18n::config::Settings`), like `inbound_chat`.
- Never stall the net/game thread on a network translation. Never introduce `%` into game text
  (format-string crash). Respect SA:MP buffer sizes exactly. Bounds-check every BitStream read;
  on ANY anomaly, pass the original packet through untouched.
- Reuse the existing async engine (`i18n::engine`) with `Job.surface` routing in `hooks::apply_result`.
- Two decode regimes (see spec): chat/dialog/label = ANSI codepage (cp1251/cp1252); textdraw/gametext
  = GTA bitmap-glyph slots 128-175 with per-server FixChars/russifier tables.

## The two hard blockers (must be resolved before dialogs/labels can work well)

1. **Async vs. one-shot rendering.** Chat works because the original shows immediately and the
   translation is *appended* as a later line. Dialogs, labels, textdraws and gametext are
   *one-shot / modal / positional* — there is nowhere to append. Options, in order of preference:
   - **(A) Translation-memory-first (no re-show needed).** On first sight, show original + submit a
     translation into the TM cache (`i18n::cache`). Every subsequent identical string renders
     translated **synchronously from cache** with zero delay. This needs NO new SA:MP function
     address and cannot stall (cache hit is instant; miss shows original). **Recommended first step.**
   - **(B) Swallow + re-show.** Swallow the incoming RPC, async-translate, then re-invoke SA:MP's own
     show routine with the translated text. Needs the SA:MP **client function addresses** for
     showing a dialog / creating a label / setting a textdraw string — we currently have **only**
     `FUNC_ADD_TO_CHAT_WND = samp.dll+0x67650`. These addresses must be reversed for 0.3.DL and added
     to `src/samp/addresses.h` before (B) is possible.
   - **(C) Synchronous translate.** Unacceptable — stalls the net thread for the provider round-trip.

2. **Byte-exact re-encode of CSTRING bodies.** Dialog `szInfo` and 3D-label `text` are
   `CSTRING` = RakNet `StringCompressor` (Huffman). Re-emitting a translated body means Huffman
   **encode** + the StringCompressor **framing** (a `WriteCompressed`-prefixed length, then the
   bits). If the re-encode is off by a bit, **the game crashes parsing our packet**. Our
   `huffman.encode/decode` is verified round-trip but the framing is not implemented and none of it
   is wire-verified. **This must be validated against captured packets** (Task 0) before shipping any
   CSTRING re-encode.

---

## Task 0: Capture + verify the wire codec (prerequisite, needs the user in-game)
- [ ] Add a debug build flag that, in the incoming-RPC hook, logs the raw bytes of RPC 61 (ShowDialog)
      and RPC 36 (Create3DTextLabel) to `fuego_reversed.log` (hex).
- [ ] User joins a server that sends a dialog with a known body; capture the bytes.
- [ ] Offline: confirm `huffman.decode` (with StringCompressor framing) reproduces the known body
      from the captured `szInfo` bytes, and that re-`encode` reproduces the captured bytes **exactly**.
- [ ] Only once encode is byte-identical to captures: proceed with CSTRING re-encode surfaces.
- Verify: decode(capture) == expected text; encode(text) == original capture bytes.

## Task 1: StringCompressor framing on top of huffman
**Files:** `src/i18n/huffman.h/.cpp` (add `encode_string`/`decode_string`), or a new `stringcompressor.{h,cpp}`.
- [ ] Implement `bool decode_string(net::BitStream& in, std::string& out, size_t maxBytes)` matching
      RakNet `StringCompressor::DecodeString`: read the `WriteCompressed`-encoded bit/byte length,
      then `huffman.decode` that many characters. Check `src/net/BitStream.h` for `ReadCompressed`/
      `ReadBits`; RakNet's `WriteCompressed(unsigned short)` writes the value in nibble-runs — replicate
      exactly if our BitStream lacks it.
- [ ] Implement `void encode_string(net::BitStream& out, const std::string& s)` — inverse.
- [ ] Host test: round-trip through a `net::BitStream`; and (Task 0) match a captured packet.

## Task 2: Dialogs (RPC 61 ShowDialog) — TM-first (approach A)
**Format:** `INT16 wDialogID, UINT8 bStyle, UINT8 titleLen, char[] title, UINT8 b1Len, char[] btn1,
UINT8 b2Len, char[] btn2, CSTRING szInfo[4096]`.
**Files:** new `src/i18n/surfaces.cpp` (or extend `hooks`), register on the incoming RPC-dispatch seam
(same place compat's `HandleRPCPacketFunc` runs; add an i18n hook there or a dedicated CDetour).
- [ ] Parse the RPC (bounds-checked). Title/buttons are plain (cp1251/1252). Decode `szInfo` via Task 1.
- [ ] For each string: `cache::get` → if hit, use translated; else submit an engine job (surface=dialog)
      and use the ORIGINAL this time (TM-first: next identical dialog is instant).
- [ ] Re-encode the RPC with translated title/buttons (plain) and, **only after Task 0 passes**,
      translated `szInfo` (CSTRING). Until then, translate title+buttons only and pass `szInfo` through.
- [ ] Never exceed the original field caps; if a translation is longer than a `UINT8` length allows
      (255) or the dialog cap, truncate on a UTF-8/codepage boundary.
- Verify (in-game, user): a cached dialog shows translated; buttons translated; no crash.

## Task 3: List-dialog response reverse-map (RPC 62 DialogResponse)
**Format (outgoing):** `INT16 wDialogID, UINT8 bResponse, INT16 wListItem, UINT8 textLen, char[] Text`.
- [ ] When we translate a LIST/TABLIST dialog's items, keep a per-dialog map: translated row text →
      original row text (and row index). On the outgoing RPC 62, replace `Text` with the ORIGINAL row
      text so the server sees what it sent. `wListItem` index is unchanged, so index-based servers are
      already fine; only text-matching servers need this.
- Verify: selecting a translated row sends the original row text.

## Task 4: 3D Text Labels (RPC 36 Create3DTextLabel)
**Format:** `... CSTRING text[4096]` (Huffman). Struct: `stTextLabel.pText`.
- [ ] Same TM-first pattern as dialogs; `text` is CSTRING → gated on Task 0/1.
- [ ] Labels are created once and persist; TM-first means a label translates on the *next* create, or
      (approach B, if a label-create function address is reversed) re-create with translated text.

## Task 5: Textdraws (RPC 134 ShowTextDraw, 105 TextDrawSetString) + gametext (RPC 73)
**Formats:** textdraw text is **plain** (`UINT16 len, char[]`); gametext is plain (`UINT32 len, char[]`).
No Huffman. BUT these use the **GTA bitmap-glyph charset** (slots 128-175) + per-server FixChars tables,
not a straight ANSI codepage — see spec §"two decode regimes". Struct: `stTextdraw.szText[801]`.
- [ ] Build the glyph-slot ↔ Unicode mapping (default table + a hook to load a per-server override).
- [ ] Decode textdraw/gametext bytes via the glyph table → UTF-8 → translate → re-encode to glyph slots.
- [ ] Re-encode plain (len-prefixed), respecting `szText` 800-byte cap.
- Verify (in-game): a known textdraw/gametext line translates and renders (no missing glyphs).

## Task 6: Menu + config
- [ ] Add per-surface toggles to `i18n::config::Settings` (dialogs/labels/textdraws/gametext),
      persisted in the unified config, surfaced in the Translator tab. All default OFF.
- [ ] Extend `hooks::apply_config` / the RPC-seam install to honor them at runtime.

## Review Focus
- A malformed/short packet must pass through untouched (never crash, never desync).
- A translation longer than the field cap must truncate on a codepage boundary, never overflow `szText`
  / a `UINT8` length.
- CSTRING re-encode must be byte-identical to captures BEFORE it ships (Task 0 gates Tasks 2/4 bodies).
- List reverse-map must send the ORIGINAL row text (Task 3).
- Nothing here may stall the net/game thread (TM-first is synchronous only on cache HIT).

## Why this is a plan, not shipped code (2026-09-27)
Dialogs/labels/textdraws all require **byte-exact re-encoding of received packets that the game then
parses** (crash-on-error) and/or **SA:MP client function addresses we do not yet have** (for a
non-stalling re-show), and cannot be verified without in-game captures. The chat surfaces shipped
because chat can show original-then-append and needs no re-encode. Shipping unverified packet
re-encoders would risk crashing the game the moment a dialog/label arrives — contrary to this
project's faithful-port, stability-first ethos. Task 0 (a capture pass with the user in-game) unblocks
the rest safely.
