#include "launcher_module_runtime.h"
#include "launcher_module_contract.h"
#include "launcher_module_sdl.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RECOMP_LAUNCHER_V2_BUILD_IDENTITY
#error A SHA-256 source/build receipt identity is required for the module.
#endif

static atomic_flag g_running = ATOMIC_FLAG_INIT;
static const RecompLauncherV2Run* g_run;
static RecompLauncherV2Result* g_result;
static LauncherStartEntry g_entries[LNG_START_MAX];
static LauncherStartNotes g_notes;
static int g_entry_count;

_Noreturn void launcher_v2_worker_join_failed(const char* worker, uint32_t wait_status) {
    /* Returning an error would release the host's borrowed stack/context
     * while worker termination is unknown. Stop without normal destructors. */
    fprintf(stderr, "Launcher cannot confirm %s worker termination (wait status 0x%08x); stopping the title before releasing callback contexts.\n",
        worker, (unsigned)wait_status);
    fflush(stderr);
    _Exit(EXIT_FAILURE);
}

const RecompLauncherV2Run* launcher_v2_current_run(void) { return g_run; }

static uint32_t read_starts(const RecompLauncherV2Run* run, char* error, uint32_t cap) {
    g_entry_count = 0;
    memset(&g_notes, 0, sizeof(g_notes));
    if (!run->starts) return RECOMP_LAUNCHER_V2_OK;
    RecompLauncherV2StartNotes notes = {0};
    notes.header = launcher_v2_header(sizeof(notes));
    const int32_t count = run->starts->count(run->starts->ctx, &notes);
    if (count < 0 || launcher_v2_check_header(&notes.header, sizeof(notes))) goto bad;
    g_notes.build_known = !!notes.build_known;
    g_notes.states_elsewhere = (int)notes.states_elsewhere;
    g_notes.states_unlisted = (int)notes.states_unlisted;
    g_notes.replays_unlisted = (int)notes.replays_unlisted;
    for (int32_t i = 0; i < count && i < LNG_START_MAX; ++i) {
        RecompLauncherV2StartEntry entry = {0};
        entry.header = launcher_v2_header(sizeof(entry));
        if (run->starts->get(run->starts->ctx, (uint32_t)i, &entry) != 1 ||
            launcher_v2_check_header(&entry.header, sizeof(entry)) ||
            (entry.kind != RECOMP_LAUNCHER_V2_SAVE_SLOT && entry.kind != RECOMP_LAUNCHER_V2_REPLAY) ||
            (entry.kind == RECOMP_LAUNCHER_V2_SAVE_SLOT && (entry.slot < 0 || entry.slot > 11)) ||
            entry.build > LNG_START_BUILD_OTHER || entry.power_on > 1 || entry.partial > 1 ||
            !memchr(entry.label, 0, sizeof(entry.label)) || !memchr(entry.replay_path, 0, sizeof(entry.replay_path)) ||
            (entry.kind == RECOMP_LAUNCHER_V2_REPLAY && !entry.replay_path[0]) ||
            (entry.thumbnail_format && (entry.thumbnail_format != RECOMP_LAUNCHER_V2_THUMB_BGRA8 ||
                !run->starts->thumbnail || !entry.thumbnail_width || !entry.thumbnail_height ||
                entry.thumbnail_width > 2048 || entry.thumbnail_height > 2048))) goto bad;
        LauncherStartEntry* target = &g_entries[g_entry_count++];
        memset(target, 0, sizeof(*target));
        target->kind = (int)entry.kind;
        target->slot = entry.slot;
        target->disc = entry.disc;
        target->build = (int)entry.build;
        target->power_on = (int)entry.power_on;
        target->partial = (int)entry.partial;
        target->frames = entry.frames;
        target->when = entry.when;
        memcpy(target->label, entry.label, sizeof(target->label));
        memcpy(target->path, entry.replay_path, sizeof(target->path));
        /* Module-only thumbnail index, never a filename/retail format guess. */
        target->thumb_offset = (uint32_t)i;
        if (entry.thumbnail_format) {
            target->thumb_w = (int)entry.thumbnail_width;
            target->thumb_h = (int)entry.thumbnail_height;
        }
    }
    return RECOMP_LAUNCHER_V2_OK;
bad:
    launcher_v2_diagnostic(error, cap, "Host start-list provider returned an invalid v1 record; launcher refused before opening or saving settings.");
    return RECOMP_LAUNCHER_V2_START_ERROR;
}

void launcher_v2_model_starts(LauncherModel* model) {
    launcher_model_set_start_entries(model, g_entries, g_entry_count, &g_notes);
}

uint32_t launcher_v2_thumbnail(uint32_t index, uint8_t* pixels, uint32_t capacity) {
    if (!g_run || !g_run->starts || !g_run->starts->thumbnail) return 0;
    return g_run->starts->thumbnail(g_run->starts->ctx, index, pixels, capacity);
}

