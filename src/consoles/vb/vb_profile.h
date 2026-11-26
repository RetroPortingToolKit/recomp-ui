#ifndef RUI_CONSOLE_VB_PROFILE_H
#define RUI_CONSOLE_VB_PROFILE_H
#include "launcher_system_types.h"
#include "recomp_launcher.h"
static const ButtonDef kVbPadButtons[]={
    {"Left Up",0},{"Left Down",1},{"Left Left",2},{"Left Right",3},
    {"Right Up",4},{"Right Down",5},{"Right Left",6},{"Right Right",7},
    {"A",8},{"B",9},{"L",10},{"R",11},{"Start",12},{"Select",13}
};
static const char* const kVbRomPatterns[]={"*.vb","*.vboy","*.bin"};
static const SystemProfile kSystemProfile_vb={
    "vb","VIRTUAL BOY","vb","ROM",
    {kVbPadButtons,14,"pad_vb.tga",NULL,NULL,4,0,1,NULL,0,1,NULL,0,0,0},
    {SAVE_SRAM,1,NULL},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,NULL},LNG_HOTKEYS_ALL,
    kPanelsDashboardCommon,kPanelsSettingsStub,kPanelsControllerCommon,
    NULL,0,{kVbRomPatterns,3,"Virtual Boy ROM (.vb .vboy .bin)"},
    NULL,1,"brand_vb.tga",NULL
};
static inline int launcher_console_is_vb(const char* name) {
 return lps_streq_ci(name,"vb") || lps_streq_ci(name,"virtualboy");
}
static inline void launcher_profile_apply_vb(RecompLauncherCGameInfo* gi) {
 gi->theme="vb";gi->platform="VIRTUAL BOY";gi->rom_noun="ROM";
}
#endif
