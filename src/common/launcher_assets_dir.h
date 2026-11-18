/* launcher_assets_dir.h -- how a host-supplied assets_dir is interpreted.
 *
 * recomp_launcher_run_window()'s assets_dir has always meant "the assets
 * folder itself" (whatever it is called). Some hosts instead pass the folder
 * that CONTAINS assets/ (their exe dir). The two are told apart by probing:
 * if <assets_dir>/assets is a directory, assets_dir is the base; otherwise it
 * is the assets folder, exactly as before. */
#ifndef LAUNCHER_ASSETS_DIR_H
#define LAUNCHER_ASSETS_DIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes the resolved assets FOLDER for `assets_dir` (UTF-8, trailing
 * separators stripped, "/assets" appended when the probe above matches) into
 * out. Returns its length, or 0 when assets_dir is NULL/empty or out is too
 * small. The probe result is cached per assets_dir (single entry, launcher
 * thread only), so this is cheap to call per asset lookup. */
size_t launcher_assets_dir_resolve(const char* assets_dir, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
