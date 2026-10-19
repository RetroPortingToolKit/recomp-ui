#ifndef RECOMP_LAUNCHER_MODULE_CONTRACT_H
#define RECOMP_LAUNCHER_MODULE_CONTRACT_H
#include "recomp_launcher_module.h"

#ifdef __cplusplus
extern "C" {
#endif
RecompLauncherV1Header launcher_v1_header(uint32_t bytes);
RecompLauncherV1Layout launcher_v1_layout(void);
uint32_t launcher_v1_architecture(void);
uint32_t launcher_v1_byte_order(void);
uint32_t launcher_v1_check_header(const RecompLauncherV1Header*, uint32_t bytes);
int launcher_v1_absolute_root(const char*);
void launcher_v1_diagnostic(char*, uint32_t, const char*);
uint32_t launcher_v1_validate_query(const RecompLauncherV1Query*, const RecompLauncherV1SDL*, uint64_t supported, char*, uint32_t);
uint32_t launcher_v1_validate_run(const RecompLauncherV1Run*, const RecompLauncherV1Result*, const RecompLauncherV1SDL*, uint64_t supported, char*, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
