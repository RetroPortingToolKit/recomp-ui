// launcher_binds.c — real binding persistence (keybinds.ini + config.ini [KeyMap]).

#include "launcher_binds.h"
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)
#include "keybinds.h"             // engine keyboard-binding store
#include "launcher_system.h"      // SystemProfile / ControllerSpec.button_count
#include "pad_binds.h"            // shared per-GUID input.ini store (path rule)
#include "consoles/psx/psx_binds.h"   // PSX-native keybind bridge (psx_keybinds.c format)
#include "consoles/psx/psx_pad_binds.h" // PSX gamepad input.ini per-GUID bridge
#include "consoles/n64/n64_binds.h"   // N64-native input.cfg bridge (keyboard table)
#include "consoles/n64/n64_pad_binds.h" // N64 gamepad input.ini per-GUID bridge
#include "consoles/nes/nes_binds.h"   // NES-native keybind bridge (nesrecomp keybinds.c format)
#include "consoles/genesis/genesis_binds.h"   // Genesis-native bridge (settings.ini [input.pN])
#include "consoles/gb/gb_binds.h"     // Game Boy-native bridge (keybinds.ini [controls])
#include "consoles/snes/snes_pad_binds.h" // SNES pad shape (default deadzone)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC spells these differently than POSIX.
#ifdef _MSC_VER
  #define strtok_r     strtok_s
  #define strncasecmp  _strnicmp
  #define strdup       _strdup
#endif

const char* g_launcher_config_path = NULL;
const char* g_launcher_keybinds_path = NULL;

// Per-system rebind-spec index -> keybinds.c button index. keybinds order is
// a,b,x,y,l,r,start,select,up,down,left,right,l2,r2,l3,r3 (see keybinds.h).
//
// SNES rebind spec order (launcher_system.h kSnesPadButtons): Up,Down,Left,
// Right,A,B,X,Y,L,R,Start,Select. UNCHANGED from before per-system vocab —
// SNES bind persistence stays byte-identical (indices 0..11, same mapping).
//
// PSX no longer routes through this generic table/keybinds.c at all — see
// consoles/psx/psx_binds.c, which persists through psxrecomp's own
// psx_keybinds.c format instead so rebinds actually reach the game (routing
// PSX's 24 buttons through this 16-slot generic format silently discarded
// the 8 stick-direction binds and, worse, wrote a file the game's runtime
// can't parse at all). Every profile that still uses this table today
// borrows kSnesPadButtons (launcher_system.h stub macro), so it always
// matches LNG_SNES_PAD_BUTTON_COUNT.
static const int kKbIndexSnes[LNG_SNES_PAD_BUTTON_COUNT] = {
    /* UP    */ 8, /* DOWN  */ 9, /* LEFT  */ 10, /* RIGHT */ 11,
    /* A     */ 0, /* B     */ 1, /* X     */ 2,  /* Y     */ 3,
    /* L     */ 4, /* R     */ 5, /* START */ 6,  /* SELECT*/ 7,
};

// Resolve the keybinds-index table (and its length) for the model's ACTIVE
// SystemProfile. PSX is handled entirely by the native bridge below (see
// is_psx_profile()) before this is ever consulted; GBA persists through the
// generic store with its own 10-button vocabulary (kGbaKbIndex,
// consoles/gba/gba_profile.h); everything else is SNES-shaped (12 buttons).
static const int* active_kb_index(const LauncherModel* m, int* out_n) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    if (prof && prof->id && !strcmp(prof->id, "gba")) {
        *out_n = LNG_GBA_PAD_BUTTON_COUNT;
        return kGbaKbIndex;
    }
    *out_n = LNG_SNES_PAD_BUTTON_COUNT;
    return kKbIndexSnes;
}

// ---- PSX-native keybind bridge ---------------------------------------------
// Lives in consoles/psx/psx_binds.c (the console's own unit) — for a PSX
// SystemProfile, persistence routes through psxrecomp's native 24-scancode
// psx_keybinds.c format instead of the generic keybinds.c store, so rebinds
// actually reach the game. This file only decides WHICH store to use (by the
// active profile) and resolves the file path.
static int is_psx_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "psx");
}

// NES routes to its own native bridge too (consoles/nes/nes_binds.c): the
// nesrecomp runner's keybinds.ini format has 8 NES-named keys plus [zapper]
// and [gamepad1]/[gamepad2] sections the generic store's whole-file rewrite
// would destroy.
static int is_nes_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "nes");
}

static const char* keybinds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "keybinds.ini";
}

// input.ini lives next to keybinds.ini (same exe dir the runtime uses). The
// directory rule itself is in common/pad_binds.c because the GAME process has
// to resolve the same file; two copies would be two places to disagree.
static const char* psx_input_ini_path(void) {
    static char buf[1024];
    rui_pad_binds_sibling_path(keybinds_file_path(), "input.ini",
                               buf, (int)sizeof(buf));
    return buf;
}

static const char* psx_player_guid(const LauncherModel* m, int player /*1-based*/) {
    if (!m || player < 1 || player > LNG_MAX_PLAYERS) return "";
    return m->s.player_gamepad_guid[player - 1];
}

/* Defined below; load() hydrates names after init. */
void launcher_binds_hydrate_psx_pad_names(LauncherModel* m);

// ---- N64-native input.cfg bridge --------------------------------------------
// Lives in consoles/n64/n64_binds.c. The N64 runners persist bindings in
// their own input.cfg format — TWO device-type tables (keyboard, controller —
// shared by all pads, NOT per-port) with two alternate slots per input, where
// a controller bind can be a pad button, a signed pad axis, or a raw joystick
// field. This file only decides WHICH store to use and, for N64, WHICH device
// table a player edits: the one their current input source selects.
static int is_n64_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "n64");
}

static const char* n64_binds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "input.cfg";
}

// Device table (0 kb / 1 pad) player p's Configure page edits.
//
// Only the KEYBOARD table is still addressed this way. The controller half
// moved to the per-GUID store (consoles/n64/n64_pad_binds.h), because
// input.cfg's `pad.*` table is shared by every controller ever plugged in:
// two players could not hold different layouts, and swapping a pad silently
// inherited the other one's mapping.
static int n64_device_for_player(const LauncherModel* m, int player /*0-based*/) {
    return m->s.player_src[player] == 2 ? 1 : 0;
}

// input.ini beside input.cfg — same rule, same shared helper, as PSX above.
static const char* n64_input_ini_path(void) {
    static char buf[1024];
    rui_n64_pad_binds_path(n64_binds_file_path(), buf, (int)sizeof(buf));
    return buf;
}

// Translate a launcher pad bind (kind/code/axis_dir) into an input.cfg field
// and write it to the shared controller table, slot 0. Slot 1 is cleared by
// the store itself, matching PSR's load-order contract.
static void n64_mirror_pad_bind_to_cfg(int b, int kind, int code, int axis_dir) {
    int type;
    if (kind == LNG_PADBIND_BUTTON)   type = RUI_N64_FIELD_PAD_BUTTON;
    else if (kind == LNG_PADBIND_AXIS) type = axis_dir < 0 ? RUI_N64_FIELD_PAD_AXIS_N
                                                           : RUI_N64_FIELD_PAD_AXIS_P;
    else                               type = RUI_N64_FIELD_NONE;
    rui_n64_binds_set(n64_binds_file_path(), 1, b, 0, type,
                      type == RUI_N64_FIELD_NONE ? -1 : code);
}

static const char* n64_player_guid(const LauncherModel* m, int player /*1-based*/) {
    if (!m || player < 1 || player > LNG_MAX_PLAYERS) return "";
    return m->s.player_gamepad_guid[player - 1];
}

// ---- Genesis-native bind bridge ----------------------------------------------
// Lives in consoles/genesis/genesis_binds.c — for a Genesis SystemProfile,
// persistence routes through segagenesisrecomp's own settings.ini
// [input.pN] key.<Name>/pad.<Name> format (runner/app_config.c) instead of
// the generic keybinds.c store, so rebinds actually reach the game's
// g_input_map. Note the DIFFERENT default filename: the Genesis engine's
// bind store is settings.ini, not keybinds.ini.
static int is_genesis_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "genesis");
}