void launcher_v2_model_result(const LauncherModel* model, int action) {
    if (!g_result || action != RECOMP_LAUNCHER_RESULT_LAUNCH || model->s.netplay_launch.enabled) return;
    int slot = -1;
    const char* path = NULL;
    const int kind = launcher_model_start_handover(model, &slot, &path);
    if (kind == LNG_START_STATE) {
        g_result->start_kind = RECOMP_LAUNCHER_V2_SAVE_SLOT;
        g_result->save_slot = slot;
    } else if (kind == LNG_START_REPLAY && path && path[0]) {
        g_result->start_kind = RECOMP_LAUNCHER_V2_REPLAY;
        snprintf(g_result->replay_path, g_result->replay_path_capacity, "%s", path);
    }
}

static uint32_t RECOMP_LAUNCHER_V2_CALL run_module(const RecompLauncherV2Run* run,
                                                RecompLauncherV2Result* result,
                                                char* error, uint32_t cap) {
    RecompLauncherV2SDL local_sdl = {0};
    uint32_t status = launcher_v2_sdl_identity(&local_sdl, error, cap);
    if (status) return status;
    uint64_t supported = RECOMP_LAUNCHER_V2_CAPABILITIES;
#if !RECOMP_UI_ENABLE_MODS
    supported &= ~RECOMP_LAUNCHER_V2_MODS;
#endif
    status = launcher_v2_validate_run(run, result, &local_sdl, supported, error, cap);
    if (status) return status;
    /* All validation precedes callback dispatch, output/settings changes and
     * persistent file access. Only one blocking UI run owns module globals. */
    if (atomic_flag_test_and_set_explicit(&g_running, memory_order_acquire)) {
        launcher_v2_diagnostic(error, cap, "A launcher run is already active in this module.");
        return RECOMP_LAUNCHER_V2_BUSY;
    }
    status = read_starts(run, error, cap);
    if (!status) {
        RecompLauncherCGameInfo game = run->game->value;
        game.mods = run->mods ? &run->mods->value : NULL;
        game.netplay = run->netplay ? &run->netplay->value : NULL;
        result->start_kind = RECOMP_LAUNCHER_V2_POWER_ON;
        result->save_slot = -1;
        result->relaunch_path[0] = '\0';
        result->replay_path[0] = '\0';
        g_run = run;
        g_result = result;
        recomp_launcher_set_preserve_sdl(1);
        result->action = recomp_launcher_run_window(run->window_title, &run->settings->value,
            &game, run->assets_root, run->initial_disc, result->selected_disc, result->selected_disc_capacity);
        /* Model/backend workers retain the borrowed callbacks and context.
         * The module waits for actual thread termination, not a done flag. */
        launcher_model_finish_worker();
        launcher_cover_job_finish();
        if (result->action == RECOMP_LAUNCHER_RESULT_RELAUNCH)
            recomp_launcher_relaunch_exe(result->relaunch_path, result->relaunch_path_capacity);
        g_result = NULL;
        g_run = NULL;
    }
    g_entry_count = 0;
    atomic_flag_clear_explicit(&g_running, memory_order_release);
    return status;
}

RECOMP_LAUNCHER_V2_EXPORT uint32_t RECOMP_LAUNCHER_V2_CALL recomp_launcher_query_v2(
    const RecompLauncherV2Query* query, RecompLauncherV2Module* module,
    char* error, uint32_t cap) {
    uint32_t status = launcher_v2_check_header(module ? &module->header : NULL, sizeof(*module));
    if (status) {
        launcher_v2_diagnostic(error, cap, "Launcher module-information buffer has the wrong size/version.");
        return status;
    }
    uint64_t supported = RECOMP_LAUNCHER_V2_CAPABILITIES;
#if !RECOMP_UI_ENABLE_MODS
    supported &= ~RECOMP_LAUNCHER_V2_MODS;
#endif
    /* Give version/architecture/layout/capability refusals priority over SDL
     * inspection. This preliminary check never accepts a load by itself. */
    status = launcher_v2_validate_query(query, query ? &query->sdl : NULL, supported, error, cap);
    if (status) return status;
    RecompLauncherV2SDL local_sdl = {0};
    status = launcher_v2_sdl_identity(&local_sdl, error, cap);
    if (status) return status;
    status = launcher_v2_validate_query(query, &local_sdl, supported, error, cap);
    if (status) return status;
    RecompLauncherV2Module answer = {0};
    answer.header = launcher_v2_header(sizeof(answer));
    answer.supported = supported;
    answer.layout = launcher_v2_layout();
    answer.sdl = local_sdl;
    answer.run = run_module;
    snprintf(answer.build_identity, sizeof(answer.build_identity), "%s", RECOMP_LAUNCHER_V2_BUILD_IDENTITY);
    *module = answer;
    return RECOMP_LAUNCHER_V2_OK;
}

