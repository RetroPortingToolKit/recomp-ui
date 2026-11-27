// SPDX-License-Identifier: MIT
// Vita host profile. Native savedata and touch/motion are host-owned.
#ifndef RUI_CONSOLE_PSVITA_PROFILE_H
#define RUI_CONSOLE_PSVITA_PROFILE_H
#include "launcher_system_types.h"
static const ButtonDef kVitaPadButtons[] = {
    {"Up",0},{"Down",1},{"Left",2},{"Right",3},
    {"Cross",4},{"Circle",5},{"Square",6},{"Triangle",7},
    {"L",8},{"R",9},{"Start",10},{"Select",11},
    {"Left stick up",12},{"Left stick down",13},{"Left stick left",14},{"Left stick right",15},
    {"Right stick up",16},{"Right stick down",17},{"Right stick left",18},{"Right stick right",19},
};
static const char* const kVitaImagePatterns[]={"*.vpk","*.zip","*.rar","*.pup"};
static const char* const kVitaDashboard[]={"game","controller",NULL};
static const char* const kVitaSettings[]={"video","audio",NULL};
static const SystemProfile kSystemProfileVita = {
    "psvita","PLAYSTATION VITA",NULL,"Backup",
    {kVitaPadButtons,20,"pad_vita.tga",NULL,NULL,1,0,1},
    {SAVE_NONE,0,NULL},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,NULL,1},
    0,kVitaDashboard,kVitaSettings,kPanelsControllerCommon,
    NULL,0,
    {kVitaImagePatterns,4,"Vita original backup or firmware (.vpk .zip .rar .pup)"},
    NULL,1,"",NULL,
};
static inline int launcher_console_is_psvita(const char* n) {
    return lps_streq_ci(n,"psvita")||lps_streq_ci(n,"vita")||lps_streq_ci(n,"playstationvita");
}
static inline void launcher_profile_apply_psvita(RecompLauncherCGameInfo* gi) {
    gi->theme=NULL;gi->platform="PLAYSTATION VITA";gi->rom_noun="Backup";
    gi->num_players=1;gi->settings_bindings=1;
    gi->has_deadzone_pct=1;gi->has_window_size=0;
    gi->pad_mode_supported=0;gi->widescreen_supported=0;
}
#endif
