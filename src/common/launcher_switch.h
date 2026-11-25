#ifndef RECOMP_LAUNCHER_SWITCH_H
#define RECOMP_LAUNCHER_SWITCH_H

/* devkitPro SDL2 exposes eight fixed slots, including disconnected pads.
 * Resolve the physical connection and friendly style through libnx. */
#ifdef __SWITCH__
#include <switch.h>
#include <stdio.h>

static inline int launcher_switch_pad(int slot, char *name, size_t capacity)
{
    PadState pad;
    if(slot<0 || slot>=8)return 0;
    if(slot==0)padInitializeDefault(&pad);
    else padInitialize(&pad,(HidNpadIdType)(HidNpadIdType_No1+slot));
    padUpdate(&pad);
    if(!padIsConnected(&pad))return 0;
    const u32 style=padGetStyleSet(&pad);
    const char *label=(style&HidNpadStyleTag_NpadHandheld)?"Attached Joy-Cons":
        (style&HidNpadStyleTag_NpadJoyDual)?"Joy-Con pair":
        (style&HidNpadStyleTag_NpadJoyLeft)?"Left Joy-Con":
        (style&HidNpadStyleTag_NpadJoyRight)?"Right Joy-Con":
        (style&HidNpadStyleTag_NpadLager)?"Sega Genesis controller":
        (style&HidNpadStyleTag_NpadLagon)?"Nintendo 64 controller":"Switch controller";
    if(name && capacity)snprintf(name,capacity,"%s (Player %d)",label,slot+1);
    return 1;
}
#endif
#endif
