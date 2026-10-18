// Original Xbox identity. Hosts provide their binding adapter and assets;
// selecting this profile does not advertise unimplemented runtime features.
#ifndef RUI_CONSOLE_XBOX_PROFILE_H
#define RUI_CONSOLE_XBOX_PROFILE_H

#include "launcher_system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Xbox pad report order: eight digital bits, then eight analog buttons.
// Stick axes remain the host's analog input contract, not digital bindings.
static const ButtonDef kXboxPadButtons[] = {
    { "Up", 0 }, { "Down", 1 }, { "Left", 2 }, { "Right", 3 },
    { "Start", 4 }, { "Back", 5 }, { "Left Stick", 6 }, { "Right Stick", 7 },
    { "A", 8 }, { "B", 9 }, { "X", 10 }, { "Y", 11 },
    { "Black", 12 }, { "White", 13 }, { "Left Trigger", 14 }, { "Right Trigger", 15 },
};
#define LNG_XBOX_PAD_BUTTON_COUNT \
    ((int)(sizeof(kXboxPadButtons) / sizeof(kXboxPadButtons[0])))
static const char* const kXboxExecutablePatterns[] = { "*.xbe" };

static const SystemProfile kSystemProfileXbox = {
    /* identity */ "xbox", "XBOX", "xbox", "XBE",
    /* controller: no substitute art from another console */ {
        kXboxPadButtons, LNG_XBOX_PAD_BUTTON_COUNT, "pad_xbox.tga", NULL, NULL, 4, 0,
    },
    /* HDD saves are host-managed; no foreign SRAM/memory-card UI. */
    { SAVE_NONE, 0, NULL },
    /* Only basic host window/presentation controls by default. */
    { 1, 1, 0, 0, 0,0,0,0,0, 0,0,0,0,0, 0,0 },
    { 0, NULL },
    /* Hotkey capabilities must be supplied by the Xbox host. */ 0,
    kPanelsDashboardCommon, kPanelsSettingsStub, kPanelsControllerCommon,
    NULL, 0,
    { kXboxExecutablePatterns, 1, "Xbox executable (.xbe)" },
    NULL, 0, NULL, NULL,
};

static inline int launcher_console_is_xbox(const char* name) {
    return lps_streq_ci(name, "xbox") || lps_streq_ci(name, "ogxbox");
}

static inline void launcher_profile_apply_xbox(RecompLauncherCGameInfo* gi) {
    gi->theme = "xbox";
    gi->platform = "XBOX";
    gi->rom_noun = "XBE";
    gi->num_players = 4;
}

#ifdef __cplusplus
}
#endif
#endif
