# recomp-ui + PSX prototype — HANDOFF (2026-07-16)

> **STATUS UPDATE (later, 2026-07-16).** This is a point-in-time PSX-era
> snapshot; several details below are now stale. Since it was written:
> - The source tree was **restructured** into `src/common/` (core) +
>   `src/consoles/<id>/` units + `assets/common|consoles/<id>/`. `SystemProfile`
>   rows now live in `src/consoles/<id>/<id>_profile.h`, aggregated by
>   `src/launcher_system.h`. `docs/ARCHITECTURE.md` reflects the current shapes
>   (incl. `PadModeDef`, `ControllerSpec.modes/has_pad_binds`,
>   `VideoSpec.widescreen_cells`, `RomFilterSpec`).
> - Console units now exist for **snes, psx, gba, genesis**. The PSX bind page
>   renders the real 24-input DualShock vocabulary and the disc-verdict panel
>   renders (serial/region/ISO) — the two PSX "NOT FINALIZED" items about those
>   are addressed; treat the rest of that section as historical until re-checked.
> - **Genesis** adds three new capabilities used by the shared UI: a custom
>   pad-mode list (`ControllerSpec.modes`, 3-Button/6-Button, mode-driven rebind
>   row count), a per-row **GAMEPAD** bind column (`has_pad_binds`, persisted via
>   `src/consoles/genesis/genesis_binds.c` → settings.ini `[input.pN]`), and a
>   widescreen "extra cells / side" stepper (`VideoSpec.widescreen_cells`).
> Read `docs/ARCHITECTURE.md` for the authoritative, current contract.

Context for continuing this work in a fresh session. Read `docs/ARCHITECTURE.md`
first — it is the design contract for the panel/module system.

## Big picture
`recomp-ui` (F:\Projects\recomp-ui, own git repo, branch `master`, **no remote**)
is a shared, console-agnostic Dear ImGui pre-boot launcher, extracted from the
SNES `launcher_ng`. Goal: ONE launcher every recomp ecosystem consumes as a
submodule; per-console differences are DATA (a `SystemProfile`), not forks.
Proven end-to-end on PSX (ApeEscapeRecomp). Extends the SNES work in the memory
note [[legacy-launcher-replacement]] and [[recomp-ui-shared-submodule]].

## ⚠️ NOT FINALIZED — do not treat as settled
- **PSX theme AND PSX layout are BOTH unapproved** — they are my working draft.
  Only **SNES theme + SNES layout are user-finalized**. Get the user's sign-off
  on the PlayStation look (blue accent, scanlines off) and the PSX panel
  arrangement before treating them as done.
- **Parity Phase 3 is NOT built**: the PSX **memory-card panel** (dual slots,
  Browse/New, 15-block usage grid) and the **disc-verdict** system (serial +
  region + ISO-header + CRC → ok/warn/bad/none icons + 3-check row). The legacy
  PSX launcher has these; recomp-ui does not yet.
- **PSX hotkeys are wrong**: the Settings HOTKEYS panel currently shows the SNES
  [KeyMap] set on PSX too. Per the design it should be a universal catalog with a
  per-system opt-in `hotkeys_mask` (the field exists; both profiles set
  `LNG_HOTKEYS_ALL` today so nothing filters). PSX needs its own hotkey set.
- **PSX controller bind page** uses the SNES 12-button set, not PSX buttons
  (needs `ControllerSpec.buttons` = L1/L2/R1/R2/L3/R3/…).
- **Disc verify**: PSX shows "ROM not recognized" for a valid disc
  (has_expected_crc=0); needs the disc-verdict probe.
- **Rebind not bridged**: the launcher writes recomp-ui's own keybinds.ini, not
  PSX's `psx_keybinds` — pluggable binds are future.

