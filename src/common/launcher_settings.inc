/* Settings edited by the SNES launcher belong to the launcher, including
 * when an older host returns immediately on Quit. Use the runtime's existing
 * INI keys so direct-ROM / SkipLauncher boots see the same saved preferences.
 * Do not serialize the ABI struct: it also contains transient netplay state
 * and settings owned by other console formats. Included by launcher_ng_capi.c
 * to keep existing consumer source manifests valid on a pin-only update. */
#include "launcher_settings.h"
#include "launcher_ini.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { NUMBER, BOOLEAN, TEXT, OUTPUT, VSYNC, SNES_ASPECT } ValueKind;
typedef struct {
    const char* section;
    const char* key;
    size_t offset, size, capability;
    ValueKind kind;
    int minimum, maximum;
} Setting;

#define ALWAYS ((size_t)-1)
#define FIELD(section, key, field, kind, lo, hi, cap) \
    {section, key, offsetof(RecompLauncherCSettings, field), \
     sizeof(((RecompLauncherCSettings*)0)->field), cap, kind, lo, hi}
#define INT(section, key, field, lo, hi) \
    FIELD(section, key, field, NUMBER, lo, hi, ALWAYS)
#define BOOL(section, key, field) \
    FIELD(section, key, field, BOOLEAN, 0, 1, ALWAYS)
#define OPTIONAL(section, key, field, kind, lo, hi, cap) \
    FIELD(section, key, field, kind, lo, hi, offsetof(RecompLauncherCGameInfo, cap))

static const Setting kSettings[] = {
    INT("Graphics", "WindowScale", window_scale, 1, 8),
    INT("Graphics", "Fullscreen", fullscreen, 0, 2),
    BOOL("Graphics", "IgnoreAspectRatio", ignore_aspect),
    BOOL("Graphics", "LinearFiltering", linear_filter),
    FIELD("Graphics", "OutputMethod", output_method, OUTPUT, 0, 2, ALWAYS),
    OPTIONAL("Graphics", "Shader", shader_path, TEXT, 0, 0, has_shader),
    OPTIONAL("Graphics", "DisplayAspect", aspect_index, SNES_ASPECT, 0, 2,
             has_snes_display_aspect),
    OPTIONAL("Graphics", "FrameBlend", frame_blend, BOOLEAN, 0, 1, has_frame_blend),
    OPTIONAL("Graphics", "VSync", vsync, VSYNC, 1, 3, has_vsync),
    OPTIONAL("Graphics", "Renderer", renderer_id, TEXT, 0, 0, has_renderer),
    BOOL("Sound", "EnableAudio", enable_audio),
    INT("Sound", "AudioFreq", audio_freq, 8000, 96000),
    INT("Sound", "Volume", volume, 0, 100),
    OPTIONAL("Sound", "Msu1Enabled", msu1_enabled, BOOLEAN, 0, 1, msu1_supported),
    OPTIONAL("Sound", "Msu1Dir", msu1_dir, TEXT, 0, 0, msu1_supported),
    BOOL("General", "SkipLauncher", skip_launcher),
    OPTIONAL("General", "RunAhead", run_ahead, NUMBER, 0,
             RECOMP_LAUNCHER_RUN_AHEAD_MAX, has_run_ahead),
    OPTIONAL("Rewind", "Enabled", rewind_enabled, BOOLEAN, 0, 1, has_rewind_depth),
    OPTIONAL("Rewind", "Depth", rewind_depth, NUMBER, 1, 600, has_rewind_depth),
    OPTIONAL("Rewind", "Interval", rewind_interval, NUMBER, 1, 60, has_rewind_depth),
    INT("Controller", "SourceP1", player_src[0], 0, 2),
    INT("Controller", "SourceP2", player_src[1], 0, 2),
    INT("GamepadMap", "Deadzone", deadzone[0], 0, 100),
    INT("GamepadMap", "DeadzoneP2", deadzone[1], 0, 100),
    FIELD("Controller", "GuidP1", player_gamepad_guid[0], TEXT, 0, 0, ALWAYS),
    FIELD("Controller", "GuidP2", player_gamepad_guid[1], TEXT, 0, 0, ALWAYS),
};

#undef OPTIONAL
#undef BOOL
#undef INT
#undef FIELD

static int equal(const char* a, const char* b) {
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        ++a; ++b;
    }
    return *a == *b;
}

int launcher_settings_supported(const RecompLauncherCGameInfo* game) {
    /* Require the actual profile identity. A capability heuristic must never
     * decide that an unknown console's settings.toml is a SNES INI file. */
    return game && game->platform && equal(game->platform, "SUPER NINTENDO");
}

static int offered(const Setting* setting, const RecompLauncherCGameInfo* game) {
    return setting->capability == ALWAYS ||
        *(const int*)((const char*)game + setting->capability) != 0;
}

static const char* config_path(const RecompLauncherCGameInfo* game) {
    return game->config_path && game->config_path[0] ? game->config_path : "config.ini";
}

static char* trim(char* text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
    return text;
}