// ---- Game Boy-native bind bridge ---------------------------------------------
// Lives in consoles/gb/gb_binds.c — for a gb/gbc SystemProfile, persistence
// routes through gb-recompiled's own keybinds.ini [controls] format (SDL
// scancode names) instead of the generic keybinds.c store, so rebinds reach
// the game. Single player (the Game Boy is a one-player handheld). Both the
// "gb" and "gbc" profiles share this bridge; the default file is keybinds.ini.
static int is_gb_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && (!strcmp(prof->id, "gb") || !strcmp(prof->id, "gbc"));
}

static const char* gb_binds_file_path(void) {
    // gb-recompiled drives input from runtime_prefs.ini keyboard.<btn>.0 lines
    // (keybinds.ini [controls] is vestigial). The seam points keybinds_path at
    // the exe-anchored runtime_prefs.ini; fall back to it by name too.
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "runtime_prefs.ini";
}

static const char* genesis_binds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "settings.ini";
}

// The engine's config.ini [KeyMap] keys, in LngHotkey order.
static const char* kHotkeyKey[] = {
    "Fullscreen", "Reset", "Pause", "PauseDimmed", "Turbo",
    "WindowBigger", "WindowSmaller", "VolumeUp", "VolumeDown",
    "DisplayPerf", "ToggleRenderer",
    "SolarBrighter", "SolarDimmer", "SolarLive",
    "Rewind", "SaveStateMenu", "OpenLauncher", "TurboToggle"
};
// Built-in defaults (shown when config.ini has no line; "" = unbound).
static const char* kHotkeyDef[] = {
    "Alt+Return", "Ctrl+R", "Shift+P", "P", "Tab",
    /* WindowBigger/Smaller unbound; VolumeUp/Down default keypad +/-
     * (psxrecomp host_keymap reads these from [KeyMap]). */
    "", "", "Keypad +", "Keypad -", "F", "R",
    "", "", "",
    "F8", "F7", "Ctrl+L", "F9"
};
/* Sized by their initializers, so a hotkey added to LngHotkey without a key
 * and a default here fails the build instead of reading a NULL. */
typedef char kHotkeyKey_covers_every_hotkey[
    (sizeof(kHotkeyKey) / sizeof(kHotkeyKey[0]) == LNG_HK_COUNT) ? 1 : -1];
typedef char kHotkeyDef_covers_every_hotkey[
    (sizeof(kHotkeyDef) / sizeof(kHotkeyDef[0]) == LNG_HK_COUNT) ? 1 : -1];

static void copy_str(char* d, size_t cap, const char* s) {
    if (!d || !cap) return;
    if (!s) { d[0] = 0; return; }
    size_t n = strlen(s); if (n >= cap) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
}

static const char* scancode_label(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "(unbound)";
    /* Mouse buttons are bound as pseudo-scancodes ABOVE SDL keyboard space
     * (512 + SDL button), so SDL_GetScancodeName knows nothing about them
     * and every mouse bind rendered as "(unbound)" - the bind had actually
     * been written and persisted, it was purely a display failure. */
    if ((int)sc > 512 && (int)sc <= 517) {
        static const char* const kMouseNames[5] =
            { "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5" };
        return kMouseNames[(int)sc - 513];
    }
    const char* n = SDL_GetScancodeName(sc);
    return (n && n[0]) ? n : "(unbound)";
}

static int is_snes_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "snes");
}

static void snes_gamepad_read_controls(int player, char out[LNG_SNES_PAD_BUTTON_COUNT][48]);
static void snes_gamepad_label(const char* token, char* out, size_t cap);
static int snes_gamepad_set_button(LauncherModel* m, int player, int b,
                                   int kind, int code, int axis_dir);
static void snes_gamepad_reset_player(int player);

// Display label for a Genesis gamepad bind: SDL's own controller button/axis
// names ("dpup", "a", "leftshoulder"; axes get a direction suffix, "leftx+").
static void genesis_pad_label(int kind, int code, int axis_dir, char* out, size_t cap) {
    if (kind == RUI_GEN_BIND_BUTTON) {
        const char* n = SDL_GetGamepadStringForButton((LNG_GamepadButton)code);
        copy_str(out, cap, (n && n[0]) ? n : "(unbound)");
    } else if (kind == RUI_GEN_BIND_AXIS) {
        const char* n = SDL_GetGamepadStringForAxis((LNG_GamepadAxis)code);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%c", (n && n[0]) ? n : "axis",
                 axis_dir < 0 ? '-' : '+');
        copy_str(out, cap, buf);
    } else {
        copy_str(out, cap, "(unbound)");
    }
}

static void reload_player_display(LauncherModel* m, int player) {
    if (is_psx_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        const char* guid = psx_player_guid(m, player);
        for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b) {
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_psx_binds_get_slot(
                         keybinds_file_path(), player - 1, b, 0)));
            copy_str(m->binds_alt[player - 1][b],
                     sizeof(m->binds_alt[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_psx_binds_get_slot(
                         keybinds_file_path(), player - 1, b, 1)));
            rui_psx_pad_binds_label(psx_input_ini_path(), guid, b,
                                    m->pad_binds[player - 1][b],
                                    (int)sizeof(m->pad_binds[player - 1][b]));
        }
        return;
    }
    if (is_nes_profile(m)) {
        for (int b = 0; b < LNG_NES_PAD_BUTTON_COUNT; ++b)
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_nes_binds_get(
                         keybinds_file_path(), player - 1, b)));
        return;
    }
    if (is_genesis_profile(m)) {
        const char* path = genesis_binds_file_path();
        for (int b = 0; b < LNG_GENESIS_PAD_BUTTON_COUNT; ++b) {
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_genesis_binds_get_key(path, player - 1, b)));
            int kind = 0, code = 0, dir = 0;
            rui_genesis_binds_get_pad(path, player - 1, b, &kind, &code, &dir);
            genesis_pad_label(kind, code, dir,
                              m->pad_binds[player - 1][b], sizeof(m->pad_binds[player - 1][b]));
        }
        return;
    }
    if (is_gb_profile(m)) {
        if (player != 1) return;   // Game Boy is single-player
        const char* path = gb_binds_file_path();
        for (int b = 0; b < LNG_GB_PAD_BUTTON_COUNT; ++b) {
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_gb_binds_get_slot(path, b, 0)));
            copy_str(m->binds_alt[player - 1][b], sizeof(m->binds_alt[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_gb_binds_get_slot(path, b, 1)));
        }
        return;
    }
    if (is_n64_profile(m)) {
        // Two stores, one row each. The KEY chip is input.cfg's keyboard table
        // (device 0 — still per-device-TYPE, which is right for a keyboard:
        // there is one). The GAMEPAD chip is this pad's own input.ini section.
        // binds_alt is deliberately not filled: the page is single-chip now
        // (n64_profile.h binds_per_input), so a second label nothing draws
        // would only be a stale string waiting to be believed.
        const char* guid = n64_player_guid(m, player);
        for (int b = 0; b < LNG_N64_PAD_BUTTON_COUNT; ++b) {
            int type = 0, id = -1;
            rui_n64_binds_get(n64_binds_file_path(), 0, b, 0, &type, &id);
            rui_n64_binds_label(type, id, m->binds[player - 1][b],
                                sizeof(m->binds[player - 1][b]));
            rui_n64_pad_binds_source(n64_input_ini_path(), guid, b,
                                     m->pad_binds[player - 1][b],
                                     (int)sizeof(m->pad_binds[player - 1][b]));
        }
        return;
    }
    if (player > 2) return;   // generic keybinds.c store is 2-player
    int n = 0;
    const int* kb_index = active_kb_index(m, &n);
    for (int b = 0; b < n; ++b) {
        SDL_Scancode sc = recompui_keybinds_get_button(player, kb_index[b]);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]), scancode_label(sc));
    }
    if (is_snes_profile(m)) {
        char controls[LNG_SNES_PAD_BUTTON_COUNT][48];
        snes_gamepad_read_controls(player - 1, controls);
        for (int b = 0; b < LNG_SNES_PAD_BUTTON_COUNT; ++b)
            snes_gamepad_label(controls[b], m->pad_binds[player - 1][b],
                               sizeof(m->pad_binds[player - 1][b]));
    }
}

