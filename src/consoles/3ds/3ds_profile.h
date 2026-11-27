#ifndef RUI_CONSOLE_3DS_PROFILE_H
#define RUI_CONSOLE_3DS_PROFILE_H
#include "consoles/nds/nds_profile.h"
static const char* const kThreeDsPatterns[] = { "*.json" };
static const SystemProfile kSystemProfileThreeDs = {
    "3ds", "NINTENDO 3DS", NULL, "Profile",
    { kNdsPadButtons, LNG_NDS_PAD_BUTTON_COUNT, "pad_nds.tga", NULL, NULL, 1, 0 },
    { SAVE_NONE, 0, NULL },
    { 1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0, NULL }, 0, kPanelsDashboardCommon, kPanelsSettingsNds, kPanelsControllerCommon,
    NULL, 0, { kThreeDsPatterns, 1, "Old 3DS profile (.json)" }, NULL, 1, NULL, NULL,
};
static inline int launcher_console_is_3ds(const char* name) {
    return lps_streq_ci(name, "3ds") || lps_streq_ci(name, "nintendo3ds");
}
static inline void launcher_profile_apply_3ds(RecompLauncherCGameInfo* gi) {
    gi->platform = "NINTENDO 3DS"; gi->rom_noun = "Profile"; gi->num_players = 1;
    gi->theme = NULL; gi->linear_filter_hidden = 1; gi->settings_bindings = 1;
}
#endif
