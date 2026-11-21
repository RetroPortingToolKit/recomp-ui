#ifndef LAUNCHER_MOD_TEXT_PATH_H
#define LAUNCHER_MOD_TEXT_PATH_H
#include "recomp_launcher.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The same provider operation used for a typed TEXT value. NULL is cancel;
 * empty is Clear. Never truncate a selected path into a different filename. */
int launcher_mod_text_path_set(const RecompLauncherCModProvider* provider,
    const char* package_id, const char* feature_id, const char* option_id,
    const char* value);
#ifdef __cplusplus
}
#endif
#endif
