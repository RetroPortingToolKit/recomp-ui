#ifndef RUI_SMSGG_PROFILE_H
#define RUI_SMSGG_PROFILE_H
#include "launcher_system_types.h"
static const ButtonDef kSmsggButtons[]={ {"Up",0},{"Down",1},{"Left",2},{"Right",3},{"Button 1",4},{"Button 2",5},{"Start / Pause",6} };
static const char* const kSmsggPatterns[]={"*.sms","*.gg"};
static const char* const kSmsggSettings[]={"video","audio",NULL};
static const SystemProfile kSystemProfile_smsgg={
 "smsgg","MASTER SYSTEM",NULL,"ROM",
 {kSmsggButtons,7,"",NULL,NULL,1,0,1,NULL,0,1},
 {SAVE_NONE,0,NULL},
 {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
 {0,NULL},0,kPanelsDashboardCommon,kSmsggSettings,kPanelsControllerCommon,
 NULL,0,{kSmsggPatterns,2,"Master System / Game Gear ROM"},NULL,1,""
};
#endif
