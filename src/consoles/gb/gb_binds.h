// consoles/gb/gb_binds.h — the Game Boy-native bind persistence bridge.
//
// gb-recompiled's runtime drives input from g_keyboard_bindings, persisted in
// runtime_prefs.ini as FLAT keys (runtime/src/platform_sdl.cpp):
//
//     keyboard.<action>.<slot> = key:<SDL_Scancode as decimal int>
//
// where <action> is the engine's input_action_config_name() spelling
// (up/down/left/right/a/b/select/start/…) and <slot> is 0 (primary) or 1
// (secondary). The launcher edits BOTH slots: the page draws a chip per slot,
// so the secondary defaults (WASD / J / K, RShift) are visible and clearable
// instead of silently driving an action the player thinks they unbound.
// NOTE: the older keybinds.ini
// [controls] file (runtime/src/keybinds.c) is vestigial — it is initialized
// but never consulted for input — so this bridge writes runtime_prefs.ini,
// the file that actually drives the game.
//
// Like PSX and Genesis, the Game Boy family gets a native bridge (rather than
// the generic keybinds.c store) because gb-recompiled's key format is its own.
// Single player (the Game Boy is a one-player handheld). Button indices are
// the kGbPadButtons rebind-spec order (gb_profile.h: Up/Down/Left/Right/A/B/
// Start/Select); the bridge maps each to its <action> name. All writes are
// SURGICAL: only the eight `keyboard.<btn>.0` lines are replaced/inserted;
// every other byte of runtime_prefs.ini (audio.*, game.*, the secondary and
// hotkey keyboard.* lines, controller.*) is preserved. The `path` every call
// takes is the resolved runtime_prefs.ini path (launcher_binds.c owns path
// resolution; the seam points it at the exe-anchored runtime_prefs.ini).
//
// ONE KEY, ONE ACTION. Setting a key clears it from every other button/slot of
// this store, and loading a file that already binds one key twice drops the
// duplicate (the non-default claim is kept, else slot 0, else the lower button
// index). gb-recompiled applies the same rule in platform_sdl.cpp, so the two
// halves of the rebinding UI cannot disagree about who owns a key.

#ifndef RUI_CONSOLE_GB_BINDS_H
#define RUI_CONSOLE_GB_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

// Seed the in-memory store with gb-recompiled's set_default_input_bindings()
// defaults (both slots), then overlay whatever keyboard.<btn>.<slot> lines
// exist in `path` and drop any key bound twice. Does NOT write the file (an
// absent file means the game's own defaults already agree; the first rebind
// creates the lines).
void rui_gb_binds_init(const char* path);

// Slot count this store keeps per button; matches gb_profile.h's
// ControllerSpec.binds_per_input, which is what the page draws.
#define RUI_GB_BIND_SLOTS 2

// Current keyboard binding (SDL_Scancode as int; 0 = unbound) for rebind-spec
// button b (0..7), slot 0 (primary) or 1 (secondary). Auto-initializes from
// `path` on first use.
int  rui_gb_binds_get_slot(const char* path, int b, int slot);

// Primary slot, for callers that predate the secondary one.
int  rui_gb_binds_get(const char* path, int b);

// Rebind one slot + persist (surgical runtime_prefs.ini upsert of the sixteen
// keyboard.<btn>.<slot> lines, preserving every other line). The scancode is
// taken away from every other button/slot first, so one key drives exactly one
// action. Mouse pseudo-scancodes (>= RUI_GB_MOUSE_SC_BASE, which other consoles'
// stores accept) are rejected: gb-recompiled's binding_is_valid() would drop
// them on load, which would look like the rebind silently failing.
void rui_gb_binds_set_slot(const char* path, int b, int slot, int scancode);

// Primary slot, for callers that predate the secondary one.
void rui_gb_binds_set(const char* path, int b, int scancode);

// Reset all buttons, both slots, to gb-recompiled's defaults + persist.
void rui_gb_binds_reset(const char* path);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_GB_BINDS_H
