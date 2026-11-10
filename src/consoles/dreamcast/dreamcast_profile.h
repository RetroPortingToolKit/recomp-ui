#ifndef RUI_CONSOLE_DREAMCAST_PROFILE_H
#define RUI_CONSOLE_DREAMCAST_PROFILE_H

#include "launcher_system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

static const ButtonDef kDreamcastPadButtons[] = {
    { "Up", 0 }, { "Down", 1 }, { "Left", 2 }, { "Right", 3 },
    { "A", 4 }, { "B", 5 }, { "X", 6 }, { "Y", 7 },
    { "L Trigger", 8 }, { "R Trigger", 9 }, { "Start", 10 },
    { "Stick Up", 11 }, { "Stick Down", 12 },
    { "Stick Left", 13 }, { "Stick Right", 14 },
};
#define LNG_DREAMCAST_PAD_BUTTON_COUNT \
    ((int)(sizeof(kDreamcastPadButtons) / sizeof(kDreamcastPadButtons[0])))

/* The launcher settings ABI uses the generic keybind vocabulary. The host
 * translates these slots to its Dreamcast bindings, including analog trigger
 * values and the main stick; no PlayStation pad-mode or Select button exists. */
static const int kDreamcastKbIndex[LNG_DREAMCAST_PAD_BUTTON_COUNT] = {
    8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 12, 13, 14, 15,
};
static const char* const kDreamcastDiscPatterns[] = { "*.gdi", "*.chd", "*.cdi" };
static const char* const kPanelsSettingsDreamcast[] = {
    "video", "audio", "system", "input", NULL
};
static const char* const kPanelsDashboardDreamcast[] = {
    "game", "controller", "save", NULL
};

static const SystemProfile kSystemProfileDreamcast = {
    "dreamcast", "SEGA DREAMCAST", NULL, "Disc",
    { kDreamcastPadButtons, LNG_DREAMCAST_PAD_BUTTON_COUNT,
      NULL, NULL, NULL, 4, 0 },
    /* VMU ownership and inspection belong to the host. The PSX memory-card
     * editor/formatter must never be offered for a Dreamcast VMU. */
    { SAVE_MEMCARD, 1, NULL },
    { 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1 },
    { 1, NULL },
    0,
    kPanelsDashboardDreamcast, kPanelsSettingsDreamcast, kPanelsControllerCommon,
    NULL, 0,
    { kDreamcastDiscPatterns, 3, "Dreamcast disc (.gdi, .chd, .cdi)" },
    NULL, 1, "", NULL,
};

static inline int launcher_console_is_dreamcast(const char* name) {
    return lps_streq_ci(name, "dreamcast") || lps_streq_ci(name, "dc");
}

static inline void launcher_profile_apply_dreamcast(RecompLauncherCGameInfo* gi) {
    gi->theme = NULL;
    gi->platform = "SEGA DREAMCAST";
    gi->rom_noun = "Disc";
    gi->num_players = 4;
    gi->has_bios = 1;
    gi->has_deadzone_pct = 1;
    gi->has_supersampling = 1;
    gi->settings_bindings = 1;
    gi->memcard_requires_host = 1;
    gi->memcard_blocks = 200;
    gi->memcard_name = "VMU";
}

#ifdef __cplusplus
}
#endif
#endif
