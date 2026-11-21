#ifndef RECOMP_LAUNCHER_INI_H
#define RECOMP_LAUNCHER_INI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Shared surgical INI writer. Does not own the rest of the host's file. */
void launcher_ini_kv_write(const char* path, const char* section,
                           const char* key, const char* value);

#ifdef __cplusplus
}
#endif

#endif
