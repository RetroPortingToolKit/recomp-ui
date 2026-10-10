# Runtime UI architecture

The in-game menu is cross-platform at the model and design-system layers. It
does not assume a console, emulator core, game configuration structure, window
library, or GPU API.

## Ownership

`recomp-ui` owns:

- item/section descriptors and callbacks;
- navigation, selection, enabled state, status, and persistence requests;
- the shared `LauncherTheme` design tokens;
- a modern active-context ImGui presentation;
- a compact ARGB8888 framebuffer fallback.

The game host owns:

- translating SDL/native events into semantic menu inputs;
- applying settings to its live renderer, audio, and emulated machine;
- config-file format and persistence;
- pause/resume policy and input suppression;
- beginning/submitting the renderer frame.

That boundary is necessary: recomp-ui cannot safely pause an N64 recomp, reset
a GBA core, or recreate a Genesis renderer without the host's lifecycle rules.

## Presentation choices

| Host shape | Presentation | Notes |
|---|---|---|
| Existing runtime ImGui (for example RT64) | `recomp_runtime_ui_render_imgui` | Preferred; host owns context and draw submission. |
| GPU host that can add an ImGui frame | `recomp_runtime_ui_render_imgui` | Preferred; use the existing recomp-ui ImGui dependency. |
| SDL2 `SDL_Renderer` host | `recomp_runtime_ui_render_imgui` + `imgui_impl_sdlrenderer2` | Use `recomp_target_runtime_ui_sdlrenderer2()` once on the runtime target. |
| Writable low-resolution CPU framebuffer | `recomp_runtime_ui_render_argb8888` | Compatibility fallback; compact by design. |
| Different UI toolkit | Implement a presentation over the shared model | Do not duplicate settings behavior or console policy. |

The ImGui presentation is responsive: wide windows show persistent section
navigation beside the selected page; narrow windows use a drill-in hierarchy.
Both use the same palette, spacing, radius, and semantic colors as the launcher.
An N64 host therefore gets the clean graphite/red/blue N64 theme with no CRT
scanlines; SNES uses its console theme; handheld themes remain flat rather than
pretending to be a CRT.

Physically small, high-density touch devices are an explicit host opt-in rather
than an operating-system guess. Set
`RecompRuntimeUiConfig.presentation_flags` to include
`RECOMP_RUNTIME_UI_PRESENTATION_TOUCH_FRIENDLY`. The ImGui presentation then
uses almost the full drawable, scales rows and spacing from the short display
axis, widens the section rail and scroll grab, replaces desktop key hints with
touch copy, and gives stepped values and Resume phone-sized hit targets. A zero
flag retains the desktop/TV geometry byte-for-byte, including on platforms that
also happen to support touch.

The vendored SDL_Renderer2 backend is the official backend from the matching
Dear ImGui `v1.91.9b` release. It requires SDL 2.0.17 or newer. A host using an
SDL logical presentation size must temporarily render the ImGui frame at the
renderer output size, then restore its logical size and viewport before the
next game frame. Event routing remains the host's responsibility through the
shared SDL2 platform backend.

`recomp_runtime_ui_set_backdrop(ui, dim, opacity)` lets the host expose how
much the menu covers the game (how dark the game behind it gets, and how solid
the panel is), so a player can see a picture setting's effect through it.
`recomp_runtime_ui_set_status(ui, text)` shows a message in the footer, e.g. a
slot summary from a save-state action (return 0 from its `run_action` so
"Done" does not replace it).

An action that is hard to undo -- restart, quit, return to the launcher --
should take a second press. `recomp_runtime_ui_confirm(ui, item, prompt)` is
that press: call it from `run_action` and return 0 while it returns 0. The
first press puts `prompt` in the footer; pressing the same row again while it
is still up confirms. Any other status, another row's prompt, or closing the
menu withdraws it, and a held button never confirms. The standard
`RECOMP_RUNTIME_UI_STANDARD_RETURN_TO_LAUNCHER` and
`RECOMP_RUNTIME_UI_STANDARD_QUIT` rows do this themselves: the host's
`run_action` only sees the confirmed press, for
`RECOMP_RUNTIME_UI_KEY_RETURN_TO_LAUNCHER` / `RECOMP_RUNTIME_UI_KEY_QUIT`.

`recomp_runtime_ui_set_toast(ui, title, body)` shows a small notice near the
top of the display with or without the menu -- the answer to a host shortcut,
such as what a drive holds after its button was pressed. The model keeps no
clock: the host sets, refreshes and clears it (`NULL, NULL`) and decides what
a press means while one is up. Both presentations draw it; like the menu it
is presentation only, so a host captures the game's own picture before
drawing either.

Selection is the model's alone: the ImGui window takes no keyboard/gamepad
navigation of its own (`ImGuiWindowFlags_NoNavInputs`, except while a text row
is edited), or Enter / A would also activate the row under ImGui's own cursor.

## What “every recomp-ui game” requires

Consuming recomp-ui is the anchor, but updating the git submodule alone cannot
inject an in-game frame into an arbitrary host loop. Universal rollout needs
one small adapter for each host/runtime family, not one implementation per
game:

1. create the shared runtime model with that game's supported settings;
2. route menu input before emulated input;
3. call the appropriate presentation in the host's render frame;
4. pause or continue simulation according to that host's policy;
5. apply and persist values through callbacks.

Games built from the same host template should share that adapter. Per-game
code should be limited to capabilities, labels/choices, and live-setting
callbacks. Console detection must never select behavior in the runtime core;
the system id selects design tokens only.

## Portability rules

- Do not add console-specific keys or settings to `recomp_runtime_ui.c`.
- Do not make the ARGB8888 fallback the required render path.
- Do not create or destroy a host's ImGui context in
  `recomp_runtime_ui_render_imgui`.
- Use the same choice arrays for pre-boot and runtime settings when both expose
  a value.
- The shared `save` callback follows successful value changes. Actions own any
  persistence they require and do not implicitly rewrite the settings file.
- Keep input semantic (`ACCEPT`, `BACK`, directions); hosts choose physical
  buttons and supply their prompt labels.
- Prefer UTF-8-capable modern presentation; the compact fallback is ASCII-only.
- A renderer/backend switch that cannot be applied live should remain in the
  pre-boot launcher until the host provides a safe restart workflow.
