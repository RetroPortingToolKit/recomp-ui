#include "recomp_launcher.h"
#include <stdio.h>
#include <string.h>

static char game_version[128];

int recomp_launcher_parse_cli_args(int* argc, char** argv) {
    int mode = RECOMP_LAUNCHER_CLI_DEFAULT, write = 1;
    if (!argc || !argv || *argc < 1) return mode;
    for (int i=1; i<*argc; ++i) if (!argv[i]) return mode;
    for (int read = 1; read < *argc; ++read) {
        const char* arg = argv[read];
        if (!strcmp(arg, "--")) {
            while (read < *argc) argv[write++] = argv[read++];
            break;
        }
        if (!strcmp(arg, "--no-launcher") || !strcmp(arg, "--skip-launcher"))
            mode = RECOMP_LAUNCHER_CLI_SKIP;
        else if (!strcmp(arg, "--launcher"))
            mode = RECOMP_LAUNCHER_CLI_SHOW;
        else argv[write++] = argv[read];
    }
    argv[write] = NULL;
    *argc = write;
    return mode;
}

int recomp_launcher_cli_should_skip(int mode, int saved_skip_launcher) {
    return mode == RECOMP_LAUNCHER_CLI_SKIP ||
        (mode != RECOMP_LAUNCHER_CLI_SHOW && saved_skip_launcher != 0);
}

void recomp_launcher_set_game_version(const char* version) {
    size_t len=version?strlen(version):0;
    if (len>=sizeof(game_version)) len=sizeof(game_version)-1;
    if (len) memmove(game_version,version,len);
    game_version[len]=0;
}

const char* recomp_launcher_get_game_version(void) { return game_version; }
