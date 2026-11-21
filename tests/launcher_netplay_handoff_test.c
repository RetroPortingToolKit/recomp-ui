/* RECOMP_NETPLAY_LAUNCH through the real C entry point: a record is handed to
 * the game's ingest_launch, the match comes back from fill_launch, the mods are
 * cleared through commit_netplay, the window never opens, and the hub reads the
 * outcome from "<record>.status". Refusals quit rather than start a match the
 * game could not settle (recomp-ai-rules NETPLAY.md section 4). */
#include "launcher_backend.h"
#include "launcher_platform.h"
#include "launcher_ini.h"
#include "../src/common/launcher_ini.inc"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#define setenv(k, v, o) _putenv_s(k, v)
#else
#include <unistd.h>
#endif

static int window_opened = 0;

bool launcher_platform_open(LauncherPlatform* p, const char* title, int w, int h) {
    (void)p; (void)title; (void)w; (void)h;
    window_opened = 1;
    return true;
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
    (void)p; (void)m; (void)theme;
    return LNG_ACTION_QUIT;
}

static int failures = 0;
static void require(int ok, const char* label) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

/* ---- a fake engine ------------------------------------------------------ */

static char ingested[256];
static int refuse_ingest = 0, fill_ok = 1, mods_cleared = 0, pending_cleared = 0;

static int fake_ingest(void* ctx, const char* json, char* why, size_t why_cap) {
    (void)ctx;
    snprintf(ingested, sizeof ingested, "%s", json);
    if (refuse_ingest) {
        snprintf(why, why_cap, "disc \"A\" is not this game's");
        return 1;
    }
    return 0;
}
static int fake_fill(void* ctx, RecompLauncherCNetplayLaunch* out) {
    (void)ctx;
    if (!fill_ok) return 0;
    out->enabled = 1;
    out->session_id = 4242;
    out->local_slot = 1;
    snprintf(out->peer_hostport, sizeof out->peer_hostport, "203.0.113.5:9000");
    return 1;
}
static const char* fake_last_error(void* ctx) { (void)ctx; return "missing_endpoints"; }
static void fake_clear_pending(void* ctx) { (void)ctx; ++pending_cleared; }
static int fake_commit_netplay(void* ctx, const char* image) {
    (void)ctx; (void)image;
    ++mods_cleared;
    return 1;
}

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}
static void read_text(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "rb");
    size_t n = 0;
    out[0] = '\0';
    if (!f) return;
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
}

static int run(RecompLauncherCGameInfo* game, RecompLauncherCSettings* s,
               char* rom, size_t rom_cap) {
    rom[0] = '\0';
    return recomp_launcher_run_window("handoff", s, game, ".", "disc A.cue", rom, rom_cap);
}

int main(int argc, char** argv) {
    RecompLauncherCNetplayCallbacks np;
    RecompLauncherCModProvider mods;
    RecompLauncherCGameInfo game;
    RecompLauncherCSettings s;
    char rom[256], status[512];
    int rc;

    if (argc > 1 && chdir(argv[1]) != 0) return 2;

    memset(&np, 0, sizeof np);
    np.ingest_launch = fake_ingest;
    np.fill_launch = fake_fill;
    np.last_error = fake_last_error;
    np.clear_launch_pending = fake_clear_pending;
    memset(&mods, 0, sizeof mods);
    mods.commit_netplay = fake_commit_netplay;
    memset(&game, 0, sizeof game);
    game.netplay_supported = 1;
    game.netplay = &np;
    game.mods = &mods;

    /* 1. A record the game adopts: LAUNCH, no window, the match filled in, the
     *    record consumed from the environment, mods cleared, status ok. */
    write_text("rec1.json", "{\"v\":1,\"player_id\":\"p2\"}");
    remove("rec1.json.status");
    setenv("RECOMP_NETPLAY_LAUNCH", "rec1.json", 1);
    memset(&s, 0, sizeof s);
    rc = run(&game, &s, rom, sizeof rom);
    require(rc == RECOMP_LAUNCHER_RESULT_LAUNCH, "adopted record launches");
    require(!window_opened, "no window for a handed-off match");
    require(strcmp(ingested, "{\"v\":1,\"player_id\":\"p2\"}") == 0, "record text reaches ingest_launch");
    require(s.netplay_launch.enabled && s.netplay_launch.session_id == 4242 &&
            s.netplay_launch.local_slot == 1, "fill_launch's match comes back in io");
    require(strcmp(rom, "disc A.cue") == 0, "the initial image is the one booted");
    require(mods_cleared == 1, "mods cleared through commit_netplay");
    require(pending_cleared == 1, "launch_pending cleared");
    require(getenv("RECOMP_NETPLAY_LAUNCH") == NULL ||
            getenv("RECOMP_NETPLAY_LAUNCH")[0] == '\0', "record consumed from the environment");
    read_text("rec1.json.status", status, sizeof status);
    require(strcmp(status, "{\"ok\":true}\n") == 0, "status ok");

    /* 2. Without the variable the window opens as before. */
    window_opened = 0;
    memset(&s, 0, sizeof s);
    rc = run(&game, &s, rom, sizeof rom);
    require(window_opened, "no record: the launcher opens");
    require(rc == RECOMP_LAUNCHER_RESULT_QUIT, "no record: the backend's answer");
    require(!s.netplay_launch.enabled, "no record: no match");

    /* 3. The game refuses the record: QUIT, its reason in the status, quoted. */
    window_opened = 0;
    refuse_ingest = 1;
    write_text("rec3.json", "{}");
    setenv("RECOMP_NETPLAY_LAUNCH", "rec3.json", 1);
    memset(&s, 0, sizeof s);
    rc = run(&game, &s, rom, sizeof rom);
    require(rc == RECOMP_LAUNCHER_RESULT_QUIT, "refused record quits");
    require(!window_opened && !s.netplay_launch.enabled, "refused record: no window, no match");
    read_text("rec3.json.status", status, sizeof status);
    require(strcmp(status, "{\"ok\":false,\"why\":\"disc \\\"A\\\" is not this game's\"}\n") == 0,
            "refusal reason in the status, escaped");
    refuse_ingest = 0;

    /* 4. fill_launch cannot settle: QUIT with the engine's last error. */
    fill_ok = 0;
    write_text("rec4.json", "{}");
    setenv("RECOMP_NETPLAY_LAUNCH", "rec4.json", 1);
    rc = run(&game, &s, rom, sizeof rom);
    read_text("rec4.json.status", status, sizeof status);
    require(rc == RECOMP_LAUNCHER_RESULT_QUIT, "unsettled match quits");
    require(strstr(status, "missing_endpoints") != NULL, "last_error in the status");
    fill_ok = 1;

    /* 5. A build without ingest_launch says so instead of opening a lobby. */
    np.ingest_launch = NULL;
    window_opened = 0;
    write_text("rec5.json", "{}");
    setenv("RECOMP_NETPLAY_LAUNCH", "rec5.json", 1);
    rc = run(&game, &s, rom, sizeof rom);
    read_text("rec5.json.status", status, sizeof status);
    require(rc == RECOMP_LAUNCHER_RESULT_QUIT && !window_opened, "no ingest_launch: quit, no window");
    require(strstr(status, "update the game") != NULL, "no ingest_launch: says to update");

    if (failures) return 1;
    printf("launcher_netplay_handoff_test: ok\n");
    return 0;
}
