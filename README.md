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
  render/   D3D9 EndScene/Reset vtable hook (game device)       [done]
  menu/     in-house Dear ImGui overlay (F2)                    [done]
  cheats/   net-seam + per-frame cheat driver + visuals         [done]
  commands/ /fuego_cmds custom commands                         [done]
  gta/      bridge to the gta-reversed game layer               [done]
  i18n/     in-game translation layer                           [planned]
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
- [x] **In-house menu** — Dear ImGui DX9 (compiled in, no external dll), F2 to toggle.
- [x] **Cheats** — the net-seam packet-rewrite cheats **and** the GTA-coupled ones
  (silent aim, ESP, chams, car trolls, fly/air-brake, carcontrol, gethere/njkill,
  gun-stealer) — faithful ports bound to the menu.
- [x] **Commands** — the full `/fuego_cmds` set (see `src/commands/commands.cpp`).
- [x] **SA:MP pools** — player/vehicle pool structs + accessors (offset-pinned).
- [x] **gta-reversed game layer — LINKED.** Built from the `external/gta-reversed`
  submodule by `scripts/bootstrap.ps1` into `gta_reversed.lib` and linked in; the
  bridge (`src/gta/gtahost.cpp`) drives the real game through it.

**Working in-game** (loads, hooks SA:MP, chat, menu, compat, cheats). Builds green.

## gta-reversed integration

The game layer is **gta-reversed** (the full GTA:SA reimplementation), pinned as the
`external/gta-reversed` **git submodule** — accurate `CPed`/`CVehicle`/`CWorld`/pools
with correct offsets. `scripts/bootstrap.ps1` builds it **unmodified** (its own
conan + cmake) and archives its object files — minus the process entry points
(`dllmain`/`WinMain`/`WinPlatform`) — into `external/generated/gta_reversed.lib`, then
generates `external/generated/gta_deps.props` (the exact include roots, defines,
language mode and link inputs upstream used, plus the single imgui Conan package the
menu also uses). `deps.props` imports that into the build.

We **coexist** with the running game: `gtahost.cpp` uses gta-reversed's headers for
accurate layouts + inline accessors and *calls the real game's functions at their
addresses*; we never install gta-reversed's own hooks (`InjectHooksMain` is never
called), so the real GTA:SA keeps running underneath. Because of that, gta-reversed's
ongoing reversing work has ~zero behavioural impact here — only a struct/offset rename
could matter, and the single bridge TU compiles clean against current upstream.

## Build

Everything this depends on is **inside the repo** — no sibling checkouts, no
hand-installed SDKs.

1. **Clone with submodules** (the game layer + the packet-list wiki are pinned
   submodules): `git clone --recurse-submodules …`, or in an existing clone
   `git submodule update --init --recursive`.
2. **Run the bootstrap once:** `scripts\bootstrap.cmd` (double-click or from a
   terminal). It finds Visual Studio 2026, CMake (uses the one bundled with VS if
   none is on PATH) and Conan (pip-installs it into a project-local cache if you
   don't have it), then builds the `external/gta-reversed` submodule and produces
   the game library + the generated `deps` props. Re-run it (or
   `scripts\bootstrap.cmd -Clean`) only when the gta-reversed submodule moves.
3. **Build the ASI:** the bootstrap builds it at the end. For later incremental
   builds after editing source, run **`scripts\build.cmd`** (one command; it finds
   MSBuild itself and rebuilds `bin/FuegoReversed.asi` — no dependency re-check).

**No IDE required.** The `.sln`/`.vcxproj` are the build definition, but the whole
project builds headless from the command line via `scripts\build.cmd` (or
`scripts\bootstrap.cmd` for a from-scratch build) — Visual Studio's IDE is optional.
If you *do* want the IDE, opening `FuegoReversed.sln` and building **Release / Win32**
works too. (The project builds via MSBuild directly; CMake is used only to build the
gta-reversed submodule during bootstrap.)

Toolset **v145**, C++20 (the one gta-reversed bridge TU is C++23, scoped to
itself). Drop the `.asi` in the GTA:SA folder via your ASI loader; progress is
logged to `fuego_reversed.log`.

Prerequisites the bootstrap checks for: **Visual Studio 2026** with the *Desktop
development with C++* workload (x86 toolset), **Python 3** (only if Conan isn't
already installed), and **git**. First run downloads/builds the Conan packages and
compiles gta-reversed, so it takes a while; later runs are incremental.
