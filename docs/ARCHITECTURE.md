# recomp-ui architecture — composition + inheritance

recomp-ui is one shared launcher core that serves every recomp system (SNES,
PSX, N64, Genesis, GBA, NES, GBC, SMS/GG, Virtual Boy, ...). A system's launcher
is **composed** from panels; panels that share a shape but differ by system are
**specialized** from a base via a small per-system spec. No per-console fork; no
mega-`if`. Adding a system is (ideally) one `SystemProfile` row.

## Two mechanisms

- **Composition** decides *which* cards appear. Each view renders a **panel list**
  the SystemProfile composes. "PSX has memory cards, SNES has SRAM, this game has
  neither" = include/omit the panel.
- **Inheritance** decides *how* a card behaves. Cards derive from a base (shared
  chrome/behavior) and take a per-system **spec** (data). "Every game has a
  controller, but its image/buttons/count differ" = one base card, per-system data.

## Four layers

```
1. Card chrome (base every card inherits): begin_card(title)/end_card, eyebrow,
   border, theme tokens, focus ring. Reusable widgets: row_label, cycle_button,
   toggle, stepper, picker_row, segmented.
2. Modules (abstract capabilities, specialized per system):
     Controller · Save · Video · Verify · Hotkeys
3. SystemProfile: concrete specs for the modules + the panel composition arrays.
   ONE ROW PER CONSOLE.
4. Game (instance): gates/overrides what the system offers (players exposed,
   aspects offered, saves present, pad-mode locked, box art, hashes).
```

The System is the **template**; the Game **refines** it.

**What may be a Settings row at all** is fixed for every console by
[SETTINGS_STANDARD.md](SETTINGS_STANDARD.md): Settings holds PC/system
concerns only; anything that changes the game's picture, sound or play is a
mod; one control per value; no quick-settings cards; controls sized to their
labels. That file also tracks, per console, the rows still waiting to move.

## Module catalog (base → per-system specialization)

### Controller  (every game has one)
- **Base (universal):** device source `none / keyboard / gamepad`, connected
  status, N player cards laid out by count (1 = single, 2 = split, 4 = grid),
  a **Configure** action that opens the controller-binds page.
- **Spec (per system):** pad **image(s)** (SNES pad / DualShock / N64 trident /
  Genesis 6-button), the **base button set** for rebinding, `max_players`
  (2 or 4), optional **pad-mode** selector. Two selector shapes: legacy
  analog/digital (swaps the image, gated by `allow_hybrid`), or a **custom mode
  list** (`ControllerSpec.modes`, e.g. PSX **Auto** or Genesis
  **3-Button / 6-Button**) where
  the selected mode's `button_count` also sets how many rebind rows show
  (3-Button hides X/Y/Z/Mode).
- **Binds:** the game-pad buttons are rebound on a **page reached from the
  Controller panel's Configure** (controller view). Per-system base button set.
  A console can set `has_pad_binds` to add a second **GAMEPAD** chip per row
  (KEY + GAMEPAD, e.g. Genesis — the engine stores a controller button/axis
  bind alongside the keyboard scancode). This is SEPARATE from emulator hotkeys.

### Save  (optional — omitted when the game has none)
- **Base:** a persistence card built from a reusable **picker row** (label +
  path + actions).
- **Spec (shared, kind-switched):** `SaveSpec { kind, slots, probe_cb }`.
  - `SAVE_SRAM`   → one picker (Import / Clear).
  - `SAVE_MEMCARD`→ `slots` pickers (PSX = 2), each Browse / New + a block grid.
  - `SAVE_NONE`   → panel not composed.
  One base widget, two subtypes — not two copy-pasted panels.

### Video / Display  (always present)
- **Base (universal):** Display + **Window scale** (+ Fullscreen-on-launch).
- **Spec (per system):** SNES adds linear-filter + widescreen toggle; Genesis
  adds — while widescreen is on — an **"extra cells per side"** stepper
  (`VideoSpec.widescreen_cells`, N extra 8-px background cells, 1..16); PSX adds
  renderer and VSync (its picture controls -- internal resolution, filtering,
  screen model -- are psxrecomp mods). Picture-changing rows on other systems
  are migration items under [SETTINGS_STANDARD.md](SETTINGS_STANDARD.md).

### Verify  ("is this the right game?")
- ROM CRC/SHA line (cart systems) vs PSX **disc verdict** (serial + region +
  ISO header → ok/warn/bad/none icons + checklist). Pluggable host probe.

### Hotkeys  (emulator hotkeys — live in SETTINGS, not the controller)
- **Universal catalog** of hotkeys (Fullscreen, Reset, Pause, Turbo, Save-state,
  Screenshot, Volume ±, Window ±, FPS, Toggle-renderer, ...).
- Each system **opts into** a subset via a `hotkeys_mask` (a grab-bag; Fullscreen
  is a near-universal opt-in). Rebound in a Settings panel.

## Universal panels (single implementation, theme-colored, never re-authored)
- Shell: marquee header (brand + game title + platform subtitle), Settings/Back nav.
- GAME card: box art + Verify line + Change-<rom_noun>.
- Footer: **PLAY** (colorized by theme accent) + Skip-launcher-on-boot (+ confirm).

