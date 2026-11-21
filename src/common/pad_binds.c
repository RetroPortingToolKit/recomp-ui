// common/pad_binds.c — per-GUID gamepad bind persistence (input.ini), shared.
//
// Extracted from consoles/psx/psx_pad_binds.c; the only changes are that the
// button table, the file banner and the legacy probe now come from a
// RuiPadSpec, and that state lives in a per-spec store instead of file
// statics (every console unit is compiled into every binary, so PSX and SNES
// stores are live at the same time).

#include "pad_binds.h"
#include "launcher_sdlcompat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char guid[RUI_PAD_GUID_CAP];
    char name[RUI_PAD_NAME_CAP];   // display name (driver default or custom)
    int  name_custom;              // 1 = user renamed; sync won't overwrite
    int  deadzone_pct;             // 0..100
    char src[RUI_PAD_MAX_BUTTONS][RUI_PAD_SRC_CAP];
    int  used;
} PadGuidMap;

typedef struct {
    const RuiPadSpec* spec;
    char global[RUI_PAD_MAX_BUTTONS][RUI_PAD_SRC_CAP];
    PadGuidMap maps[RUI_PAD_MAX_KNOWN];
    int  init;
    char path[1024];
} PadStore;

/* One per console that actually gets used. A spec that cannot be seated is
 * reported rather than silently sharing another console's store. */
#define RUI_PAD_MAX_STORES 4
static PadStore s_stores[RUI_PAD_MAX_STORES];

static PadStore* store_for(const RuiPadSpec* spec) {
    if (!spec) return NULL;
    for (int i = 0; i < RUI_PAD_MAX_STORES; ++i)
        if (s_stores[i].spec == spec) return &s_stores[i];
    for (int i = 0; i < RUI_PAD_MAX_STORES; ++i) {
        if (s_stores[i].spec) continue;
        memset(&s_stores[i], 0, sizeof(s_stores[i]));
        s_stores[i].spec = spec;
        return &s_stores[i];
    }
    fprintf(stderr, "[pad_binds] no free store for spec %p — raise "
                    "RUI_PAD_MAX_STORES\n", (const void*)spec);
    return NULL;
}

static int spec_count(const RuiPadSpec* spec) {
    int n = spec ? spec->count : 0;
    if (n < 0) n = 0;
    if (n > RUI_PAD_MAX_BUTTONS) n = RUI_PAD_MAX_BUTTONS;
    return n;
}

static void copy_str(char* d, size_t cap, const char* s) {
    if (!d || !cap) return;
    if (!s) { d[0] = 0; return; }
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}

static void seed_defaults_into(const RuiPadSpec* spec,
                               char dest[][RUI_PAD_SRC_CAP]) {
    for (int b = 0; b < spec_count(spec); ++b)
        copy_str(dest[b], RUI_PAD_SRC_CAP, spec->defaults[b]);
}

static void tolower_inplace(char* s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

static int key_index(const RuiPadSpec* spec, const char* key) {
    for (int b = 0; b < spec_count(spec); ++b)
        if (!strcmp(key, spec->key_names[b])) return b;
    return -1;
}

static PadGuidMap* find_map(PadStore* st, const char* guid) {
    if (!st || !guid || !guid[0]) return NULL;
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i)
        if (st->maps[i].used && !strcmp(st->maps[i].guid, guid))
            return &st->maps[i];
    return NULL;
}

static PadGuidMap* alloc_map(PadStore* st, const char* guid) {
    if (!st) return NULL;
    PadGuidMap* m = find_map(st, guid);
    if (m) return m;
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i) {
        if (st->maps[i].used) continue;
        memset(&st->maps[i], 0, sizeof(st->maps[i]));
        st->maps[i].used = 1;
        st->maps[i].deadzone_pct = st->spec->default_deadzone_pct;
        copy_str(st->maps[i].guid, sizeof(st->maps[i].guid), guid);
        seed_defaults_into(st->spec, st->maps[i].src);
        for (int b = 0; b < spec_count(st->spec); ++b)
            if (st->global[b][0])
                copy_str(st->maps[i].src[b], RUI_PAD_SRC_CAP, st->global[b]);
        return &st->maps[i];
    }
    return NULL;
}

