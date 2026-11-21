// n64_pad_binds.c — the N64 shape of the shared per-GUID input.ini store.
// See n64_pad_binds.h for why this exists alongside n64_binds.c.
//
// The persistence itself lives in common/pad_binds.c: the file format, the
// GUID registry, deadzones, custom names and the d-pad collision healing are
// identical for every console, and a second copy could not inherit a fix made
// to the first. What is genuinely N64 here is the button table, the defaults,
// and the banner written into a fresh input.ini.

#include "n64_pad_binds.h"
#include "n64_profile.h"          // LNG_N64_PAD_BUTTON_COUNT (rebind-spec order)
#include "pad_binds.h"

// kN64PadButtons order == PSR's N64Input enum order. Lowercase, and spelled
// the way input.cfg spells the same inputs (DPAD_UP -> dpad_up) so a reader
// who knows one file can read the other.
static const char* const kN64PadKeyName[LNG_N64_PAD_BUTTON_COUNT] = {
    "a", "b", "z", "start",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "l", "r",
    "c_up", "c_down", "c_left", "c_right",
    "stick_up", "stick_down", "stick_left", "stick_right",
};

// PSR's reset_defaults_locked() controller table, transcribed into SDL source
// names (consoles/n64/n64_binds.c kN64DefaultsPad): bumpers for L/R, a trigger
// for Z, the right stick for the C cluster, the left stick for the analog
// stick. Axes carry an explicit sign because a direction is half an axis.
//
// Z is the one place this store cannot say everything input.cfg says: PSR
// binds BOTH triggers to Z across its two alternate slots, and a per-GUID
// mapping keeps one source per button. The left trigger is kept, which is the
// slot-0 bind — so the default that survives is the primary one, not a guess.
static const char* const kN64PadDefaults[LNG_N64_PAD_BUTTON_COUNT] = {
    "a", "b", "lefttrigger+", "start",
    "dpup", "dpdown", "dpleft", "dpright",
    "leftshoulder", "rightshoulder",
    "righty-", "righty+", "rightx-", "rightx+",
    "lefty-", "lefty+", "leftx-", "leftx+",
};

static const char kN64Seed[] =
    "; n64lle input mapping. An N64 input is active when its listed source is\n"
    "; pressed. Per-device overrides live in [mapping.<guid>], keyed by SDL\n"
    "; joystick GUID so each controller keeps its own layout.\n"
    "; [gamepads] lists previously used pads (guid = display name).\n"
    "; Keyboard binds are NOT here — they stay in the runners' own input.cfg.\n\n";

static const RuiPadSpec kN64PadSpec = {
    kN64PadKeyName,
    kN64PadDefaults,
    LNG_N64_PAD_BUTTON_COUNT,
    4,                                   /* four d-pad cardinals ...          */
    RUI_N64_PAD_DEFAULT_DEADZONE_PCT,
    0,                                   /* derive: no shipped files to match */
    kN64Seed,
    0,                                   /* no legacy shape to migrate from   */
    4,                                   /* ... starting at dpad_up, index 4  */
};

void rui_n64_pad_binds_path(const char* bind_cfg_path, char* out, int cap) {
    rui_pad_binds_sibling_path(bind_cfg_path, "input.ini", out, cap);
}

void rui_n64_pad_binds_init(const char* path) {
    rui_pad_binds_init(&kN64PadSpec, path);
}

void rui_n64_pad_binds_source(const char* path, const char* guid, int b,
                              char* out, int cap) {
    rui_pad_binds_label(&kN64PadSpec, path, guid, b, out, cap);
}

void rui_n64_pad_binds_set(const char* path, const char* guid, int b,
                           int kind, int code, int axis_dir) {
    rui_pad_binds_set(&kN64PadSpec, path, guid, b, kind, code, axis_dir);
}

void rui_n64_pad_binds_reset(const char* path, const char* guid) {
    rui_pad_binds_reset(&kN64PadSpec, path, guid);
}

void rui_n64_pad_binds_remember(const char* path, const char* guid,
                                const char* name, int deadzone_pct) {
    rui_pad_binds_remember(&kN64PadSpec, path, guid, name, deadzone_pct);
}

void rui_n64_pad_binds_save_profile(const char* path, const char* guid,
                                    const char* name, int name_custom,
                                    int deadzone_pct) {
    rui_pad_binds_save_profile(&kN64PadSpec, path, guid, name, name_custom,
                               deadzone_pct);
}

void rui_n64_pad_binds_rename(const char* path, const char* guid,
                              const char* name) {
    rui_pad_binds_rename(&kN64PadSpec, path, guid, name);
}

void rui_n64_pad_binds_delete(const char* path, const char* guid) {
    rui_pad_binds_delete(&kN64PadSpec, path, guid);
}

int rui_n64_pad_binds_known_count(const char* path) {
    return rui_pad_binds_known_count(&kN64PadSpec, path);
}

int rui_n64_pad_binds_known_at(const char* path, int index,
                               char* guid, int guid_cap,
                               char* name, int name_cap) {
    return rui_pad_binds_known_at(&kN64PadSpec, path, index, guid, guid_cap,
                                  name, name_cap);
}

void rui_n64_pad_binds_name(const char* path, const char* guid,
                            char* out, int cap) {
    rui_pad_binds_name(&kN64PadSpec, path, guid, out, cap);
}

int rui_n64_pad_binds_name_is_custom(const char* path, const char* guid) {
    return rui_pad_binds_name_is_custom(&kN64PadSpec, path, guid);
}

int rui_n64_pad_binds_deadzone(const char* path, const char* guid) {
    return rui_pad_binds_deadzone(&kN64PadSpec, path, guid);
}
