// launcher_files_spawn_test.c — what the Linux native file picker hands to the
// backend it spawns, and what it makes of the way that backend exits.
//
// Both halves of the FZeroSNESRecomp 1.6.0 "the ROM button doesn't respond"
// bug live here:
//
//   1. The picker used to spawn zenity/kdialog with the launcher's OWN
//      environ. Inside an AppImage, AppRun had prepended the bundle's usr/lib
//      to LD_LIBRARY_PATH, so the HOST zenity loaded the BUNDLE's glib/pcre2
//      against the host GTK it was linked against and died on start.
//
//   2. Whatever non-zero code that death produced was mapped to 0 — "the user
//      cancelled" — so the click did nothing at all and the caller never
//      learned it should open the built-in browser instead.
//
// The test builds stub backends on a private PATH: they record the environment
// they were given and exit with a code we choose. No display, no zenity, no
// GTK — it runs anywhere.

#include "launcher_files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int cond, const char* what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_failures++;
}

#if defined(__linux__)

static char g_dir[512];
static char g_bin[640];
static char g_env_file[768];
static const char* g_env_tool = NULL;

static void die(const char* what) {
    fprintf(stderr, "test setup failed: %s: %s\n", what, strerror(errno));
    exit(2);
}

/* PATH inside the stubs is our own stub directory, so `env` must be named by
 * absolute path or the stub cannot record anything — and every "the backend
 * did not receive X" check below would pass for the wrong reason. */
static const char* find_env_tool(void) {
    static const char* const candidates[] = {"/usr/bin/env", "/bin/env"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (access(candidates[i], X_OK) == 0) return candidates[i];
    fprintf(stderr, "test setup failed: no env(1) found\n");
    exit(2);
}

/* A stub named `name` that dumps its environment to g_env_file, optionally
 * prints `emit` on stdout, and exits with $STUB_EXIT (default 1). */
static void write_stub(const char* name, const char* emit) {
    char path[800];
    snprintf(path, sizeof(path), "%s/%s", g_bin, name);
    FILE* f = fopen(path, "w");
    if (!f) die("fopen stub");
    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "'%s' > '%s'\n", g_env_tool, g_env_file);
    if (emit && emit[0]) fprintf(f, "printf '%%s\\n' '%s'\n", emit);
    fprintf(f, "exit ${STUB_EXIT:-1}\n");
    fclose(f);
    if (chmod(path, 0755) != 0) die("chmod stub");
}

/* Did the stub actually record anything? Without this the environment checks
 * are vacuous: a stub that failed to run at all "did not receive" every
 * variable we ask about. */
static int recording_exists(void) { return access(g_env_file, R_OK) == 0; }

static void remove_stub(const char* name) {
    char path[800];
    snprintf(path, sizeof(path), "%s/%s", g_bin, name);
    unlink(path);
}

/* Value of `key` in the environment the stub recorded, or NULL if the stub was
 * not given it at all. Returned buffer is static. */
static const char* recorded_env(const char* key) {
    static char value[4096];
    value[0] = '\0';
    FILE* f = fopen(g_env_file, "r");
    if (!f) return NULL;
    const size_t klen = strlen(key);
    char line[4096];
    const char* found = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        snprintf(value, sizeof(value), "%s", line + klen + 1);
        size_t n = strlen(value);
        while (n && (value[n - 1] == '\n' || value[n - 1] == '\r'))
            value[--n] = '\0';
        found = value;
        break;
    }
    fclose(f);
    return found;
}

static void forget_recording(void) { unlink(g_env_file); }

static int run_pick(char* out, size_t out_cap) {
    return launcher_try_pick_file("Select game file", NULL, 0, NULL, out,
                                  out_cap);
}

