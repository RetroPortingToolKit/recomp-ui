// psx_kb_profiles.c - see psx_kb_profiles.h.
//
// Paths are joined and opened as the C runtime takes them, the way psx_binds.c
// opens keybinds.ini, so a folder under the user's own name is reached through
// the same code page as the variable that named it.

#include "psx_kb_profiles.h"
#include "psx_binds.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define KBP_SEP '\\'
static int kbp_mkdir(const char* path) { return _mkdir(path); }
#else
#include <dirent.h>
#include <sys/stat.h>
#define KBP_SEP '/'
static int kbp_mkdir(const char* path) { return mkdir(path, 0755); }
#endif

#define KBP_PATH 1024
#define KBP_MAX  128   /* profiles read from one folder */
#define KBP_NAME (RUI_PSX_KB_PROFILE_NAME_MAX + 1)

static char kbp_fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Order without letter case; 0 for two spellings of one name. */
static int kbp_compare(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char ca = kbp_fold(*a), cb = kbp_fold(*b);
        if (ca != cb) return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
        if (!ca) return 0;
    }
}

static int kbp_is_default(const char* name) {
    return kbp_compare(name, RUI_PSX_KB_PROFILE_DEFAULT) == 0;
}

static int kbp_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')' || c == '+';
}

/* CON, PRN, AUX, NUL, COM0..9 and LPT0..9 name devices on Windows, with any
 * extension, so "NUL.ini" is not a file there. */
static int kbp_is_device_name(const char* name) {
    char stem[5] = {0};
    size_t n = 0;
    while (name[n] && name[n] != '.') ++n;
    if (n != 3 && n != 4) return 0;
    for (size_t i = 0; i < n; ++i) stem[i] = kbp_fold(name[i]);
    if (n == 3)
        return !strcmp(stem, "con") || !strcmp(stem, "prn") || !strcmp(stem, "aux") ||
               !strcmp(stem, "nul");
    return (!strncmp(stem, "com", 3) || !strncmp(stem, "lpt", 3)) &&
           stem[3] >= '0' && stem[3] <= '9';
}

static int kbp_check_name(const char* name) {
    const size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > RUI_PSX_KB_PROFILE_NAME_MAX) return RUI_PSX_KBP_BAD_NAME;
    for (size_t i = 0; i < n; ++i)
        if (!kbp_char_ok(name[i])) return RUI_PSX_KBP_BAD_NAME;
    if (name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.')
        return RUI_PSX_KBP_BAD_NAME;
    if (kbp_is_device_name(name)) return RUI_PSX_KBP_BAD_NAME;
    if (kbp_is_default(name)) return RUI_PSX_KBP_PROTECTED;
    return RUI_PSX_KBP_OK;
}

static int kbp_file_of(const char* dir, const char* name, char* out, size_t cap) {
    const int n = snprintf(out, cap, "%s%c%s.ini", dir, KBP_SEP, name);
    return n > 0 && (size_t)n < cap;
}

/* A file of the folder is a profile when its name is "<a profile name>.ini".
 * Default.ini is not one: that name is the built-in map. */
static void kbp_take_file(const char* file, char names[][KBP_NAME], int* count) {
    const size_t n = strlen(file);
    char name[KBP_NAME];
    if (*count >= KBP_MAX || n < 5 || n - 4 > RUI_PSX_KB_PROFILE_NAME_MAX) return;
    if (kbp_compare(file + n - 4, ".ini") != 0) return;
    memcpy(name, file, n - 4);
    name[n - 4] = '\0';
    if (kbp_check_name(name) != RUI_PSX_KBP_OK) return;
    memcpy(names[(*count)++], name, n - 3);
}

static int kbp_order(const void* a, const void* b) {
    const int folded = kbp_compare((const char*)a, (const char*)b);
    return folded ? folded : strcmp((const char*)a, (const char*)b);
}

/* The profiles the folder holds, as their files spell them, in alphabetical
 * order. An absent folder holds none. */
