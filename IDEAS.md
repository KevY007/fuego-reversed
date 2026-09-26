# Fuego Reversed — Ideas & Suggestions

Running list of improvements/ideas found while building. Nothing here is done —
it's a menu of things we *could* do, for you to pick from.

## Fidelity port pass — 2026-09-26 (status)

A four-way audit of Project Fuego vs fuego-reversed drove a fidelity pass. **Done**
this session: hotkeys routed through a Fuego-style keyhook (`src/core/input.*`) so
they no longer fire while typing in chat / alt-tabbed; F12 cheat-panic + proper
panic transitions (clears health-hack flags, closes/restores the menu, RPC handlers
respect panic); `no_big_hacks` default ON + it force-resets the big toggles;
health-hack now sets Fuego's exact `4|8|64|128` ped flags (no HP raise, tyre
protection, trailer chain) so nothing extra leaks past the ClientCheck strip;
No-Reload = RELOADING→READY (not infinite ammo); `damage_vehicle_doors` fixed to
Fuego's wire format (4× u32 + 3× unoccupied restream + 3× RPC); `/.takedmgrpc`
takes `[id] [amount] [weapon]`; near-slapper skips dead/downed players; trigger-bot
skips fists; extra-godmode only guards our own vehicle; DisableFlyingBirds always on;
RPC 58 enum corrected to Delete3DTextLabel.

**Still to port** (lower severity; several need one more gta-reversed accessor):
- **iTroll target** should be the driver of the nearest *occupied* vehicle
  (Fuego `cheat_handle_itroll_targetfinder`), not the nearest player of any state;
  add the read-only "iTroll Target" name in the menu.
- **Attach/Tow** vehicle is a stub — wire it to `CVehicle::SetTowLink` (click-pick,
  like Fuego).
- **`1`-key car repair** (driver of own car: +50 hp/press, full Fix + cap at 945).
- **Unlock** should use Fuego's door-state dance (`LockDoors(false)` +
  `SetScriptLocked(false)` + `SetDoorsUndamageable(false)`), not just `m_nDoorLock`.
- Silent-aim visuals + centre, nametag/ESP look, fake-lag marker, etc. — being
  brought to Fuego's exact look in `cheats/visuals.cpp` (in progress).
- mod_sa **base** ini patches (sprint-any-surface, keep-HUD, no-connect-delay) are
  deliberately out of scope (base, not Fuego-custom) unless you want them as toggles.

**Net layer verdict: byte-accurate, zero wire bugs.** Un-ported *features* only:
the `netPatch` RPC/packet id-block table (a menu feature to drop arbitrary ids),
incoming-chat anti-spam + chatlog-to-file, and the `SetupSAMPHook` memcmp safety
pre-check. Low priority. The two backdoors (C2 socket, chat kill-switch) stay out.

Full audit notes: session scratchpad `audit_{A,B,C,D,E}_*.md`.

## Build / infrastructure
- **Eliminate the `/FORCE:MULTIPLE` + `LNK4088` warning.** The 15 duplicate
  `notsa::PoolOf<>` / `Compress*` template symbols come from `gtahost.obj` re-emitting
  what the gta-reversed lib's PCH already defines. Could isolate the gta-reversed
  usage so those aren't re-instantiated, giving a clean link with no "image may not
  run" caution.
- **Cache the 465 MB gta-reversed static lib.** It only needs rebuilding when
  gta-reversed changes. A prebuilt `gta_reversed.lib` checked in (or a build-once
  script) saves the ~minutes-long non-unity compile.
- **Crisp bigger font.** Right now the menu uses `FontGlobalScale = 1.35` which
  upscales the default bitmap font (slightly blurry). Embedding a TTF (e.g. a clean
  UI font) at the target size would be sharp.

## Menu / UX
- **Config persistence.** Save/load cheat toggles + settings to a file so they
  survive a restart (Fuego had ini-based settings).
- **Rebindable menu key + per-cheat hotkeys.** F2 is hardcoded; a keybind section
  in the menu would be nice.
- **Search box / favourites** in the menu once the cheat/command count grows.

## Cheats (now that gta-reversed is integrated)
- **Bone-accurate ESP + aimbot.** gta-reversed exposes the ped skeleton, so ESP
  boxes/health bars and aim can target specific bones (head/spine) precisely.
- **Aim prediction** using the target's synced velocity (lead moving targets).
- **Local visual cheats** that were hard before are now easy via gta-reversed:
  no-fall-damage, ped/vehicle god mode, super-jump, etc.

## Compat / networking
- **Broaden the 0.3.7 compat rewrites.** Currently skin RPCs; could add any other
  0.3.7↔0.3.DL wire differences as they surface (weapon/vehicle RPCs, etc.).

## Cheats still to port (need special mechanisms)
- **The "affect another player" trolls** — gethere (drag a player to you), itroll (interior warp them),
  fuckup / freezetroll. These inject a *sync packet on the target's behalf* (e.g. a fake in-car sync
  putting their vehicle at your position), which is fiddlier than the damage RPCs already done. Fuego's
  `cheat_samp.cpp` has the exact packet tricks.
- **no_spread deeper** — currently zeroes the bullet's centre offset (server-side accuracy). A fuller
  version would also patch the local `CWeaponInfo` accuracy so the on-screen shots are pinpoint too.
- **fake_headshot / anti-aim bypass RNG** — Fuego randomises the body-part + drops some GiveDamage sends
  per weapon to look legit to anti-cheat. Easy to add to `rpc::give_damage`.

## Findings from this build
- **`LNK4088` "image may not run"** is unavoidable while `/FORCE:MULTIPLE` is used. Root cause: a handful
  of gta-reversed helper functions (`CompressVector`, `notsa::PoolOf<T>`) are defined *non-inline in
  headers*, so both `gtahost.obj` and the lib's PCH emit strong copies. Real fixes: (a) mark those inline
  upstream, or (b) compile `gtahost.cpp` against gta-reversed's own PCH. Cosmetic only — the image runs.
- **Fly on foot** overrides the ped position each frame; if it feels "slidey" near geometry we can add a
  small collision-ignore flag while flying.

(Will append as more come up.)