void launcher_binds_refresh_camera(LauncherModel* m) {
    if (!m || !is_nes_profile(m)) return;
    for (int action = 0; action < LNG_CAMERA_BIND_COUNT; ++action) {
        copy_str(
            m->camera_binds[action], sizeof(m->camera_binds[action]),
            scancode_label((SDL_Scancode)rui_nes_camera_bind_get(
                keybinds_file_path(), action)));
    }
}

// ---- config.ini [KeyMap] surgical read/write (ported from the legacy launcher) --

static int ieq(const char* a, size_t alen, const char* b) {
    size_t bl = strlen(b);
    if (alen != bl) return 0;
    for (size_t i = 0; i < alen; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static char* read_whole(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (buf) { *out_len = (long)fread(buf, 1, (size_t)n, f); buf[*out_len] = 0; }
    fclose(f);
    return buf;
}

static const char* config_path(void) {
    return (g_launcher_config_path && g_launcher_config_path[0])
             ? g_launcher_config_path : "config.ini";
}

// Fill m->hotkeys[] from config.ini [KeyMap] (or defaults where absent).
static void reload_hotkey_display(LauncherModel* m) {
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), kHotkeyDef[h]);

    long len = 0; char* text = read_whole(config_path(), &len);
    if (!text) return;

    int in_keymap = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        size_t l = strlen(p);
        while (l && (p[l-1] == '\r' || p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char* close = strchr(p, ']');
            size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
            in_keymap = ieq(p + 1, sl, "KeyMap");
            continue;
        }
        if (!in_keymap) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* ke = eq; while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        size_t klen = (size_t)(ke - p);
        char* v = eq + 1; while (*v == ' ' || *v == '\t') ++v;
        char* hash = strchr(v, '#'); if (hash) *hash = 0;
        size_t vl = strlen(v); while (vl && (v[vl-1] == ' ' || v[vl-1] == '\t')) v[--vl] = 0;
        for (int h = 0; h < LNG_HK_COUNT; ++h)
            if (ieq(p, klen, kHotkeyKey[h])) {
                const int unbound = !v[0] || ieq(v, strlen(v), "None") ||
                                    ieq(v, strlen(v), "(unbound)");
                copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), unbound ? "(unbound)" : v);
                break;
            }
    }
    free(text);
}

#include "launcher_ini.inc"

// Original config.ini [KeyMap] entry point, now a thin wrapper.
static void keymap_write(const char* key, const char* value) {
    launcher_ini_kv_write(config_path(), "KeyMap", key, value);
}

// ---- SNES config.ini [GamepadMap] bridge -----------------------------------
// Mega Man X already persists gamepad input through config.ini [GamepadMap].
// The launcher presents SNES buttons in UI order, while the runtime's Controls
// line is command order: Up,Down,Left,Right,Select,Start,A,B,X,Y,L,R.
static const int kSnesUiToGamepadConfig[LNG_SNES_PAD_BUTTON_COUNT] = {
    0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 5, 4
};

static const char* const kSnesGamepadDefaultControls[LNG_SNES_PAD_BUTTON_COUNT] = {
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Back", "Start",
    "B", "A", "Y", "X", "Lb", "Rb",
};

static const char* snes_gamepad_config_key(int player) {
    return player == 1 ? "ControlsP2" : "Controls";
}

static char* trim_token(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;
    return s;
}

static int token_eq(const char* a, const char* b) {
    return ieq(a, strlen(a), b);
}

static void snes_gamepad_read_config_order(int player,
                                           char out[LNG_SNES_PAD_BUTTON_COUNT][48]) {
    for (int i = 0; i < LNG_SNES_PAD_BUTTON_COUNT; ++i)
        copy_str(out[i], sizeof(out[i]), kSnesGamepadDefaultControls[i]);

    long len = 0;
    char* text = read_whole(config_path(), &len);
    if (!text) return;

    const char* key = snes_gamepad_config_key(player);
    int in_gamepad = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char* p = trim_token(line);
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char* close = strchr(p, ']');
            size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
            in_gamepad = ieq(p + 1, sl, "GamepadMap");
            continue;
        }
        if (!in_gamepad) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* ke = eq;
        while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        if (!ieq(p, (size_t)(ke - p), key)) continue;

        char* value = eq + 1;
        char* hash = strchr(value, '#');
        if (hash) *hash = 0;
        /* Split POSITIONALLY. An empty entry is this store's unbound value
         * (the runtime's ParseGamepadArray keeps it in place and skips it),
         * and strtok would have collapsed a leading one, shifting every
         * control after a cleared D-Pad Up one seat to the left. */
        int i = 0;
        char* item = value;
        while (i < LNG_SNES_PAD_BUTTON_COUNT) {
            char* comma = strchr(item, ',');
            if (comma) *comma = 0;
            copy_str(out[i], sizeof(out[i]), trim_token(item));
            ++i;
            if (!comma) break;
            item = comma + 1;
        }
        break;
    }
    free(text);
}

static void snes_gamepad_read_controls(int player,
                                       char out[LNG_SNES_PAD_BUTTON_COUNT][48]) {
    char config_order[LNG_SNES_PAD_BUTTON_COUNT][48];
    snes_gamepad_read_config_order(player, config_order);
    for (int b = 0; b < LNG_SNES_PAD_BUTTON_COUNT; ++b)
        copy_str(out[b], sizeof(out[b]), config_order[kSnesUiToGamepadConfig[b]]);
}

static void snes_gamepad_write_config_order(int player,
                                            char controls[LNG_SNES_PAD_BUTTON_COUNT][48]) {
    char value[768] = {};
    for (int i = 0; i < LNG_SNES_PAD_BUTTON_COUNT; ++i) {
        if (i) strncat(value, ", ", sizeof(value) - strlen(value) - 1);
        strncat(value, controls[i], sizeof(value) - strlen(value) - 1);
    }
    launcher_ini_kv_write(config_path(), "GamepadMap",
                          snes_gamepad_config_key(player), value);
}

static void snes_gamepad_label(const char* token, char* out, size_t cap) {
    if (!token || !token[0]) { copy_str(out, cap, "(unbound)"); return; }
    if (token_eq(token, "DpadUp"))    { copy_str(out, cap, "D-Pad Up"); return; }
    if (token_eq(token, "DpadDown"))  { copy_str(out, cap, "D-Pad Down"); return; }
    if (token_eq(token, "DpadLeft"))  { copy_str(out, cap, "D-Pad Left"); return; }
    if (token_eq(token, "DpadRight")) { copy_str(out, cap, "D-Pad Right"); return; }
    if (token_eq(token, "Back"))      { copy_str(out, cap, "Back"); return; }
    if (token_eq(token, "Guide"))     { copy_str(out, cap, "Guide"); return; }
    if (token_eq(token, "Start"))     { copy_str(out, cap, "Start"); return; }
    if (token_eq(token, "L1") || token_eq(token, "Lb")) { copy_str(out, cap, "L1"); return; }
    if (token_eq(token, "R1") || token_eq(token, "Rb")) { copy_str(out, cap, "R1"); return; }
    if (token_eq(token, "L2"))        { copy_str(out, cap, "L2"); return; }
    if (token_eq(token, "R2"))        { copy_str(out, cap, "R2"); return; }
    if (token_eq(token, "L3"))        { copy_str(out, cap, "L3"); return; }
    if (token_eq(token, "R3"))        { copy_str(out, cap, "R3"); return; }
    copy_str(out, cap, token);
}