static int kbp_files(const char* dir, char names[][KBP_NAME]) {
    int count = 0;
    if (!dir || !dir[0]) return 0;
#ifdef _WIN32
    char pattern[KBP_PATH];
    struct _finddata_t found;
    if (!kbp_file_of(dir, "*", pattern, sizeof(pattern))) return 0;
    const intptr_t h = _findfirst(pattern, &found);
    if (h != -1) {
        do {
            if (!(found.attrib & _A_SUBDIR)) kbp_take_file(found.name, names, &count);
        } while (_findnext(h, &found) == 0);
        _findclose(h);
    }
#else
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) kbp_take_file(e->d_name, names, &count);
        closedir(d);
    }
#endif
    qsort(names, (size_t)count, KBP_NAME, kbp_order);
    return count;
}

/* The path of the profile `name`, whatever letter case its file has. */
static int kbp_find(const char* dir, const char* name, char* path, size_t cap) {
    char names[KBP_MAX][KBP_NAME];
    const int count = kbp_files(dir, names);
    for (int i = 0; i < count; ++i)
        if (kbp_compare(names[i], name) == 0) return kbp_file_of(dir, names[i], path, cap);
    return 0;
}

static void kbp_make_dirs(const char* dir) {
    char part[KBP_PATH];
    const size_t n = strlen(dir);
    if (n >= sizeof(part)) return;
    memcpy(part, dir, n + 1);
    for (size_t i = 1; i < n; ++i) {
        if (part[i] != '/' && part[i] != '\\') continue;
        const char sep = part[i];
        part[i] = '\0';
        kbp_mkdir(part);
        part[i] = sep;
    }
    kbp_mkdir(part);
}

int rui_psx_kb_profiles_dir(char* out, int cap) {
    const char* named = getenv("PSXRECOMP_KEYBOARD_PROFILES_DIR");
    int n = 0;
    if (!out || cap < 1) return 0;
    out[0] = '\0';
    if (named && named[0]) {
        n = snprintf(out, (size_t)cap, "%s", named);
    } else {
#if defined(_WIN32)
        const char* base = getenv("APPDATA");
        const char* under = "\\psxrecomp\\keyboard-profiles";
#elif defined(__APPLE__)
        const char* base = getenv("HOME");
        const char* under = "/Library/Application Support/psxrecomp/keyboard-profiles";
#else
        const char* base = getenv("XDG_CONFIG_HOME");
        const char* under = "/psxrecomp/keyboard-profiles";
        if (!base || !base[0]) {
            base = getenv("HOME");
            under = "/.config/psxrecomp/keyboard-profiles";
        }
#endif
        if (base && base[0]) n = snprintf(out, (size_t)cap, "%s%s", base, under);
    }
    if (n < 1 || n >= cap) { out[0] = '\0'; return 0; }
    return 1;
}

int rui_psx_kb_profile_check_name(const char* name) {
    return kbp_check_name(name);
}

int rui_psx_kb_profiles_list(const char* dir,
                             char names[][RUI_PSX_KB_PROFILE_NAME_MAX + 1],
                             int max) {
    char files[KBP_MAX][KBP_NAME];
    int n = 0;
    if (!names || max < 1) return 0;
    snprintf(names[n++], KBP_NAME, "%s", RUI_PSX_KB_PROFILE_DEFAULT);
    const int count = kbp_files(dir, files);
    for (int i = 0; i < count && n < max; ++i) memcpy(names[n++], files[i], KBP_NAME);
    return n;
}

