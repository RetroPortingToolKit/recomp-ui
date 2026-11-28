// SPDX-License-Identifier: MIT
// PSP identity and capability composition for the shared recomp-ui SystemProfile.
#ifndef RUI_CONSOLE_PSP_PROFILE_H
#define RUI_CONSOLE_PSP_PROFILE_H
#include "launcher_system_types.h"
static const ButtonDef kPspButtons[] = {
    {"Up",0},{"Right",1},{"Down",2},{"Left",3},{"Triangle",4},{"Circle",5},
    {"Cross",6},{"Square",7},{"L",8},{"R",9},{"Start",10},{"Select",11},
};
static const char* const kPspFiles[]={"*.iso","*.prx"};
static const char* const kPspDashboard[]={"game","controller",NULL};
static const char* const kPspSettings[]={"system","audio",NULL};
static const char* const kPspController[]={"controller_config",NULL};
static const SystemProfile kSystemProfilePsp={
    "psp","PSP","psp","Game / VSH",{kPspButtons,12,"",NULL,NULL,1,0},
    {SAVE_NONE,0,NULL}, {0}, {1,NULL,1}, 0,
    kPspDashboard,kPspSettings,kPspController,NULL,0,
    {kPspFiles,2,"PSP ISO / VSH module"},NULL,1,"",NULL,
};
static inline int launcher_console_is_psp(const char* name){return lps_streq_ci(name,"psp");}
static inline void launcher_profile_apply_psp(RecompLauncherCGameInfo* gi){
    gi->theme="psp";gi->platform="PSP";gi->rom_noun="Game / VSH";gi->num_players=1;
}
#endif
