// Wii launcher composition. Hosts own title verification, NAND persistence,
// remote reports and the settings returned through the shared C ABI.
#ifndef RUI_CONSOLE_WII_PROFILE_H
#define RUI_CONSOLE_WII_PROFILE_H

#include "launcher_system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stable host binding indices for a sideways Wii Remote. Motion, pointing
// and extension controls are separate host capabilities, not extra pad buttons.
static const ButtonDef kWiiPadButtons[] = {
    { "Up", 0 }, { "Down", 1 }, { "Left", 2 }, { "Right", 3 },
    { "1", 4 }, { "2", 5 }, { "+", 6 }, { "-", 7 },
    { "A", 8 }, { "B", 9 }, { "HOME", 10 },
};
#define LNG_WII_PAD_BUTTON_COUNT ((int)(sizeof(kWiiPadButtons) / sizeof(kWiiPadButtons[0])))

static const char* const kPanelsSettingsWii[] = { "video", "audio", NULL };
static const char* const kWiiTitlePatterns[] = { "*.app", "*.dol" };

static const SystemProfile kSystemProfileWii = {
    "wii", "NINTENDO WII", NULL, "Game",
    { kWiiPadButtons, LNG_WII_PAD_BUTTON_COUNT,
      "pad_wii.tga", NULL, NULL, 4, 0, 1, NULL, 0, 1 },
    // Original in-game menus manage NAND saves. SRAM and memory-card
    // importers do not describe this persistence format.
    { SAVE_NONE, 0, NULL },
    // Per-title GameInfo capability flags remain authoritative. In particular
    // the profile does not enable widescreen or scaling on an unready host.
    { 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { 0, NULL },
    0, // Hosts expose only the hotkeys their runtime actually consumes.
    kPanelsDashboardCommon, kPanelsSettingsWii, kPanelsControllerCommon,
    NULL, 0,
    { kWiiTitlePatterns, 2, "Wii game (.app, .dol)" },
    NULL, 1, "",
};

static inline int launcher_console_is_wii(const char* name) {
    return lps_streq_ci(name, "wii") || lps_streq_ci(name, "nintendowii");
}

static inline void launcher_profile_apply_wii(RecompLauncherCGameInfo* gi) {
    gi->theme = NULL;
    gi->platform = "NINTENDO WII";
    gi->rom_noun = "Game";
    gi->settings_bindings = 1;
}

#ifdef __cplusplus
}
#endif
#endif
