# recomp-ui

> ℹ️ **Note from mstan:** This repo and I are now part of [RetroPortingToolkit](https://retroportingtoolkit.com/). I remain a primary maintainer of recomp-ui alongside the team. [More info](https://1379.tech/forming-a-collective-retro-porting-toolkit/).

A shared, **console-agnostic launcher and in-game settings UI** for static-
recompilation game ports. One Dear ImGui launcher core serves every recomp
ecosystem — SNES, PSX, N64, Nintendo DS, Genesis, NES, and beyond — while a small runtime
overlay API lets the same hosts expose live settings after boot.

It is the reusable extraction of the SNES-recomp "launcher_ng" launcher,
generalized behind a small C ABI. Consume it as a git submodule, hand it your
game's facts through a plain-C struct, and it renders the right UI for that
console.

Proven in production driving two very different consoles from the **same
binary core**:

- **Mega Man X** (Super Nintendo) — CRT theme, SNES pad, ROM verification.
- **Ape Escape** (PlayStation) — blue theme, DualShock + pad-modes, real disc
  verification, memory cards, deep PSX video settings.

---

## Architecture: composition + inheritance

recomp-ui is **one** launcher composed differently per console — never a
per-console fork, never a mega-`if`. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

- **Composition** decides *which* cards appear. A `SystemProfile` lists the
  panels each view (dashboard / settings / controller) gets. "PSX has memory
  cards, SNES has SRAM, this game has neither" = include/omit the panel.
- **Inheritance** decides *how* a card behaves. Each module — Controller, Save,
  Video, Verify, Hotkeys — has one base implementation specialized per console
  by a small data **spec**. "Every game has a controller; its art/buttons/count
  differ" = one base card, per-system data.

Adding a console is (ideally) **one `SystemProfile` row** in
[`src/launcher_system.h`](src/launcher_system.h). The console difference is pure
data passed through the C ABI — theme, platform label, box art, pad image,
which panels — so there is no `../snes` / `../psx` source fork.

The DRY heart is [`src/launcher_model.h`](src/launcher_model.h): all launcher
STATE + BEHAVIOR, free of any UI toolkit, built purely from the C ABI structs —
so behavior is identical across every game.

---

## Optional experience presets

Hosts may opt into a preset picker by supplying four mod-provider callbacks.
No picker or default presets appear in games that omit them. Bundled MSU
source choices are a separate optional capability; existing hosts retain the
custom-folder UI. See [the API guide](docs/OPTIONAL_PRESETS.md).

## Consuming it

### 1. Vendor the repo (git submodule)

```sh
git submodule add https://github.com/mstan/recomp-ui.git recomp-ui
```

### 2. Wire it into your CMake target

```cmake
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(my-game-runtime
    CONSOLE psx                                  # required: psx/snes/nds/n64/nes/...
    BOXART ${CMAKE_SOURCE_DIR}/art/boxart.tga   # optional: game box art
    PAD    ${CMAKE_SOURCE_DIR}/art/pad.tga       # optional: per-console controller image
    BRAND  ${CMAKE_SOURCE_DIR}/art/brand.tga)    # optional: per-console top-left mark
```

`recomp_ui.cmake` is fully self-contained: it bundles Dear ImGui, crc32/sha256/
keybinds, tinyfiledialogs, an IPS applier, and a PS1 memcard formatter, and
stages its fonts/images next to your exe. No network / FetchContent — builds
offline. It defines `RECOMP_LAUNCHER` on the target and links OpenGL. The
standalone recomp-ui project defaults to SDL3; a generic project that directly
includes `recomp_ui.cmake` defaults to the SDL2 compatibility backend unless it
sets `RECOMP_UI_SDL3=ON` or supplies the ecosystem-level
`SNESRECOMP_SDL_BACKEND=SDL3` selection. See
[Build configuration reference](#build-configuration-reference) for precedence
and the definitions produced on the target.

`CONSOLE` is required and selects the runtime asset manifest for that target:
`psx`, `snes`, `n64`, `nes`, `gba`, `genesis`, `gb`, or `gbc`. Common fonts
and launcher chrome are always included; controller, brand, memory-card, and
cartridge art comes only from the selected console family. `CONSOLE all` is
reserved for targets that intentionally switch between multiple console
profiles, such as recomp-ui's standalone preview.

The schema-driven Mods view is developer opt-in. Configure the consuming build
with `-DRECOMP_UI_ENABLE_MODS=ON`, then provide a non-null
`RecompLauncherCGameInfo.mods` provider. These are separate gates: the CMake
option enables the frontend, while the provider supplies the actual catalog and
behavior. See [Mods frontend](#mods-frontend) for the complete integration.

Multi-display systems can provide `GameInfo.display_layout_labels`; recomp-ui
then exposes a dedicated Display → Screen layout cycle backed by
`Settings.display_layout`. This remains independent of aspect or widescreen
features. The Nintendo DS profile uses it for stacked versus separate windows,
keeps Player 1's ordinary buttons controller-configurable, and leaves the
bottom-screen pointer/touch mapping to the host.

### 3. Call it from your `main()`

```c
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#include "launcher_profile.h"

RecompLauncherCSettings io = { /* seed from your config */ };
RecompLauncherCGameInfo gi = {0};
launcher_profile_apply("snes", &gi);   // "psx", "n64", ... — one call sets the console identity
gi.name = "My Game";
gi.expected_crc = 0x1B4B2E9C; gi.has_expected_crc = 1;
RecompLauncherCSettings defaults = { /* authoritative first-run values */ };
gi.default_settings = &defaults;       // optional Restore Defaults action
gi.has_assist_tools = 1;               // optional host-defined opt-in page
gi.assist_tools_note = "Enables host convenience features.";
gi.credits_text = "Original game and port credits."; // optional read-only page
/* ...per-game overrides... */

char out_rom[512];
int rc = recomp_launcher_run_window("My Game — Launcher", &io, &gi,
                                    ".", initial_rom, out_rom, sizeof(out_rom));
// rc: 0 = LAUNCH (boot out_rom with the edited io), 1 = QUIT, 2 = UNAVAILABLE
#endif
```

The whole contract is [`src/recomp_launcher.h`](src/recomp_launcher.h): a plain-C
settings struct in/out, a game-facts struct, and (optionally) **host callbacks**
for console-specific verification the launcher re-runs on change — e.g. PSX disc
identification (`disc_verify`) and memory-card inspection (`memcard_inspect`).

## Mods frontend

recomp-ui provides a generic, schema-driven **frontend** for mods. It displays
features and their boolean/choice/integer options, installs local archives,
manages installed versions, shows structured diagnostics, and asks the host to
commit the staged selection before launch.

It deliberately does **not** define a mod file format, parse packages, resolve
dependencies, patch a ROM/disc, execute package code, or decide which mods a
game supports. Those jobs remain with the host's mod manager. A PSX host may use
`.psxmod`; another ecosystem can advertise `.gbamod` or another archive type
through the same UI ABI.

### The three gates

A Mods button appears only when all three integration layers are present:

1. The target calls `recomp_target_launcher_ui(...)`, which compiles the
   launcher into that target and defines `RECOMP_LAUNCHER`.
2. The build is configured with `RECOMP_UI_ENABLE_MODS=ON`. The helper turns
   this CMake cache value into the private target definition
   `RECOMP_UI_ENABLE_MODS=1`.
3. The host sets `RecompLauncherCGameInfo.mods` to a live
   `RecompLauncherCModProvider` before calling
   `recomp_launcher_run_window()`.

If the CMake option is off, recomp-ui deliberately discards the provider while
initializing its model. If the option is on but `game.mods` is `NULL`, the
launcher has no catalog to show and omits the Mods button. This is why enabling
the flag in recomp-ui's standalone harness does not produce a sample catalog:
the harness does not fabricate a package manager.

Enable the frontend during the **configure** step:

```sh
cmake -S . -B build -DRECOMP_UI_ENABLE_MODS:BOOL=ON
cmake --build build
```

For a consuming game, those commands point at the game's source and build
directories, not necessarily the recomp-ui submodule itself:

```sh
cmake -S path/to/game -B path/to/build \
  -DRECOMP_UI_ENABLE_MODS:BOOL=ON
cmake --build path/to/build
```

`-D...=ON` belongs on `cmake -S/-B`, not on `cmake --build`. Reconfiguring an
existing build directory with the explicit value updates its cache and causes
CMake to rebuild affected launcher sources; a clean rebuild is not normally
required. Inspect the stored value with:

```sh
cmake -LA -N build | grep RECOMP_UI_ENABLE_MODS
```

PowerShell equivalent:

```powershell
cmake -LA -N build | Select-String RECOMP_UI_ENABLE_MODS
```

If a host wants Mods enabled by default without overriding an explicit user
choice, set the cache default **before** including `recomp_ui.cmake`:

```cmake
if(NOT DEFINED RECOMP_UI_ENABLE_MODS)
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL
        "Enable this game's Mods launcher view")
endif()

include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
recomp_target_launcher_ui(my-game-runtime CONSOLE psx)
```

The option is build-tree-wide: every target passed to
`recomp_target_launcher_ui()` in that CMake build receives the same value. Use
separate build directories if one produced executable must expose Mods and
another must compile with the frontend disabled.

### Supplying the provider

Zero-initialize a `RecompLauncherCModProvider`, populate its callbacks, keep it
alive for the entire launcher call, and assign it to the game info:

```c
RecompLauncherCModProvider mods = {0};
mods.ctx = &my_mod_manager;
mods.package_count = my_package_count;
mods.package_get = my_package_get;
mods.feature_count = my_feature_count;
mods.feature_get = my_feature_get;
mods.feature_option_get = my_feature_option_get;
mods.feature_choice_get = my_feature_choice_get;
mods.feature_enable = my_feature_enable;
mods.feature_set_option = my_feature_set_option;
mods.commit = my_mod_commit;
mods.last_error = my_mod_last_error;

RecompLauncherCGameInfo game = {0};
launcher_profile_apply("psx", &game);
game.mods = &mods;
```

The complete append-only ABI is
[`RecompLauncherCModProvider`](src/recomp_launcher.h). New integrations should
implement its feature-oriented callback group as the primary player-facing
surface and the package callbacks as the secondary installation/version/removal
surface:

- `feature_count` / `feature_get` enumerate independently toggleable features.
- `feature_option_get` and `feature_choice_get` describe validated typed
  controls; `feature_enable` and `feature_set_option` stage mutations.
- `package_count` / `package_get`, version callbacks, `select_version`,
  `install_archive`, and `remove_package` drive package maintenance.
- `option_get`, `choice_get`, `set_enabled`, and `set_option` retain the legacy
  package-only surface for providers that have not adopted independent
  features.
- `diagnostic_count` / `diagnostic_get` attach information, warnings, and
  errors to a package or feature.
- `archive_extension` and `archive_description` replace the default
  `.psxmod` file-picker vocabulary for other ecosystems.
- `last_error` supplies the visible reason after any callback returns failure.

The launcher selects the feature-first view only when the complete core feature
group (`feature_count`, `feature_get`, `feature_option_get`, `feature_enable`,
and `feature_set_option`) is present. A package-only provider remains supported;
an accidentally partial feature implementation falls back to that package
view, so implement the core group together.

All query structs contain fixed-size character buffers. recomp-ui copies their
contents and does not retain provider-owned string pointers, so a successful
mutation may safely rebuild or rescan the host catalog before the next query.
Mutation callbacks return `1` on success and `0` on failure.

### Commit and netplay behavior

Normal edits are staged through the provider. When the player presses **PLAY**,
recomp-ui calls `commit(ctx, selected_image_path)` before returning launch
success. A failed commit keeps the launcher open and displays `last_error`; the
host can perform final target verification, dependency resolution, collision
checks, persistence, and plan construction there.

Hosted-lobby, LAN, and direct netplay launches never call the normal `commit()`.
If supplied, `commit_netplay()` should clear any in-session mod plan without
changing the user's persisted offline selection. If it is `NULL`, recomp-ui
skips mod commit entirely. This keeps network matches vanilla unless a host
later defines and implements a synchronized mod contract.

The provider ABI stays present in `recomp_launcher.h` even when
`RECOMP_UI_ENABLE_MODS=OFF`, so the option does not change
`RecompLauncherCGameInfo` layout. The generated macro is private to the target
passed to `recomp_target_launcher_ui()`; it is a frontend exposure switch, not
a build flag for a separately compiled package-manager library.

### PSXRecomp integration

PSXRecomp supplies the provider through
`mod_runtime_launcher_provider()` and sets `game.mods` for its launcher. Its
`PSX_RECOMP_UI` option controls whether the entire recomp-ui launcher is built;
`RECOMP_UI_ENABLE_MODS` controls the Mods surface inside that launcher:

```sh
cmake -S path/to/psx-game -B build \
  -DPSX_RECOMP_UI:BOOL=ON \
  -DRECOMP_UI_ENABLE_MODS:BOOL=ON
cmake --build build
```

Current PSXRecomp versions default `RECOMP_UI_ENABLE_MODS` to `ON` for a fresh
game build cache while still honoring an explicit `OFF`. An older build
directory may retain the former recomp-ui default (`OFF`); reconfigure it with
the explicit `-DRECOMP_UI_ENABLE_MODS:BOOL=ON` command above. PSXRecomp emits a
CMake warning when it detects this Mods-less cached state.

### In-game settings overlay

[`src/recomp_runtime_ui.h`](src/recomp_runtime_ui.h) is the separate runtime
contract. A game supplies sectioned item descriptors plus callbacks to read,
apply, persist, and enable its own settings. `recomp-ui` owns hierarchical
navigation, selection state, help/status text, and drawing.

The runtime model is renderer-independent. For a modern/GPU host, call
`recomp_runtime_ui_render_imgui()` inside the host's active Dear ImGui frame;
it uses the same responsive layout and console theme tokens as the launcher,
without exposing ImGui types through the C ABI. This is the preferred path for
RT64/N64 and other high-resolution renderers.

SDL2 hosts that retain an `SDL_Renderer` can add the matching official
SDL_Renderer2 backend to one runtime target with
`recomp_target_runtime_ui_sdlrenderer2(<target>)`. The helper is idempotent and
does not compile another ImGui core or SDL2 platform backend, avoiding duplicate
symbols when the launcher and runtime menu share a binary.

`recomp_runtime_ui_render_argb8888()` is a compatibility presentation for
games that expose a writable CPU framebuffer. It is intentionally compact and
works well for low-resolution framebuffer consoles, but it is not the visual
baseline for every platform. Both presentations drive the exact same menu
state, descriptors, callbacks, and navigation.

The host continues to own the game loop: while
`recomp_runtime_ui_is_open()` is true it should consume menu input before
emulated input and decide whether to pause simulation/audio. See
[`docs/RUNTIME_UI.md`](docs/RUNTIME_UI.md) for the integration boundary and
rollout matrix.

```c
static const RecompRuntimeUiItem rows[] = {
    { "fullscreen", "Display", "Fullscreen", "Choose the window mode.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, modes, 3, NULL },
    { "reset", "System", "Reset game", "Reset the emulated machine.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
};

RecompRuntimeUiConfig menu = {0};
menu.title = "My Game";
menu.subtitle = "SETTINGS";
menu.items = rows;
menu.item_count = sizeof(rows) / sizeof(rows[0]);
menu.callbacks = callbacks;
menu.theme = "n64"; /* same id used by launcher_profile_apply() */
/* Optional for a physically small, high-density touchscreen. */
menu.presentation_flags =
    RECOMP_RUNTIME_UI_PRESENTATION_TOUCH_FRIENDLY;
RecompRuntimeUi *ui = recomp_runtime_ui_create(&menu);
```

New integrations should zero-initialize `RecompRuntimeUiConfig`, then assign
its fields. Set `theme` to the same system id passed to
`launcher_profile_apply()` (`"snes"`, `"n64"`, `"gba"`, `"genesis"`, ...),
so pre-boot and in-game UI cannot drift visually.

The API deliberately does not prescribe settings. Backend swaps that require a
restart can be omitted or exposed as actions; safe live values can be applied
immediately in `set_value`. This keeps game-specific state and policy in the
game while making the menu implementation reusable.

### Optional netplay surface

Netplay is enabled by the game developer, not by an end-user setting. Set
`RecompLauncherCGameInfo.netplay_supported` and provide a
`RecompLauncherCNetplayCallbacks` table to expose the Netplay button. The host
owns player-name and lobby-server persistence, LAN/remote lobby discovery,
password checks, member ordering, and synchronized launch signaling. The UI
only presents that state and returns a `RecompLauncherCNetplayLaunch` result
after the host starts the lobby.

Hosts with more than one local network interface may implement the optional
`local_address_get` callback to enumerate labeled address choices. The launcher
prefills the first address, preserves the user's selection across refreshes,
and falls back to the legacy single-address `local_ip` callback when the new
callback is absent or returns no choices.

The initial waiting-room UI supports two players. Hosts always bind UDP on
`0.0.0.0`. **LAN/Direct IP** (unchecked by default) enables the interface
dropdown and port so hosts can advertise a chosen LAN address to joiners; when
unchecked those controls stay visible but disabled (selection preserved) —
create ignores the greyed port field, prefers UDP `7777`, and auto-picks a
nearby free port (`7777`..`7808`) for lobby-server rewrite / ICE. Before
`join()`, the UI fills `guest_bind` the same way (prefer `7778`..`7809`) so
online guests advertise a real UDP port — hosts must pass that buffer through
to the lobby client rather than rewriting to `:0`. Direct / LAN join still
receives a prepared bind; games may ignore it for file-registry LAN rooms.

**Soft-return rematch:** closing the launcher after a match calls `SDL_Quit()`
(see `launcher_platform_close`). Hosts that `goto` a mid-`main` rematch label
must re-`SDL_Init` video/audio/gamecontroller before opening the game window —
otherwise rematch fails with `Audio subsystem is not initialized`. Full host
checklist: [`docs/HOST_NETPLAY.md`](docs/HOST_NETPLAY.md). Engine-side notes:
snesrecomp `docs/RECOMP_NET.md` → "Soft-return rematch checklist".

---

## Feature surface (per console, capability-gated)

- **Game / verification** — box art, ROM CRC/SHA verification, or PSX disc
  verdict (serial / region / ISO header) via the host `disc_verify` callback.
- **Controllers** — none/keyboard/gamepad per player, per-console pad art +
  button vocabulary, PSX analog/digital/hybrid pad modes or a custom mode list
  (Genesis 3-Button / 6-Button, which also sets the visible rebind-row count),
  deadzone, keyboard rebinding that writes each runtime's native keybind format,
  and an optional per-row **GAMEPAD** bind column (Genesis) alongside the key.
- **Video** — window scale/size, renderer, supersampling, aspect (4:3/16:9/21:9),
  widescreen 16:9 with an optional "extra cells / side" stepper (Genesis),
  texture filtering, antialiasing (Off/2×/4×/8×), screen model, frame
  interpolation, and more — each shown only when the console exposes it.
- **Save** — SRAM Import/Clear (with `.bak` backup) or PS1 memory cards (per-slot
  enable, Browse / New-formats-a-blank-card, real block-usage grid via the host
  `memcard_inspect` callback).
- **MSU-1** — dashboard IPS auto-patching for SNES MSU-1 titles.
- **Hotkeys** — per-console subset of the emulator hotkey catalog, editable.
- **Mods** — default-off, host-provided package catalog with local archive
  installation, enable/disable, version rollback, removal, and schema-generated
  boolean/choice/integer controls. The host owns package formats, defaults,
  resolution, and committing changes before launch; recomp-ui never executes
  package code.
- **Footer** — PLAY, skip-launcher-on-boot (+ confirm modal), gamepad navigation.
- **Optional host pages** — an Assist Tools opt-in backed by
  `RecompLauncherCSettings.assist_tools`, plus host-owned read-only Credits
  text. Games that do not supply these capabilities retain the original three
  views.

---

When `RecompLauncherCGameInfo.default_settings` is non-NULL, the Settings
footer also exposes a confirmed **Restore Defaults** action. The snapshot is
copied at launcher initialization; resetting never changes the separately
owned ROM selection or deletes save files.

`RecompLauncherCGameInfo.has_assist_tools` adds an Assist Tools page and
persists its checkbox through `RecompLauncherCSettings.assist_tools`.
An opt-in `settings_bindings` host can keep keyboard and standard-controller
bindings directly in the settings structure instead of pointing the launcher
at a runtime-specific bind file. The same mode can supply named global Assist
actions; they appear on the Assist page and are returned to the host as SDL
scancodes plus portable standard-controller button/axis encodings.
`credits_text` adds a Credits page. Both are additive host capabilities:
recomp-ui defines presentation and navigation, while the game host defines
the meaning of Assist Tools and owns the UTF-8 credits text.

---

## Build configuration reference

There are three different kinds of configuration in a recomp-ui integration:

- **CMake cache variables** select one implementation for the build tree.
- **Arguments to `recomp_target_launcher_ui()`** configure one target and its
  staged assets.
- **Preprocessor definitions** are generated by the helper. Consumers should
  set the corresponding CMake option instead of injecting these definitions
  manually.

### CMake cache variables

| Variable | Type and default | Effect |
|---|---|---|
| `RECOMP_UI_ENABLE_MODS` | `BOOL`, `OFF` | Exposes a non-null host mod provider to the launcher model and defines `RECOMP_UI_ENABLE_MODS=0/1` on every integrated target. It does not create a provider or a catalog. |
| `RECOMP_UI_SDL3` | `BOOL`, `OFF` for a generic include | Selects `launcher_platform_sdl3.c` plus Dear ImGui's SDL3 backend and defines `LNG_SDL3=1`. `OFF` selects the SDL2 compatibility sources. Ignored as an independent choice when `SNESRECOMP_SDL_BACKEND` is defined. |
| `SNESRECOMP_SDL_BACKEND` | `STRING`, `SDL3` in recomp-ui's standalone project | Ecosystem-level backend selector. Valid values are `SDL3` and `SDL2`; when defined before `recomp_ui.cmake`, it takes precedence and forces `RECOMP_UI_SDL3` to match. |
| `RECOMP_UI_ROOT` | `PATH`, directory containing `recomp_ui.cmake` | Lets a host use a checkout outside `${CMAKE_SOURCE_DIR}/recomp-ui`. Set it before `include()`. |
| `BUILD_TESTING` | Standard CTest `BOOL`, `ON` for the standalone project | Builds recomp-ui's model, asset-staging, and runtime-UI tests. It matters when recomp-ui is the top-level project; consuming games normally own their test policy. |

All of these are configure-time values. CMake caches them in the build
directory, so switching branches or updating the submodule does not reset an
old selection. Pass an explicit `-Dname=value` to change it, use a new build
directory, or remove the single cache entry with `cmake -Uname -S ... -B ...`
when you intentionally want the project's default to run again.

### SDL backend examples and precedence

Standalone recomp-ui build, SDL3 and Mods frontend:

```sh
cmake -G Ninja -S . -B build \
  -DSNESRECOMP_SDL_BACKEND=SDL3 \
  -DRECOMP_UI_ENABLE_MODS=ON
cmake --build build
```

Standalone SDL2 compatibility build:

```sh
cmake -G Ninja -S . -B build-sdl2 \
  -DSNESRECOMP_SDL_BACKEND=SDL2
cmake --build build-sdl2
```

Generic consuming host that does not define `SNESRECOMP_SDL_BACKEND`:

```sh
cmake -S path/to/game -B build -DRECOMP_UI_SDL3:BOOL=ON
```

When `SNESRECOMP_SDL_BACKEND` exists, it wins over
`RECOMP_UI_SDL3`. This lets an ecosystem choose SDL once for the game, runtime,
and launcher instead of allowing the UI submodule to drift to another ABI.
The host still owns finding and linking the matching SDL target; the integration
helper adds recomp-ui's platform/backend sources but intentionally does not
guess how the host obtained SDL.

### Per-target helper arguments

```cmake
recomp_target_launcher_ui(game-target
    CONSOLE psx
    BOXART "${CMAKE_SOURCE_DIR}/art/boxart.tga"
    BOXART_NAME "my-game-boxart.tga"
    PAD "${CMAKE_SOURCE_DIR}/art/pad.tga"
    BRAND "${CMAKE_SOURCE_DIR}/art/brand.tga")
```

| Argument | Meaning |
|---|---|
| `CONSOLE <id>` | Required asset family: `psx`, `snes`, `n64`, `nes`, `gba`, `genesis`, `gb`, or `gbc`. `all` is reserved for an intentional multi-profile target such as the standalone preview. |
| `BOXART <path>` | Optional per-game TGA copied next to the executable. |
| `BOXART_NAME <name>` | Optional destination basename when several targets share one output directory. Pair it with `GameInfo.boxart_path`. |
| `PAD <path>` | Optional controller-art override staged as `assets/img/pad.tga`. |
| `BRAND <path>` | Optional top-left brand override staged as `assets/img/brand_mark.tga`. |
| `HOST_IMGUI` + `IMGUI_DIR <path>` | Reuse Dear ImGui already compiled by the host, avoiding duplicate core/backend symbols. `IMGUI_DIR` must contain `imgui.h` and the backend headers. |

`RECOMP_UI_HOST_IMGUI=ON` plus `RECOMP_UI_HOST_IMGUI_INCLUDE=<path>` remains
available as a global compatibility route for existing hosts. New integrations
should prefer the per-target `HOST_IMGUI IMGUI_DIR ...` arguments.

### Definitions added to the target

`recomp_target_launcher_ui(target ...)` adds definitions with `PRIVATE`
visibility:

| Definition | When | Meaning |
|---|---|---|
| `RECOMP_LAUNCHER` | Always | Lets host source in the same target compile its launcher call sites. |
| `RECOMP_UI_ENABLE_MODS=0` or `1` | Always | Mirrors the CMake Mods option consistently across C and C++ launcher sources. |
| `SDL_MAIN_HANDLED` | Always | Keeps the host's real `main()` as the entry point instead of SDL redirecting it. |
| `LNG_SDL3=1` | SDL3 selected | Chooses SDL3 types, events, and Dear ImGui platform glue. Absence means SDL2. |
| `LNG_GLES2=1`, `IMGUI_IMPL_OPENGL_ES2=1` | Android | Selects the GLES2 renderer path. |

Because the definitions are `PRIVATE`, they do not propagate to libraries that
link the game target. The public provider ABI does not need
`RECOMP_UI_ENABLE_MODS`; a separately built mod manager can always implement
`RecompLauncherCModProvider`. Only the target containing recomp-ui and the
launcher call needs the generated frontend definition.

## Build & self-test

recomp-ui builds a standalone harness that fabricates the same C ABI a real host
passes, so you can iterate on the UI without a game:

```sh
cmake -G Ninja -S . -B build
cmake --build build -j
# Preview a console: LNG_VARIANT = psx | snes | gba | genesis | ...
LNG_VARIANT=genesis ./build/recomp-ui-launcher
```

`LNG_SCRIPT` drives it headless for screenshot regression, e.g.
`LNG_SCRIPT="wait:40;view:settings;shot:out.png;quit"` (see
[`src/launcher_debug.h`](src/launcher_debug.h)).

Requires SDL3 (`find_package(SDL3 CONFIG)`) by default, OpenGL, and a C++17
compiler. Configure with `-DSNESRECOMP_SDL_BACKEND=SDL2` to exercise the
supported SDL2 fallback.

---

## Layout

```
src/common/             launcher core (.c/.h), Dear ImGui backend, C ABI header
src/common/launcher_model.*   UI-toolkit-free view-model (state + behavior)
src/consoles/<id>/      per-console unit: <id>_profile.h (SystemProfile row) +
                        any native bridge (e.g. genesis_binds.c, psx_binds.c)
src/launcher_system.h   aggregates the console units + by-id / infer lookups
src/third_party/        vendored Dear ImGui, stb, tinyfiledialogs
assets/common/          fonts + chrome art shared by every console
assets/consoles/<id>/   per-console art (pad_genesis.tga, pad_gba.tga, ...)
recomp_ui.cmake         reusable recomp_target_launcher_ui() integration helper
docs/ARCHITECTURE.md    composition + inheritance design
CMakeLists.txt          standalone self-test build (recomp-ui-launcher)
```

Adding a console = one directory under `src/consoles/<id>/` (profile row +
optional native bind bridge) registered in `src/launcher_system.h`, a theme in
`src/common/launcher_theme.h`, per-console art under `assets/consoles/<id>/`,
and its `.c` files + assets wired into `recomp_ui.cmake`.

## License

recomp-ui is licensed under the [MIT License](LICENSE). Copyright (c) 2026
Matthew Stanley.

Bundled third-party code (Dear ImGui, stb, tinyfiledialogs) retains its own
licenses under `src/third_party/`.
