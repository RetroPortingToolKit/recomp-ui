// common/kb_profiles.c - see kb_profiles.h.
//
// Paths are joined and opened as the C runtime takes them, the way the bind
// stores open their own files, so a folder under the user's own name is
// reached through the same code page as the variable that named it.

#include "kb_profiles.h"
#include "launcher_sdlcompat.h"   // SDL scancode names (SDL 2 or 3)

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#define KBP_PATH    1024
#define KBP_MAX     128   /* profiles read from one folder */
#define KBP_NAME    (RUI_KBP_NAME_MAX + 1)
#define KBP_KEYS    (RUI_KBP_MAX_BUTTONS * RUI_KBP_MAX_SLOTS)
#define KBP_PLAYERS 8

/* Bumped by everything that changes a profile or the keys. */
static unsigned s_generation = 1;

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
    return kbp_compare(name, RUI_KBP_DEFAULT) == 0;
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
    if (n < 1 || n > RUI_KBP_NAME_MAX) return RUI_KBP_BAD_NAME;
    for (size_t i = 0; i < n; ++i)
        if (!kbp_char_ok(name[i])) return RUI_KBP_BAD_NAME;
    if (name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.')
        return RUI_KBP_BAD_NAME;
    if (kbp_is_device_name(name)) return RUI_KBP_BAD_NAME;
    if (kbp_is_default(name)) return RUI_KBP_PROTECTED;
    return RUI_KBP_OK;
}

static int kbp_file_of(const char* dir, const char* name, char* out, size_t cap) {
    const int n = snprintf(out, cap, "%s%c%s.ini", dir, KBP_SEP, name);
    return n > 0 && (size_t)n < cap;
}

/* A file of the folder is a profile when its name is "<a profile name>.ini".
 * Default.ini is not one: that name is the built-in map. A leftover
 * "<name>.ini.tmp" of an interrupted save is not one either. */
