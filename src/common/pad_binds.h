// common/pad_binds.h — per-GUID gamepad bind persistence (input.ini), shared.
//
// The file format and every rule around it are console-independent:
//
//   [gamepads]           guid = display name        (the registry)
//   [mapping]            button = SDL source        (global fallback)
//   [mapping.<guid>]     button = SDL source, plus deadzone and name_custom
//
// Identity is the SDL joystick GUID; the display name is only a label. A
// user-chosen name (Rename Gamepad) wins over the driver name until cleared.
//
// What differs between consoles is a button table and a file banner, nothing
// else — so that is all a console supplies. This module was extracted from
// consoles/psx/psx_pad_binds.c when SNES needed the same behaviour: a second
// copy would have been ~500 lines of ini machinery that could not inherit a
// fix made to the first (see the workspace rules on re-implemented rules).
//
// Every console unit is compiled into every launcher binary, so PSX and SNES
// stores coexist at runtime. State is therefore keyed by spec pointer, not
// held in file statics.

#ifndef RUI_COMMON_PAD_BINDS_H
#define RUI_COMMON_PAD_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

#define RUI_PAD_MAX_KNOWN   16   // GUID profiles remembered per store
#define RUI_PAD_MAX_BUTTONS 24   // PSX (24) is the widest console; SNES uses 12
#define RUI_PAD_SRC_CAP     48
#define RUI_PAD_GUID_CAP    40
#define RUI_PAD_NAME_CAP    64

// One console's shape. All pointers must outlive the process (static const).
typedef struct RuiPadSpec {
    // input.ini key for each button, in the console's own button order.
    // `count` entries, lowercase, <= RUI_PAD_MAX_BUTTONS.
    const char* const* key_names;
    // Default SDL gamepad source per button ("a", "dpup", "lefty-", ...).
    const char* const* defaults;
    int count;
    // How many CONSECUTIVE entries starting at `cardinal_first` are d-pad
    // cardinals (up/down/left/right). Those get collision healing; 0 disables
    // it. See heal_cardinals().
    int cardinal_count;
    int default_deadzone_pct;
    // Width of the key column when writing. 0 = derive from the longest key.
    // PSX pins 9 because that is what its shipped files already use, and a
    // width change would rewrite every line of an existing input.ini.
    int key_field_width;
    // Written verbatim when the file does not exist yet. May be NULL.
    const char* file_seed;
    // NULL-terminated key prefixes that must be present in an existing file;
    // if none is found the file is rewritten to pick up newer keys. NULL
    // disables the check. (PSX uses this to migrate files written before the
    // analog stick keys existed.)
    const char* const* legacy_probe_prefixes;
    // Index of the FIRST cardinal in key_names[]. Zero (the fill of every
    // positional initializer written before this existed) keeps PSX and SNES
    // on their leading Up/Down/Left/Right block. N64's table is PSR's N64Input
    // order, where the d-pad starts at index 4 — an offset, not a reordering:
    // that identity order is what n64_binds.c and the host both address by.
    int cardinal_first;
} RuiPadSpec;

// Load `path` (or seed defaults). Forces a re-read even if already loaded.
void rui_pad_binds_init(const RuiPadSpec* spec, const char* path);

// Display label for `guid`'s button `b`. Falls back to global [mapping] then
// the spec defaults. Does NOT create a GUID section.
void rui_pad_binds_label(const RuiPadSpec* spec, const char* path,
                         const char* guid, int b, char* out, int cap);

// Rebind button `b` for `guid`. kind: 0 none, 1 button, 2 axis.
void rui_pad_binds_set(const RuiPadSpec* spec, const char* path,
                       const char* guid, int b, int kind, int code,
                       int axis_dir);

// Reset one GUID's sources to spec defaults (keeps name and deadzone).
void rui_pad_binds_reset(const RuiPadSpec* spec, const char* path,
                         const char* guid);

// Ensure a GUID profile exists. `name` is the driver name when not custom;
// never clears an existing custom name. deadzone_pct < 0 leaves it unchanged.
void rui_pad_binds_remember(const RuiPadSpec* spec, const char* path,
                            const char* guid, const char* name,
                            int deadzone_pct);

// Full profile save: name, custom flag, deadzone and current mappings.
void rui_pad_binds_save_profile(const RuiPadSpec* spec, const char* path,
                                const char* guid, const char* name,
                                int name_custom, int deadzone_pct);

// Set a user-chosen display name (marks name_custom).
void rui_pad_binds_rename(const RuiPadSpec* spec, const char* path,
                          const char* guid, const char* name);

// Remove a GUID's mapping and registry entry.
void rui_pad_binds_delete(const RuiPadSpec* spec, const char* path,
                          const char* guid);

int  rui_pad_binds_known_count(const RuiPadSpec* spec, const char* path);
int  rui_pad_binds_known_at(const RuiPadSpec* spec, const char* path,
                            int index, char* guid, int guid_cap,
                            char* name, int name_cap);

void rui_pad_binds_name(const RuiPadSpec* spec, const char* path,
                        const char* guid, char* out, int cap);
int  rui_pad_binds_name_is_custom(const RuiPadSpec* spec, const char* path,
                                  const char* guid);

// Deadzone percent (0..100); spec->default_deadzone_pct when unknown.
int  rui_pad_binds_deadzone(const RuiPadSpec* spec, const char* path,
                            const char* guid);

// Resolve `file` (e.g. "input.ini") in the directory holding `ref` — the
// console's own bind file, whose path the launcher already owns. Falls back to
// a bare `file` when `ref` names no directory. One implementation because the
// host process has to reach the same file the launcher wrote, and a second
// copy of the path rule is a second place for the two to disagree.
void rui_pad_binds_sibling_path(const char* ref, const char* file,
                                char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif // RUI_COMMON_PAD_BINDS_H
