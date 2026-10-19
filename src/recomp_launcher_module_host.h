#ifndef RECOMP_LAUNCHER_MODULE_HOST_H
#define RECOMP_LAUNCHER_MODULE_HOST_H
#include "recomp_launcher_module.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Host-only compiled adapter. Set absolute roots before each boot/rematch
 * call. This does not change the process environment or working directory.
 * Call from the owning UI thread, outside an active blocking launcher run. */
int recomp_launcher_module_set_roots(const char* product, const char* saves, const char* assets);
/* Read the typed choice after LAUNCH. Power-on leaves pre-existing explicit
 * CLI/environment replay/slot inputs to the host's established precedence.
 * The module itself never writes PSX_LOAD_SLOT/PSX_REPLAY_FILE. */
uint32_t recomp_launcher_module_start_choice(int32_t* slot, char* replay, uint32_t capacity);
const char* recomp_launcher_module_diagnostic(void);
/* Close only after blocking run returns and all module workers have joined.
 * A compatible replacement takes effect in a subsequent host process. */
void recomp_launcher_module_close(void);

/* A module refusal is distinct from genuine GL/assets UNAVAILABLE=2. Both
 * boot and rematch callers must stop on this value, before settings writes.
 * An explicit headless/skip-launcher caller does not enter the adapter. */
#define RECOMP_LAUNCHER_RESULT_REFUSED 4
#ifdef __cplusplus
}
#endif
#endif