## Requirement → home

| Requirement | Where |
|---|---|
| Every game has a controller; image varies | Controller module always composed; `ControllerSpec.image` |
| none / keyboard / controller | base controller card |
| Bindable buttons, set varies | `ControllerSpec.buttons[]`, Configure page |
| Display + window scale always | Video base |
| Widescreen some games | per-game aspect capability |
| Fullscreen hotkey everywhere | Hotkeys catalog (opt-in), in Settings |
| Box art every game; skip-launcher every game | universal GAME card + footer |
| Saves not always; SRAM vs memcard | Save module, optional, `kind`-specialized |
| 2 vs 4 pads; expose 1/2/4 | `SystemProfile.max_players` + `Game.exposed_players` |

Hosts may optionally provide a complete `default_settings` snapshot through
the C ABI. The launcher copies it during model initialization and then exposes
a confirmed Restore Defaults action in the Settings footer. Confirmation
replaces only the editable settings value; ROM selection and save files remain
separately owned.

Hosts may also add two top-level pages without forking the renderer.
`has_assist_tools` exposes a host-defined opt-in backed by the additive
`Settings.assist_tools` field and an optional explanatory note.
`settings_bindings` is a separate opt-in for hosts whose runtime consumes the
returned settings directly. It adds per-player keyboard/controller arrays and
optional named Assist-action arrays. Keyboard entries are SDL scancodes;
controller entries encode standard SDL gamepad buttons or signed axes. Hosts
without this flag continue through their existing native bind bridges.
`credits_text` exposes read-only, host-owned UTF-8 credits. Both capabilities
default absent, preserving the original Dashboard/Settings/Controller view
set for existing games.

## Concrete C shape (data-driven, no fake OOP)

```c
typedef struct { const char* label; int code; } ButtonDef;
typedef struct { int mode; const char* label; int button_count; } PadModeDef;

typedef struct {
  const ButtonDef* buttons; int button_count;   // per-system base set
  const char* image;                             // base pad art (fallback)
  const char* image_analog, * image_digital;     // optional mode-swap pair
  int  max_players;                              // 2 or 4
  int  has_pad_mode;                             // analog/digital selector
  // ---- appended additively (older positional initializers zero-fill) ----
  const PadModeDef* modes; int mode_count;       // custom mode list (NULL => legacy PSX set)
  int  has_pad_binds;                            // rebind page adds a GAMEPAD bind column
} ControllerSpec;

typedef enum { SAVE_NONE, SAVE_SRAM, SAVE_MEMCARD } SaveKind;
typedef struct { SaveKind kind; int slots; SaveProbeFn probe; } SaveSpec;

typedef struct {                                 // capability flags for the video panel
  int window_scale, fullscreen;                  // base
  int linear_filter, widescreen;                 // SNES-ish
  int renderer, supersampling, screen_kind, frame_interp, aspect, texture_filter,
      antialiasing, spu_hq, skip_fmv, turbo_loads, bios, deadzone; // PSX-ish
  int  widescreen_cells;                         // Genesis: "extra cells / side" stepper
} VideoSpec;

typedef struct { int mode; /* 0 rom-hash, 1 disc-verdict */ VerifyProbeFn probe; } VerifySpec;
typedef struct { const char* const* patterns; int pattern_count; const char* desc; } RomFilterSpec;

typedef struct {
  const char* id, *platform, *theme, *rom_noun;
  ControllerSpec controller;   SaveSpec save;   VideoSpec video;   VerifySpec verify;
  uint32_t hotkeys_mask;                         // universal hotkeys opted into
  const char* const* panels_dashboard;           // composition (panel ids, in slot order)
  const char* const* panels_settings;
  const char* const* panels_controller;
  const char* const* screen_kind_names; int screen_kind_count;  // "Screen model" vocab (NULL => legacy set)
  RomFilterSpec rom_filter;                      // "Change ROM" native-dialog filter, per console
} SystemProfile;                                  // ONE ROW PER CONSOLE

// A panel is a composition unit.
typedef struct {
  const char* id; LngView view; int slot;
  int  (*available)(const LauncherModel*);        // shown for this game?
  void (*draw)(LauncherModel*, const LauncherTheme*);
} LauncherPanel;
```

The renderer looks up each id in the profile's `panels_*` list, skips those whose
`available()` is false, and draws the rest into their slots. Each panel reads its
module spec (from the SystemProfile) plus the game's per-instance gates.

## Refactor plan (from today's monolithic renderer)
1. Land the contracts: `launcher_panels.h` (registry), `launcher_system.h`
   (SystemProfile + specs), extend the model to carry the active SystemProfile.
2. Carve `launcher_imgui.cpp`'s inline sections into registered panel `draw`
   functions (game, controller, save, video, verify, hotkeys, footer) — behavior
   unchanged, just relocated.
3. Author `SystemProfile` rows: `snes`, `psx` fully; others stubbed.
4. Verify SNES and PSX both compose byte-for-byte the same as today.
