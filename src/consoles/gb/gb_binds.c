// gb_binds.c — Game Boy-native keybind persistence bridge (see gb_binds.h).
//
// Reads/writes gb-recompiled's runtime_prefs.ini keyboard.<action>.<slot> =
// key:<sc> bindings (the store that actually drives input; the keybinds.ini
// [controls] file is vestigial). All writes are a SURGICAL upsert of the
// sixteen keyboard lines — every other line of runtime_prefs.ini is preserved.
// One key drives exactly one action: see gb_take_key() / gb_sanitise().

#include "gb_binds.h"
#include "gb_profile.h"           // LNG_GB_PAD_BUTTON_COUNT (rebind-spec order)
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Rebind-spec order (kGbPadButtons: Up/Down/Left/Right/A/B/Start/Select) ->
// engine input_action_config_name() spelling.
static const char* kGbActionName[LNG_GB_PAD_BUTTON_COUNT] = {
    "up", "down", "left", "right", "a", "b", "start", "select"
};

// gb-recompiled set_default_input_bindings() defaults, in rebind-spec order,
// slot 0 (primary) then slot 1 (secondary). Keep these identical to
// platform_sdl.cpp's table: the launcher writes every slot it knows about, so a
// disagreement here would silently rewrite the player's secondaries.
static const SDL_Scancode kGbDefaults[LNG_GB_PAD_BUTTON_COUNT][RUI_GB_BIND_SLOTS] = {
    /* Up    */ { SDL_SCANCODE_UP,     SDL_SCANCODE_W },
    /* Down  */ { SDL_SCANCODE_DOWN,   SDL_SCANCODE_S },
    /* Left  */ { SDL_SCANCODE_LEFT,   SDL_SCANCODE_A },
    /* Right */ { SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D },
    /* A     */ { SDL_SCANCODE_Z,      SDL_SCANCODE_J },
    /* B     */ { SDL_SCANCODE_X,      SDL_SCANCODE_K },
    /* Start */ { SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN },
    /* Select*/ { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RSHIFT },
};

// First pseudo-scancode a bind store may use for a mouse button (PSX's
// PSX_MOUSE_SC_BASE). Spelled locally rather than as SDL_NUM_SCANCODES /
// SDL_SCANCODE_COUNT because those are the SDL2 / SDL3 names for the same
// value and this tree builds against both.
#define RUI_GB_MOUSE_SC_BASE 512

static SDL_Scancode s_gb_binds[LNG_GB_PAD_BUTTON_COUNT][RUI_GB_BIND_SLOTS];
static int s_gb_binds_init = 0;

// ---- one key, one action -------------------------------------------------------
// Take `sc` away from every button/slot except (keep_b, keep_slot).
static void gb_take_key(SDL_Scancode sc, int keep_b, int keep_slot) {
    if (sc == SDL_SCANCODE_UNKNOWN) return;
    for (int b = 0; b < LNG_GB_PAD_BUTTON_COUNT; ++b)
        for (int slot = 0; slot < RUI_GB_BIND_SLOTS; ++slot) {
            if (b == keep_b && slot == keep_slot) continue;
            if (s_gb_binds[b][slot] == sc) s_gb_binds[b][slot] = SDL_SCANCODE_UNKNOWN;
        }
}

// Drop duplicates left by a file written before the one-key-one-action rule
// existed. runtime_prefs.ini carries no edit order, so recency is not
// derivable; keep the claim that differs from the shipped default, else slot 0,
// else the lower button index — the same tie-break gb-recompiled's
// sanitise_binding_conflicts() uses, so both halves agree on the survivor.
static void gb_sanitise(void) {
    for (int b = 0; b < LNG_GB_PAD_BUTTON_COUNT; ++b)
        for (int slot = 0; slot < RUI_GB_BIND_SLOTS; ++slot) {
            const SDL_Scancode sc = s_gb_binds[b][slot];
            if (sc == SDL_SCANCODE_UNKNOWN) continue;

            int keep_b = b, keep_slot = slot;
            int keep_claimed = (sc != kGbDefaults[b][slot]);
            for (int ob = 0; ob < LNG_GB_PAD_BUTTON_COUNT; ++ob)
                for (int os = 0; os < RUI_GB_BIND_SLOTS; ++os) {
                    if (ob == keep_b && os == keep_slot) continue;
                    if (s_gb_binds[ob][os] != sc) continue;
                    const int claimed = (sc != kGbDefaults[ob][os]);
                    int wins;
                    if (claimed != keep_claimed)   wins = claimed;
                    else if (os != keep_slot)      wins = (os < keep_slot);
                    else                           wins = (ob < keep_b);
                    if (wins) { keep_b = ob; keep_slot = os; keep_claimed = claimed; }
                }
            gb_take_key(sc, keep_b, keep_slot);
        }
}

