#include "launcher_theme_overrides.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVERRIDE_SECTION "colors"
#define OVERRIDE_MAX_FILE_BYTES (64 * 1024)
#define OVERRIDE_WARN_PREFIX "recomp-ui: theme_game.ini: "

typedef struct {
    const char* key;
    size_t offset;
} OverrideToken;

// Order defines the bit in LauncherThemeOverrides.set_mask.
static const OverrideToken k_tokens[LAUNCHER_THEME_OVERRIDE_TOKENS] = {
    {"background", offsetof(LauncherTheme, background)},
    {"background2", offsetof(LauncherTheme, background2)},
    {"panel", offsetof(LauncherTheme, panel)},
    {"panel_hovered", offsetof(LauncherTheme, panel_hovered)},
    {"control", offsetof(LauncherTheme, control)},
    {"control_hovered", offsetof(LauncherTheme, control_hovered)},
    {"border", offsetof(LauncherTheme, border)},
    {"accent", offsetof(LauncherTheme, accent)},
    {"accent_dim", offsetof(LauncherTheme, accent_dim)},
    {"accent_text", offsetof(LauncherTheme, accent_text)},
    {"accent2", offsetof(LauncherTheme, accent2)},
    {"text", offsetof(LauncherTheme, text)},
    {"text_muted", offsetof(LauncherTheme, text_muted)},
    {"good", offsetof(LauncherTheme, good)},
    {"warn", offsetof(LauncherTheme, warn)},
    {"focus_ring", offsetof(LauncherTheme, focus_ring)},
};

static LauncherThemeOverrides g_game_overrides;
static int g_game_overrides_loaded;

static void trim(const char** begin, const char** end) {
    while (*begin < *end && isspace((unsigned char)**begin)) ++*begin;
    while (*end > *begin && isspace((unsigned char)(*end)[-1])) --*end;
}

// Drops a trailing comment from a value: ';' anywhere, or '#' after whitespace.
// The value's own leading '#' (the color) is never a comment.
static void strip_inline_comment(const char* begin, const char** end) {
    for (const char* p = begin + 1; p < *end; ++p) {
        if (*p == ';' || (*p == '#' && isspace((unsigned char)p[-1]))) {
            *end = p;
            break;
        }
    }
    trim(&begin, end);
}

static int token_index(const char* key, size_t len) {
    for (int i = 0; i < LAUNCHER_THEME_OVERRIDE_TOKENS; ++i)
        if (strlen(k_tokens[i].key) == len && strncmp(k_tokens[i].key, key, len) == 0) return i;
    return -1;
}

static int hex_byte(const char* s, float* out) {
    if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return 0;
    const char pair[3] = {s[0], s[1], 0};
    *out = (float)strtoul(pair, NULL, 16) / 255.0f;
    return 1;
}

// "#RRGGBB" or "#RRGGBBAA"; anything else is rejected.
static int parse_color(const char* v, size_t len, LngColor* out) {
    if ((len != 7 && len != 9) || v[0] != '#') return 0;
    LngColor c = lng_rgba(0, 0, 0, 1.0f);
    if (!hex_byte(v + 1, &c.r) || !hex_byte(v + 3, &c.g) || !hex_byte(v + 5, &c.b)) return 0;
    if (len == 9 && !hex_byte(v + 7, &c.a)) return 0;
    *out = c;
    return 1;
}

static int parse_entry(const char* line, const char* end, LauncherThemeOverrides* out) {
    const char* eq = memchr(line, '=', (size_t)(end - line));
    if (!eq) return 0;
    const char *kb = line, *ke = eq, *vb = eq + 1, *ve = end;
    trim(&kb, &ke);
    trim(&vb, &ve);
    strip_inline_comment(vb, &ve);
    const int index = token_index(kb, (size_t)(ke - kb));
    LngColor color;
    if (index < 0 || !parse_color(vb, (size_t)(ve - vb), &color)) {
        fprintf(stderr, OVERRIDE_WARN_PREFIX "skipped \"%.*s\"\n", (int)(end - line), line);
        return 0;
    }
    out->color[index] = color;
    out->set_mask |= 1u << index;
    return 1;
}

int launcher_theme_overrides_parse(const char* text, LauncherThemeOverrides* out) {
    memset(out, 0, sizeof *out);
    int count = 0, in_section = 0;
    for (const char* p = text; p && *p;) {
        const char* nl = strchr(p, '\n');
        const char* end = nl ? nl : p + strlen(p);
        const char *b = p, *e = end;
        trim(&b, &e);
        if (b < e && *b == '[')
            in_section = (size_t)(e - b) == strlen(OVERRIDE_SECTION) + 2 &&
                         strncmp(b + 1, OVERRIDE_SECTION, strlen(OVERRIDE_SECTION)) == 0;
        else if (b < e && *b != ';' && in_section)
            count += parse_entry(b, e, out);
        p = nl ? nl + 1 : end;
    }
    return count;
}

void launcher_theme_overrides_apply(const LauncherThemeOverrides* o, LauncherTheme* t) {
    for (int i = 0; i < LAUNCHER_THEME_OVERRIDE_TOKENS; ++i)
        if (o->set_mask & (1u << i))
            *(LngColor*)((char*)t + k_tokens[i].offset) = o->color[i];
}

static char* read_small_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* buf = malloc(OVERRIDE_MAX_FILE_BYTES + 1);
    const size_t n = buf ? fread(buf, 1, OVERRIDE_MAX_FILE_BYTES, f) : 0;
    fclose(f);
    if (buf) buf[n] = '\0';
    return buf;
}

int launcher_theme_load_game_overrides(const char* path) {
    char* text = path ? read_small_file(path) : NULL;
    const int count = launcher_theme_overrides_parse(text, &g_game_overrides);
    free(text);
    g_game_overrides_loaded = 1;
    return count;
}

const LauncherThemeOverrides* launcher_theme_game_overrides(void) { return &g_game_overrides; }

int launcher_theme_game_overrides_loaded(void) { return g_game_overrides_loaded; }

void launcher_theme_apply_game_overrides(LauncherTheme* t) {
    launcher_theme_overrides_apply(&g_game_overrides, t);
}

LauncherTheme launcher_theme_for_game(const char* theme_name) {
    LauncherTheme t = launcher_theme_by_name(theme_name);
    launcher_theme_apply_game_overrides(&t);
    return t;
}
