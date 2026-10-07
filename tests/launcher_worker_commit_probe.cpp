/* Optional desktop probe of the real backend. Run in an isolated directory:
 *   probe <existing small ROM> worker|sync|session success|fail launch|quit
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
    int commits = 0;
    bool success = true, on_worker = false;
    std::string image, error = "Probe: verified resource is missing.";
};
static int count(void* ctx) {
    auto& p = *static_cast<Provider*>(ctx);
    if (p.busy.load() || std::this_thread::get_id() != p.owner) ++p.violations;
    return 0;
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
    mods.ctx = &provider; mods.package_count = count;
    mods.commit = commit; mods.last_error = error; mods.commit_worker_safe = worker || session;
    RecompLauncherCGameInfo game{};
    game.name = "Worker commit probe"; game.region = "TEST"; game.num_players = 1;
    game.mods = &mods; game.in_session = session;
    RecompLauncherCSettings settings{};
    settings.window_scale = 3; settings.player_src[0] = 1;
    LauncherModel model;
    launcher_model_init(&model, &settings, &game, argv[1]);
    if (!model.mods || !launcher_model_can_launch(&model)) return 3;
    LauncherPlatform platform;
    if (!launcher_platform_open(&platform, "Worker commit probe", 1100, 880)) return 4;
    LauncherTheme theme = launcher_theme_default();
    const LngAction action = launcher_backend_run(&platform, &model, &theme);
    launcher_platform_close(&platform);
    const bool wants_launch = std::strcmp(argv[4], "launch") == 0;
    bool ok = provider.commits == 1 && !provider.busy && provider.violations == 0 &&
        provider.on_worker == worker && provider.image == argv[1] &&
        action == (wants_launch ? LNG_ACTION_LAUNCH : LNG_ACTION_QUIT);
    if (!provider.success && !wants_launch && !session)
        ok = ok && (std::strcmp(model.mod_status, provider.error.c_str()) == 0 || platform.should_quit);
    if (!ok) std::fprintf(stderr, "FAIL: actual backend ownership/result contract\n");
    return ok ? 0 : 1;
}
