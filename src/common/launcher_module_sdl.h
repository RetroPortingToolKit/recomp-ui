#ifndef RECOMP_LAUNCHER_MODULE_SDL_H
#define RECOMP_LAUNCHER_MODULE_SDL_H
#include "recomp_launcher_module.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Inspect the actual loaded provider, not find_package or an inventory pin.
 * Read-only. Returns zero only after path, file SHA-256, version and two
 * imported symbol addresses are bound to the same shared provider. */
uint32_t launcher_v2_sdl_identity(RecompLauncherV2SDL*, char*, uint32_t);
#ifdef __cplusplus
}
#endif
#endif

