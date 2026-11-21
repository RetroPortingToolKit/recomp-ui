// psx_pad_binds.c — the PSX shape of the shared per-GUID input.ini store.
//
// The persistence itself lives in common/pad_binds.c: the file format, the
// GUID registry, deadzones, custom names and the d-pad collision healing are
// identical for every console, and a second copy could not inherit a fix made
// to the first. What is genuinely PSX here is the button table, the defaults,
// and the banner written into a fresh input.ini.
//
// The public API below is unchanged, so psxrecomp's call sites are untouched.

#include "psx_pad_binds.h"
#include "psx_profile.h"
#include "pad_binds.h"

static const char* const kPsxPadKeyName[LNG_PSX_PAD_BUTTON_COUNT] = {
    "up", "down", "left", "right",
    "triangle", "circle", "cross", "square",
    "l1", "l2", "r1", "r2",
    "l3", "r3", "start", "select",
    "ls_up", "ls_down", "ls_left", "ls_right",
    "rs_up", "rs_down", "rs_left", "rs_right",
};

static const char* const kPsxPadDefaults[LNG_PSX_PAD_BUTTON_COUNT] = {
    "dpup", "dpdown", "dpleft", "dpright",
    "y", "b", "a", "x",
    "leftshoulder", "lefttrigger", "rightshoulder", "righttrigger",
    "leftstick", "rightstick", "start", "back",
    "lefty-", "lefty+", "leftx-", "leftx+",
    "righty-", "righty+", "rightx-", "rightx+",
};

/* Files written before the analog stick keys existed carry neither prefix;
 * finding neither means the file predates them and is rewritten. */
static const char* const kPsxLegacyProbe[] = { "ls_", "rs_", 0 };

static const char kPsxSeed[] =
    "; PSXRecomp input mapping. PSX buttons are active when any listed\n"
    "; source is pressed. Per-device overrides live in [mapping.<guid>].\n"
    "; [gamepads] lists previously used pads (guid = display name).\n\n"
    "[controller]\n"
    "enabled = true\n"
    "device = 0\n"
    "deadzone = 3277\n\n";

static const RuiPadSpec kPsxPadSpec = {
    kPsxPadKeyName,
    kPsxPadDefaults,
    LNG_PSX_PAD_BUTTON_COUNT,
    4,                                  /* up/down/left/right lead the table */
    RUI_PSX_PAD_DEFAULT_DEADZONE_PCT,
    9,                                  /* the width shipped files already use */
    kPsxSeed,
    kPsxLegacyProbe,
};

void rui_psx_pad_binds_init(const char* path) {
    rui_pad_binds_init(&kPsxPadSpec, path);
}

void rui_psx_pad_binds_label(const char* path, const char* guid, int b,
                             char* out, int cap) {
    rui_pad_binds_label(&kPsxPadSpec, path, guid, b, out, cap);
}

void rui_psx_pad_binds_set(const char* path, const char* guid, int b,
                           int kind, int code, int axis_dir) {
    rui_pad_binds_set(&kPsxPadSpec, path, guid, b, kind, code, axis_dir);
}

void rui_psx_pad_binds_reset(const char* path, const char* guid) {
    rui_pad_binds_reset(&kPsxPadSpec, path, guid);
}

void rui_psx_pad_binds_remember(const char* path, const char* guid,
                                const char* name, int deadzone_pct) {
    rui_pad_binds_remember(&kPsxPadSpec, path, guid, name, deadzone_pct);
}

void rui_psx_pad_binds_save_profile(const char* path, const char* guid,
                                    const char* name, int name_custom,
                                    int deadzone_pct) {
    rui_pad_binds_save_profile(&kPsxPadSpec, path, guid, name, name_custom,
                               deadzone_pct);
}

void rui_psx_pad_binds_rename(const char* path, const char* guid,
                              const char* name) {
    rui_pad_binds_rename(&kPsxPadSpec, path, guid, name);
}

void rui_psx_pad_binds_delete(const char* path, const char* guid) {
    rui_pad_binds_delete(&kPsxPadSpec, path, guid);
}

int rui_psx_pad_binds_known_count(const char* path) {
    return rui_pad_binds_known_count(&kPsxPadSpec, path);
}

int rui_psx_pad_binds_known_at(const char* path, int index,
                               char* guid, int guid_cap,
                               char* name, int name_cap) {
    return rui_pad_binds_known_at(&kPsxPadSpec, path, index, guid, guid_cap,
                                  name, name_cap);
}

void rui_psx_pad_binds_name(const char* path, const char* guid,
                            char* out, int cap) {
    rui_pad_binds_name(&kPsxPadSpec, path, guid, out, cap);
}

int rui_psx_pad_binds_name_is_custom(const char* path, const char* guid) {
    return rui_pad_binds_name_is_custom(&kPsxPadSpec, path, guid);
}

int rui_psx_pad_binds_deadzone(const char* path, const char* guid) {
    return rui_pad_binds_deadzone(&kPsxPadSpec, path, guid);
}
