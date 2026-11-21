#ifndef RUI_CONSOLE_SATURN_PROFILE_H
#define RUI_CONSOLE_SATURN_PROFILE_H

#include "launcher_system_types.h"

static const ButtonDef kSaturnPadButtons[] = {
    {"Up", 0}, {"Down", 1}, {"Left", 2}, {"Right", 3}, {"Start", 4},
    {"A", 5}, {"B", 6}, {"C", 7}, {"X", 8}, {"Y", 9}, {"Z", 10}, {"L", 11}, {"R", 12},
};
static const char* const kSaturnDiscPatterns[] = {"*.cue"};
static const char* const kPanelsSaturnDashboard[] = {"game", NULL};
static const char* const kPanelsSaturnSettings[] = {"video", "audio", "system", NULL};

static const SystemProfile kSystemProfileSaturn = {
    "saturn", "SEGA SATURN", "saturn", "Disc",
    {kSaturnPadButtons, sizeof(kSaturnPadButtons) / sizeof(*kSaturnPadButtons), "", NULL, NULL, 2, 0},
    {SAVE_NONE, 0, NULL},
    {1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0},
    {1, NULL, 1},
    0,
    kPanelsSaturnDashboard, kPanelsSaturnSettings, kPanelsControllerCommon,
    NULL, 0, {kSaturnDiscPatterns, 1, "Sega Saturn disc image (.cue)"},
    NULL, 1, "", NULL,
};

static inline int launcher_console_is_saturn(const char* name) {
    return lps_streq_ci(name, "saturn") || lps_streq_ci(name, "sega saturn");
}

static inline void launcher_profile_apply_saturn(RecompLauncherCGameInfo* game) {
    game->platform = kSystemProfileSaturn.platform;
    game->theme = kSystemProfileSaturn.theme;
    game->rom_noun = kSystemProfileSaturn.rom_noun;
    game->num_players = 2;
    game->has_bios = 1;
    game->lock_device = 1;
}

#endif
