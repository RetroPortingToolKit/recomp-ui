/* Exercise the real C entry point, model, and disk store with a scripted
 * backend instead of a window. In particular, the host deliberately does
 * nothing with the returned settings on Quit, as old SNES hosts do. */
#include "launcher_backend.h"
#include "launcher_platform.h"
#include "launcher_profile.h"
#include "launcher_settings.h"
#include "launcher_ini.h"
#include "../src/common/launcher_ini.inc"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

static int window_available = 1, edit = 0, toggle_tf = 0;
static LngAction action = LNG_ACTION_QUIT;
static RecompLauncherCSettings observed;
static int opened_width, opened_height, resized_width, resized_height;

bool launcher_platform_open(LauncherPlatform* p, const char* title, int w, int h) {
    (void)title;
    opened_width = p->logical_w = w;
    opened_height = p->logical_h = h;
    return window_available != 0;
}
void launcher_platform_close(LauncherPlatform* p) { (void)p; }
void launcher_platform_set_icon(LauncherPlatform* p, const char* icon) { (void)p; (void)icon; }
void launcher_platform_set_quit_sdl(bool value) { (void)value; }
void launcher_boot_timing_mark(const char* phase) { (void)phase; }
int launcher_file_picker_selftest(void) { return 0; }
void launcher_binds_load(LauncherModel* m, const char* config, const char* binds) {
    (void)m; (void)config; (void)binds;
}
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }
LngAction launcher_backend_run(LauncherPlatform* p, LauncherModel* m,
                               const LauncherTheme* theme) {
    (void)theme;
    if (resized_width && resized_height) {
        p->logical_w = resized_width;
        p->logical_h = resized_height;
    }
    observed = m->s;
    if (toggle_tf) launcher_model_toggle_texture_filter(m);
    if (edit) {
        launcher_model_set_fullscreen(m, edit);
        launcher_model_set_scale(m, 4);
        m->s.linear_filter = 1;
        m->s.volume = 37;
        m->s.vsync = RECOMP_LAUNCHER_VSYNC_ADAPTIVE;
        m->s.rewind_enabled = 1;
        m->s.rewind_depth = 100;
        m->s.rewind_interval = 8;
        m->s.skip_launcher = 1;
        m->s.aspect_index = edit;
        m->s.msu1_enabled = 1;
        snprintf(m->s.msu1_dir, sizeof(m->s.msu1_dir), "audio/owned PCM pack");
        m->s.player_src[0] = 0;
        snprintf(m->s.shader_path, sizeof(m->s.shader_path), "shaders/path with spaces.glsl");
        m->s.netplay_launch.enabled = 1; /* must not leak from Quit or disk */
    }
    return action;
}

static void require(int ok, const char* label) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", label); exit(1); }
}
static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    require(f != NULL, "open fixture");
    require(fputs(text, f) >= 0, "write fixture");
    require(fclose(f) == 0, "close fixture");
}
static void read_text(const char* path, char* text, size_t size) {
    FILE* f = fopen(path, "rb");
    require(f != NULL, "open result");
    size_t n = fread(text, 1, size - 1, f);
    require(!ferror(f) && feof(f), "read complete result");
    text[n] = 0;
    fclose(f);
}
static RecompLauncherCSettings defaults(void) {
    RecompLauncherCSettings s = {0};
    s.window_scale = 3;
    s.enable_audio = 1;
    s.audio_freq = 32040;
    s.volume = 100;
    s.player_src[0] = 1;
    return s;
}
static int run(RecompLauncherCGameInfo* game, RecompLauncherCSettings* s) {
    char rom[1024] = "";
    return recomp_launcher_run_window("Settings test", s, game, ".", "", rom, sizeof(rom));
}

