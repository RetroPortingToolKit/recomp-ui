/* launcher_assets_dir.c -- see launcher_assets_dir.h. */
#include "launcher_assets_dir.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define ASSETS_DIR_MAX 4096

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* UTF-8 safe on Windows (the ANSI stat family would mangle non-ASCII paths). */
static int is_directory(const char* utf8) {
#ifdef _WIN32
    wchar_t wide[ASSETS_DIR_MAX];
    DWORD attr;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, ASSETS_DIR_MAX)) return 0;
    attr = GetFileAttributesW(wide);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(utf8, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static char s_key[ASSETS_DIR_MAX];
static char s_val[ASSETS_DIR_MAX];
static int s_have;

size_t launcher_assets_dir_resolve(const char* assets_dir, char* out, size_t cap) {
    size_t n, len;
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!assets_dir || !assets_dir[0]) return 0;
    if (!s_have || strcmp(s_key, assets_dir) != 0) {
        char probe[ASSETS_DIR_MAX + 8];
        n = strlen(assets_dir);
        if (n >= ASSETS_DIR_MAX) return 0;
        memcpy(s_val, assets_dir, n + 1);
        while (n > 1 && is_sep(s_val[n - 1])) s_val[--n] = '\0';
        if (n + 8 < ASSETS_DIR_MAX) {
            snprintf(probe, sizeof probe, "%s/assets", s_val);
            if (is_directory(probe)) memcpy(s_val + n, "/assets", 8);
        }
        memcpy(s_key, assets_dir, strlen(assets_dir) + 1);
        s_have = 1;
    }
    len = strlen(s_val);
    if (len >= cap) return 0;
    memcpy(out, s_val, len + 1);
    return len;
}