static void read_value(const Setting* setting, char* value,
                       RecompLauncherCSettings* settings) {
    char* field = (char*)settings + setting->offset;
    if (setting->kind == TEXT) {
        /* Empty paths clear the choice; oversize/multiline paths are invalid. */
        if (strlen(value) < setting->size) memcpy(field, value, strlen(value) + 1);
        return;
    }
    int parsed;
    if (setting->kind == OUTPUT) {
        if (equal(value, "SDL")) parsed = 0;
        else if (equal(value, "SDL-Software")) parsed = 1;
        else if (equal(value, "OpenGL")) parsed = 2;
        else return;
    } else if (setting->kind == SNES_ASPECT) {
        if (equal(value, "4:3") || equal(value, "CRT") || equal(value, "0")) parsed = 0;
        else if (equal(value, "8:7") || equal(value, "SquarePixels") || equal(value, "1")) parsed = 1;
        else if (equal(value, "1:1") || equal(value, "SquareFrame") || equal(value, "2")) parsed = 2;
        else return;
    } else if (setting->kind == VSYNC) {
        if (equal(value, "adaptive")) parsed = RECOMP_LAUNCHER_VSYNC_ADAPTIVE;
        else if (equal(value, "0") || equal(value, "false")) parsed = RECOMP_LAUNCHER_VSYNC_OFF;
        else if (equal(value, "1") || equal(value, "true")) parsed = RECOMP_LAUNCHER_VSYNC_ON;
        else return;
    } else if (setting->kind == BOOLEAN && equal(value, "true")) parsed = 1;
    else if (setting->kind == BOOLEAN && equal(value, "false")) parsed = 0;
    else {
        char* end;
        errno = 0;
        long number = strtol(value, &end, 10);
        if (errno || end == value || *end || number < setting->minimum ||
            number > setting->maximum) return;
        parsed = (int)number;
    }
    memcpy(field, &parsed, sizeof(parsed));
}

void launcher_settings_load(RecompLauncherCSettings* settings,
                            const RecompLauncherCGameInfo* game) {
    if (!settings || !launcher_settings_supported(game)) return;
    FILE* file = fopen(config_path(game), "rb");
    if (!file) return;
    char line[2048], section[64] = "";
    while (fgets(line, sizeof(line), file)) {
        /* Consume rather than interpreting the tail of an overlong line. */
        if (!strchr(line, '\n') && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {}
            continue;
        }
        char* text = trim(line);
        if (!*text || *text == '#' || *text == ';') continue;
        if (*text == '[') {
            char* end = strchr(text, ']');
            section[0] = 0;
            if (end) {
                *end = 0;
                snprintf(section, sizeof(section), "%s", trim(text + 1));
            }
            continue;
        }
        char* separator = strchr(text, '=');
        if (!separator) continue;
        *separator = 0;
        char* key = trim(text);
        char* value = trim(separator + 1);
        for (size_t i = 0; i < sizeof(kSettings) / sizeof(kSettings[0]); ++i) {
            const Setting* setting = &kSettings[i];
            if (!equal(section, setting->section) || !equal(key, setting->key) ||
                !offered(setting, game)) continue;
            if (setting->kind != TEXT) {
                char* comment = strpbrk(value, "#;");
                if (comment) *comment = 0;
                value = trim(value);
            }
            read_value(setting, value, settings);
            break;
        }
    }
    fclose(file);
}

void launcher_settings_save(const RecompLauncherCSettings* settings,
                            const RecompLauncherCSettings* before,
                            const RecompLauncherCGameInfo* game) {
    if (!settings || !before || !launcher_settings_supported(game)) return;
    const char* path = config_path(game);
    for (size_t i = 0; i < sizeof(kSettings) / sizeof(kSettings[0]); ++i) {
        const Setting* setting = &kSettings[i];
        if (!offered(setting, game)) continue;
        const char* value = (const char*)settings + setting->offset;
        const char* old = (const char*)before + setting->offset;
        char number[32];
        if (setting->kind == TEXT) {
            if (!memchr(value, 0, setting->size) || !memchr(old, 0, setting->size) ||
                strcmp(value, old) == 0 || strpbrk(value, "\r\n")) continue;
        } else {
            int current, previous;
            memcpy(&current, value, sizeof(current));
            memcpy(&previous, old, sizeof(previous));
            if (current == previous || current < setting->minimum ||
                current > setting->maximum) continue;
            if (setting->kind == OUTPUT) {
                static const char* const names[] = {"SDL", "SDL-Software", "OpenGL"};
                value = names[current];
            } else if (setting->kind == SNES_ASPECT) {
                static const char* const names[] = {"4:3", "8:7", "1:1"};
                value = names[current];
            } else if (setting->kind == VSYNC) {
                value = current == RECOMP_LAUNCHER_VSYNC_OFF ? "0" :
                        current == RECOMP_LAUNCHER_VSYNC_ADAPTIVE ? "adaptive" : "1";
            } else {
                snprintf(number, sizeof(number), "%d", current);
                value = number;
            }
        }
        launcher_ini_kv_write(path, setting->section, setting->key, value);
    }
    /* Legacy hosts parse these booleans before the UI. The canonical source
     * keys above additionally preserve Keyboard vs None across UI restarts. */
    for (int p = 0; p < 2; ++p) {
        if (settings->player_src[p] != before->player_src[p])
            launcher_ini_kv_write(path, "GamepadMap", p ? "EnableGamepad2" : "EnableGamepad1",
                                  settings->player_src[p] == 2 ? "true" : "false");
    }
}
