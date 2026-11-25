#ifndef RUI_GBA_PAD_BINDS_H
#define RUI_GBA_PAD_BINDS_H
#include "pad_binds.h"
#define RUI_GBA_PAD_BIND_SLOTS 2
/* Same order as the GBA controller page, shared with the native runner. */
/* The first ten keys retain the original file format and defaults. Alternate
 * sources start unbound and use the same per-controller store. */
enum { RUI_GBA_PAD_ACTIONS = 10, RUI_GBA_PAD_SOURCES = 20 };
static const char* const kRuiGbaPadKeys[]={"up","down","left","right","a","b","l","r","start","select",
    "up_alt","down_alt","left_alt","right_alt","a_alt","b_alt","l_alt","r_alt","start_alt","select_alt"};
static const char* const kRuiGbaPadDefaults[]={"dpup","dpdown","dpleft","dpright","a","b","leftshoulder","rightshoulder","start","back",
    "","","","","","","","","",""};
static const int kRuiGbaKeyBits[]={6,7,5,4,0,1,9,8,3,2};
static const RuiPadSpec kRuiGbaPadSpec={kRuiGbaPadKeys,kRuiGbaPadDefaults,RUI_GBA_PAD_SOURCES,4,20,0,0,0,0};
#endif
