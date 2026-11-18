/* assets_dir contract: the assets folder itself (any name) or the base dir
 * that contains assets/ -- probed, defaulting to the old behaviour. */
#include "launcher_assets_dir.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#endif

static int fails;
static void expect_eq(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) == 0) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s: got '%s' want '%s'\n", what, got, want);
    ++fails;
}

static void resolve(const char* in, char* out, size_t cap) {
    if (!launcher_assets_dir_resolve(in, out, cap)) out[0] = '\0';
}

int main(int argc, char** argv) {
    char root[1024], exe[1100], exe_assets[1200], assets[1100], res[1100], in[1300], out[1300], want[1300];
    if (argc < 2) return 2;
    snprintf(root, sizeof root, "%s", argv[1]);
    MKDIR(root);
    snprintf(exe, sizeof exe, "%s/exe", root);
    snprintf(exe_assets, sizeof exe_assets, "%s/exe/assets", root);
    snprintf(assets, sizeof assets, "%s/assets", root);
    snprintf(res, sizeof res, "%s/res", root);
    MKDIR(exe); MKDIR(exe_assets); MKDIR(assets); MKDIR(res);

    /* the assets folder passed directly: unchanged */
    resolve(assets, out, sizeof out);
    expect_eq("assets folder passed directly", out, assets);

    /* exe dir containing assets/: resolves to the assets folder */
    snprintf(want, sizeof want, "%s/assets", exe);
    resolve(exe, out, sizeof out);
    expect_eq("exe dir containing assets/", out, want);

    /* renamed assets folder (no assets/ inside): old behaviour, kept as is */
    resolve(res, out, sizeof out);
    expect_eq("renamed folder passed directly", out, res);

    /* trailing slashes (both kinds) are tolerated in every shape */
    snprintf(in, sizeof in, "%s/", res);
    resolve(in, out, sizeof out);
    expect_eq("renamed folder, trailing slash", out, res);
    snprintf(in, sizeof in, "%s/", exe);
    snprintf(want, sizeof want, "%s/assets", exe);
    resolve(in, out, sizeof out);
    expect_eq("exe dir, trailing slash", out, want);
    snprintf(in, sizeof in, "%s\\", assets);
    resolve(in, out, sizeof out);
    expect_eq("assets folder, trailing backslash", out, assets);

    /* empty / NULL / too-small buffer */
    assert(launcher_assets_dir_resolve(NULL, out, sizeof out) == 0);
    assert(launcher_assets_dir_resolve("", out, sizeof out) == 0);
    assert(launcher_assets_dir_resolve(exe, out, 4) == 0);

    /* cache: same answer on repeat, and a different dir is re-probed */
    resolve(exe, out, sizeof out);
    snprintf(want, sizeof want, "%s/assets", exe);
    expect_eq("repeat call (cached)", out, want);
    resolve(res, out, sizeof out);
    expect_eq("different dir re-probed", out, res);

    return fails ? 1 : 0;
}
