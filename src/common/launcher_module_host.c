#include "recomp_launcher_module_host.h"
#include "launcher_module_contract.h"
#include "launcher_module_sdl.h"
#include "launcher_module_host_starts.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define MODULE_NAME "recomp-launcher-v1.dll"
#elif defined(__APPLE__)
#define MODULE_NAME "librecomp-launcher-v1.dylib"
#else
#define MODULE_NAME "librecomp-launcher-v1.so"
#endif

static SDL_SharedObject* g_module;
static RecompLauncherV1QueryFn g_query;
static char g_product[1024], g_saves[1024], g_assets[1024], g_error[512];
static char g_relaunch[512], g_replay[512];
static uint32_t g_start_kind;
static int32_t g_save_slot = -1;
static int g_active;

static int copy_root(char* out, size_t cap, const char* root) {
    if (!launcher_v1_absolute_root(root) || strlen(root) >= cap) return 0;
    snprintf(out, cap, "%s", root);
    return 1;
}

int recomp_launcher_module_set_roots(const char* product, const char* saves, const char* assets) {
    char product_copy[1024] = {0}, saves_copy[1024] = {0}, assets_copy[1024] = {0};
    if (g_active || !copy_root(product_copy, sizeof(product_copy), product) ||
        !copy_root(saves_copy, sizeof(saves_copy), saves) || !copy_root(assets_copy, sizeof(assets_copy), assets)) return 0;
    memcpy(g_product, product_copy, sizeof(g_product));
    memcpy(g_saves, saves_copy, sizeof(g_saves));
    memcpy(g_assets, assets_copy, sizeof(g_assets));
    return 1;
}

const char* recomp_launcher_module_diagnostic(void) { return g_error; }

uint32_t recomp_launcher_module_start_choice(int32_t* slot, char* replay, uint32_t capacity) {
    if (slot) *slot = g_save_slot;
    if (replay && capacity) snprintf(replay, capacity, "%s", g_replay);
    return g_start_kind;
}

void recomp_launcher_module_close(void) {
    if (g_active) return;
    if (g_module) SDL_UnloadObject(g_module);
    g_module = NULL; g_query = NULL;
}

void recomp_launcher_set_preserve_sdl(int preserve) { (void)preserve; /* host owns the one provider */ }
int recomp_launcher_relaunch_exe(char* out, size_t capacity) {
    if (!out || !capacity || !g_relaunch[0]) return 0;
    snprintf(out, capacity, "%s", g_relaunch);
    return 1;
}

