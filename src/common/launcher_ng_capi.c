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

#include <stdio.h>
#include <stdlib.h>
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

/* ---- a match the hub negotiated (RECOMP_NETPLAY_LAUNCH) ------------------
 *
 * retcomm-launcher docs/NETPLAY_HANDOFF.md. The hub holds the lobby seat and
 * the room; this process only plays the match. Everything the in-game lobby
 * would have settled is settled by the engine's own code from the server
 * messages the record carries (ingest_launch, then fill_launch), so a match
 * started here and one started from the in-game lobby cannot disagree. */

static void handoff_status(const char* record, int ok, const char* why) {
    char path[1100];
    FILE* f;
    const char* c;
    if (!record || !record[0]) return;
    snprintf(path, sizeof path, "%s.status", record);
    f = fopen(path, "wb");
    if (!f) return;
    if (ok) {
        fputs("{\"ok\":true}\n", f);
    } else {
        fputs("{\"ok\":false,\"why\":\"", f);
        for (c = why ? why : ""; *c; ++c) {
            if (*c == '"' || *c == '\\') fputc('\\', f), fputc(*c, f);
            else if ((unsigned char)*c < 0x20) fputc(' ', f);
            else fputc(*c, f);
        }
        fputs("\"}\n", f);
    }
    fclose(f);
}

/* 1 when RECOMP_NETPLAY_LAUNCH named a record (handled, *rc set); 0 when it
 * did not and the window should open as usual. */
static int netplay_handoff(RecompLauncherCSettings* io,
                           const RecompLauncherCGameInfo* game,
                           const char* initial_rom,
                           char* out_rom_path, size_t out_rom_path_len,
                           int* rc) {
    char record[1024];
    char why[512];
    char* text = NULL;
    const RecompLauncherCNetplayCallbacks* np;
    RecompLauncherCNetplayLaunch launch;
    const char* env = getenv("RECOMP_NETPLAY_LAUNCH");
    FILE* f;
    long len;

    if (!env || !env[0]) return 0;
    snprintf(record, sizeof record, "%s", env);
    /* ONE MATCH PER RECORD. A psx match that ends soft-returns into this
     * launcher; it must open as a launcher then, not start the same match
     * again from a stale record. */
#if defined(_WIN32)
    _putenv("RECOMP_NETPLAY_LAUNCH=");
#else
    unsetenv("RECOMP_NETPLAY_LAUNCH");
#endif
    *rc = RECOMP_LAUNCHER_RESULT_QUIT;
    why[0] = '\0';

    np = game ? game->netplay : NULL;
    if (!np || !np->ingest_launch || !np->fill_launch) {
        snprintf(why, sizeof why,
                 "this build of the game cannot start a match it did not "
                 "negotiate itself; update the game");
        goto fail;
    }
    f = fopen(record, "rb");
    if (!f) {
        snprintf(why, sizeof why, "cannot read the launch record %s", record);
        goto fail;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (4L << 20)) {
        fclose(f);
        snprintf(why, sizeof why, "the launch record %s is empty or too large", record);
        goto fail;
    }
    text = (char*)malloc((size_t)len + 1);
    if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        snprintf(why, sizeof why, "cannot read the launch record %s", record);
        goto fail;
    }
    fclose(f);
    text[len] = '\0';

    if (np->ingest_launch(np->ctx, text, why, sizeof why) != 0) {
        if (!why[0]) snprintf(why, sizeof why, "the game refused the launch record");
        goto fail;
    }
    memset(&launch, 0, sizeof launch);
    if (!np->fill_launch(np->ctx, &launch) || !launch.enabled) {
        const char* e = np->last_error ? np->last_error(np->ctx) : NULL;
        snprintf(why, sizeof why, "the game could not settle the match%s%s",
                 e && e[0] ? ": " : "", e && e[0] ? e : "");
        goto fail;
    }
    /* MODS ARE CLEARED, not merged (recomp-ai-rules MODS.md section 7), through
     * the same netplay entry point the in-game lobby uses. */
    if (game->mods && game->mods->commit_netplay &&
        !game->mods->commit_netplay(game->mods->ctx, initial_rom ? initial_rom : "")) {
        snprintf(why, sizeof why, "the game's mods could not be cleared for the match");
        goto fail;
    }
    if (np->clear_launch_pending) np->clear_launch_pending(np->ctx);
    io->netplay_launch = launch;
    if (out_rom_path && out_rom_path_len)
        snprintf(out_rom_path, out_rom_path_len, "%s", initial_rom ? initial_rom : "");
    free(text);
    fprintf(stderr, "netplay handoff: %s -> session %u, slot %d\n", record,
            (unsigned)launch.session_id, launch.local_slot);
    handoff_status(record, 1, NULL);
    *rc = RECOMP_LAUNCHER_RESULT_LAUNCH;
    return 1;

fail:
    free(text);
    fprintf(stderr, "netplay handoff: %s\n", why);
    handoff_status(record, 0, why);
    return 1;
}

int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len) {
    (void)assets_dir;   // launcher_ng resolves assets next to the exe (SDL base path)
    g_last_relaunch_exe[0] = '\0';

    /* Packaging self-test (RECOMP_UI_PICKER_SELFTEST): exercise the native
     * file picker and report the outcome, before any window/GL work so it
     * runs headless. Then take the no-launcher path, exactly as if the
     * launcher window had been unavailable. */
    if (launcher_file_picker_selftest())
        return RECOMP_LAUNCHER_RESULT_UNAVAILABLE;

    {
        int rc = RECOMP_LAUNCHER_RESULT_QUIT;
        if (netplay_handoff(io, game, initial_rom, out_rom_path, out_rom_path_len, &rc))
            return rc;
    }

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
    launcher_boot_timing_mark("rui:model+binds_ready");

    LauncherTheme theme = launcher_theme_by_name(game ? game->theme : NULL);

    LngAction act = launcher_backend_run(&plat, &model, &theme);

    launcher_window_size_save(window_size_path, plat.logical_w, plat.logical_h);
    launcher_platform_close(&plat);
    launcher_boot_timing_mark("rui:platform_closed");
    /* A verified regional handoff must not persist the foreign disc or any
     * partially edited controls into the current region's settings. */
    if (model.profile_handoff) return RECOMP_LAUNCHER_RESULT_PROFILE;

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