static const char* snes_gamepad_token_for_capture(int kind, int code, int axis_dir) {
    if (kind == LNG_PADBIND_BUTTON) {
#if defined(LNG_SDL3)
        switch ((SDL_GamepadButton)code) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return "A";
        case SDL_GAMEPAD_BUTTON_EAST:           return "B";
        case SDL_GAMEPAD_BUTTON_WEST:           return "X";
        case SDL_GAMEPAD_BUTTON_NORTH:          return "Y";
        case SDL_GAMEPAD_BUTTON_BACK:           return "Back";
        case SDL_GAMEPAD_BUTTON_GUIDE:          return "Guide";
        case SDL_GAMEPAD_BUTTON_START:          return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return "L3";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return "R3";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return "Lb";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Rb";
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return "DpadUp";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return "DpadDown";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return "DpadLeft";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return "DpadRight";
        default:                                return NULL;
        }
#else
        switch ((SDL_GameControllerButton)code) {
        case SDL_CONTROLLER_BUTTON_A:             return "A";
        case SDL_CONTROLLER_BUTTON_B:             return "B";
        case SDL_CONTROLLER_BUTTON_X:             return "X";
        case SDL_CONTROLLER_BUTTON_Y:             return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:          return "Back";
        case SDL_CONTROLLER_BUTTON_GUIDE:         return "Guide";
        case SDL_CONTROLLER_BUTTON_START:         return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "R3";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "Lb";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Rb";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "DpadUp";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "DpadDown";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "DpadLeft";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "DpadRight";
        default:                                  return NULL;
        }
#endif
    }
    if (kind == LNG_PADBIND_AXIS && axis_dir >= 0) {
#if defined(LNG_SDL3)
        if (code == (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return "L2";
        if (code == (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return "R2";
#else
        if (code == (int)SDL_CONTROLLER_AXIS_TRIGGERLEFT) return "L2";
        if (code == (int)SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return "R2";
#endif
    }
    return NULL;
}

static int snes_gamepad_set_button(LauncherModel* m, int player, int b,
                                   int kind, int code, int axis_dir) {
    if (!m || player < 0 || player > 1 || b < 0 || b >= LNG_SNES_PAD_BUTTON_COUNT)
        return 0;
    /* An empty token IS the store's unbound value (snes_gamepad_label draws
     * it as "(unbound)"), so a clear writes one rather than being refused. */
    const char* token = (kind == LNG_PADBIND_NONE)
                            ? ""
                            : snes_gamepad_token_for_capture(kind, code, axis_dir);
    if (!token) return 0;

    char controls[LNG_SNES_PAD_BUTTON_COUNT][48];
    snes_gamepad_read_config_order(player, controls);
    copy_str(controls[kSnesUiToGamepadConfig[b]], sizeof(controls[0]), token);
    snes_gamepad_write_config_order(player, controls);
    snes_gamepad_label(token, m->pad_binds[player][b],
                       sizeof(m->pad_binds[player][b]));
    return 1;
}

/* ---- SNES per-GUID controller profiles ---------------------------------
 *
 * A profile is one section of the game's own config.ini:
 *
 *   [Controller.<guid>]
 *   Name       = DualSense Wireless Controller   ; display label
 *   NameCustom = 1                               ; the player renamed it
 *   Deadzone   = 10
 *   Controls   = DpadUp, DpadDown, ...           ; the [GamepadMap] line
 *
 * Same shape PSX keeps in input.ini, in the file this console already uses --
 * the game reads [GamepadMap] and knows nothing about profiles, so a saved one
 * only takes effect when it is applied back onto that section. Storing the
 * mapping per GUID is what lets two different pads keep different layouts
 * instead of overwriting one shared line.
 *
 * The identity is the GUID: SDL instance ids are per-connection. */
static void snes_profile_section(const char* guid, char* out, size_t cap) {
    snprintf(out, cap, "Controller.%s", guid ? guid : "");
}

/* One key out of one section. Mirrors snes_gamepad_read_config_order's walk
 * rather than adding a general ini reader for four call sites. */
static int snes_profile_get(const char* guid, const char* key,
                            char* out, size_t cap) {
    char want[96];
    long len = 0;
    char* text;
    char* save = NULL;
    int in_sect = 0;
    int found = 0;

    if (out && cap) out[0] = '\0';
    if (!guid || !guid[0] || !key || !out || !cap) return 0;
    snes_profile_section(guid, want, sizeof(want));
    text = read_whole(config_path(), &len);
    if (!text) return 0;

    for (char* line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char* p = trim_token(line);
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char* close = strchr(p, ']');
            size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
            in_sect = ieq(p + 1, sl, want);
            continue;
        }
        if (!in_sect) continue;
        {
            char* eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            if (ieq(trim_token(p), strlen(trim_token(p)), key)) {
                copy_str(out, cap, trim_token(eq + 1));
                found = out[0] != '\0';
                break;
            }
        }
    }
    free(text);
    return found;
}

int launcher_binds_snes_profile_exists(LauncherModel* m, int player) {
    char v[64];
    if (!m || !is_snes_profile(m) || player < 1 || player > 2) return 0;
    return snes_profile_get(m->s.player_gamepad_guid[player - 1], "Controls",
                            v, sizeof(v)) ||
           snes_profile_get(m->s.player_gamepad_guid[player - 1], "Name",
                            v, sizeof(v));
}

/* Put a readable name on a slot restored from settings.
 *
 * Settings carry the GUID -- the only identity that survives a reconnect --
 * but not a label, so a restored slot showed "050057564c05..." in the Input
 * source box where the device name belongs. The saved profile's Name is
 * preferred (it may be the player's own rename); otherwise the live device is
 * asked, and failing both the slot keeps whatever it had. */
void launcher_binds_hydrate_snes_pad_names(LauncherModel* m,
                                           const LauncherPad* pads,
                                           int pad_count) {
    if (!m || !is_snes_profile(m)) return;

    /* A slot whose source is "gamepad" with no GUID is what a first launch
     * looks like: config.ini says EnableGamepad, and nothing has ever named a
     * device. The Input source box then reads the generic placeholder while
     * the game is in fact about to use a specific controller. Bind the first
     * live pad no other slot has claimed, so the box names the pad it will
     * actually play with -- and apply that pad's saved profile, or the console
     * default deadzone when it has none. One shot: once a GUID is set this
     * whole block is skipped, so it never fights a later choice. */
    for (int p = 0; p < 2 && pads; ++p) {
        if (m->s.player_src[p] != 2 || m->s.player_gamepad_guid[p][0]) continue;
        for (int i = 0; i < pad_count; ++i) {
            int taken = 0;
            if (!pads[i].guid[0]) continue;
            for (int o = 0; o < 2; ++o)
                if (o != p && m->s.player_src[o] == 2 &&
                    !strcmp(m->s.player_gamepad_guid[o], pads[i].guid))
                    taken = 1;
            if (taken) continue;
            launcher_model_set_source(m, p, 2, pads[i].id, pads[i].name,
                                      pads[i].guid);
            launcher_binds_apply_snes_pad_profile(m, p + 1);
            break;
        }
    }

    for (int p = 0; p < 2; ++p) {
        const char* guid = m->s.player_gamepad_guid[p];
        char name[64];
        if (m->s.player_src[p] != 2 || !guid[0]) continue;
        if (snes_profile_get(guid, "Name", name, sizeof(name)) && name[0]) {
            copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), name);
            continue;
        }
        if (!pads) continue;
        for (int i = 0; i < pad_count; ++i) {
            if (pads[i].guid[0] && !strcmp(pads[i].guid, guid)) {
                copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]),
                         pads[i].name);
                m->player_pad_id[p] = pads[i].id;
                break;
            }
        }
    }
}

