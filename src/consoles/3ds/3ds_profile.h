#ifndef RUI_CONSOLE_3DS_PROFILE_H
#define RUI_CONSOLE_3DS_PROFILE_H
#include "launcher_system_types.h"

/* Generic binding indices. Hosts translate them to their own HID layout.
 * Original 3DS: one Circle Pad, without C-stick/ZL/ZR controls. */
static const ButtonDef k3dsPadButtons[] = {
    {"Up",0}, {"Down",1}, {"Left",2}, {"Right",3},
    {"A",4}, {"B",5}, {"X",6}, {"Y",7},
    {"Start",8}, {"Select",9}, {"L",10}, {"R",11},
    {"Circle Pad Up",12}, {"Circle Pad Down",13},
    {"Circle Pad Left",14}, {"Circle Pad Right",15}
};
static const char* const k3dsSettingsPanels[] = {"video","audio",NULL};
static const char* const k3dsRomPatterns[] = {"*.3ds","*.cci"};

/* Common chrome and text branding; hosts may supply artwork. Enhancements,
 * firmware import and touch layouts are host-owned capabilities. */
static const SystemProfile kSystemProfile3ds = {
    "3ds", "NINTENDO 3DS", NULL, "Cartridge",
    {k3dsPadButtons,16,"",NULL,NULL,1,0,0,NULL,0,0},
    {SAVE_NONE,0,NULL},
    {1,1,0,0, 0,0,0,0,0,0,0,0,0,0,0,0},
    {0,NULL}, 0,
    kPanelsDashboardCommon,k3dsSettingsPanels,kPanelsControllerCommon,
    NULL,0,{k3dsRomPatterns,2,"Nintendo 3DS cartridge (.3ds .cci)"},NULL,1,""
};
static inline int launcher_console_is_3ds(const char* name) {
    return lps_streq_ci(name,"3ds") || lps_streq_ci(name,"nintendo3ds") || lps_streq_ci(name,"ctr");
}
static inline void launcher_profile_apply_3ds(RecompLauncherCGameInfo* gi) {
    gi->platform="NINTENDO 3DS"; gi->theme=NULL; gi->rom_noun="Cartridge";
    gi->num_players=1; gi->has_bios=0;
}
#endif