## What IS done (recomp-ui, all committed on `master`, NOT pushed)
HEAD `0371a6e`. Commit order (newest first): panel/module refactor `0371a6e`;
ARCHITECTURE.md `699d7da`; variant profiles + Phase-2 settings `3a4943d`;
deeper PSX settings `7fbf0eb`; pad-mode + aspect `0af8918`; namespaced helpers
`b959f76`; short-window scroll fix `3b91847`; per-console pad `cb32547`;
… (earlier: theme system, extraction).
1. **Console-agnostic core + generic C ABI** `recomp_launcher_run_window(...)` in
   `src/recomp_launcher.h` (creates its own SDL2/GL window; returns 0 LAUNCH /
   1 QUIT / 2 UNAVAILABLE). Self-contained: bundles vendored ImGui + crc32/
   sha256/keybinds (symbols namespaced `recompui_*` so they never collide with a
   host — PSX has its own crc32). `recomp_ui.cmake` → `recomp_target_launcher_ui(
   <target> [BOXART <tga>] [PAD <tga>])`, defines `RECOMP_LAUNCHER`, stages assets.
2. **Themes** (`launcher_theme.h`): `launcher_theme_default()` = CRT-console
   (violet, scanlines ON) — the finalized SNES look; `launcher_theme_psx()` =
   PlayStation blue, scanlines OFF (DRAFT); `launcher_theme_by_name("psx")`.
   Subtitle is data-driven (`GameInfo.platform`); scanlines gated on
   `theme.scanlines`.
3. **Variant profiles** (`launcher_profile.h` + new `launcher_system.h`): one row
   per console bundling theme+platform+rom_noun+capabilities so nothing drifts.
   `launcher_profile_apply("psx", &gi)` sets the PS defaults; the host overrides
   per-game. `SystemProfile` (launcher_system.h) has ControllerSpec/SaveSpec/
   VideoSpec/VerifySpec + `panels_*` composition arrays; `snes`/`psx` fully
   authored, others stubbed. `launcher_system_infer(gi)` picks the active profile.
4. **PSX feature parity so far**: pad-mode selector (Hybrid/Analog/D-Pad, per
   player, gated by lock_mode/allow_hybrid/lock_device) with **real DualShock art
   that swaps** (pad_analog.tga / pad_digital.tga); **aspect dropdown**
   (4:3/16:9/21:9 gated per-game by `aspect_mask`); full **deeper settings**
   (window size, renderer, supersampling, AA, texture filter, screen model,
   frame interp + target fps, SPU-HQ, skip-FMV, turbo, fullscreen, language,
   BIOS, deadzone) — all capability-gated; `rom_noun` ("Disc"/"ROM"/…).
5. **Panel/module architecture** (`0371a6e`, behavior byte-identical — verified by
   diffing a pre-refactor worktree across 8 configs): `launcher_panels.h`
   registry (`LauncherPanel{id,view,slot,available,draw}`); renderer carved into
   `panel_*_draw` adapters + a registry; views compose from the profile's
   `panels_*` lists. NOTE: SAVES still folds into the GAME card (matches today's
   SNES pixel layout); a standalone `panel_save` (SAVE_SRAM/SAVE_MEMCARD) is
   registered but NOT yet in any profile's dashboard — it's the seam for the
   Phase-3 memcard card.
6. **Real PSX assets already converted** into `assets/img/`: pad_analog.tga,
   pad_digital.tga, disc.tga, memcard.tga, verdict_ok/warn/bad/none.tga,
   check_on/off.tga, caret.tga (from `psxrecomp/runtime/launcher/assets/img`,
   32-bit alpha TGA). Box art: Ape Escape PS1 cover (libretro-thumbnails).
   The goofy hand-drawn SVG pad is GONE (user rejected it — use the real art).

## PSX prototype (ApeEscapeRecomp) — WORKS, on isolated branches, NOT pushed
- Both `F:\Projects\psxrecomp\psxrecomp` (framework) and
  `F:\Projects\psxrecomp\ApeEscapeRecomp` on branch **`feat/recomp-ui-launcher`**
  (main untouched). Key commits: framework main.cpp launcher path + deep settings
  map `edb003a` (on top of `53fee8f`); ApeEscape game.toml `offer_ultrawide=true`
  `a6128fc`, CMakeLists wiring, and a junction `ApeEscapeRecomp/recomp-ui` →
  `F:\Projects\recomp-ui`.
