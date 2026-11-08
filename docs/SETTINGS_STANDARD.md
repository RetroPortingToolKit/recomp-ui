# Settings standard (every console)

Private PSX017 update (Alex,2026-11-08): PlayStation Settings now starts on
General (BIOS, audio, hotkeys and input) and has a Display tab for graphics.
CRT, filtering, internal resolution and PGXP controls move from Mods to
Display; enhanced audio and rewind controls move to General. Their existing
provider and persisted values remain authoritative. Game-specific and
third-party packages stay in Mods. Other console profiles keep their current
surfaces and capabilities. This PSX instruction supersedes the older
presentation policy below for these built-in preferences.

The PSX Hotkeys editor exposes every runtime action, up to four alternatives
per action, individual removal and Reset hotkeys to defaults. Reset restores
current defaults, including F11/Alt+Return/Ctrl+F, F6 scanlines, F12 recording
and F5/Shift+F5 quick state controls; it does not change the proposed F-key
layout or unrelated configuration sections.


The launcher's Settings page, and the in-game menu's Settings view, hold the
player's **PC and system** choices only. Anything that changes the game's
picture, sound or play is a **mod**. This applies to every console recomp-ui
serves, not to one title or one host. (Owner decision, 2026-10-24;
psxrecomp tracker beads-eio.3.398.)

## The rules

1. **Settings holds PC/system concerns only:**
   - window size or scale, display mode (fullscreen, borderless, exclusive),
     screen layout across windows (multi-screen systems);
   - renderer / graphics API and VSync;
   - audio volume and output device;
   - input: devices, bindings, deadzone, controller-port hardware (multitap,
     pad mode, peripherals such as a solar sensor or virtual stylus), hotkeys;
   - BIOS / firmware;
   - launcher behaviour (skip launcher, language of the launcher itself).
2. **Anything that changes the game's picture, sound or play is a mod**, on
   the Mods page, with the host's mod system behind it. Examples: scaling and
   texture filters, CRT and screen models, scanlines, shaders, internal
   resolution and supersampling, antialiasing, enhanced audio, music
   replacement, rewind, run-ahead, widescreen and extended views, frame rate
   and frame interpolation, PGXP-style precision, HD texture packs, integer
   scaling, frame blending, translations of the game's text.
3. **One control per value.** A value is either a Settings row or a mod
   option, never both. When a row becomes a mod, the row and its persisted
   player setting go (old keys are read once at most, then ignored and dropped
   on the next save); the title's own default stays its baseline, and a mod
   option's "Game default" keeps that baseline.
4. **No quick-settings cards on the dashboard.** The dashboard shows the game,
   its controllers and its saves. (The Quick Mods card is a shortcut to mods,
   not to settings, and stays.)
5. **Controls are sized to their labels.** A settings button or dropdown is
   as wide as the widest value it can show (`ImGui::CalcTextSize` over every
   value, plus frame padding and a dropdown's arrow), never narrower than the
   shared minimum and never wider than the card leaves beside the label.
   `settings_fit_w` / `settings_cycle_w` in `launcher_imgui.cpp` do this for
   every row; `launcher_model_each_cycle_label` enumerates a cycle button's
   values. No row hard-codes a wider constant of its own.

Hosts follow the standard through the existing ABI: a row is offered only when
the host sets its capability (`GameInfo.has_*`, the profile's video caps and
`hide_audio_freq`). Moving a row to a mod means the host clears that
capability and ships the mod; recomp-ui keeps the generic row for hosts that
have not moved yet. A host with **no mod system keeps its rows** until it has
one: removing them would delete features, so they are listed below as
migration items instead.

## Where each console stands (audit, 2026-10-24)

What each profile offers today (`src/consoles/<id>/<id>_profile.h`: the
`SystemProfile` panel lists and the `launcher_profile_apply_<id>` capability
defaults), plus the rows a host turns on per game through the same ABI. Only
rows that break the standard are listed. "Mod system" says whether recomp-ui
can see a mod provider for that console's host: the only provider documented
in this repository is psxrecomp's (`README.md`, "PSXRecomp integration";
`.gbamod` in `recomp_launcher.h` is an example extension, not a shipped
provider). For every other console it is **not established here** and must
be confirmed in the host before a row moves.

### PSX (psxrecomp) -- mod system: yes