int main(int argc, char** argv) {
    require(argc == 2 && chdir(argv[1]) == 0, "isolated test directory");
    RecompLauncherCGameInfo game = {0};
    launcher_profile_apply("snes", &game);
    game.widescreen_supported = 0; /* mods own it */
    game.config_path = "custom settings.ini";
    game.rom_cache_path = "unused-rom-cache.cfg";
    game.has_shader = game.has_vsync = game.has_rewind_depth = 1;
    game.has_snes_display_aspect = 1;
    static const char* const aspects[] = {"4:3", "8:7", "1:1"};
    game.aspect_labels = aspects; game.num_aspect_labels = 3;
    game.msu1_supported = 1;
    write_text("config.ini", "DO NOT TOUCH THE DEFAULT PATH\n");
    write_text(game.config_path,
        "# Preserve the player's comments\n[Graphics]\n"
        "Fullscreen = 0\nWindowScale = 3\nWidescreen = Adaptive\n"
        "NoSpriteLimits = 1\n[KeyMap]\nFullscreen = Alt+Return\n"
        "[GamepadMap]\nControls = a,b,x,y\n[Unknown]\nOpaque = keep me\n");

    RecompLauncherCSettings s = defaults();
    edit = 1;
    require(run(&game, &s) == RECOMP_LAUNCHER_RESULT_QUIT, "Quit result");
    require(!s.netplay_launch.enabled, "Quit clears transient launch");
    /* Fresh process: old host re-seeds hardcoded defaults, ignores old io. */
    s = defaults(); edit = 0;
    require(run(&game, &s) == RECOMP_LAUNCHER_RESULT_QUIT, "reopen");
    require(observed.fullscreen == 1 && observed.window_scale == 4 &&
            observed.linear_filter == 1 && observed.volume == 37 &&
            observed.skip_launcher == 1 && observed.player_src[0] == 0,
            "Quit persisted the full edited surface despite stale host defaults");
    require(observed.vsync == RECOMP_LAUNCHER_VSYNC_ADAPTIVE &&
            observed.rewind_enabled == 1 && observed.rewind_depth == 100 &&
            observed.rewind_interval == 8, "optional fields and enum conversion");
    require(strcmp(observed.shader_path, "shaders/path with spaces.glsl") == 0,
            "string round trip");
    require(observed.aspect_index == 1, "SNES square pixels persist after Quit");
    require(observed.msu1_enabled == 1 &&
            strcmp(observed.msu1_dir, "audio/owned PCM pack") == 0, "MSU-1 settings round trip");
    require(!observed.netplay_launch.enabled, "no transient session on restart");
    char result[8192], untouched[8192];
    read_text(game.config_path, result, sizeof(result));
    require(strstr(result, "# Preserve the player's comments") &&
            strstr(result, "Widescreen = Adaptive") &&
            strstr(result, "NoSpriteLimits = 1") &&
            strstr(result, "Fullscreen = Alt+Return") &&
            strstr(result, "Controls = a,b,x,y") &&
            strstr(result, "Opaque = keep me"), "unrelated config preserved");
    read_text("config.ini", untouched, sizeof(untouched));
    require(strcmp(untouched, "DO NOT TOUCH THE DEFAULT PATH\n") == 0, "custom path honored");

    edit = 2; action = LNG_ACTION_LAUNCH; s = defaults();
    require(run(&game, &s) == RECOMP_LAUNCHER_RESULT_LAUNCH && s.fullscreen == 2,
            "Play returns edits");
    edit = 0; action = LNG_ACTION_QUIT; s = defaults();
    run(&game, &s);
    require(observed.fullscreen == 2, "Play persists exclusive fullscreen");
    require(observed.aspect_index == 2, "SNES square frame persists after Play");
    read_text(game.config_path, untouched, sizeof(untouched));
    window_available = 0; edit = 1;
    require(run(&game, &s) == RECOMP_LAUNCHER_RESULT_UNAVAILABLE, "unavailable result");
    read_text(game.config_path, result, sizeof(result));
    require(strcmp(result, untouched) == 0, "unavailable never writes");
    window_available = 1;

    /* A PSX host owns TOML; an unknown profile must not be guessed as SNES. */
    const char* platforms[] = {"PLAYSTATION", "SOMETHING ELSE", NULL};
    for (size_t i = 0; i < 3; ++i) {
        game.platform = platforms[i]; s = defaults(); run(&game, &s);
        require(observed.fullscreen == 0, "other profile is not loaded");
        read_text(game.config_path, result, sizeof(result));
        require(strcmp(result, untouched) == 0, "other profile is not written");
    }

    launcher_profile_apply("snes", &game);
    edit = 0;
    write_text(game.config_path,
        "[gRaPhIcS]\nFullscreen = 1 # borderless\nWindowScale = invalid\n"
        "LinearFiltering = true\nVSync = 0\nShader = \n"
        "[Sound]\nVolume = 99999999999999999999999\nAudioFreq = 48000\n");
    s = defaults(); strcpy(s.shader_path, "old.glsl");
    run(&game, &s);
    require(observed.fullscreen == 1 && observed.window_scale == 3 &&
            observed.linear_filter == 1 && observed.volume == 100 &&
            observed.vsync == RECOMP_LAUNCHER_VSYNC_OFF &&
            observed.audio_freq == 48000 && !observed.shader_path[0],
            "legacy spelling, whitespace, comments, validation and explicit empty path");
    read_text(game.config_path, result, sizeof(result));
    run(&game, &s);
    read_text(game.config_path, untouched, sizeof(untouched));
    require(strcmp(result, untouched) == 0, "no edits means no rewrite/default clobber");

    game.has_shader = game.has_vsync = game.has_rewind_depth = game.msu1_supported = 0;
    write_text(game.config_path, "[Graphics]\nShader = keep.glsl\nVSync = 0\n"
               "[Rewind]\nDepth = 200\n[Sound]\nMsu1Dir = keep/audio\n");
    s = defaults(); edit = 1; action = LNG_ACTION_RELAUNCH;
    require(run(&game, &s) == RECOMP_LAUNCHER_RESULT_RELAUNCH, "Relaunch result");
    require(observed.vsync == 0 && observed.shader_path[0] == 0,
            "unsupported fields not loaded");
    read_text(game.config_path, result, sizeof(result));
    require(strstr(result, "Shader = keep.glsl") && strstr(result, "VSync = 0") &&
            strstr(result, "Depth = 200") && strstr(result, "Msu1Dir = keep/audio"),
            "unsupported fields not written");
    s = defaults(); launcher_settings_load(&s, &game);
    require(s.fullscreen == 1, "Relaunch persists edits");

    /* Game-defined aspect indices must not read/write the SNES pixel aspect. */
    game.has_snes_display_aspect = 0;
    write_text(game.config_path, "[Graphics]\nDisplayAspect = 8:7\n");
    s = defaults(); s.aspect_index = 2; edit = 1; run(&game, &s);
    require(observed.aspect_index == 2, "custom aspect index not replaced by SNES config");
    read_text(game.config_path, result, sizeof(result));
    require(strstr(result, "DisplayAspect = 8:7") != NULL, "custom aspect does not overwrite SNES config");
    game.has_snes_display_aspect = 1;
    const char* const names[] = {"4:3", "8:7", "1:1", "CRT", "SquarePixels", "SquareFrame", "0", "1", "2"};
    for (int i = 0; i < 9; ++i) {
        snprintf(result, sizeof(result), "[Graphics]\nDisplayAspect = %s\n", names[i]);
        write_text(game.config_path, result);
        s = defaults(); launcher_settings_load(&s, &game);
        require(s.aspect_index == i % 3, "SNES aspect aliases match runtime config");
    }

    /* Fresh install, default path, and no-op open/close. */
    require(remove("config.ini") == 0, "remove default-path sentinel");
    game.config_path = NULL; edit = 0; action = LNG_ACTION_QUIT;
    s = defaults(); run(&game, &s);
    FILE* absent = fopen("config.ini", "rb");
    require(absent == NULL, "opening a fresh launcher does not pin defaults");
    edit = 1; run(&game, &s);
    s = defaults(); edit = 0; run(&game, &s);
    require(observed.fullscreen == 1 && observed.linear_filter == 1,
            "fresh file/default path round trip");
    /* A real C-ABI exit/reopen must retain launcher dimensions independently
     * of the host's game-window options and profile-specific settings. */
    game.config_path = "window host.ini";
    game.platform = "PLAYSTATION";
    write_text("launcher-window.ini", "logical_width=1100\nlogical_height=880\n");
    write_text(game.config_path, "[video]\nwindow_width=960\n");
    edit = 0; resized_width = 1280; resized_height = 960;
    s = defaults(); run(&game, &s);
    resized_width = resized_height = 0;
    run(&game, &s);
    require(opened_width == 1280 && opened_height == 960, "launcher resize survives Quit and reopen");
    read_text(game.config_path, result, sizeof(result));
    require(!strcmp(result, "[video]\nwindow_width=960\n"), "launcher size never edits host game-window settings");
    write_text("launcher-window.ini", "logical_width=99999999999999999999999\nlogical_height=-1\n");
    run(&game, &s);
    require(opened_width == 1100 && opened_height == 880, "invalid geometry keeps fitted startup defaults");
    window_available = 0;
    write_text("launcher-window.ini", "logical_width=1200\nlogical_height=900\n");
    run(&game, &s);
    read_text("launcher-window.ini", result, sizeof(result));
    require(!strcmp(result, "logical_width=1200\nlogical_height=900\n"), "unavailable window never overwrites geometry");
    window_available = 1;

    /* Texture filtering across the frontend boundary: a host value from its
     * catalog (PSX xBR = 3) survives the launcher's startup / soft-return
     * path unchanged, the row cycles the host's values, and a host without a
     * catalog keeps the legacy Nearest/Bilinear coercion. */
    {
        RecompLauncherCGameInfo psx = {0};
        launcher_profile_apply("psx", &psx);
        psx.config_path = "psx settings.ini";
        psx.has_texture_filter = 1;
        static const char* const tf_labels[] = {"Nearest", "Bilinear", "xBR"};
        static const int tf_values[] = {0, 1, 3};
        psx.texture_filter_labels = tf_labels;
        psx.texture_filter_values = tf_values;
        psx.num_texture_filters = 3;
        RecompLauncherCSettings t = defaults();
        t.texture_filter = 3;
        action = LNG_ACTION_LAUNCH; edit = 0; toggle_tf = 0;
        run(&psx, &t);
        require(observed.texture_filter == 3 && t.texture_filter == 3,
                "xBR (3) survives the launcher: 3 -> return -> still 3");
        toggle_tf = 1;
        run(&psx, &t);
        require(t.texture_filter == 0, "the row cycles the host's values (3 -> 0)");
        run(&psx, &t);
        run(&psx, &t);
        require(t.texture_filter == 3, "and back through 1 to 3");
        toggle_tf = 0;
        t.texture_filter = 2;   /* not offered by this host */
        run(&psx, &t);
        require(observed.texture_filter == 0, "a value the host does not offer becomes its first");
        psx.texture_filter_labels = NULL; psx.texture_filter_values = NULL; psx.num_texture_filters = 0;
        t.texture_filter = 3;
        run(&psx, &t);
        require(observed.texture_filter == 1, "a host without a catalog keeps Nearest/Bilinear");
        action = LNG_ACTION_QUIT;
    }
    puts("launcher settings persistence: PASS");
    return 0;
}
