#ifndef RUI_GBA_PAD_BINDS_H
#define RUI_GBA_PAD_BINDS_H
#include "pad_binds.h"
/* Same order as the GBA controller page, shared with the native runner. */
static const char* const kRuiGbaPadKeys[]={"up","down","left","right","a","b","l","r","start","select"};
static const char* const kRuiGbaPadDefaults[]={"dpup","dpdown","dpleft","dpright","a","b","leftshoulder","rightshoulder","start","back"};
static const int kRuiGbaKeyBits[]={6,7,5,4,0,1,9,8,3,2};
static const RuiPadSpec kRuiGbaPadSpec={kRuiGbaPadKeys,kRuiGbaPadDefaults,10,4,20,0,0,0,0};
#endif