int recomp_launcher_run_window(const char* title, RecompLauncherCSettings* settings,
                              const RecompLauncherCGameInfo* game, const char* assets,
                              const char* initial_disc, char* selected, size_t capacity) {
    (void)assets; /* roots are explicitly supplied by the owning host */
    if (g_active || !settings || !game || !g_product[0] || !g_saves[0] || !g_assets[0] ||
        !selected || capacity < 512 || capacity > UINT32_MAX) {
        launcher_v1_diagnostic(g_error, sizeof(g_error), "Launcher host needs explicit absolute roots and valid caller-owned settings/result buffers.");
        return RECOMP_LAUNCHER_RESULT_REFUSED;
    }
    if (!g_module) {
        char module_path[1200];
        snprintf(module_path, sizeof(module_path), "%s/%s", g_product, MODULE_NAME);
        g_module = SDL_LoadObject(module_path);
        if (!g_module) {
            snprintf(g_error, sizeof(g_error), "Cannot load matching launcher module: %.380s. Restore the matching v1 module.", SDL_GetError());
            return RECOMP_LAUNCHER_RESULT_REFUSED;
        }
        g_query = (RecompLauncherV1QueryFn)SDL_LoadFunction(g_module, "recomp_launcher_query_v1");
        if (!g_query) {
            launcher_v1_diagnostic(g_error, sizeof(g_error), "Launcher library has no recomp_launcher_query_v1 entry; restore the matching module.");
            recomp_launcher_module_close();
            return RECOMP_LAUNCHER_RESULT_REFUSED;
        }
    }
    RecompLauncherV1Query query = {0};
    query.header = launcher_v1_header(sizeof(query));
    query.pointer_bits = sizeof(void*) * 8;
    query.byte_order = launcher_v1_byte_order();
    query.architecture = launcher_v1_architecture();
    query.calling_convention = RECOMP_LAUNCHER_V1_CDECL;
    query.layout = launcher_v1_layout();
    query.offered = RECOMP_LAUNCHER_V1_CAPABILITIES;
    query.required = RECOMP_LAUNCHER_V1_SETTINGS | RECOMP_LAUNCHER_V1_DISC_BIOS_CARD |
                     RECOMP_LAUNCHER_V1_INPUT | RECOMP_LAUNCHER_V1_START_CHOICE;
    if (game->mods) query.required |= RECOMP_LAUNCHER_V1_MODS;
    if (game->netplay || game->netplay_supported) query.required |= RECOMP_LAUNCHER_V1_NETPLAY;
    if (game->setup_wizard_supported) query.required |= RECOMP_LAUNCHER_V1_SETUP_PROGRESS;
    if (game->in_session) query.required |= RECOMP_LAUNCHER_V1_IN_SESSION;
    if (launcher_v1_sdl_identity(&query.sdl, g_error, sizeof(g_error))) return RECOMP_LAUNCHER_RESULT_REFUSED;
    RecompLauncherV1Module module = {0};
    module.header = launcher_v1_header(sizeof(module));
    if (g_query(&query, &module, g_error, sizeof(g_error))) return RECOMP_LAUNCHER_RESULT_REFUSED;
    if (launcher_v1_check_header(&module.header, sizeof(module)) || !module.run ||
        (query.required & ~module.supported)) {
        launcher_v1_diagnostic(g_error, sizeof(g_error), "Launcher returned an invalid v1 function table or missing required capability; restore a matching module.");
        return RECOMP_LAUNCHER_RESULT_REFUSED;
    }
    RecompLauncherV1Query returned = query;
    returned.layout = module.layout;
    returned.sdl = module.sdl;
    if (launcher_v1_validate_query(&returned, &query.sdl, module.supported, g_error, sizeof(g_error)))
        return RECOMP_LAUNCHER_RESULT_REFUSED;
    /* Query/load refusal happened before scanner callbacks or settings reads.
     * Keep host-owned provider snapshots and contexts alive for this run. */
    g_active = 1;
    RecompLauncherV1Settings io = {0};
    io.header = launcher_v1_header(sizeof(io)); memcpy(&io.value, settings, sizeof(io.value));
    RecompLauncherV1Game description = {0};
    description.header = launcher_v1_header(sizeof(description)); description.value = *game;
    description.value.mods = NULL; description.value.netplay = NULL;
    RecompLauncherV1Mods mods = {0};
    mods.header = launcher_v1_header(sizeof(mods));
    if (game->mods) mods.value = *game->mods;
    RecompLauncherV1Netplay netplay = {0};
    netplay.header = launcher_v1_header(sizeof(netplay));
    if (game->netplay) netplay.value = *game->netplay;
    RecompLauncherV1Run run = {0};
    run.header = launcher_v1_header(sizeof(run));
    run.contract = &query; run.settings = &io; run.game = &description;
    run.mods = game->mods ? &mods : NULL; run.netplay = game->netplay ? &netplay : NULL;
    run.product_root = g_product; run.save_root = g_saves; run.assets_root = g_assets;
    run.window_title = title; run.initial_disc = initial_disc;
    run.starts = launcher_v1_host_starts(g_product, g_saves);
    RecompLauncherV1Result result = {0};
    result.header = launcher_v1_header(sizeof(result));
    result.selected_disc = selected; result.selected_disc_capacity = (uint32_t)capacity;
    result.relaunch_path = g_relaunch; result.relaunch_path_capacity = sizeof(g_relaunch);
    result.replay_path = g_replay; result.replay_path_capacity = sizeof(g_replay);
    const uint32_t status = module.run(&run, &result, g_error, sizeof(g_error));
    launcher_v1_host_starts_clear();
    g_active = 0;
    if (status) return RECOMP_LAUNCHER_RESULT_REFUSED;
    if (launcher_v1_check_header(&result.header, sizeof(result)) || result.action < 0 || result.action > 3 ||
        result.start_kind > RECOMP_LAUNCHER_V1_REPLAY ||
        (result.start_kind == RECOMP_LAUNCHER_V1_SAVE_SLOT && (result.save_slot < 0 || result.save_slot > 11)) ||
        !memchr(g_relaunch, 0, sizeof(g_relaunch)) || !memchr(g_replay, 0, sizeof(g_replay)) ||
        (result.start_kind == RECOMP_LAUNCHER_V1_REPLAY && !g_replay[0])) {
        launcher_v1_diagnostic(g_error, sizeof(g_error), "Launcher returned an invalid action/start result; the host refused to boot or persist its settings.");
        return RECOMP_LAUNCHER_RESULT_REFUSED;
    }
    memcpy(settings, &io.value, sizeof(io.value)); /* includes Quit edits and padding bytes */
    g_start_kind = result.start_kind; g_save_slot = result.save_slot;
    return result.action;
}
