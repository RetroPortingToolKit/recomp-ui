#ifndef RECOMP_LAUNCHER_MODULE_CONTRACT_H
#define RECOMP_LAUNCHER_MODULE_CONTRACT_H
#include "recomp_launcher_module.h"

#ifdef __cplusplus
extern "C" {
#endif
RecompLauncherV2Header launcher_v2_header(uint32_t bytes);
RecompLauncherV2Layout launcher_v2_layout(void);
uint32_t launcher_v2_architecture(void);
uint32_t launcher_v2_byte_order(void);
uint32_t launcher_v2_check_header(const RecompLauncherV2Header*, uint32_t bytes);
int launcher_v2_absolute_root(const char*);
void launcher_v2_diagnostic(char*, uint32_t, const char*);
uint32_t launcher_v2_validate_query(const RecompLauncherV2Query*, const RecompLauncherV2SDL*, uint64_t supported, char*, uint32_t);
uint32_t launcher_v2_validate_run(const RecompLauncherV2Run*, const RecompLauncherV2Result*, const RecompLauncherV2SDL*, uint64_t supported, char*, uint32_t);
#ifdef __cplusplus
}
#endif
#endif


/* This optional provider is a C-only host contract. Its complete typed layout
 * participates in the existing records fingerprint when present. */
#if defined(__has_include)
#if __has_include("psx_menu_host.h")
#include "psx_menu_host.h"
#define LAUNCHER_V2_HAS_PSX_MENU 1
#endif
#endif
#ifndef LAUNCHER_V2_HAS_PSX_MENU
#define LAUNCHER_V2_HAS_PSX_MENU 0
#endif