static int clamp_dz(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

static void parse_mapping_line(const RuiPadSpec* spec,
                               char dest[][RUI_PAD_SRC_CAP],
                               const char* key, const char* val) {
    char kbuf[32];
    copy_str(kbuf, sizeof(kbuf), key);
    tolower_inplace(kbuf);
    int b = key_index(spec, kbuf);
    if (b < 0) return;
    char vbuf[RUI_PAD_SRC_CAP];
    copy_str(vbuf, sizeof(vbuf), val);
    char* comma = strchr(vbuf, ',');
    if (comma) *comma = '\0';
    char* s = vbuf;
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    tolower_inplace(s);
    if (!strcmp(s, "none") || !strcmp(s, "disabled")) s[0] = '\0';
    copy_str(dest[b], RUI_PAD_SRC_CAP, s);
}

/* Undo mis-captures where Left (etc.) was stored as another cardinal's SDL
 * name (SFA3 #3: left = dpup while up already owns dpup). */
static int heal_cardinals(const RuiPadSpec* spec,
                          char dest[][RUI_PAD_SRC_CAP]) {
    int changed = 0;
    /* The cardinal block need not lead the table: N64's is at index 4 (PSR's
     * N64Input order puts A/B/Z/Start first). `cardinal_first` is 0 for every
     * console written before that, so their block is the leading one exactly
     * as before. */
    int first = spec->cardinal_first;
    if (first < 0) first = 0;
    int n = first + spec->cardinal_count;
    if (n > spec_count(spec)) n = spec_count(spec);
    for (int i = first; i < n; ++i) {
        if (!dest[i][0] || !strcmp(dest[i], spec->defaults[i])) continue;
        for (int j = first; j < n; ++j) {
            if (i == j) continue;
            if (!strcmp(dest[i], spec->defaults[j]) &&
                !strcmp(dest[j], spec->defaults[j])) {
                copy_str(dest[i], RUI_PAD_SRC_CAP, spec->defaults[i]);
                changed = 1;
                break;
            }
        }
    }
    return changed;
}

static void load_ini(PadStore* st, const char* path) {
    const RuiPadSpec* spec = st->spec;
    seed_defaults_into(spec, st->global);
    memset(st->maps, 0, sizeof(st->maps));

    FILE* f = fopen(path, "r");
    if (!f) return;

    char section[96] = "";
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         isspace((unsigned char)line[n - 1])))
            line[--n] = '\0';
        char* s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) *end = '\0';
            copy_str(section, sizeof(section), s + 1);
            tolower_inplace(section);
            if (!strncmp(section, "mapping.", 8)) {
                const char* guid = section + 8;
                if (guid[0]) alloc_map(st, guid);
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = s;
        char* val = eq + 1;
        size_t kl = strlen(key);
        while (kl > 0 && isspace((unsigned char)key[kl - 1])) key[--kl] = '\0';
        while (*val && isspace((unsigned char)*val)) ++val;

        if (!strcmp(section, "mapping")) {
            parse_mapping_line(spec, st->global, key, val);
        } else if (!strncmp(section, "mapping.", 8)) {
            PadGuidMap* m = find_map(st, section + 8);
            if (!m) continue;
            char kbuf[32];
            copy_str(kbuf, sizeof(kbuf), key);
            tolower_inplace(kbuf);
            if (!strcmp(kbuf, "deadzone")) {
                m->deadzone_pct = clamp_dz(atoi(val));
            } else if (!strcmp(kbuf, "name_custom")) {
                m->name_custom = (atoi(val) != 0) ? 1 : 0;
            } else if (!strcmp(kbuf, "name")) {
                // Prefer [gamepads] name when both exist; only fill if empty.
                if (!m->name[0]) copy_str(m->name, sizeof(m->name), val);
            } else {
                parse_mapping_line(spec, m->src, key, val);
            }
        } else if (!strcmp(section, "gamepads")) {
            char gbuf[RUI_PAD_GUID_CAP];
            copy_str(gbuf, sizeof(gbuf), key);
            tolower_inplace(gbuf);
            PadGuidMap* m = alloc_map(st, gbuf);
            if (m) copy_str(m->name, sizeof(m->name), val);
        }
    }
    fclose(f);
}

static int section_is_ours(const char* sec) {
    return !strcmp(sec, "mapping") || !strncmp(sec, "mapping.", 8) ||
           !strcmp(sec, "gamepads");
}

