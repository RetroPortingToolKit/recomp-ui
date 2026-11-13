#ifndef RUI_CONSOLE_CDI_PROFILE_H
#define RUI_CONSOLE_CDI_PROFILE_H
#include "launcher_system_types.h"

static const ButtonDef kCdiPadButtons[] = {
    {"Left",0}, {"Up",1}, {"Right",2}, {"Down",3}, {"Button 1",4}, {"Button 2",5}
};
static const char* const kCdiSettingsPanels[] = {"video","audio","system",NULL};
static const char* const kCdiRomPatterns[] = {"*.cue","*.bin"};
static const SystemProfile kSystemProfileCdi = {
    "cdi", "PHILIPS CD-I", "cdi", "Disc",
    {kCdiPadButtons,6,"",NULL,NULL,1,0,1,NULL,0,1},
    {SAVE_NONE,0,NULL},
    {1,1,1,0, 0,0,0,0,1,0,0,0,0,0,1,0},
    {1,NULL}, 0,
    kPanelsDashboardCommon,kCdiSettingsPanels,kPanelsControllerCommon,
    NULL,0,{kCdiRomPatterns,2,"CD-i disc (.cue .bin)"},NULL,1,""
};
static inline int launcher_console_is_cdi(const char* name) {
    return lps_streq_ci(name,"cdi") || lps_streq_ci(name,"cd-i");
}
static inline void launcher_profile_apply_cdi(RecompLauncherCGameInfo* gi) {
    gi->platform="PHILIPS CD-I";
    gi->theme="cdi";
    gi->rom_noun="Disc";
    gi->num_players=1;
    gi->has_bios=1;
    gi->settings_bindings=1;
}
#endif
