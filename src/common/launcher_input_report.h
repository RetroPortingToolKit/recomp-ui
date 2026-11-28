#ifndef LAUNCHER_INPUT_REPORT_H
#define LAUNCHER_INPUT_REPORT_H
#include "launcher_model.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* JSON-lines report. SDL must be initialized; no ROM or window is required.
 * Raw joysticks are enumerated even when SDL has no gamepad mapping. */
int launcher_input_report_start(const char* path, const LauncherModel* model,
                               const char* config_path, const char* keybinds_path,
                               uint32_t duration_ms);
void launcher_input_report_tick(const LauncherModel* model);
void launcher_input_report_stop(const LauncherModel* model);
int launcher_input_report_active(void);
#ifdef __cplusplus
}
#endif
#endif
