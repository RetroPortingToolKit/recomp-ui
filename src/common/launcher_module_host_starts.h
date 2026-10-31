#ifndef RECOMP_LAUNCHER_MODULE_HOST_STARTS_H
#define RECOMP_LAUNCHER_MODULE_HOST_STARTS_H
#include "recomp_launcher_module.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host owns the snapshot/scanner/retail formats. Module receives only typed
 * records and caller-owned pixel buffers. Explicit product/save roots. */
const RecompLauncherV2StartProvider* launcher_v2_host_starts(const char*, const char*);
void launcher_v2_host_starts_clear(void);
#ifdef __cplusplus
}
#endif
#endif

