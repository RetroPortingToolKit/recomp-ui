#ifndef RUI_CONSOLE_VB_PROFILE_H
#define RUI_CONSOLE_VB_PROFILE_H
#include "launcher_system_types.h"
// Logical local seats include game-specific versus grafts. A stock game host
// supplies num_players=1; the profile ceiling alone never enables extra seats.
static const ButtonDef kVbPadButtons[]={
    {"Left Up",0},{"Left Down",1},{"Left Left",2},{"Left Right",3},
    {"Right Up",4},{"Right Down",5},{"Right Left",6},{"Right Right",7},
    {"A",8},{"B",9},{"L",10},{"R",11},{"Start",12},{"Select",13}
};
static const char* const kVbRomPatterns[]={"*.vb","*.vboy","*.bin"};
static const SystemProfile kSystemProfile_vb={
    "vb","VIRTUAL BOY",NULL,"ROM",
    {kVbPadButtons,14,"pad.tga",NULL,NULL,4,0,1,NULL,0,1,NULL,0,0,0},
    {SAVE_SRAM,1,NULL},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,NULL},LNG_HOTKEYS_ALL,
    kPanelsDashboardCommon,kPanelsSettingsStub,kPanelsControllerCommon,
    NULL,0,{kVbRomPatterns,3,"Virtual Boy ROM (.vb .vboy .bin)"}
};
#endif