int rui_psx_kb_profile_save_as(const char* dir, const char* name,
                               const char* keybinds_path, int player,
                               int replace) {
    char path[KBP_PATH];
    const int checked = kbp_check_name(name);
    if (checked != RUI_PSX_KBP_OK) return checked;
    if (!dir || !dir[0]) return RUI_PSX_KBP_IO;
    /* A profile that is there keeps the spelling its file has: "wasd" over
     * "WASD" is one profile, not two. */
    if (kbp_find(dir, name, path, sizeof(path))) {
        if (!replace) return RUI_PSX_KBP_EXISTS;
    } else {
        if (!kbp_file_of(dir, name, path, sizeof(path))) return RUI_PSX_KBP_IO;
        kbp_make_dirs(dir);
    }
    return rui_psx_binds_save_profile(keybinds_path, player, path)
               ? RUI_PSX_KBP_OK : RUI_PSX_KBP_IO;
}

int rui_psx_kb_profile_load(const char* dir, const char* name,
                            const char* keybinds_path, int player) {
    char path[KBP_PATH];
    if (name && kbp_is_default(name)) {
        rui_psx_binds_reset(keybinds_path, player);
        return rui_psx_binds_is_default(keybinds_path, player)
                   ? RUI_PSX_KBP_OK : RUI_PSX_KBP_IO;
    }
    if (kbp_check_name(name) != RUI_PSX_KBP_OK) return RUI_PSX_KBP_BAD_NAME;
    if (!kbp_find(dir, name, path, sizeof(path))) return RUI_PSX_KBP_NOT_FOUND;
    return rui_psx_binds_load_profile(keybinds_path, player, path)
               ? RUI_PSX_KBP_OK : RUI_PSX_KBP_NOT_A_PROFILE;
}

int rui_psx_kb_profile_rename(const char* dir, const char* from, const char* to) {
    char old_path[KBP_PATH], new_path[KBP_PATH], taken[KBP_PATH];
    if (from && kbp_is_default(from)) return RUI_PSX_KBP_PROTECTED;
    const int checked = kbp_check_name(to);
    if (checked != RUI_PSX_KBP_OK) return checked;
    if (kbp_check_name(from) != RUI_PSX_KBP_OK) return RUI_PSX_KBP_BAD_NAME;
    if (!kbp_find(dir, from, old_path, sizeof(old_path))) return RUI_PSX_KBP_NOT_FOUND;
    /* Another spelling of its own name is a rename; any other profile's name is taken. */
    if (kbp_compare(from, to) != 0 && kbp_find(dir, to, taken, sizeof(taken)))
        return RUI_PSX_KBP_EXISTS;
    if (!kbp_file_of(dir, to, new_path, sizeof(new_path))) return RUI_PSX_KBP_IO;
    if (!strcmp(old_path, new_path)) return RUI_PSX_KBP_OK;
    return rename(old_path, new_path) == 0 ? RUI_PSX_KBP_OK : RUI_PSX_KBP_IO;
}

int rui_psx_kb_profile_delete(const char* dir, const char* name) {
    char path[KBP_PATH];
    if (name && kbp_is_default(name)) return RUI_PSX_KBP_PROTECTED;
    if (kbp_check_name(name) != RUI_PSX_KBP_OK) return RUI_PSX_KBP_BAD_NAME;
    if (!kbp_find(dir, name, path, sizeof(path))) return RUI_PSX_KBP_NOT_FOUND;
    return remove(path) == 0 ? RUI_PSX_KBP_OK : RUI_PSX_KBP_IO;
}

int rui_psx_kb_profile_in_use(const char* dir, const char* keybinds_path,
                              int player, char* out, int cap) {
    char names[KBP_MAX][KBP_NAME];
    char path[KBP_PATH];
    const char* found = NULL;
    if (rui_psx_binds_is_default(keybinds_path, player)) {
        found = RUI_PSX_KB_PROFILE_DEFAULT;
    } else {
        const int count = kbp_files(dir, names);
        for (int i = 0; i < count && !found; ++i)
            if (kbp_file_of(dir, names[i], path, sizeof(path)) &&
                rui_psx_binds_matches_profile(keybinds_path, player, path))
                found = names[i];
    }
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", found ? found : "");
    return found ? 1 : 0;
}
