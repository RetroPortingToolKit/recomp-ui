// snes_pad_binds.c — the SNES shape of the shared per-GUID input.ini store.
// See snes_pad_binds.h for why the button order and defaults are the runner's.

#include "snes_pad_binds.h"
#include "pad_binds.h"

#include <ctype.h>
#include <string.h>

/* Runner order — config.ini [Controls]: Up Down Left Right Select Start
 * A B X Y L R (mmx23_host_main.inc documents it against kKeys_Controls). */
static const char* const kSnesPadKeyName[LNG_SNES_PAD_BUTTON_COUNT] = {
    "up", "down", "left", "right",
    "select", "start",
    "a", "b", "x", "y",
    "l", "r",
};

/* The runner's shipped default line, in SDL gamepad names:
 *   Controls = DpadUp, DpadDown, DpadLeft, DpadRight, Back, Start,
 *              B, A, Y, X, Lb, Rb
 * The face swap is deliberate: SNES A sits where SDL puts b. */
static const char* const kSnesPadDefaults[LNG_SNES_PAD_BUTTON_COUNT] = {
    "dpup", "dpdown", "dpleft", "dpright",
    "back", "start",
    "b", "a", "y", "x",
    "leftshoulder", "rightshoulder",
};

static const char kSnesSeed[] =
    "; snesrecomp input mapping. A SNES button is active when any listed\n"
    "; source is pressed. Per-device overrides live in [mapping.<guid>],\n"
    "; keyed by SDL joystick GUID so each controller keeps its own layout.\n"
    "; [gamepads] lists previously used pads (guid = display name).\n\n";

static const RuiPadSpec kSnesPadSpec = {
    kSnesPadKeyName,
    kSnesPadDefaults,
    LNG_SNES_PAD_BUTTON_COUNT,
    4,                                   /* up/down/left/right lead the table */
    RUI_SNES_PAD_DEFAULT_DEADZONE_PCT,
    0,                                   /* derive: no shipped files to match */
    kSnesSeed,
    0,                                   /* no legacy shape to migrate from */
};

/* Launcher display order (snes_profile.h): Up Down Left Right A B X Y L R
 * Start Select. Store order is the runner's. Neither is wrong; they just are
 * not the same, and mixing them up would rebind the wrong button. */
static const signed char kUiToStore[LNG_SNES_PAD_BUTTON_COUNT] = {
    0,  /* Up     -> up     */
    1,  /* Down   -> down   */
    2,  /* Left   -> left   */
    3,  /* Right  -> right  */
    6,  /* A      -> a      */
    7,  /* B      -> b      */
    8,  /* X      -> x      */
    9,  /* Y      -> y      */
    10, /* L      -> l      */
    11, /* R      -> r      */
    5,  /* Start  -> start  */
    4,  /* Select -> select */
};

/* Legacy config.ini [GamepadMap] spelling -> SDL controller name. Mirrors the
 * runner's ParseGamepadButtonName table (mmx_config.c), including its two
 * aliases for the shoulders. Guide has no SNES button but is accepted so a
 * migration can recognise and skip it rather than call the file malformed. */
static const struct { const char* legacy; const char* sdl; } kLegacyToken[] = {
    { "dpadup",     "dpup"          },   /* runner spells these "DpadUp" etc */
    { "dpaddown",   "dpdown"        },
    { "dpadleft",   "dpleft"        },
    { "dpadright",  "dpright"       },
    { "a",          "a"             },
    { "b",          "b"             },
    { "x",          "x"             },
    { "y",          "y"             },
    { "back",       "back"          },
    { "start",      "start"         },
    { "guide",      "guide"         },
    { "l1",         "leftshoulder"  },
    { "lb",         "leftshoulder"  },   /* runner accepts both spellings */
    { "r1",         "rightshoulder" },
    { "rb",         "rightshoulder" },
    { "l2",         "lefttrigger"   },
    { "r2",         "righttrigger"  },
    { "l3",         "leftstick"     },
    { "r3",         "rightstick"    },
};

const char* rui_snes_pad_binds_legacy_token(const char* legacy) {
    if (!legacy || !legacy[0]) return 0;
    char buf[24];
    size_t n = strlen(legacy);
    if (n >= sizeof(buf)) return 0;
    for (size_t i = 0; i <= n; ++i)
        buf[i] = (char)tolower((unsigned char)legacy[i]);
    for (size_t i = 0; i < sizeof(kLegacyToken) / sizeof(kLegacyToken[0]); ++i)
        if (!strcmp(buf, kLegacyToken[i].legacy)) return kLegacyToken[i].sdl;
    return 0;
}

int rui_snes_pad_binds_index_from_ui(int ui_button) {
    if (ui_button < 0 || ui_button >= LNG_SNES_PAD_BUTTON_COUNT) return -1;
    return kUiToStore[ui_button];
}

void rui_snes_pad_binds_init(const char* path) {
    rui_pad_binds_init(&kSnesPadSpec, path);
}

void rui_snes_pad_binds_label(const char* path, const char* guid, int b,
                              char* out, int cap) {
    rui_pad_binds_label(&kSnesPadSpec, path, guid, b, out, cap);
}

void rui_snes_pad_binds_set(const char* path, const char* guid, int b,
                            int kind, int code, int axis_dir) {
    rui_pad_binds_set(&kSnesPadSpec, path, guid, b, kind, code, axis_dir);
}

void rui_snes_pad_binds_reset(const char* path, const char* guid) {
    rui_pad_binds_reset(&kSnesPadSpec, path, guid);
}

void rui_snes_pad_binds_remember(const char* path, const char* guid,
                                 const char* name, int deadzone_pct) {
    rui_pad_binds_remember(&kSnesPadSpec, path, guid, name, deadzone_pct);
}

void rui_snes_pad_binds_save_profile(const char* path, const char* guid,
                                     const char* name, int name_custom,
                                     int deadzone_pct) {
    rui_pad_binds_save_profile(&kSnesPadSpec, path, guid, name, name_custom,
                               deadzone_pct);
}

void rui_snes_pad_binds_rename(const char* path, const char* guid,
                               const char* name) {
    rui_pad_binds_rename(&kSnesPadSpec, path, guid, name);
}

void rui_snes_pad_binds_delete(const char* path, const char* guid) {
    rui_pad_binds_delete(&kSnesPadSpec, path, guid);
}

int rui_snes_pad_binds_known_count(const char* path) {
    return rui_pad_binds_known_count(&kSnesPadSpec, path);
}

int rui_snes_pad_binds_known_at(const char* path, int index,
                                char* guid, int guid_cap,
                                char* name, int name_cap) {
    return rui_pad_binds_known_at(&kSnesPadSpec, path, index, guid, guid_cap,
                                  name, name_cap);
}

void rui_snes_pad_binds_name(const char* path, const char* guid,
                             char* out, int cap) {
    rui_pad_binds_name(&kSnesPadSpec, path, guid, out, cap);
}

int rui_snes_pad_binds_name_is_custom(const char* path, const char* guid) {
    return rui_pad_binds_name_is_custom(&kSnesPadSpec, path, guid);
}

int rui_snes_pad_binds_deadzone(const char* path, const char* guid) {
    return rui_pad_binds_deadzone(&kSnesPadSpec, path, guid);
}