void launcher_binds_save_snes_gamepad(LauncherModel* m, int player) {
    char sect[96];
    char value[768] = {0};
    char controls[LNG_SNES_PAD_BUTTON_COUNT][48];
    char num[16];
    const char* guid;
    int dz;

    if (!m || !is_snes_profile(m) || player < 1 || player > 2) return;
    guid = m->s.player_gamepad_guid[player - 1];
    if (!guid[0]) return;                 /* nothing to key the profile on */
    snes_profile_section(guid, sect, sizeof(sect));

    /* The mapping as it stands in [GamepadMap] -- the profile records what the
     * game would actually run, not a separate copy that could drift from it. */
    snes_gamepad_read_config_order(player - 1, controls);
    for (int i = 0; i < LNG_SNES_PAD_BUTTON_COUNT; ++i) {
        if (i) strncat(value, ", ", sizeof(value) - strlen(value) - 1);
        strncat(value, controls[i], sizeof(value) - strlen(value) - 1);
    }
    launcher_ini_kv_write(config_path(), sect, "Controls", value);

    dz = m->s.deadzone[player - 1];
    if (dz < 0) dz = 0;
    if (dz > 100) dz = 100;
    snprintf(num, sizeof(num), "%d", dz);
    launcher_ini_kv_write(config_path(), sect, "Deadzone", num);

    if (m->player_pad_name[player - 1][0])
        launcher_ini_kv_write(config_path(), sect, "Name",
                              m->player_pad_name[player - 1]);
}

void launcher_binds_rename_snes_gamepad(LauncherModel* m, int player,
                                        const char* name) {
    char sect[96];
    const char* guid;
    if (!m || !is_snes_profile(m) || player < 1 || player > 2) return;
    guid = m->s.player_gamepad_guid[player - 1];
    if (!guid[0] || !name || !name[0]) return;
    snes_profile_section(guid, sect, sizeof(sect));
    launcher_ini_kv_write(config_path(), sect, "Name", name);
    /* Marked custom so a later reconnect does not overwrite the player's name
     * with whatever SDL reports the device as. */
    launcher_ini_kv_write(config_path(), sect, "NameCustom", "1");
    copy_str(m->player_pad_name[player - 1],
             sizeof(m->player_pad_name[player - 1]), name);
}

void launcher_binds_delete_snes_gamepad(LauncherModel* m, int player) {
    char sect[96];
    const char* guid;
    if (!m || !is_snes_profile(m) || player < 1 || player > 2) return;
    guid = m->s.player_gamepad_guid[player - 1];
    if (!guid[0]) return;
    snes_profile_section(guid, sect, sizeof(sect));
    /* Emptied rather than excised: the surgical writer edits keys in place and
     * has no notion of removing a section, and an empty Controls reads as "no
     * profile" everywhere it is consulted. Leaving a stale mapping behind
     * would be the one outcome a Delete must not produce. */
    launcher_ini_kv_write(config_path(), sect, "Controls", "");
    launcher_ini_kv_write(config_path(), sect, "Name", "");
    launcher_ini_kv_write(config_path(), sect, "NameCustom", "0");
    launcher_ini_kv_write(config_path(), sect, "Deadzone", "");
}

/* Apply a saved profile onto the live [GamepadMap] line and the model. Called
 * when a pad becomes a player's source, so selecting a controller restores the
 * layout that controller was configured with. */
void launcher_binds_apply_snes_pad_profile(LauncherModel* m, int player) {
    char stored[768];
    char name[64];
    char dzs[16];
    const char* guid;

    if (!m || !is_snes_profile(m) || player < 1 || player > 2) return;
    guid = m->s.player_gamepad_guid[player - 1];
    if (!guid[0]) return;

    if (snes_profile_get(guid, "Controls", stored, sizeof(stored)) &&
        stored[0]) {
        launcher_ini_kv_write(config_path(), "GamepadMap",
                              snes_gamepad_config_key(player - 1), stored);
    }
    if (snes_profile_get(guid, "Deadzone", dzs, sizeof(dzs)) && dzs[0]) {
        int dz = atoi(dzs);
        if (dz < 0) dz = 0;
        if (dz > 100) dz = 100;
        m->s.deadzone[player - 1] = dz;
    } else {
        /* No saved profile for this pad: the console default, not whatever the
         * previously selected pad happened to be on. A deadzone belongs to the
         * device, so selecting a device the player has never configured must
         * land on the default rather than inherit a stranger's number. */
        m->s.deadzone[player - 1] = RUI_SNES_PAD_DEFAULT_DEADZONE_PCT;
    }
    if (snes_profile_get(guid, "Name", name, sizeof(name)) && name[0])
        copy_str(m->player_pad_name[player - 1],
                 sizeof(m->player_pad_name[player - 1]), name);
    reload_player_display(m, player);
}

static void snes_gamepad_reset_player(int player) {
    char controls[LNG_SNES_PAD_BUTTON_COUNT][48];
    for (int i = 0; i < LNG_SNES_PAD_BUTTON_COUNT; ++i)
        copy_str(controls[i], sizeof(controls[i]), kSnesGamepadDefaultControls[i]);
    snes_gamepad_write_config_order(player, controls);
}

// Format SDL keycode + mods the way config.c's ParseKeyArray reads back.
static void format_hotkey(int keycode, int kmod, char* out, size_t cap) {
    out[0] = 0;
    if (keycode == 0) return;              // unbound
    char buf[96]; buf[0] = 0;
    if (kmod & SDL_KMOD_CTRL)  strncat(buf, "Ctrl+",  sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_ALT)   strncat(buf, "Alt+",   sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_SHIFT) strncat(buf, "Shift+", sizeof(buf)-strlen(buf)-1);
    const char* kn = SDL_GetKeyName((SDL_Keycode)keycode);
    if (!kn || !kn[0]) return;
    strncat(buf, kn, sizeof(buf)-strlen(buf)-1);
    copy_str(out, cap, buf);
}

// ---- public API -------------------------------------------------------------

void launcher_binds_load(LauncherModel* m, const char* config_path_in, const char* keybinds_path_in) {
    g_launcher_config_path = config_path_in;
    g_launcher_keybinds_path = keybinds_path_in;
    if (is_psx_profile(m)) {
        rui_psx_binds_init(keybinds_file_path());   // load/generate psx_keybinds.c-format keybinds.ini
        rui_psx_pad_binds_init(psx_input_ini_path()); // gamepad maps (input.ini)
        launcher_binds_hydrate_psx_pad_names(m);
    } else if (is_n64_profile(m)) {
        rui_n64_binds_init(n64_binds_file_path());      // load input.cfg (defaults if absent; never seeds the file)
        rui_n64_pad_binds_init(n64_input_ini_path());   // gamepad maps (input.ini, per GUID)
    } else if (is_nes_profile(m)) {
        rui_nes_binds_init(keybinds_file_path());   // load/generate nesrecomp-format keybinds.ini
        // Zapper switches live in the same file ([zapper]); surface them on
        // the model for the controller page's Zapper block.
        int zm = 1, zc = 1;
        rui_nes_zapper_get(keybinds_file_path(), &zm, &zc);
        m->zapper_mouse     = zm != 0;
        m->zapper_crosshair = zc != 0;
    } else if (is_genesis_profile(m)) {
        rui_genesis_binds_init(genesis_binds_file_path());   // overlay settings.ini [input.pN] onto engine defaults
    } else if (is_gb_profile(m)) {
        rui_gb_binds_init(gb_binds_file_path());   // load/generate keybinds.ini [controls] (gb-recompiled format)
    } else {
        recompui_keybinds_init(NULL);              // load/generate keybinds.ini (exe-anchored)
    }
    launcher_binds_refresh(m);
    launcher_binds_refresh_camera(m);
    reload_hotkey_display(m);
}

void launcher_binds_refresh(LauncherModel* m) {
    for (int p = 1; p <= LNG_MAX_PLAYERS; ++p)
        reload_player_display(m, p);
}

// Persist the Zapper switches to keybinds.ini [zapper] (surgical: the rest of
// the file — player binds, gamepad sections, comments — is preserved).
// Called by launcher_model_toggle_zapper_* on every flip.
void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    rui_nes_zapper_set(keybinds_file_path(), mouse_enabled, crosshair);
}

void launcher_binds_set_camera(LauncherModel* m, int action, int scancode) {
    if (!m || !is_nes_profile(m) ||
        action < 0 || action >= LNG_CAMERA_BIND_COUNT)
        return;
    rui_nes_camera_bind_set(keybinds_file_path(), action, scancode);
    copy_str(m->camera_binds[action], sizeof(m->camera_binds[action]),
             scancode_label((SDL_Scancode)scancode));
}

