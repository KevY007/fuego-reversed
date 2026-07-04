# Fuego Reversed

A modern-C++ rebuild of **Project Fuego** for **SA:MP 0.3.DL**, on gta-reversed
conventions. Ships as a self-contained **ASI** (late load — proven safe on modern
toolsets, unlike the old d3d9-proxy which crashed under v141), with **no external
DLLs** (AntTweakBar is gone).

## Goals (from the original, prioritised)

1. **Networking (RakNet / RPC) stays wire-accurate** — ported faithfully, not rewritten.
2. **The cheats the user actually uses** — the old AntTweakBar (F2) cheats + the
   custom `/fuego_cmds` commands — migrated accurately.
3. **In-house lightweight menu** replaces AntTweakBar (no shipped .dll).
4. **0.3.7 compatibility layer** carried over.
5. Native trainer / F11 menu — nice-to-have, not required 1:1.

## Architecture

```
src/
  core/     main.cpp (ASI entry + init), memory.h (typed calls), settings, logger
  samp/     samp.dll detection, addresses, structs, sync structs, pools, chat
  net/      RakNet/BitStream/RakClient + RPC hooks (accurate)   [done]
  compat/   0.3.7 compatibility layer + auto-detect             [done]
  hook/     ADE32 length-disassembler + CDetour                 [done]
  render/   D3D9 EndScene/Reset vtable hook (dummy-device)      [done]
  menu/     in-house Dear ImGui overlay (INSERT)                [done]
  cheats/   net-seam + per-frame cheat driver                   [partial]
  commands/ /fuego_cmds custom commands                         [partial]
```

**Modern conventions:** every game/SA:MP call goes through typed `mem::thiscall<>` /
`mem::ccall<>` helpers (`core/memory.h`) — no inline asm, no blind casts. All SA:MP
offsets live in `samp/addresses.h`, verified against Project Fuego (proven on v140)
and the samp-packet-list wiki.

## Status

- [x] **Foundation** — skeleton, typed-call core (`mem::`), logger, trace, SA:MP
  detection (0.3.DL sig at `samp.dll+0xBABE`), chat, 250ms load-in timing.
- [x] **Networking / RakNet core** — `BitStream` (byte-identical), RakClient,
  `HookedRakClientInterface` swap at `CNetGame+0x2C`, hook-dispatch seam. In-game verified.
- [x] **Exit-crash fix** — `~CNetGame` call sites hooked (CDetour) to restore the real
  interface before shutdown → clean quit (the no-op-`delete` approach didn't work).
- [x] **Hook infra** — ADE32 length-disassembler + CDetour.
- [x] **0.3.7 compat layer** — skin-RPC rewrites, RPC-dispatch hook, ClientJoin spoof;
  **auto-detects** a 0.3.7 reject (RPC 130 reason 1) → enables compat + prompts reconnect.
- [x] **D3D9 render hook** — EndScene(42)/Reset(16) via a dummy-device vtable grab.
- [x] **In-house menu** — Dear ImGui DX9 (compiled in, no external dll), INSERT to toggle.
- [x] **Cheats (net-seam) — 5 live** — invisibility, fake-lag, moonwalk, upside-down,
  shot-repeater (faithful packet-rewrite ports, bound to the menu).
- [x] **Commands** — `/compat`, `/fuego_cmds`, `/shotrepeater`; `cmd::add` framework.
- [x] **SA:MP pools** — player/vehicle pool structs + accessors (offset-pinned).
- [~] **gta-reversed game layer** — *proven to compile in* (vendored json/spdlog/Tracy-stub,
  C++23, `gta_reversed.props`); bridge in `src/gta/`. Staged out of the link until
  gta-reversed's `game_sa` is built as a static lib (see below).
- [ ] GTA-coupled cheats (silent aim, ESP, cars) + `/fuego_cmds` trolls — need the game layer linked.

**Working in-game** (loads, hooks SA:MP, chat, menu, compat, the 5 cheats). Builds green.

## gta-reversed integration (in progress)

The plan is to use **gta-reversed** (`../gta-reversed`, the full GTA:SA reimplementation)
as the game layer — accurate `CPed`/`CVehicle`/`CWorld`/pools with correct offsets. Its
headers already compile into this project (`src/gta/gtahost.cpp`, the single TU that pulls
gta-reversed's `StdInc.h`; built at C++23 with the deps in `vendor/gta-deps/`). Using the
classes needs gta-reversed's `game_sa` `.cpp` **linked** (the class tree emits out-of-line
vtable/ctor symbols). Next step: build `game_sa` as a static lib (its own conan+cmake),
link it here **minus** the entry points and **without** calling `InjectHooksMain` (we
coexist with the running game, we don't replace it), then flip `gtahost.cpp` back into the
build and port the GTA-coupled cheats on gta-reversed's API.

## Build

Open `FuegoReversed.sln`, build **Release / Win32** → `bin/FuegoReversed.asi`.
Toolset **v145**, C++20. Drop the `.asi` in the GTA:SA folder via your ASI loader;
progress is logged to `fuego_reversed.log`.