static void kbp_take_file(const char* file, char names[][KBP_NAME], int* count) {
    const size_t n = strlen(file);
    char name[KBP_NAME];
    if (*count >= KBP_MAX || n < 5 || n - 4 > RUI_KBP_NAME_MAX) return;
    if (kbp_compare(file + n - 4, ".ini") != 0) return;
    memcpy(name, file, n - 4);
    name[n - 4] = '\0';
    if (kbp_check_name(name) != RUI_KBP_OK) return;
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

/* Move `tmp` over `dst` in one step: a reader sees the old file or the new
 * one, never half of either. */
static int kbp_replace(const char* tmp, const char* dst) {
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

/* ---- folder ------------------------------------------------------------- */

int rui_kbp_dir(const RuiKbAdapter* a, char* out, int cap) {
    int n = 0;
    if (!out || cap < 1) return 0;
    out[0] = '\0';
    if (!a || !a->id || !a->id[0]) return 0;
    const char* alias = a->env_alias ? getenv(a->env_alias) : NULL;
    const char* root = getenv(RUI_KBP_ENV_ROOT);
    if (alias && alias[0]) {
        n = snprintf(out, (size_t)cap, "%s", alias);
    } else if (root && root[0]) {
        n = snprintf(out, (size_t)cap, "%s%c%s", root, KBP_SEP, a->id);
    } else {
        char under[300];
        const char* base;
        if (a->legacy_subdir && a->legacy_subdir[0])
            snprintf(under, sizeof(under), "%s", a->legacy_subdir);
        else
            snprintf(under, sizeof(under), "recomp-ui/keyboard-profiles/%s", a->id);
#if defined(_WIN32)
        base = getenv("APPDATA");
        for (char* c = under; *c; ++c) if (*c == '/') *c = '\\';
        const char* sep = "\\";
#elif defined(__APPLE__)
        char mac[300];
        base = getenv("HOME");
        snprintf(mac, sizeof(mac), "Library/Application Support/%s", under);
        memcpy(under, mac, sizeof(under));
        const char* sep = "/";
#else
        char home_under[300];
        base = getenv("XDG_CONFIG_HOME");
        if (!base || !base[0]) {
            base = getenv("HOME");
            snprintf(home_under, sizeof(home_under), ".config/%s", under);
            memcpy(under, home_under, sizeof(under));
        }
        const char* sep = "/";
#endif
        if (base && base[0]) n = snprintf(out, (size_t)cap, "%s%s%s", base, sep, under);
    }
    if (n < 1 || n >= cap) { out[0] = '\0'; return 0; }
    return 1;
}

int rui_kbp_check_name(const char* name) { return kbp_check_name(name); }

int rui_kbp_list(const char* dir, char names[][RUI_KBP_NAME_MAX + 1], int max) {
    char files[KBP_MAX][KBP_NAME];
    int n = 0;
    if (!names || max < 1) return 0;
    snprintf(names[n++], KBP_NAME, "%s", RUI_KBP_DEFAULT);
    const int count = kbp_files(dir, files);
    for (int i = 0; i < count && n < max; ++i) memcpy(names[n++], files[i], KBP_NAME);
    return n;
}

/* ---- the keys of one player --------------------------------------------- */

static int kbp_adapter_ok(const RuiKbAdapter* a, int player) {
    return a && a->id && a->get && a->default_key && a->apply && a->button_ids &&
           a->buttons >= 1 && a->buttons <= RUI_KBP_MAX_BUTTONS &&
           a->slots >= 1 && a->slots <= RUI_KBP_MAX_SLOTS &&
           player >= 0 && player < a->players && player < KBP_PLAYERS;
}

static size_t kbp_bytes(const RuiKbAdapter* a) {
    return sizeof(int) * (size_t)(a->buttons * a->slots);
}

static void kbp_read_keys(const RuiKbAdapter* a, const char* path, int player, int* keys) {
    for (int b = 0; b < a->buttons; ++b)
        for (int s = 0; s < a->slots; ++s)
            keys[b * a->slots + s] = a->get(a, path, player, b, s);
}

static void kbp_default_keys(const RuiKbAdapter* a, int player, int* keys) {
    for (int b = 0; b < a->buttons; ++b)
        for (int s = 0; s < a->slots; ++s)
            keys[b * a->slots + s] = a->default_key(a, player, b, s);
}

static uint32_t kbp_fingerprint(const RuiKbAdapter* a, const char* path, int player) {
    uint32_t h = 2166136261u;
    for (int b = 0; b < a->buttons; ++b)
        for (int s = 0; s < a->slots; ++s) {
            const uint32_t k = (uint32_t)a->get(a, path, player, b, s);
            for (int i = 0; i < 4; ++i) h = (h ^ ((k >> (8 * i)) & 0xFFu)) * 16777619u;
        }
    return h;
}

/* ---- the generic file format --------------------------------------------- */

static int kbp_scancode_of(const char* name) {
    char buf[40];
    size_t i = 0;
    if (!name || !*name) return 0;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return (int)sc;
    for (; name[i] && i < sizeof(buf) - 1; ++i) buf[i] = kbp_fold(name[i]);
    buf[i] = '\0';
    if (!strcmp(buf, "enter") || !strcmp(buf, "return")) return SDL_SCANCODE_RETURN;
    if (!strcmp(buf, "tab")) return SDL_SCANCODE_TAB;
    if (!strcmp(buf, "space")) return SDL_SCANCODE_SPACE;
    if (!strcmp(buf, "lshift")) return SDL_SCANCODE_LSHIFT;
    if (!strcmp(buf, "rshift")) return SDL_SCANCODE_RSHIFT;
    if (!strcmp(buf, "lctrl")) return SDL_SCANCODE_LCTRL;
    if (!strcmp(buf, "rctrl")) return SDL_SCANCODE_RCTRL;
    if (!strcmp(buf, "lalt")) return SDL_SCANCODE_LALT;
    if (!strcmp(buf, "ralt")) return SDL_SCANCODE_RALT;
    if (!strcmp(buf, "backslash")) return SDL_SCANCODE_BACKSLASH;
    if (!strcmp(buf, "escape") || !strcmp(buf, "esc")) return SDL_SCANCODE_ESCAPE;
    if (!strcmp(buf, "backspace")) return SDL_SCANCODE_BACKSPACE;
    return 0;   /* "none" and anything unknown: unbound */
}

static const char* kbp_name_of(int sc) {
    if (sc <= 0 || sc >= 512) return "None";
    const char* n = SDL_GetScancodeName((SDL_Scancode)sc);
    return (n && n[0]) ? n : "None";
}

static char* kbp_trim(char* s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static int kbp_generic_save(const RuiKbAdapter* a, const char* path, int player,
                            const char* dst) {
    int keys[KBP_KEYS];
    FILE* f = fopen(dst, "w");
    if (!f) return 0;
    kbp_read_keys(a, path, player, keys);
    fprintf(f,
        "# %s keyboard profile: one player's keys. The file name is the\n"
        "# profile's name. Values are SDL key names, which name a key by its\n"
        "# place on the keyboard; \"None\" leaves an input unbound%s.\n\n"
        "[player1]\n", a->id,
        a->slots > 1 ? "; a second name after a comma is the alternate key" : "");
    for (int b = 0; b < a->buttons; ++b) {
        if (a->slots > 1 && keys[b * a->slots + 1] > 0)
            fprintf(f, "%-9s = %s, %s\n", a->button_ids[b], kbp_name_of(keys[b * a->slots]),
                    kbp_name_of(keys[b * a->slots + 1]));
        else
            fprintf(f, "%-9s = %s\n", a->button_ids[b], kbp_name_of(keys[b * a->slots]));
    }
    const int failed = ferror(f);
    return fclose(f) == 0 && !failed;
}

/* Reads the player's section of `src` (else [player1]) into `keys`, where a
 * key the file omits is the default. Parses into local buffers: the store is
 * not touched. 0 when the file is unreadable or has no such section with a
 * key of this console. */
static int kbp_generic_read(const RuiKbAdapter* a, int player, const char* src, int* keys) {
    int got[KBP_PLAYERS][KBP_KEYS];
    int hits[KBP_PLAYERS] = {0};
    unsigned seen = 0;
    int cur = -1;
    char line[256];
    FILE* f = fopen(src, "r");
    if (!f) return 0;
    for (int p = 0; p < KBP_PLAYERS; ++p) kbp_default_keys(a, p < a->players ? p : 0, got[p]);
    while (fgets(line, sizeof(line), f)) {
        char* s = kbp_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            cur = -1;
            if (end) {
                *end = '\0';
                if (!strncmp(s + 1, "player", 6)) {
                    const int n = atoi(s + 7);
                    if (n >= 1 && n <= KBP_PLAYERS) { cur = n - 1; seen |= 1u << cur; }
                }
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq || cur < 0) continue;
        *eq = '\0';
        char* key = kbp_trim(s);
        char* val = kbp_trim(eq + 1);
        char* alt = NULL;
        char* comma = strchr(val, ',');
        if (comma) { *comma = '\0'; alt = kbp_trim(comma + 1); val = kbp_trim(val); }
        for (int b = 0; b < a->buttons; ++b) {
            if (kbp_compare(key, a->button_ids[b]) != 0) continue;
            got[cur][b * a->slots] = kbp_scancode_of(val);
            if (a->slots > 1) got[cur][b * a->slots + 1] = alt ? kbp_scancode_of(alt) : 0;
            ++hits[cur];
            break;
        }
    }
    fclose(f);
    const int from = (seen & (1u << player)) ? player : ((seen & 1u) ? 0 : -1);
    if (from < 0 || hits[from] == 0) return 0;
    memcpy(keys, got[from], kbp_bytes(a));
    return 1;
}

/* ---- file operations, generic or the adapter's own ----------------------- */

static int kbp_op_save_file(const RuiKbAdapter* a, const char* path, int player, const char* dst) {
    return a->save_file ? a->save_file(a, path, player, dst) : kbp_generic_save(a, path, player, dst);
}

static int kbp_op_load_file(const RuiKbAdapter* a, const char* path, int player, const char* src) {
    int keys[KBP_KEYS];
    if (a->load_file) return a->load_file(a, path, player, src);
    if (!kbp_generic_read(a, player, src, keys)) return 0;
    a->apply(a, path, player, keys);
    return 1;
}

static int kbp_op_matches_file(const RuiKbAdapter* a, const char* path, int player, const char* src) {
    int keys[KBP_KEYS], now[KBP_KEYS];
    if (a->matches_file) return a->matches_file(a, path, player, src);
    if (!kbp_generic_read(a, player, src, keys)) return 0;
    kbp_read_keys(a, path, player, now);
    return memcmp(keys, now, kbp_bytes(a)) == 0;
}

static int kbp_op_is_default(const RuiKbAdapter* a, const char* path, int player) {
    int def[KBP_KEYS], now[KBP_KEYS];
    if (a->is_default) return a->is_default(a, path, player);
    kbp_default_keys(a, player, def);
    kbp_read_keys(a, path, player, now);
    return memcmp(def, now, kbp_bytes(a)) == 0;
}

/* ---- public operations ---------------------------------------------------- */

int rui_kbp_save_as(const RuiKbAdapter* a, const char* dir, const char* name,
                    const char* keys_path, int player, int replace) {
    char path[KBP_PATH], tmp[KBP_PATH + 8];
    const int checked = kbp_check_name(name);
    if (checked != RUI_KBP_OK) return checked;
    if (!kbp_adapter_ok(a, player) || !dir || !dir[0]) return RUI_KBP_IO;
    /* A profile that is there keeps the spelling its file has: "wasd" over
     * "WASD" is one profile, not two. */
    if (kbp_find(dir, name, path, sizeof(path))) {
        if (!replace) return RUI_KBP_EXISTS;
    } else {
        if (!kbp_file_of(dir, name, path, sizeof(path))) return RUI_KBP_IO;
        kbp_make_dirs(dir);
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return RUI_KBP_IO;
    if (!kbp_op_save_file(a, keys_path, player, tmp) || !kbp_replace(tmp, path)) {
        remove(tmp);
        return RUI_KBP_IO;
    }
    ++s_generation;
    return RUI_KBP_OK;
}

int rui_kbp_load(const RuiKbAdapter* a, const char* dir, const char* name,
                 const char* keys_path, int player) {
    char path[KBP_PATH];
    if (!kbp_adapter_ok(a, player)) return RUI_KBP_IO;
    if (name && kbp_is_default(name)) {
        int def[KBP_KEYS];
        if (a->reset) {
            a->reset(a, keys_path, player);
        } else {
            kbp_default_keys(a, player, def);
            a->apply(a, keys_path, player, def);
        }
        ++s_generation;
        return kbp_op_is_default(a, keys_path, player) ? RUI_KBP_OK : RUI_KBP_IO;
    }
    if (kbp_check_name(name) != RUI_KBP_OK) return RUI_KBP_BAD_NAME;
    if (!kbp_find(dir, name, path, sizeof(path))) return RUI_KBP_NOT_FOUND;
    const int ok = kbp_op_load_file(a, keys_path, player, path);
    ++s_generation;
    return ok ? RUI_KBP_OK : RUI_KBP_NOT_A_PROFILE;
}

int rui_kbp_load_file(const RuiKbAdapter* a, const char* keys_path, int player,
                      const char* src) {
    if (!kbp_adapter_ok(a, player) || !src || !src[0]) return 0;
    const int ok = kbp_op_load_file(a, keys_path, player, src);
    ++s_generation;
    return ok;
}

int rui_kbp_rename(const char* dir, const char* from, const char* to) {
    char old_path[KBP_PATH], new_path[KBP_PATH], taken[KBP_PATH];
    if (from && kbp_is_default(from)) return RUI_KBP_PROTECTED;
    const int checked = kbp_check_name(to);
    if (checked != RUI_KBP_OK) return checked;
    if (kbp_check_name(from) != RUI_KBP_OK) return RUI_KBP_BAD_NAME;
    if (!kbp_find(dir, from, old_path, sizeof(old_path))) return RUI_KBP_NOT_FOUND;
    /* Another spelling of its own name is a rename; any other profile's name is taken. */
    if (kbp_compare(from, to) != 0 && kbp_find(dir, to, taken, sizeof(taken)))
        return RUI_KBP_EXISTS;
    if (!kbp_file_of(dir, to, new_path, sizeof(new_path))) return RUI_KBP_IO;
    if (!strcmp(old_path, new_path)) return RUI_KBP_OK;
    const int ok = rename(old_path, new_path) == 0;
    ++s_generation;
    return ok ? RUI_KBP_OK : RUI_KBP_IO;
}

int rui_kbp_delete(const char* dir, const char* name) {
    char path[KBP_PATH];
    if (name && kbp_is_default(name)) return RUI_KBP_PROTECTED;
    if (kbp_check_name(name) != RUI_KBP_OK) return RUI_KBP_BAD_NAME;
    if (!kbp_find(dir, name, path, sizeof(path))) return RUI_KBP_NOT_FOUND;
    const int ok = remove(path) == 0;
    ++s_generation;
    return ok ? RUI_KBP_OK : RUI_KBP_IO;
}

/* ---- the profile in use, kept --------------------------------------------- */

typedef struct {
    int valid;
    unsigned generation;
    uint32_t keys;
    char console[24];
    char dir[KBP_PATH];
    char path[KBP_PATH];
    char found[KBP_NAME];   /* "" when no profile holds the keys */
} KbpUseCache;
static KbpUseCache s_use[KBP_PLAYERS];

void rui_kbp_invalidate(void) { ++s_generation; }

int rui_kbp_in_use(const RuiKbAdapter* a, const char* dir, const char* keys_path,
                   int player, char* out, int cap) {
    char names[KBP_MAX][KBP_NAME];
    char path[KBP_PATH];
    char found[KBP_NAME] = "";
    if (out && cap > 0) out[0] = '\0';
    if (!kbp_adapter_ok(a, player)) return 0;
    if (!dir) dir = "";
    if (!keys_path) keys_path = "";
    const uint32_t keys = kbp_fingerprint(a, keys_path, player);
    KbpUseCache* c = &s_use[player];
    if (!(c->valid && c->generation == s_generation && c->keys == keys &&
          !strcmp(c->console, a->id) && !strcmp(c->dir, dir) && !strcmp(c->path, keys_path))) {
        if (kbp_op_is_default(a, keys_path, player)) {
            snprintf(found, sizeof(found), "%s", RUI_KBP_DEFAULT);
        } else {
            const int count = kbp_files(dir, names);
            for (int i = 0; i < count && !found[0]; ++i)
                if (kbp_file_of(dir, names[i], path, sizeof(path)) &&
                    kbp_op_matches_file(a, keys_path, player, path))
                    snprintf(found, sizeof(found), "%s", names[i]);
        }
        c->valid = 1;
        c->generation = s_generation;
        c->keys = keys;
        snprintf(c->console, sizeof(c->console), "%s", a->id);
        snprintf(c->dir, sizeof(c->dir), "%s", dir);
        snprintf(c->path, sizeof(c->path), "%s", keys_path);
        snprintf(c->found, sizeof(c->found), "%s", found);
    }
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", c->found);
    return c->found[0] ? 1 : 0;
}
