#ifndef RECOMP_LAUNCHER_MODULE_RUNTIME_H
#define RECOMP_LAUNCHER_MODULE_RUNTIME_H
#include "launcher_model.h"
#include "recomp_launcher_module.h"
#ifdef __cplusplus
extern "C" {
#endif
const RecompLauncherV2Run* launcher_v2_current_run(void);
void launcher_v2_model_starts(LauncherModel*);
void launcher_v2_model_result(const LauncherModel*, int action);
uint32_t launcher_v2_thumbnail(uint32_t index, uint8_t*, uint32_t);
void launcher_model_wait_worker(void);
void launcher_cover_job_wait(void);
void launcher_model_finish_worker(void);
void launcher_cover_job_finish(void);
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void launcher_v2_worker_join_failed(const char* worker, uint32_t wait_status);
#ifdef __cplusplus
}
#endif
#endif

