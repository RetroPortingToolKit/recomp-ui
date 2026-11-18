/* Setup toolchain step: native tools on POSIX, portable download on Windows.
 *
 * On Linux/macOS the build tools are the system's own, so the model must not
 * ask the host to download anything and must not word the step as a portable
 * cmake/clang install. Windows keeps its wording and its download request.
 */
#include "launcher_model.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void nap(void) { Sleep(2); }
#else
#include <unistd.h>
static void nap(void) { usleep(2000); }
#endif

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;
static void check(int ok, const char *what) {
    if (ok) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static int g_ready, g_download = -99, g_calls;
static int host_ready(void) { return g_ready; }
static int host_ensure(int download, const char *zip, char *err, size_t cap,
                       RecompLauncherCPrepareProgressFn progress, void *ctx) {
    (void)zip; (void)err; (void)cap; (void)progress; (void)ctx;
    g_download = download;
    ++g_calls;
    return 1;
}

static void fresh(LauncherModel *m) {
    static RecompLauncherCSettings settings;
    static RecompLauncherCGameInfo game;
    memset(&settings, 0, sizeof settings);
    memset(&game, 0, sizeof game);
    game.name = "Toolchain test";
    game.setup_wizard_supported = 1;
    game.setup_needs_toolchain = 1;
    game.toolchain_is_ready = host_ready;
    game.ensure_toolchain_with_progress = host_ensure;
    g_ready = 0;                     /* the init probe must not see the last case's answer */
    launcher_model_init(m, &settings, &game, NULL);
    g_download = -99;
    g_calls = 0;
}

static void run_ensure(LauncherModel *m, char *status, size_t cap, char *title, size_t tcap) {
    launcher_model_start_ensure_toolchain(m);
    snprintf(status, cap, "%s", m->setup_status);       /* wording while it runs */
    snprintf(title, tcap, "%s", m->setup_progress_title);
    for (int i = 0; m->setup_preparing && i < 5000; ++i) {
        launcher_model_poll_prepare_disc(m);
        if (m->setup_preparing) nap();
    }
}

int main(void) {
    static LauncherModel m;
    char status[256], title[128];

#if defined(_WIN32)
    check(!(fresh(&m), m.setup_tc_native), "Windows defaults to the portable toolchain");
#else
    check((fresh(&m), m.setup_tc_native), "POSIX defaults to native tools");
#endif

    /* POSIX wording and no download, whatever the host build's default. */
    fresh(&m);
    m.setup_tc_native = true;
    run_ensure(&m, status, sizeof status, title, sizeof title);
    check(g_calls == 1 && g_download == 0, "native: host is not asked to download");
    check(!strstr(status, "portable") && !strstr(status, "Downloading"),
          "native: status does not mention a portable download");
    check(!strstr(title, "Installing"), "native: title does not say installing");
    check(strstr(status, "CMake") && strstr(title, "Checking"), "native: checking wording");

    fresh(&m);
    m.setup_tc_native = true;
    m.setup_tc_update_available = true;                 /* even an offered update */
    run_ensure(&m, status, sizeof status, title, sizeof title);
    check(g_download == 0, "native: an update offer still does not download");

    fresh(&m);
    m.setup_tc_native = true;
    g_ready = 1;
    launcher_model_start_ensure_toolchain(&m);
    check(g_calls == 0 && strstr(m.setup_status, "build tools") &&
          !strstr(m.setup_status, "portable"), "native: ready tools say 'existing build tools'");

    /* Windows: unchanged. */
    fresh(&m);
    m.setup_tc_native = false;
    run_ensure(&m, status, sizeof status, title, sizeof title);
    check(g_download == 1, "portable: host is asked to download if missing");
    check(strstr(status, "Downloading portable cmake/clang") != NULL,
          "portable: status keeps its wording");
    check(strstr(title, "Installing build tools") != NULL, "portable: title keeps its wording");

    fresh(&m);
    m.setup_tc_native = false;
    g_ready = 1;
    launcher_model_start_ensure_toolchain(&m);
    check(strstr(m.setup_status, "Using existing portable toolchain.") != NULL,
          "portable: ready toolchain keeps its wording");

    return fails ? 1 : 0;
}