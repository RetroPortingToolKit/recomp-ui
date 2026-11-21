#ifndef RECOMP_LAUNCHER_SETTINGS_H
#define RECOMP_LAUNCHER_SETTINGS_H

#include "recomp_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

int launcher_settings_supported(const RecompLauncherCGameInfo* game);

/* The SNES profile uses the established config.ini schema. Other profiles
 * retain their host-owned formats. Missing/invalid keys keep host defaults.
 * Only player edits (relative to the initialized model) are written back. */
void launcher_settings_load(RecompLauncherCSettings* settings,
                            const RecompLauncherCGameInfo* game);
void launcher_settings_save(const RecompLauncherCSettings* settings,
                            const RecompLauncherCSettings* before,
                            const RecompLauncherCGameInfo* game);

#ifdef __cplusplus
}
#endif

#endif