// ---- runtime_prefs.ini parse ---------------------------------------------------
// Overlay keyboard.<action>.<slot> = key:<int> lines onto the current store.
static void gb_load_prefs(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == ';') continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = s; char* val = eq + 1;
        size_t kl = strlen(key); while (kl && isspace((unsigned char)key[kl-1])) key[--kl] = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        size_t vl = strlen(val);
        while (vl && (val[vl-1]=='\n'||val[vl-1]=='\r'||isspace((unsigned char)val[vl-1]))) val[--vl] = '\0';
        // Match "keyboard.<action>.<slot>" against our eight buttons.
        int matched = 0;
        for (int b = 0; b < LNG_GB_PAD_BUTTON_COUNT && !matched; ++b) {
            for (int slot = 0; slot < RUI_GB_BIND_SLOTS; ++slot) {
                char want[48];
                snprintf(want, sizeof(want), "keyboard.%s.%d", kGbActionName[b], slot);
                if (strcmp(key, want) != 0) continue;
                if (strncmp(val, "key:", 4) == 0)
                    s_gb_binds[b][slot] = (SDL_Scancode)strtol(val + 4, NULL, 10);
                else if (strcmp(val, "none") == 0)
                    s_gb_binds[b][slot] = SDL_SCANCODE_UNKNOWN;
                matched = 1;
                break;
            }
        }
    }
    fclose(f);
}

// ---- surgical upsert of the eight keyboard.<btn>.0 lines -----------------------
static char* gb_read_whole(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)(n > 0 ? n : 0) + 1);
    if (buf) { *out_len = (long)fread(buf, 1, (size_t)(n > 0 ? n : 0), f); buf[*out_len] = 0; }
    fclose(f);
    return buf;
}

// Does `line` (leading ws) assign flat key `key` (before the '=')?
static int gb_line_is_key(const char* line, const char* key) {
    const char* i = line;
    while (*i == ' ' || *i == '\t') ++i;
    size_t kl = strlen(key);
    if (strncmp(i, key, kl) != 0) return 0;
    i += kl;
    while (*i == ' ' || *i == '\t') ++i;
    return *i == '=';
}

static void gb_write_prefs(const char* path) {
    long len = 0; char* text = gb_read_whole(path, &len);

    // split into a growable line array, preserving blank lines
    int cap = 128, n = 0;
    char** lines = (char**)malloc(sizeof(char*) * cap);
    if (text) {
        char* start = text;
        for (long i = 0; i <= len; ++i) {
            if (i == len || text[i] == '\n') {
                char* end = text + i;
                if (end > start && end[-1] == '\r') end[-1] = 0;
                if (i < len) text[i] = 0; else text[i] = 0;
                if (i == len && start == text + len) break;   // no trailing empty
                if (n == cap) { cap *= 2; lines = (char**)realloc(lines, sizeof(char*) * cap); }
                lines[n++] = strdup(start);
                start = text + i + 1;
            }
        }
    }

    // upsert every button/slot binding
    for (int b = 0; b < LNG_GB_PAD_BUTTON_COUNT; ++b)
    for (int slot = 0; slot < RUI_GB_BIND_SLOTS; ++slot) {
        char key[48], assign[80];
        snprintf(key, sizeof(key), "keyboard.%s.%d", kGbActionName[b], slot);
        if (s_gb_binds[b][slot] == SDL_SCANCODE_UNKNOWN)
            snprintf(assign, sizeof(assign), "%s=none", key);
        else
            snprintf(assign, sizeof(assign), "%s=key:%d", key, (int)s_gb_binds[b][slot]);
        int hit = -1;
        for (int i = 0; i < n; ++i) if (gb_line_is_key(lines[i], key)) { hit = i; break; }
        if (hit >= 0) { free(lines[hit]); lines[hit] = strdup(assign); }
        else {
            if (n == cap) { cap += 8; lines = (char**)realloc(lines, sizeof(char*) * cap); }
            lines[n++] = strdup(assign);
        }
    }

    FILE* f = fopen(path, "wb");
    if (f) { for (int i = 0; i < n; ++i) { fputs(lines[i], f); fputc('\n', f); } fclose(f); }
    for (int i = 0; i < n; ++i) free(lines[i]);
    free(lines); free(text);
}

// ---- public API ----------------------------------------------------------------
void rui_gb_binds_init(const char* path) {
    memcpy(s_gb_binds, kGbDefaults, sizeof(kGbDefaults));
    if (path) gb_load_prefs(path);   // overlay; do not create the file
    gb_sanitise();                   // in memory only; the next write persists it
    s_gb_binds_init = 1;
}

int rui_gb_binds_get_slot(const char* path, int b, int slot) {
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    if (b < 0 || b >= LNG_GB_PAD_BUTTON_COUNT) return SDL_SCANCODE_UNKNOWN;
    if (slot < 0 || slot >= RUI_GB_BIND_SLOTS) return SDL_SCANCODE_UNKNOWN;
    return (int)s_gb_binds[b][slot];
}

int rui_gb_binds_get(const char* path, int b) {
    return rui_gb_binds_get_slot(path, b, 0);
}

void rui_gb_binds_set_slot(const char* path, int b, int slot, int scancode) {
    if (b < 0 || b >= LNG_GB_PAD_BUTTON_COUNT) return;
    if (slot < 0 || slot >= RUI_GB_BIND_SLOTS) return;
    // gb-recompiled stores a raw SDL_Scancode; a mouse pseudo-scancode would be
    // dropped by its binding_is_valid() and the rebind would look lost.
    if (scancode < 0 || scancode >= RUI_GB_MOUSE_SC_BASE) return;
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    gb_take_key((SDL_Scancode)scancode, b, slot);
    s_gb_binds[b][slot] = (SDL_Scancode)scancode;
    if (path) gb_write_prefs(path);
}

void rui_gb_binds_set(const char* path, int b, int scancode) {
    rui_gb_binds_set_slot(path, b, 0, scancode);
}

void rui_gb_binds_reset(const char* path) {
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    memcpy(s_gb_binds, kGbDefaults, sizeof(kGbDefaults));
    if (path) gb_write_prefs(path);
}
