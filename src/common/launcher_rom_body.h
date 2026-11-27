#ifndef RUI_LAUNCHER_ROM_BODY_H
#define RUI_LAUNCHER_ROM_BODY_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t launcher_rom_header_bytes(const char* system,
                                               const uint8_t* bytes,size_t count) {
    /* 3DS cartridges and HOME profile manifests bind every selected byte.
     * Their sector alignment is not a SNES copier-header indication. */
    if(system && strcmp(system,"3ds")==0)return 0;
    if(count>16 && bytes[0]=='N' && bytes[1]=='E' && bytes[2]=='S' && bytes[3]==0x1a)return 16;
    return count%1024==512?512:0;
}
#endif