void launcher_binds_reset_camera(LauncherModel* m) {
    if (!m || !is_nes_profile(m)) return;
    rui_nes_camera_bind_reset(keybinds_file_path());
    launcher_binds_refresh_camera(m);
}

/* Slot-aware setter for stores that keep an alternate bind per input.
 * PSX keeps two (primary + alt, either may be a mouse pseudo-scancode); the
 * Game Boy keeps two keyboard slots (Arrows/Z/X plus WASD/J/K). */
void launcher_binds_set_button_slot(LauncherModel* m, int player, int b,
                                    int slot, int scancode) {
    if (!m) return;
    if (is_gb_profile(m)) {
        if (player != 1 || b < 0 || b >= LNG_GB_PAD_BUTTON_COUNT) return;
        rui_gb_binds_set_slot(gb_binds_file_path(), b, slot, scancode);
        /* The store takes this key away from any other button/slot, so every
         * label has to be re-read, not just the one that was clicked. */
        reload_player_display(m, player);
        return;
    }
    if (!is_psx_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    if (b < 0 || b >= LNG_PSX_PAD_BUTTON_COUNT) return;
    rui_psx_binds_set_slot(keybinds_file_path(), player - 1, b, slot, scancode);
    /* The store may have cleared this input's other slot (same key twice);
     * player; re-read every label so the page shows what will be saved. */
    for (int i = 0; i < LNG_PSX_PAD_BUTTON_COUNT; ++i) {
        copy_str(m->binds[player - 1][i], sizeof(m->binds[player - 1][i]),
                 scancode_label((SDL_Scancode)rui_psx_binds_get_slot(
                     keybinds_file_path(), player - 1, i, 0)));
        copy_str(m->binds_alt[player - 1][i], sizeof(m->binds_alt[player - 1][i]),
                 scancode_label((SDL_Scancode)rui_psx_binds_get_slot(
                     keybinds_file_path(), player - 1, i, 1)));
    }
}

void launcher_binds_set_button(LauncherModel* m, int player, int b, int scancode) {
    if (is_n64_profile(m)) {
        // Keyboard capture into the N64 store routes through the field API
        // (slot 0) so there is exactly one write path for that store.
        launcher_binds_set_field(m, player, b, 0, RUI_N64_FIELD_KEY, scancode);
        return;
    }
    if (is_psx_profile(m)) {
        launcher_binds_set_button_slot(m, player, b, 0, scancode);
        return;
    }
    if (is_nes_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        if (b < 0 || b >= LNG_NES_PAD_BUTTON_COUNT) return;
        rui_nes_binds_set(keybinds_file_path(), player - 1, b, scancode);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                 scancode_label((SDL_Scancode)scancode));
        return;
    }
    if (player < 1 || player > 2) return;
    if (is_genesis_profile(m)) {
        if (b < 0 || b >= LNG_GENESIS_PAD_BUTTON_COUNT) return;
        rui_genesis_binds_set_key(genesis_binds_file_path(), player - 1, b, scancode);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                 scancode_label((SDL_Scancode)scancode));
        return;
    }
    if (is_gb_profile(m)) {
        launcher_binds_set_button_slot(m, player, b, 0, scancode);
        return;
    }
    int n = 0;
    const int* kb_index = active_kb_index(m, &n);
    if (b < 0 || b >= n) return;
    recompui_keybinds_set_button(player, kb_index[b], (SDL_Scancode)scancode);
    recompui_keybinds_save();
    copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
             scancode_label((SDL_Scancode)scancode));
}

void launcher_binds_set_field(LauncherModel* m, int player, int b, int slot,
                              int type, int id) {
    if (!is_n64_profile(m)) return;   // field binds exist only in the N64 store
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    if (b < 0 || b >= LNG_N64_PAD_BUTTON_COUNT) return;
    // A KEY belongs in the keyboard table whatever the player's source is;
    // controller fields no longer reach this store at all (they go to the
    // per-GUID input.ini), so the device is not a function of the source.
    const int dev = (type == RUI_N64_FIELD_KEY) ? 0
                                                : n64_device_for_player(m, player - 1);
    rui_n64_binds_set(n64_binds_file_path(), dev, b, slot, type, id);
    // The table is shared by every player on the same device kind — refresh
    // ALL players' display strings, not just the one that captured.
    launcher_binds_refresh(m);
}

void launcher_binds_set_pad_button(LauncherModel* m, int player, int b,
                                   int kind, int code, int axis_dir) {
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    if (is_psx_profile(m)) {
        if (b < 0 || b >= LNG_PSX_PAD_BUTTON_COUNT) return;
        const char* guid = psx_player_guid(m, player);
        if (!guid[0]) return;
        rui_psx_pad_binds_set(psx_input_ini_path(), guid, b, kind, code, axis_dir);
        // Keep the pad registered (name/deadzone unchanged unless first create).
        rui_psx_pad_binds_remember(psx_input_ini_path(), guid,
                                   m->player_pad_name[player - 1], -1);
        rui_psx_pad_binds_label(psx_input_ini_path(), guid, b,
                                m->pad_binds[player - 1][b],
                                (int)sizeof(m->pad_binds[player - 1][b]));
        return;
    }
    if (is_n64_profile(m)) {
        if (b < 0 || b >= LNG_N64_PAD_BUTTON_COUNT) return;
        const char* guid = n64_player_guid(m, player);
        if (!guid[0]) return;          // nothing to key the mapping on
        rui_n64_pad_binds_set(n64_input_ini_path(), guid, b, kind, code, axis_dir);
        // Keep the pad registered (name/deadzone unchanged unless first create).
        rui_n64_pad_binds_remember(n64_input_ini_path(), guid,
                                   m->player_pad_name[player - 1], -1);
        // ... and mirror it into input.cfg's shared `pad.*` table.
        //
        // input.ini is the identity-bearing store and what n64lle's host reads.
        // The RT64-era N64 runners read input.cfg and nothing else, so dropping
        // the mirror would leave their players with a Configure page that
        // writes a file the game never opens. The mirror is lossy by
        // construction — one table for every controller, so the last pad edited
        // wins — which is precisely the defect input.ini exists to fix, and is
        // no worse than what those runners had before this store existed.
        n64_mirror_pad_bind_to_cfg(b, kind, code, axis_dir);
        rui_n64_pad_binds_source(n64_input_ini_path(), guid, b,
                                 m->pad_binds[player - 1][b],
                                 (int)sizeof(m->pad_binds[player - 1][b]));
        return;
    }
    if (is_snes_profile(m)) {
        if (player > 2) return;
        (void)snes_gamepad_set_button(m, player - 1, b, kind, code, axis_dir);
        return;
    }
    // Genesis: has_pad_binds console (key + pad pair per logical button).
    if (!is_genesis_profile(m)) return;
    if (player > 2) return;
    if (b < 0 || b >= LNG_GENESIS_PAD_BUTTON_COUNT) return;
    rui_genesis_binds_set_pad(genesis_binds_file_path(), player - 1, b, kind, code, axis_dir);
    genesis_pad_label(kind, code, axis_dir,
                      m->pad_binds[player - 1][b], sizeof(m->pad_binds[player - 1][b]));
}

/* ---- N64 gamepad profiles ------------------------------------------------
 * Mirrors the PSX trio on the N64 per-GUID store. The mapping itself is
 * written on every capture (launcher_binds_set_pad_button), so Save is the
 * explicit commit of the identity beside it: name, whether the player chose
 * that name, and the deadzone the runner will read. */
static void n64_fallback_pad_label(const char* guid, char* out, size_t cap) {
    if (!out || !cap) return;
    if (guid && guid[0]) {
        size_t n = strlen(guid);
        if (n >= 8)
            snprintf(out, cap, "Controller \xe2\x80\xa6%s", guid + n - 8);
        else
            snprintf(out, cap, "Controller %s", guid);
        return;
    }
    copy_str(out, cap, "Controller");
}

