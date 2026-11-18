#ifndef RUI_CONSOLE_3DO_PROFILE_H
#define RUI_CONSOLE_3DO_PROFILE_H
#include "launcher_system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

static const ButtonDef k3doPadButtons[] = {
    {"Up", 0}, {"Down", 1}, {"Left", 2}, {"Right", 3},
    {"A", 4}, {"B", 5}, {"C", 6}, {"L", 7}, {"R", 8},
    {"Play / Pause", 9}, {"Stop", 10}
};
#define LNG_3DO_PAD_BUTTON_COUNT 11
/* Generic keybinds.ini slots: directions, A/B/X, L/R, Start/Select. */
static const int k3doKbIndex[LNG_3DO_PAD_BUTTON_COUNT] = {8,9,10,11,0,1,2,4,5,6,7};
static const char* const kPanelsSettings3do[] = {"video", "audio", "system", "hotkeys", NULL};
static const char* const k3doDiscPatterns[] = {"*.chd"};
static const SystemProfile kSystemProfile3do = {
    "3do", "3DO", NULL, "Disc",
    {k3doPadButtons, LNG_3DO_PAD_BUTTON_COUNT, "", NULL, NULL, 1, 0},
    {SAVE_SRAM, 1, NULL},
    {1,1,1,0, 0,0,0,0,1, 0,0,0,0,0,1,0},
    {0, NULL},
    /* Runtime implements pause/reset/volume/window resizing; renderer and
       SNES attract-loop shortcuts do not describe the 3DO hardware path. */
    (uint32_t)((1u << LNG_HK_FULLSCREEN) | (1u << LNG_HK_RESET) | (1u << LNG_HK_PAUSE) |
               (1u << LNG_HK_WINDOW_BIGGER) | (1u << LNG_HK_WINDOW_SMALLER) |
               (1u << LNG_HK_VOLUME_UP) | (1u << LNG_HK_VOLUME_DOWN)),
    kPanelsDashboardCommon, kPanelsSettings3do, kPanelsControllerCommon,
    NULL, 0, {k3doDiscPatterns, 1, "3DO disc image (.chd)"},
    NULL, 1, "", NULL
};
static inline int launcher_console_is_3do(const char* name) {
    return lps_streq_ci(name, "3do");
}
static inline void launcher_profile_apply_3do(RecompLauncherCGameInfo* gi) {
    gi->theme = NULL;
    gi->platform = "3DO";
    gi->rom_noun = "Disc";
    gi->num_players = 1;
    gi->has_bios = 1;
    gi->aspect_mask = 1;
}
#ifdef __cplusplus
}
#endif
#endif
