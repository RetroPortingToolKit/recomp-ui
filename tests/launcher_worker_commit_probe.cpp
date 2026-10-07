/* Optional desktop probe of the real backend. Run in an isolated directory:
 *   probe <existing small ROM> worker|sync|session success|fail launch|quit|close
 * Drive PLAY with LNG_SCRIPT, then wait/shot/quit while commit sleeps. No
 * provider callbacks may run concurrently; the exit status checks this and
 * the result. Screenshots are made by the existing script surface only. */
#include "launcher_backend.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

struct Provider {
    std::thread::id owner = std::this_thread::get_id();
    std::atomic<bool> busy{false};
    std::atomic<int> violations{0};
    std::atomic<int> reads{0};
    int commits = 0;
    bool success = true, on_worker = false;
    std::string image, error = "Probe: verified resource is missing.";
};
static void read(void* ctx) {
    auto& p = *static_cast<Provider*>(ctx);
    ++p.reads;
    if (p.busy.load() || std::this_thread::get_id() != p.owner) ++p.violations;
}
static int count(void* ctx) { read(ctx); return 1; }
static int package(void* ctx, int index, RecompLauncherCModPackage* out) {
    read(ctx);
    if (index != 0 || !out) return 0;
    *out = {};
    std::snprintf(out->id, sizeof out->id, "probe.content");
    std::snprintf(out->version, sizeof out->version, "1.0.0");
    std::snprintf(out->name, sizeof out->name, "Verified content");
    out->enabled = 1;
    return 1;
}
static int feature(void* ctx, int index, RecompLauncherCModFeature* out) {
    read(ctx);
    if (index != 0 || !out) return 0;
    *out = {};
    std::snprintf(out->package_id, sizeof out->package_id, "probe.content");
    std::snprintf(out->package_version, sizeof out->package_version, "1.0.0");
    std::snprintf(out->package_name, sizeof out->package_name, "Verified content");
    std::snprintf(out->id, sizeof out->id, "content");
    std::snprintf(out->name, sizeof out->name, "Content preparation");
    out->enabled = 1;
    return 1;
}
static int option(void* ctx, const char*, const char*, int, RecompLauncherCModOption*) {
    read(ctx); return 0;
}
static int enable(void* ctx, const char*, const char*, int) { read(ctx); return 1; }
static int set_option(void* ctx, const char*, const char*, const char*, const char*) {
    read(ctx); return 1;
}
static int commit(void* ctx, const char* image) {
    auto& p = *static_cast<Provider*>(ctx);
    p.busy = true;
    ++p.commits;
    p.on_worker = std::this_thread::get_id() != p.owner;
    p.image = image;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    p.busy = false;
    return p.success ? 1 : 0;
}
static const char* error(void* ctx) { return static_cast<Provider*>(ctx)->error.c_str(); }
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    Provider provider;
    provider.success = std::strcmp(argv[3], "fail") != 0;
    const bool worker = std::strcmp(argv[2], "worker") == 0;
    const bool session = std::strcmp(argv[2], "session") == 0;
    RecompLauncherCModProvider mods{};
    mods.ctx = &provider; mods.package_count = count; mods.package_get = package;
    mods.feature_count = count; mods.feature_get = feature;
    mods.feature_option_get = option; mods.feature_enable = enable;
    mods.feature_set_option = set_option;
    mods.commit = commit; mods.last_error = error; mods.commit_worker_safe = worker || session;
    RecompLauncherCGameInfo game{};
    game.name = "Worker commit probe"; game.region = "TEST"; game.num_players = 1;
    game.mods = &mods; game.in_session = session;
    RecompLauncherCSettings settings{};
    settings.window_scale = 3; settings.player_src[0] = 1;
    LauncherModel model;
    launcher_model_init(&model, &settings, &game, argv[1]);
    if (!model.mods || !launcher_model_can_launch(&model)) return 3;
    launcher_model_set_view(&model, LNG_VIEW_MODS);
    LauncherPlatform platform;
    if (!launcher_platform_open(&platform, "Worker commit probe", 1100, 880)) return 4;
    LauncherTheme theme = launcher_theme_default();
    const LngAction action = launcher_backend_run(&platform, &model, &theme);
    launcher_platform_close(&platform);
    const bool wants_launch = std::strcmp(argv[4], "launch") == 0;
    bool ok = provider.commits == 1 && !provider.busy && provider.violations == 0 && provider.reads > 0 &&
        provider.on_worker == worker && provider.image == argv[1] &&
        action == (wants_launch ? LNG_ACTION_LAUNCH : LNG_ACTION_QUIT);
    if (!provider.success && std::strcmp(argv[4], "quit") == 0 && !session)
        ok = ok && std::strcmp(model.mod_status, provider.error.c_str()) == 0;
    if (!ok) std::fprintf(stderr, "FAIL: actual backend ownership/result contract\n");
    return ok ? 0 : 1;
}
