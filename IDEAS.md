# Fuego Reversed — Ideas & Suggestions

Running list of improvements/ideas found while building. Nothing here is done —
it's a menu of things we *could* do, for you to pick from.

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