int main(int argc, char** argv) {
    const char* base = (argc > 1) ? argv[1] : ".";
    snprintf(g_dir, sizeof(g_dir), "%s/launcher-files-spawn-test", base);
    snprintf(g_bin, sizeof(g_bin), "%s/bin", g_dir);
    snprintf(g_env_file, sizeof(g_env_file), "%s/backend.env", g_dir);
    mkdir(g_dir, 0755);
    if (mkdir(g_bin, 0755) != 0 && errno != EEXIST) die("mkdir bin");
    g_env_tool = find_env_tool();

    /* A PATH holding ONLY our stubs, so a zenity or kdialog installed on the
     * build machine can never be picked up instead — and so "no backend at
     * all" below is a real state and not a property of this host. The stubs
     * are #!/bin/sh scripts; the kernel resolves that shebang without PATH. */
    setenv("PATH", g_bin, 1);
    unsetenv("XDG_SESSION_DESKTOP");
    unsetenv("XDG_CURRENT_DESKTOP");

    char out[4096];

    /* ---- 1. The spawned backend must not inherit the bundle's loader env --
     * This is the AppImage case: AppRun set LD_LIBRARY_PATH to the bundle and
     * saved the host's (empty) value. zenity must see NEITHER. */
    write_stub("zenity", NULL);
    setenv("LD_LIBRARY_PATH", "/tmp/appdir/usr/lib:", 1);
    setenv("LD_PRELOAD", "/tmp/appdir/usr/lib/libsomething.so", 1);
    setenv("APPDIR", "/tmp/appdir", 1);
    setenv("APPIMAGE", "/tmp/Game.AppImage", 1);
    setenv("GTK_PATH", "/tmp/appdir/usr/lib/gtk-3.0", 1);
    setenv("RECOMP_HOST_LD_LIBRARY_PATH", "", 1);
    setenv("STUB_EXIT", "1", 1);
    /* A variable we did not inject must still reach the backend. */
    setenv("DISPLAY", ":0", 1);

    forget_recording();
    (void)run_pick(out, sizeof(out));

    check(recording_exists(),
          "the stub backend really ran and recorded its environment");
    check(recorded_env("LD_LIBRARY_PATH") == NULL,
          "bundle LD_LIBRARY_PATH is not inherited by the spawned backend");
    check(recorded_env("LD_PRELOAD") == NULL,
          "LD_PRELOAD is not inherited by the spawned backend");
    check(recorded_env("APPDIR") == NULL,
          "APPDIR is not inherited by the spawned backend");
    check(recorded_env("APPIMAGE") == NULL,
          "APPIMAGE is not inherited by the spawned backend");
    check(recorded_env("GTK_PATH") == NULL,
          "GTK_PATH is not inherited by the spawned backend");
    {
        const char* d = recorded_env("DISPLAY");
        check(d && strcmp(d, ":0") == 0,
              "the rest of the environment IS passed through (DISPLAY)");
    }

    /* ---- 2. A saved host LD_LIBRARY_PATH is restored, not dropped -------- */
    setenv("LD_LIBRARY_PATH", "/tmp/appdir/usr/lib:/opt/host/lib", 1);
    setenv("RECOMP_HOST_LD_LIBRARY_PATH", "/opt/host/lib", 1);
    forget_recording();
    (void)run_pick(out, sizeof(out));
    check(recording_exists(), "the stub backend ran for the restore check");
    {
        const char* v = recorded_env("LD_LIBRARY_PATH");
        check(v && strcmp(v, "/opt/host/lib") == 0,
              "a saved host LD_LIBRARY_PATH is restored for the backend");
    }
    unsetenv("RECOMP_HOST_LD_LIBRARY_PATH");
    unsetenv("LD_LIBRARY_PATH");

    /* ---- 3. Exit-code mapping -------------------------------------------
     * 1 is the documented cancel for zenity AND kdialog, and the only
     * non-zero code that means cancel. Everything else means the backend is
     * unusable, which the caller must be able to tell apart so it can open
     * the built-in browser. */
    setenv("STUB_EXIT", "1", 1);
    out[0] = 'x';
    check(run_pick(out, sizeof(out)) == 0, "exit 1 is a cancel (0)");
    check(out[0] == '\0', "a cancel clears the output path");

    setenv("STUB_EXIT", "127", 1);
    check(run_pick(out, sizeof(out)) == -1,
          "exit 127 (backend could not start) is unusable (-1), not a cancel");

    setenv("STUB_EXIT", "2", 1);
    check(run_pick(out, sizeof(out)) == -1, "exit 2 is unusable (-1)");

    setenv("STUB_EXIT", "255", 1);
    check(run_pick(out, sizeof(out)) == -1, "exit 255 is unusable (-1)");

    /* Exit 0 with nothing on stdout is not a selection and not a cancel: a
     * working dialog that reports success always prints the path. */
    setenv("STUB_EXIT", "0", 1);
    check(run_pick(out, sizeof(out)) == -1,
          "exit 0 with no path printed is unusable (-1)");

    /* ---- 4. The success path still works --------------------------------- */
    write_stub("zenity", "/tmp/some/game.sfc");
    setenv("STUB_EXIT", "0", 1);
    out[0] = '\0';
    check(run_pick(out, sizeof(out)) == 1, "exit 0 with a path selects (1)");
    check(strcmp(out, "/tmp/some/game.sfc") == 0,
          "the selected path is returned verbatim");

    /* ---- 5. A killed backend is unusable, not a cancel -------------------- */
    {
        char path[800];
        snprintf(path, sizeof(path), "%s/zenity", g_bin);
        FILE* f = fopen(path, "w");
        if (!f) die("fopen kill stub");
        fprintf(f, "#!/bin/sh\nenv > '%s'\nkill -TERM $$\n", g_env_file);
        fclose(f);
        if (chmod(path, 0755) != 0) die("chmod kill stub");
    }
    check(run_pick(out, sizeof(out)) == -1,
          "a backend that dies on SIGTERM is unusable (-1), not a cancel");

    /* ---- 6. No backend at all ------------------------------------------- */
    remove_stub("zenity");
    remove_stub("kdialog");
    check(!launcher_native_file_picker_available(),
          "no zenity and no kdialog reports no native picker");
    check(run_pick(out, sizeof(out)) == -1,
          "with no backend installed the caller is told to use its own UI");

    printf("%s\n", g_failures ? "launcher_files spawn test FAILED"
                              : "launcher_files spawn test passed");
    return g_failures ? 1 : 0;
}

#else /* !__linux__ */

int main(void) {
    check(1, "launcher_files spawn test is Linux-only (nothing to check here)");
    printf("launcher_files spawn test skipped\n");
    return 0;
}

#endif