Moved to mods in this change: screen model, scanlines (+ strength), smoothing
(linear present), texture filtering, FMV filtering, enhanced audio
(high-quality SPU), rewind (+ buffer, interval), perspective textures (PGXP
Precision), dynamic resolution and internal resolution (Internal Resolution
mod, Auto included). Removed outright: Graphics preset, Sample rate (the
runtime opens audio at its built-in rate; Volume stays), the Quick Settings
dashboard card.

| Row still offered | Why it breaks the standard | Status |
|---|---|---|
| Language (Audio card, LOCALIZATION; titles that declare languages) | Translates the game's own text | Migration item: a translation mod per title |

Frame interpolation, Skip FMVs, the aspect (View mode) row and Turbo loads
are already off for PSX (mod-owned: `has_frame_interp`, `has_skip_fmv` and
`aspect_mask` are 0, and Turbo loads draws no row). The Render thread /
Present thread / Smooth motion rows are drawn only on the legacy Display
branch, which PSX does not use.

### Game Boy / Game Boy Color (gb, gbc) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Screen model (DMG, Pocket, Light, B&W, Amber, Super Game Boy colours/border) | Changes the picture | Migration item (keep until the host has mods) |
| Linear filtering | Changes the picture | Migration item |
| Aspect ratio / extended view (when the host offers it per game, e.g. Megaman Xtreme 2) | Changes the picture and the play area | Migration item |
| Sample rate | Not a Settings concern (the profile notes the rate is fixed) | Migration item: drop the row once the host confirms |

### Game Boy Advance (gba) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Screen model (Raw, Unlit, Frontlit, Backlit, Classic) | Changes the picture | Migration item |
| Linear filtering | Changes the picture | Migration item |
| Aspect ratio / extended view (when the host offers it per game, e.g. Mega Man Zero) | Changes the picture and the play area | Migration item |
| Sample rate | Not a Settings concern | Migration item |

The Solar Sensor card is a cartridge peripheral (game input), so it stays.

### Sega Genesis (genesis) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Linear filtering | Changes the picture | Migration item |
| Widescreen / aspect (when the host offers it per game; the profile's "extra cells per side" spec has no row today) | Changes the picture and the play area | Migration item |
| Shader, Frame blending, Run-ahead (when the host offers them) | Picture / play | Migration item |
| Sample rate | Not a Settings concern | Migration item |

### Nintendo 64 (n64) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Supersampling (or an Internal resolution list) | Internal resolution is a mod | Migration item |
| Antialiasing (MSAA) | Changes the picture | Migration item |
| Linear filtering | Changes the picture | Migration item |
| Aspect / widescreen (when the host offers it) | Picture and play area | Migration item |
| Sample rate | Not a Settings concern | Migration item |

Renderer (graphics API), fullscreen, VSync and the output device stay.

### Nintendo DS (nds) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Supersampling | Internal resolution is a mod | Migration item |
| Antialiasing | Changes the picture | Migration item |
| Linear filtering | Changes the picture | Migration item |

Screen layout (stacked or separate windows) and the virtual stylus stay: they
arrange windows and input, not the game.

### NES (nes) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Integer scaling | Changes the picture | Migration item |
| Linear filtering | Changes the picture | Migration item |
| HD texture pack | Replaces the game's graphics | Migration item |
| Widescreen / aspect (when the host offers it per game) | Picture and play area | Migration item |

Renderer (Accelerated / Software SDL output) and the BIOS card (Famicom Disk
System) stay; Sample rate is already hidden.

### Super Nintendo (snes) -- mod system: not established

| Row | Why | Status |
|---|---|---|
| Linear filtering | Changes the picture | Migration item |
| Widescreen / aspect (16:9, when the host offers the aspect row) | Picture and play area | Migration item |
| Shader, Frame blending, Run-ahead (when the host offers them) | Picture / play | Migration item |
| MSU-1 music (per game) | Replaces the game's sound | Migration item |
| Sample rate | Not a Settings concern | Migration item |

### Virtual Boy (vb) -- mod system: not established

The profile is a stub (`kPanelsSettingsStub`; `launcher_profile_apply` sets
identity only), so its rows come from the generic legacy surface:

| Row | Why | Status |
|---|---|---|
| Linear filtering | Changes the picture | Migration item |
| Sample rate | Not a Settings concern | Migration item |

## Adding or changing a row

Before adding a Settings row, ask whether it changes what the game shows,
plays or sounds like. If it does, it belongs in the host's mod system; if the
host has none yet, add the row with a note in this file's table for that
console, so the migration is tracked rather than forgotten. Size the control
with `settings_fit_w` / `settings_cycle_w`.