void launcher_binds_save_n64_gamepad(LauncherModel* m, int player) {
    if (!m || !is_n64_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const int p = player - 1;
    if (m->s.player_src[p] != 2) return;
    const char* guid = m->s.player_gamepad_guid[p];
    if (!guid[0]) return;
    const char* path = n64_input_ini_path();
    char name[64];
    if (m->player_pad_name[p][0] && strcmp(m->player_pad_name[p], "Gamepad") != 0)
        copy_str(name, sizeof(name), m->player_pad_name[p]);
    else
        n64_fallback_pad_label(guid, name, sizeof(name));
    const int custom = rui_n64_pad_binds_name_is_custom(path, guid);
    int dz = m->s.deadzone[p];
    if (dz < 0) dz = 0;
    if (dz > 100) dz = 100;
    rui_n64_pad_binds_save_profile(path, guid, name, custom, dz);
    copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), name);
    launcher_binds_refresh(m);
}

void launcher_binds_rename_n64_gamepad(LauncherModel* m, int player,
                                       const char* name) {
    if (!m || !is_n64_profile(m) || !name || !name[0]) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const int p = player - 1;
    const char* guid = m->s.player_gamepad_guid[p];
    if (!guid[0]) return;
    rui_n64_pad_binds_rename(n64_input_ini_path(), guid, name);
    copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), name);
}

void launcher_binds_delete_n64_gamepad(LauncherModel* m, int player) {
    if (!m || !is_n64_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const char* guid = n64_player_guid(m, player);
    if (!guid[0]) return;
    rui_n64_pad_binds_delete(n64_input_ini_path(), guid);
    /* Every player that pointed at this GUID now points at a profile that
     * does not exist; put them back on the keyboard rather than leave a
     * source naming a mapping the file no longer holds. */
    for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
        if (m->s.player_src[p] == 2 &&
            m->s.player_gamepad_guid[p][0] &&
            !strcmp(m->s.player_gamepad_guid[p], guid)) {
            launcher_model_set_source(m, p, 1, 0, NULL, NULL);
        }
    }
    launcher_binds_refresh(m);
}

static int psx_guid_claimed_by_other(const LauncherModel* m, int self,
                                     const char* guid) {
    if (!m || !guid || !guid[0]) return 0;
    for (int o = 0; o < LNG_MAX_PLAYERS; ++o) {
        if (o == self) continue;
        if (m->s.player_src[o] == 2 && m->s.player_gamepad_guid[o][0] &&
            !strcmp(m->s.player_gamepad_guid[o], guid))
            return 1;
    }
    return 0;
}

static void psx_fallback_pad_label(const char* guid, char* out, size_t cap) {
    if (!out || !cap) return;
    if (guid && guid[0]) {
        size_t n = strlen(guid);
        if (n >= 8)
            snprintf(out, cap, "Controller …%s", guid + n - 8);
        else
            snprintf(out, cap, "Controller %s", guid);
        return;
    }
    copy_str(out, cap, "Controller");
}

void launcher_binds_hydrate_psx_pad_names(LauncherModel* m) {
    if (!m || !is_psx_profile(m)) return;
    const char* path = psx_input_ini_path();
    for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
        if (m->s.player_src[p] != 2) continue;
        const char* guid = m->s.player_gamepad_guid[p];
        if (!guid[0]) continue;
        if (m->player_pad_name[p][0] &&
            strcmp(m->player_pad_name[p], "Gamepad") != 0)
            continue;
        rui_psx_pad_binds_name(path, guid, m->player_pad_name[p],
                               (int)sizeof(m->player_pad_name[p]));
        if (!m->player_pad_name[p][0] ||
            !strcmp(m->player_pad_name[p], "Gamepad"))
            psx_fallback_pad_label(guid, m->player_pad_name[p],
                                   sizeof(m->player_pad_name[p]));
    }
}

void launcher_binds_sync_psx_pad_sources(LauncherModel* m,
                                         const LauncherPad* pads, int pad_count) {
    if (!m || !is_psx_profile(m)) return;
    const char* path = psx_input_ini_path();
    const int nplayers = launcher_model_visible_player_count(m);

    for (int p = 0; p < nplayers; ++p) {
        if (m->s.player_src[p] != 2) continue;

        // Legacy device="gamepad" with no GUID: pin to a concrete pad.
        if (!m->s.player_gamepad_guid[p][0]) {
            int chosen = -1;
            if (pads && pad_count > 0) {
                for (int i = 0; i < pad_count; ++i) {
                    if (!pads[i].guid[0]) continue;
                    if (psx_guid_claimed_by_other(m, p, pads[i].guid)) continue;
                    chosen = i;
                    break;
                }
                if (chosen < 0) chosen = 0;
                launcher_model_set_source(m, p, 2, pads[chosen].id,
                                          pads[chosen].name, pads[chosen].guid);
            } else {
                const int known = rui_psx_pad_binds_known_count(path);
                for (int i = 0; i < known; ++i) {
                    char guid[40] = {}, name[64] = {};
                    if (!rui_psx_pad_binds_known_at(path, i, guid, (int)sizeof(guid),
                                                    name, (int)sizeof(name)))
                        continue;
                    if (psx_guid_claimed_by_other(m, p, guid)) continue;
                    if (!name[0] || !strcmp(name, "Gamepad"))
                        psx_fallback_pad_label(guid, name, sizeof(name));
                    launcher_model_set_source(m, p, 2, 0, name, guid);
                    break;
                }
            }
        }

        const char* guid = m->s.player_gamepad_guid[p];
        if (!guid[0]) continue;

        // Prefer the live SDL name when this GUID is connected.
        int live = -1;
        if (pads) {
            for (int i = 0; i < pad_count; ++i) {
                if (pads[i].guid[0] && !strcmp(pads[i].guid, guid)) {
                    live = i;
                    break;
                }
            }
        }
        const int custom = rui_psx_pad_binds_name_is_custom(path, guid);
        if (live >= 0) {
            m->player_pad_id[p] = pads[live].id;
            if (custom) {
                // Keep the user's rename in the model / dropdown.
                char reg[64] = {};
                rui_psx_pad_binds_name(path, guid, reg, (int)sizeof(reg));
                if (reg[0])
                    copy_str(m->player_pad_name[p],
                             sizeof(m->player_pad_name[p]), reg);
            } else if (pads[live].name[0] &&
                       strcmp(pads[live].name, "Gamepad") != 0) {
                const int name_changed =
                    strcmp(m->player_pad_name[p], pads[live].name) != 0;
                copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]),
                         pads[live].name);
                if (name_changed)
                    rui_psx_pad_binds_remember(path, guid, pads[live].name, -1);
            }
            continue;
        }

        // Disconnected: registry name, else a non-generic fallback.
        char reg[64] = {};
        rui_psx_pad_binds_name(path, guid, reg, (int)sizeof(reg));
        if (reg[0] && strcmp(reg, "Gamepad") != 0)
            copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), reg);
        else if (!m->player_pad_name[p][0] ||
                 !strcmp(m->player_pad_name[p], "Gamepad"))
            psx_fallback_pad_label(guid, m->player_pad_name[p],
                                   sizeof(m->player_pad_name[p]));
        m->player_pad_id[p] = 0;
    }
}

void launcher_binds_apply_psx_pad_profile(LauncherModel* m, int player) {
    if (!m || !is_psx_profile(m)) return;
    player = (player < 0) ? 0 : (player >= LNG_MAX_PLAYERS ? LNG_MAX_PLAYERS - 1
                                                           : player);
    if (m->s.player_src[player] != 2) return;
    const char* guid = m->s.player_gamepad_guid[player];
    if (!guid[0]) return;
    const char* path = psx_input_ini_path();
    char name[64] = {};
    rui_psx_pad_binds_name(path, guid, name, (int)sizeof(name));
    if (name[0] && strcmp(name, "Gamepad") != 0)
        copy_str(m->player_pad_name[player], sizeof(m->player_pad_name[player]),
                 name);
    m->s.deadzone[player] = rui_psx_pad_binds_deadzone(path, guid);
    reload_player_display(m, player + 1);
}