/* Widest key we write, so the columns line up whatever the console's names
 * are. "name_custom" is written too, so it participates. */
static int key_field_width(const RuiPadSpec* spec) {
    if (spec->key_field_width > 0) return spec->key_field_width;
    int w = (int)strlen("name_custom");
    for (int b = 0; b < spec_count(spec); ++b) {
        int n = (int)strlen(spec->key_names[b]);
        if (n > w) w = n;
    }
    return w;
}

static void write_ini(PadStore* st, const char* path) {
    const RuiPadSpec* spec = st->spec;
    const int kw = key_field_width(spec);
    char* preserved = NULL;
    size_t preserved_len = 0;
    {
        FILE* in = fopen(path, "rb");
        if (in) {
            fseek(in, 0, SEEK_END);
            long n = ftell(in);
            fseek(in, 0, SEEK_SET);
            if (n > 0) {
                char* buf = (char*)malloc((size_t)n + 1);
                if (buf) {
                    size_t got = fread(buf, 1, (size_t)n, in);
                    buf[got] = '\0';
                    size_t cap = got + 1;
                    preserved = (char*)malloc(cap);
                    preserved_len = 0;
                    if (preserved) {
                        int skip = 0;
                        for (char* line = strtok(buf, "\n"); line;
                             line = strtok(NULL, "\n")) {
                            char* p = line;
                            while (*p == ' ' || *p == '\t') ++p;
                            if (*p == '[') {
                                char sec[96];
                                copy_str(sec, sizeof(sec), p + 1);
                                char* br = strchr(sec, ']');
                                if (br) *br = '\0';
                                tolower_inplace(sec);
                                skip = section_is_ours(sec);
                            }
                            if (skip) continue;
                            size_t ln = strlen(line);
                            if (preserved_len + ln + 2 > cap) {
                                cap = (preserved_len + ln + 2) * 2;
                                char* nb = (char*)realloc(preserved, cap);
                                if (!nb) break;
                                preserved = nb;
                            }
                            memcpy(preserved + preserved_len, line, ln);
                            preserved_len += ln;
                            preserved[preserved_len++] = '\n';
                            preserved[preserved_len] = '\0';
                        }
                    }
                    free(buf);
                }
            }
            fclose(in);
        }
    }

    FILE* f = fopen(path, "w");
    if (!f) {
        free(preserved);
        return;
    }
    if (preserved && preserved_len) {
        fwrite(preserved, 1, preserved_len, f);
        if (preserved[preserved_len - 1] != '\n') fputc('\n', f);
    } else if (spec->file_seed && spec->file_seed[0]) {
        fputs(spec->file_seed, f);
    }
    free(preserved);

    int any_named = 0;
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i)
        if (st->maps[i].used && st->maps[i].name[0]) { any_named = 1; break; }
    if (any_named) {
        fputs("[gamepads]\n", f);
        for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i) {
            if (!st->maps[i].used || !st->maps[i].name[0]) continue;
            fprintf(f, "%s = %s\n", st->maps[i].guid, st->maps[i].name);
        }
        fputc('\n', f);
    }

    fputs("[mapping]\n", f);
    for (int b = 0; b < spec_count(spec); ++b) {
        const char* v = st->global[b][0] ? st->global[b] : "none";
        fprintf(f, "%-*s = %s\n", kw, spec->key_names[b], v);
    }
    fputc('\n', f);

    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i) {
        if (!st->maps[i].used) continue;
        fprintf(f, "[mapping.%s]\n", st->maps[i].guid);
        fprintf(f, "%-*s = %d\n", kw, "deadzone",
                clamp_dz(st->maps[i].deadzone_pct));
        fprintf(f, "%-*s = %d\n", kw, "name_custom",
                st->maps[i].name_custom ? 1 : 0);
        for (int b = 0; b < spec_count(spec); ++b) {
            const char* v = st->maps[i].src[b][0] ? st->maps[i].src[b] : "none";
            fprintf(f, "%-*s = %s\n", kw, spec->key_names[b], v);
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* An existing file that predates one of the spec's key groups is rewritten so
 * the newer keys appear. NULL prefixes = the console has never changed shape. */
static int file_has_probe_key(const RuiPadSpec* spec, const char* path) {
    if (!spec->legacy_probe_prefixes || !spec->legacy_probe_prefixes[0])
        return 1;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int hit = 0;
    while (!hit && fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        for (int i = 0; spec->legacy_probe_prefixes[i]; ++i) {
            const char* p = spec->legacy_probe_prefixes[i];
            if (!strncmp(s, p, strlen(p))) { hit = 1; break; }
        }
    }
    fclose(f);
    return hit;
}

static PadStore* ensure_init(const RuiPadSpec* spec, const char* path) {
    PadStore* st = store_for(spec);
    if (!st) return NULL;
    if (st->init && path && st->path[0] && !strcmp(st->path, path)) return st;
    copy_str(st->path, sizeof(st->path), path ? path : "input.ini");
    load_ini(st, st->path);
    int healed = heal_cardinals(spec, st->global);
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i) {
        if (st->maps[i].used)
            healed |= heal_cardinals(spec, st->maps[i].src);
    }
    FILE* test = fopen(st->path, "r");
    if (!test) {
        write_ini(st, st->path);
    } else {
        fclose(test);
        if (healed || !file_has_probe_key(spec, st->path))
            write_ini(st, st->path);
    }
    st->init = 1;
    return st;
}

static void source_from_bind(int kind, int code, int axis_dir,
                             char* out, size_t cap) {
    out[0] = 0;
    if (kind == 1) {
        const char* n = SDL_GetGamepadStringForButton((LNG_GamepadButton)code);
        copy_str(out, cap, (n && n[0]) ? n : "");
    } else if (kind == 2) {
        const char* n = SDL_GetGamepadStringForAxis((LNG_GamepadAxis)code);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%c", (n && n[0]) ? n : "axis",
                 axis_dir < 0 ? '-' : '+');
        copy_str(out, cap, buf);
    }
}