- main.cpp: `#if defined(RECOMP_LAUNCHER)` block calls `recomp_launcher_run_window`
  instead of the legacy `psx_launcher::run` (the legacy launcher stays default
  when the define is off). Uses `launcher_profile_apply("psx", &gi)` then per-game overrides
  (lock_mode etc.), and maps PSX `UserSettings` ↔ `RecompLauncherCSettings` both
  ways (window_width, renderer, supersampling, antialiasing, texture_filter,
  screen_kind, frame_interpolation(+fps), spu_hq, auto_skip_fmv, turbo_loads,
  bios_path, deadzone*100/32767, aspect_num/den↔aspect_index, p1/p2_mode↔pad_mode).
- Ape Escape sets `lock_mode=true` (DualShock-only) → pad-mode selector correctly
  HIDDEN, art locked to analog. aspect_mask 0x7 (4:3/16:9/21:9).

## Build & run
- **recomp-ui standalone** (mingw): `export PATH="/c/msys64/mingw64/bin:$PATH"`;
  `cmake -G Ninja -S F:/Projects/recomp-ui -B F:/Projects/recomp-ui/build
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`; `cmake --build build`. Run:
  copy `/c/msys64/mingw64/bin/SDL2.dll` next to `build/recomp-ui-launcher.exe`;
  `LNG_VARIANT=psx` (or snes) selects a coherent profile; screenshot harness
  `LNG_SCRIPT="wait:40;[view:settings;wait:8;]shot:<path>;quit"`.
- **Ape Escape**: `cmake --build F:/Projects/psxrecomp/ApeEscapeRecomp/build-recompui`;
  exe at `build-recompui/ApeEscapeRecomp.exe` (SDL2.dll staged there). Force the
  launcher with `--launcher`. Disc + BIOS staged in the repo.

## Verification notes / gotchas
- Screenshot harness glReadPixels→PNG works headless. **Synthesized keyboard nav
  is FLAKY** — you CANNOT reliably validate gamepad/keyboard nav from screenshots;
  boot default-focus IS reliable. Gamepad nav was solved (in SNES launcher_ng and
  carried here): pass `ImGuiButtonFlags_EnableNav` to the PLAY `InvisibleButton`
  (it's NoNav by default) + Start/Circle re-home focus to PLAY via
  `SetKeyboardFocusHere`.
- Struct-layout ABI mismatches: after adding GameInfo/Settings fields, do a CLEAN
  rebuild of the standalone (stale .obj → garbage fields).
- `SDL_getenv` before SDL_Init returns garbage in proto_main — use plain `getenv`.

## Next steps (priority)
1. **Get user sign-off on PSX theme + layout** (both unfinalized).
2. **Phase 3**: memcard panel (compose `panel_save` SAVE_MEMCARD → dual slots +
   grid) + disc-verdict — via PLUGGABLE host callbacks in the ABI (host provides
   `identify_disc`/memcard summary; launcher renders). Assets already converted.
3. **PSX hotkeys**: build the universal hotkey catalog + per-system `hotkeys_mask`;
   give PSX its own set. **PSX controller buttons**: `ControllerSpec.buttons`.
4. Bridge rebind to PSX `psx_keybinds` (pluggable binds).
5. Roll to other PSX games (MMX4/5/6, Tomba2, …) — mostly new `game.toml` reads;
   MMX X6 + Tomba2 offer 21:9, others 16:9 (verified from tomls).
6. Roll to other ecosystems (nes/gba/n64=PokemonStadium/Snap/StadiumJP/genesis/
   vb=virtualboyrecomp/smsgg/gbc) — a `SystemProfile` row each + host wiring.
7. **Port recomp-ui back into snesrecomp** (SNES currently uses its own in-tree
   launcher_ng; user wants SNES to adopt the submodule once landed). Carry the
   short-window-scroll fix + namespacing.

## SNES side (separate, DONE this session)
MMX **v1.2.0 released** (github.com/mstan/MegaManXSNESRecomp releases/tag/v1.2.0)
with launcher_ng; all 5 SNES games (MMX/Zelda/StarFox/SuperMetroid/SMW) build with
the launcher; MMX AppImage verified. That is the finalized SNES launcher_ng — the
recomp-ui/PSX work above is the new, in-progress abstraction.