void launcher_binds_rename_psx_gamepad(LauncherModel* m, int player,
                                       const char* name) {
    if (!m || !is_psx_profile(m) || !name || !name[0]) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const int p = player - 1;
    const char* guid = m->s.player_gamepad_guid[p];
    if (!guid[0]) return;
    rui_psx_pad_binds_rename(psx_input_ini_path(), guid, name);
    copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), name);
}

void launcher_binds_save_psx_gamepad(LauncherModel* m, int player) {
    if (!m || !is_psx_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const int p = player - 1;
    if (m->s.player_src[p] != 2) return;
    const char* guid = m->s.player_gamepad_guid[p];
    if (!guid[0]) return;
    const char* path = psx_input_ini_path();
    char name[64];
    if (m->player_pad_name[p][0] && strcmp(m->player_pad_name[p], "Gamepad") != 0)
        copy_str(name, sizeof(name), m->player_pad_name[p]);
    else
        psx_fallback_pad_label(guid, name, sizeof(name));
    const int custom = rui_psx_pad_binds_name_is_custom(path, guid);
    int dz = m->s.deadzone[p];
    if (dz < 0) dz = 0;
    if (dz > 100) dz = 100;
    rui_psx_pad_binds_save_profile(path, guid, name, custom, dz);
    copy_str(m->player_pad_name[p], sizeof(m->player_pad_name[p]), name);
    reload_player_display(m, player);
}

/* Save Profile, keyboard source: the keyboard map has no per-device registry
 * the way a GUID profile does -- every capture already wrote keybinds.ini --
 * so this flushes the store and refreshes the display strings. It exists so
 * the KEYBOARD BINDINGS card offers the same explicit commit the GAMEPAD
 * BINDINGS card does, rather than a button that is present but does nothing. */
void launcher_binds_save_psx_keyboard(LauncherModel* m, int player) {
    if (!m || !is_psx_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    rui_psx_binds_save(keybinds_file_path());
    reload_player_display(m, player);
}

void launcher_binds_delete_psx_gamepad(LauncherModel* m, int player) {
    if (!m || !is_psx_profile(m)) return;
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    const char* guid = psx_player_guid(m, player);
    if (!guid[0]) return;
    rui_psx_pad_binds_delete(psx_input_ini_path(), guid);
    // Clear any other players that pointed at the same GUID.
    for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
        if (m->s.player_src[p] == 2 &&
            m->s.player_gamepad_guid[p][0] &&
            !strcmp(m->s.player_gamepad_guid[p], guid)) {
            launcher_model_set_source(m, p, 1, 0, NULL, NULL);
        }
    }
    launcher_binds_refresh(m);
}

void launcher_binds_prepare_psx_launch(LauncherModel* m,
                                       const LauncherPad* pads, int pad_count) {
    if (!m || !is_psx_profile(m)) return;
    const char* path = psx_input_ini_path();
    for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
        if (m->s.player_src[p] != 2) continue;
        // Bare "Gamepad" (legacy auto): pin to a live pad when available so
        // settings.toml stores a real GUID and input.ini gets a mapping.
        if (!m->s.player_gamepad_guid[p][0] && pads && pad_count > 0) {
            // Prefer a pad not already claimed by an earlier player.
            int chosen = -1;
            for (int i = 0; i < pad_count; ++i) {
                int claimed = 0;
                for (int o = 0; o < p; ++o) {
                    if (m->s.player_src[o] == 2 &&
                        m->s.player_gamepad_guid[o][0] &&
                        !strcmp(m->s.player_gamepad_guid[o], pads[i].guid)) {
                        claimed = 1;
                        break;
                    }
                }
                if (!claimed) { chosen = i; break; }
            }
            if (chosen < 0) chosen = 0;
            launcher_model_set_source(m, p, 2, pads[chosen].id,
                                      pads[chosen].name, pads[chosen].guid);
        }
        const char* guid = m->s.player_gamepad_guid[p];
        if (!guid[0]) continue;
        char name[64];
        const int custom = rui_psx_pad_binds_name_is_custom(path, guid);
        if (custom) {
            rui_psx_pad_binds_name(path, guid, name, (int)sizeof(name));
            if (!name[0])
                psx_fallback_pad_label(guid, name, sizeof(name));
        } else if (m->player_pad_name[p][0] &&
                   strcmp(m->player_pad_name[p], "Gamepad") != 0) {
            copy_str(name, sizeof(name), m->player_pad_name[p]);
        } else {
            psx_fallback_pad_label(guid, name, sizeof(name));
        }
        if (!custom && pads) {
            for (int i = 0; i < pad_count; ++i) {
                if (pads[i].guid[0] && !strcmp(pads[i].guid, guid) &&
                    pads[i].name[0]) {
                    copy_str(name, sizeof(name), pads[i].name);
                    copy_str(m->player_pad_name[p],
                             sizeof(m->player_pad_name[p]), name);
                    m->player_pad_id[p] = pads[i].id;
                    break;
                }
            }
        }
        int dz = m->s.deadzone[p];
        if (dz < 0) dz = 0;
        if (dz > 100) dz = 100;
        rui_psx_pad_binds_save_profile(path, guid, name, custom, dz);
    }
}

int launcher_binds_psx_known_count(void) {
    return rui_psx_pad_binds_known_count(psx_input_ini_path());
}

int launcher_binds_psx_known_at(int index, char* guid, int guid_cap,
                                char* name, int name_cap) {
    return rui_psx_pad_binds_known_at(psx_input_ini_path(), index,
                                      guid, guid_cap, name, name_cap);
}

int launcher_binds_psx_name_is_custom(const char* guid) {
    if (!guid || !guid[0]) return 0;
    return rui_psx_pad_binds_name_is_custom(psx_input_ini_path(), guid);
}

void launcher_binds_reset_player(LauncherModel* m, int player) {
    if (is_n64_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        // Reset the half the page is showing, and only that half: a player on
        // a controller pressing Reset must not silently rewrite the keyboard
        // table every other player shares.
        const char* guid = n64_player_guid(m, player);
        if (n64_device_for_player(m, player - 1) == 1 && guid[0]) {
            rui_n64_pad_binds_reset(n64_input_ini_path(), guid);
            rui_n64_binds_reset_device(n64_binds_file_path(), 1);   // keep the mirror in step
        } else {
            rui_n64_binds_reset_device(n64_binds_file_path(), 0);
        }
        launcher_binds_refresh(m);
        return;
    }
    if (is_psx_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        // Configure's Gamepad Bindings panel resets the selected gamepad's
        // input.ini map. Keyboard keybinds.ini is left alone (not shown there).
        const char* guid = psx_player_guid(m, player);
        if (guid[0])
            rui_psx_pad_binds_reset(psx_input_ini_path(), guid);
        else
            rui_psx_binds_reset(keybinds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    if (is_nes_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        rui_nes_binds_reset(keybinds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    if (player < 1 || player > 2) return;
    if (is_genesis_profile(m)) {
        rui_genesis_binds_reset(genesis_binds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    if (is_gb_profile(m)) {
        if (player != 1) return;
        rui_gb_binds_reset(gb_binds_file_path());
        reload_player_display(m, player);
        return;
    }
    if (is_snes_profile(m)) {
        recompui_keybinds_reset_player(player);
        recompui_keybinds_save();
        snes_gamepad_reset_player(player - 1);
        reload_player_display(m, player);
        return;
    }
    recompui_keybinds_reset_player(player);
    recompui_keybinds_save();
    reload_player_display(m, player);
}

void launcher_binds_set_hotkey(LauncherModel* m, LngHotkey h, int keycode, int kmod) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    char val[64];
    format_hotkey(keycode, kmod, val, sizeof(val));
    // keycode 0 = explicit unbind. Write the literal "None" rather than an
    // empty value: the PSX runtime treats a present-but-empty/None line as
    // "the user cleared this" and skips its built-in default, whereas a
    // missing line keeps the default.
    keymap_write(kHotkeyKey[h], val[0] ? val : "None");
    copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), val[0] ? val : "(unbound)");
}