void rui_pad_binds_init(const RuiPadSpec* spec, const char* path) {
    PadStore* st = store_for(spec);
    if (!st) return;
    st->init = 0;
    ensure_init(spec, path ? path : "input.ini");
}

void rui_pad_binds_remember(const RuiPadSpec* spec, const char* path,
                            const char* guid, const char* name,
                            int deadzone_pct) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0]) return;
    PadGuidMap* m = alloc_map(st, guid);
    if (!m) return;
    // Driver-name refresh: never clobber a user rename.
    if (!m->name_custom && name && name[0] && strcmp(name, "Gamepad") != 0)
        copy_str(m->name, sizeof(m->name), name);
    else if (!m->name[0] && name && name[0] && strcmp(name, "Gamepad") != 0)
        copy_str(m->name, sizeof(m->name), name);
    if (deadzone_pct >= 0)
        m->deadzone_pct = clamp_dz(deadzone_pct);
    write_ini(st, st->path);
}

void rui_pad_binds_save_profile(const RuiPadSpec* spec, const char* path,
                                const char* guid, const char* name,
                                int name_custom, int deadzone_pct) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0]) return;
    PadGuidMap* m = alloc_map(st, guid);
    if (!m) return;
    if (name && name[0])
        copy_str(m->name, sizeof(m->name), name);
    m->name_custom = name_custom ? 1 : 0;
    m->deadzone_pct = clamp_dz(deadzone_pct >= 0 ? deadzone_pct
                                                 : spec->default_deadzone_pct);
    write_ini(st, st->path);
}

void rui_pad_binds_rename(const RuiPadSpec* spec, const char* path,
                          const char* guid, const char* name) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0] || !name || !name[0]) return;
    PadGuidMap* m = alloc_map(st, guid);
    if (!m) return;
    copy_str(m->name, sizeof(m->name), name);
    m->name_custom = 1;
    write_ini(st, st->path);
}

void rui_pad_binds_delete(const RuiPadSpec* spec, const char* path,
                          const char* guid) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0]) return;
    PadGuidMap* m = find_map(st, guid);
    if (!m) return;
    memset(m, 0, sizeof(*m));
    write_ini(st, st->path);
}

void rui_pad_binds_label(const RuiPadSpec* spec, const char* path,
                         const char* guid, int b, char* out, int cap) {
    PadStore* st = ensure_init(spec, path);
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!st || b < 0 || b >= spec_count(spec)) {
        copy_str(out, (size_t)cap, "(unbound)");
        return;
    }
    const char* src = NULL;
    if (guid && guid[0]) {
        PadGuidMap* m = find_map(st, guid);
        if (m) src = m->src[b];
    }
    if (!src || !src[0]) src = st->global[b];
    if (!src || !src[0]) src = spec->defaults[b];
    if (!src || !src[0]) copy_str(out, (size_t)cap, "(unbound)");
    else copy_str(out, (size_t)cap, src);
}

