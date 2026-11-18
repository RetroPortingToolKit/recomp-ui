/* The title's own folder (launcher_model_user_dir) and the cover template the
 * model mirrors from GameInfo. A bare settings file name must not leave the
 * cover and the icon in whatever the current folder is later: it resolves to
 * an absolute folder when the model is built. No settings file: no folder. */
#include "launcher_model.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;
static void check(int ok, const char *what) {
    if (ok) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static const char *dir_for(LauncherModel *m, const char *config, const char *tmpl) {
    static RecompLauncherCSettings settings;
    static RecompLauncherCGameInfo game;
    memset(&settings, 0, sizeof settings);
    memset(&game, 0, sizeof game);
    game.name = "User dir test";
    game.config_path = config;
    game.cover_url_template = tmpl;
    launcher_model_init(m, &settings, &game, NULL);
    return launcher_model_user_dir(m);
}

static void slashes(char *s) { for (; *s; ++s) if (*s == '\\') *s = '/'; }

int main(int argc, char **argv) {
    static LauncherModel m;
    char cwd[1024], want[1100], got[1100];

    check(!strcmp(dir_for(&m, "/games/x/config.ini", NULL), "/games/x/"), "a path: its folder, with the separator");
    check(!strcmp(dir_for(&m, "C:\\games\\x\\config.ini", NULL), "C:\\games\\x\\"), "a backslash path: its folder");
    check(!strcmp(dir_for(&m, NULL, NULL), ""), "no settings file: no folder");
    check(!strcmp(dir_for(&m, "", NULL), ""), "an empty settings path: no folder");

    if (argc > 1 && chdir(argv[1]) != 0) { fprintf(stderr, "FAIL: chdir\n"); return 1; }
    if (!getcwd(cwd, sizeof cwd)) { fprintf(stderr, "FAIL: getcwd\n"); return 1; }
    snprintf(want, sizeof want, "%s/", cwd);
    slashes(want);
    snprintf(got, sizeof got, "%s", dir_for(&m, "config.ini", NULL));
    slashes(got);
    check(got[0] != '\0' && !strcmp(got, want), "a bare file name: the current folder, absolute, with a separator");
    check(strcmp(got, "/") && strcmp(got, "./") && got[0] != '.', "the bare name's folder is not a relative one");

    check(dir_for(&m, "/g/config.ini", "https://a.example/{serial}.jpg") &&
          !strcmp(m.cover_url_template, "https://a.example/{serial}.jpg"),
          "the cover template is mirrored from GameInfo");
    dir_for(&m, "/g/config.ini", NULL);
    check(m.cover_url_template == NULL, "no template in GameInfo: none in the model");

    return fails ? 1 : 0;
}