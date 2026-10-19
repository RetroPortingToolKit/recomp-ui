// launcher_ng_capi.c — the C ABI the game calls, backed by the launcher_ng UI.
//
// Implements recomp_launcher_run_window() (declared in recomp_launcher.h),
// the generic C ABI a host app calls to run the pre-boot launcher UI. Hosts
// seed the C structs, call the function, and read back the chosen ROM +
// settings.
//
// Returns: 0 = LAUNCH, 1 = QUIT, 2 = UNAVAILABLE, 3 = RELAUNCH.

#include "recomp_launcher.h"

#include "launcher_backend.h"
#include "launcher_binds.h"
#include "launcher_boot_timing.h"
#include "launcher_files.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_settings.h"
#include "launcher_theme.h"
#include "launcher_window_size.h"
#include "consoles/psx/psx_start_from.h"   // "Start from": the lists and the hand-over
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
#include "launcher_module_runtime.h"
#endif

#include <stdio.h>
#include <string.h>

#include "launcher_settings.inc"

static char g_last_relaunch_exe[512];

void recomp_launcher_set_preserve_sdl(int preserve) {
    launcher_platform_set_quit_sdl(!preserve);
}

int recomp_launcher_relaunch_exe(char* out, size_t out_cap) {
    if (!out || out_cap == 0 || !g_last_relaunch_exe[0])
        return 0;
    snprintf(out, out_cap, "%s", g_last_relaunch_exe);
    return 1;
}

int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len) {
    (void)assets_dir;   // v1 backend uses the validated explicit asset/product roots
    g_last_relaunch_exe[0] = '\0';

    /* Packaging self-test (RECOMP_UI_PICKER_SELFTEST): exercise the native
     * file picker and report the outcome, before any window/GL work so it
     * runs headless. Then take the no-launcher path, exactly as if the
     * launcher window had been unavailable. */
    if (launcher_file_picker_selftest())
        return RECOMP_LAUNCHER_RESULT_UNAVAILABLE;

    launcher_boot_timing_mark("rui:run_window:enter");

    char window_size_path[1024] = {0};
    int window_width = 1100, window_height = 880;
    if (game && launcher_window_size_path(game->config_path, window_size_path,
                                          sizeof(window_size_path)))
        launcher_window_size_load(window_size_path, &window_width, &window_height);
    LauncherPlatform plat = {0};
    if (!launcher_platform_open(&plat, window_title ? window_title : "Launcher",
                                window_width, window_height)) {
        // Window/GL init failed — tell the caller to boot as if the launcher was
        // skipped, exactly like the old launcher's UNAVAILABLE path.
        return RECOMP_LAUNCHER_RESULT_UNAVAILABLE;
    }

    /* Same icon the host's game window carries — see GameInfo.window_icon_path.
     * Applied before the model is built so the window is never briefly shown
     * under the placeholder icon. */
    launcher_platform_set_icon(&plat, game ? game->window_icon_path : NULL);

    /* In session the host seeded *io from the RUNNING game, which can be
     * ahead of the file: fullscreen and volume hotkeys, a config.local.ini
     * overlay and values the host clamped all live only in memory until it
     * writes them. Re-reading the file would hand those back as edits and
     * undo them on RESUME. */
    if (!game || !game->in_session)
        launcher_settings_load(io, game);
    LauncherModel model;
    launcher_model_init(&model, io, game, initial_rom);
    model.settings_saved_on_exit = launcher_settings_supported(game) != 0;
    launcher_binds_load(&model, game ? game->config_path : NULL,
                                game ? game->keybinds_path : NULL);
    const RecompLauncherCSettings before = model.s;
    /* The dashboard's "Start from" block: the title's save states and
     * replays, read from the product's own folder. */
    if (launcher_model_start_from_available(&model)) {
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
        launcher_v1_model_starts(&model);
#else
#if defined(LNG_SDL3)
        psx_start_from_load(&model, SDL_GetBasePath(), NULL);
#else
        char* base = SDL_GetBasePath();
        psx_start_from_load(&model, base, NULL);
        SDL_free(base);
#endif
#endif
    }
    launcher_boot_timing_mark("rui:model+binds_ready");

    LauncherTheme theme = launcher_theme_by_name(game ? game->theme : NULL);

    LngAction act = launcher_backend_run(&plat, &model, &theme);

#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
    /* Prepare workers borrow this stack model. Join while it still exists,
     * before platform teardown, settings persistence and typed readback. */
    launcher_model_wait_worker();
    launcher_cover_job_wait();
#endif
    launcher_window_size_save(window_size_path, plat.logical_w, plat.logical_h);
    launcher_platform_close(&plat);
    launcher_boot_timing_mark("rui:platform_closed");

    /* Edited settings go back to the caller on EVERY exit, quit included.
     *
     * Commit used to run only on LAUNCH/RELAUNCH, so a player who opened the
     * launcher, changed Fullscreen or a controller source, and then closed the
     * window threw the whole edit away -- the host still held the values it
     * seeded, wrote those back to its config, and the next run reopened on the
     * old settings. The launcher's own direct-write stores (keybinds.ini,
     * [KeyMap], [GamepadMap]) already persist on quit; the settings
     * struct was the one surface that did not. A setting changed and then
     * dismissed is still a setting changed. Commit also persists verified
     * dashboard cartridge picks; those hosts never enter the setup wizard's
     * sidecar-writing path. */
    launcher_model_commit(&model, io);   // edited settings back to the caller
    launcher_settings_save(&model.s, &before, game);
    if (act != LNG_ACTION_LAUNCH && act != LNG_ACTION_RELAUNCH && io) {
        /* netplay_launch is a transient OUTPUT, not a setting: only a real
         * lobby launch may arm it. Quitting must never hand the host a
         * pending session. */
        memset(&io->netplay_launch, 0, sizeof(io->netplay_launch));
    }

    /* "Start from": PLAY hands the chosen save state or replay to the host
     * through the start-up variables it reads next. Any other way out, and a
     * netplay launch, leave no such variable of ours behind. */
    if (launcher_model_start_from_available(&model))
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
        launcher_v1_model_result(&model, act == LNG_ACTION_LAUNCH ?
            RECOMP_LAUNCHER_RESULT_LAUNCH : RECOMP_LAUNCHER_RESULT_QUIT);
#else
        psx_start_from_apply(&model, act != LNG_ACTION_LAUNCH ||
                                         (io && io->netplay_launch.enabled));
#endif

    if (act == LNG_ACTION_LAUNCH || act == LNG_ACTION_RELAUNCH) {
        const char* rom = launcher_model_effective_rom_path(&model);
        if (out_rom_path && out_rom_path_len) {
            if (rom && rom[0])
                snprintf(out_rom_path, out_rom_path_len, "%s", rom);
            else if (initial_rom)
                snprintf(out_rom_path, out_rom_path_len, "%s", initial_rom);
            else
                out_rom_path[0] = '\0';
        }
        if (act == LNG_ACTION_RELAUNCH) {
            if (model.relaunch_exe[0])
                snprintf(g_last_relaunch_exe, sizeof(g_last_relaunch_exe),
                         "%s", model.relaunch_exe);
            return RECOMP_LAUNCHER_RESULT_RELAUNCH;
        }
        return RECOMP_LAUNCHER_RESULT_LAUNCH;
    }

    return RECOMP_LAUNCHER_RESULT_QUIT;
}