void rui_pad_binds_set(const RuiPadSpec* spec, const char* path,
                       const char* guid, int b, int kind, int code,
                       int axis_dir) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0]) return;
    if (b < 0 || b >= spec_count(spec)) return;
    PadGuidMap* m = alloc_map(st, guid);
    if (!m) return;
    source_from_bind(kind, code, axis_dir, m->src[b], RUI_PAD_SRC_CAP);
    write_ini(st, st->path);
}

void rui_pad_binds_reset(const RuiPadSpec* spec, const char* path,
                         const char* guid) {
    PadStore* st = ensure_init(spec, path);
    if (!st || !guid || !guid[0]) return;
    PadGuidMap* m = alloc_map(st, guid);
    if (!m) return;
    seed_defaults_into(spec, m->src);
    write_ini(st, st->path);
}

int rui_pad_binds_known_count(const RuiPadSpec* spec, const char* path) {
    PadStore* st = ensure_init(spec, path);
    if (!st) return 0;
    int n = 0;
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i)
        if (st->maps[i].used) ++n;
    return n;
}

int rui_pad_binds_known_at(const RuiPadSpec* spec, const char* path,
                           int index, char* guid, int guid_cap,
                           char* name, int name_cap) {
    PadStore* st = ensure_init(spec, path);
    if (!st || index < 0) return 0;
    int n = 0;
    for (int i = 0; i < RUI_PAD_MAX_KNOWN; ++i) {
        if (!st->maps[i].used) continue;
        if (n == index) {
            if (guid && guid_cap > 0)
                copy_str(guid, (size_t)guid_cap, st->maps[i].guid);
            if (name && name_cap > 0) {
                if (st->maps[i].name[0] &&
                    strcmp(st->maps[i].name, "Gamepad") != 0)
                    copy_str(name, (size_t)name_cap, st->maps[i].name);
                else {
                    size_t gl = strlen(st->maps[i].guid);
                    if (gl >= 8)
                        snprintf(name, (size_t)name_cap, "Controller …%s",
                                 st->maps[i].guid + gl - 8);
                    else
                        copy_str(name, (size_t)name_cap, "Controller");
                }
            }
            return 1;
        }
        ++n;
    }
    return 0;
}

void rui_pad_binds_name(const RuiPadSpec* spec, const char* path,
                        const char* guid, char* out, int cap) {
    PadStore* st = ensure_init(spec, path);
    if (!out || cap <= 0) return;
    out[0] = 0;
    PadGuidMap* m = find_map(st, guid);
    if (m && m->name[0]) copy_str(out, (size_t)cap, m->name);
}

int rui_pad_binds_name_is_custom(const RuiPadSpec* spec, const char* path,
                                 const char* guid) {
    PadStore* st = ensure_init(spec, path);
    PadGuidMap* m = find_map(st, guid);
    return (m && m->name_custom) ? 1 : 0;
}

int rui_pad_binds_deadzone(const RuiPadSpec* spec, const char* path,
                           const char* guid) {
    PadStore* st = ensure_init(spec, path);
    PadGuidMap* m = find_map(st, guid);
    if (!m) return spec->default_deadzone_pct;
    return clamp_dz(m->deadzone_pct);
}

/* input.ini lives beside the console's own bind file — the same directory the
 * runtime resolves, which is the exe directory in every shipped layout. Both
 * the launcher and the game's host process call this, so there is one rule for
 * where the file is rather than two that can drift. */
void rui_pad_binds_sibling_path(const char* ref, const char* file,
                                char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!file || !file[0]) return;
    const char* slash = ref ? strrchr(ref, '/') : NULL;
#ifdef _WIN32
    const char* bslash = ref ? strrchr(ref, '\\') : NULL;
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (!slash) { copy_str(out, (size_t)cap, file); return; }
    size_t dir_len = (size_t)(slash - ref + 1);
    if (dir_len >= (size_t)cap) dir_len = (size_t)cap - 1;
    memcpy(out, ref, dir_len);
    out[dir_len] = 0;
    size_t room = (size_t)cap - dir_len - 1;
    strncat(out, file, room);
}
