/* Genesis controller/lobby seat ceiling in launcher_model_init().
 *
 * The Genesis SystemProfile ceiling is the engine's six logical players
 * (RUI_GEN_MAX_PLAYERS; segagenesisrecomp GameSpec.logical_players), while
 * GameInfo.num_players stays the per-game capability. Sonic 2's party mod
 * declares six players and must get six controller cards and six lobby
 * seats; every two-port Genesis title must keep exactly two, and nothing may
 * exceed the six the engine and bind store can route.
 *
 * Includes the model translation unit (as launcher_pad_mode_test.c does). */
#include "launcher_model.c"
#include "consoles/genesis/genesis_binds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to seats. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect_int(int got, int want, const char* what) {
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
    ++fails;
}

/* The lobby seat ceiling the model starts with (Host panel "Max Players",
 * the create op's max_slots). It must follow the same profile clamp as the
 * controller seats: a lobby with more seats than the console can route
 * admits players who have no pad. */
static int g_last_host_max;

static int genesis_player_count(int num_players) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    int n;
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return -1; }

    memset(&game, 0, sizeof(game));
    launcher_profile_apply_genesis(&game);
    game.name = "Genesis Seat Fixture";
    game.num_players = num_players;
    memset(&io, 0, sizeof(io));

    launcher_model_init(m, &io, &game, NULL);
    n = m->player_count;
    g_last_host_max = m->netplay_host_max_players;
    free(m);
    return n;
}

int main(void) {
    const SystemProfile* p = launcher_system_by_id("genesis");
    expect_int(p ? p->controller.max_players : -1, RUI_GEN_MAX_PLAYERS,
               "profile ceiling equals the Genesis bind store width");
    expect_int(genesis_player_count(4), 4, "four-player title gets four seats");
    expect_int(genesis_player_count(2), 2, "two-port title keeps two seats");
    expect_int(genesis_player_count(6), 6, "six-player title gets six controller seats");
    expect_int(g_last_host_max, 6, "six-player title hosts six lobby seats");
    expect_int(genesis_player_count(8), 6, "request above the ceiling clamps to six");
    expect_int(g_last_host_max, 6, "lobby seat ceiling follows the profile clamp (8 -> 6)");
    genesis_player_count(4);
    expect_int(g_last_host_max, 4, "four-player title hosts four lobby seats");
    genesis_player_count(2);
    expect_int(g_last_host_max, 2, "two-port title hosts two lobby seats");
    genesis_player_count(1);
    expect_int(g_last_host_max, 2, "a one-player title still hosts the two-seat minimum");
    return fails ? 1 : 0;
}
