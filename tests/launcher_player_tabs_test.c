/* Dashboard player tabs: how many tabs a title gets, and which one is open.
 *
 * The controller cards of a title stood side by side. With four players the
 * second row of cards did not fit above the memory cards: "PLAYER 4" was cut
 * off and the section scrolled. The PSX profile now asks for one tab per
 * player (ControllerSpec.player_tabs) with one card under the strip.
 *
 * The tab count is the number of cards the launcher drew before
 * (launcher_model_visible_player_count): the players the title declares, and
 * two for a title of three or more until Multitap is on. A one-player title
 * gets no strip.
 *
 * Includes the model translation unit, as launcher_pad_mode_test.c does.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect_int(int got, int want, const char* what) {
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
    ++fails;
}

enum { PSX = 0, N64 = 1, SNES = 2 };

static LauncherModel* session(int console, int players, int multitap) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    if (console == PSX) launcher_profile_apply_psx(&game);
    else if (console == N64) launcher_profile_apply_n64(&game);
    else launcher_profile_apply_snes(&game);
    game.name = "Player Tabs Fixture";
    game.num_players = players;
    memset(&io, 0, sizeof(io));
    io.multitap_enabled = multitap;
    launcher_model_init(m, &io, &game, NULL);
    return m;
}

static int tabs(int console, int players, int multitap) {
    LauncherModel* m = session(console, players, multitap);
    int n;
    if (!m) return -1;
    n = launcher_model_player_tabs(m);
    free(m);
    return n;
}

static void test_tab_count(void) {
    expect_int(tabs(PSX, 1, 0), 0, "one player: no tab strip");
    expect_int(tabs(PSX, 2, 0), 2, "two players: two tabs");
    expect_int(tabs(PSX, 4, 1), 4, "four players with Multitap on: four tabs");
    expect_int(tabs(PSX, 8, 1), 8, "eight players with Multitap on: eight tabs");
    expect_int(tabs(PSX, 3, 1), 3, "three players with Multitap on: three tabs");
    expect_int(tabs(PSX, 4, 0), 2, "four players with Multitap off: the two ports");
    expect_int(tabs(PSX, 8, 0), 2, "eight players with Multitap off: the two ports");
    expect_int(tabs(PSX, 2, 1), 2, "Multitap means nothing to a two-player title");
}

static void test_tabs_follow_the_cards(void) {
    for (int players = 1; players <= RECOMP_LAUNCHER_MAX_PLAYERS; ++players) {
        for (int multitap = 0; multitap <= 1; ++multitap) {
            LauncherModel* m = session(PSX, players, multitap);
            char what[96];
            if (!m) return;
            const int cards = launcher_model_visible_player_count(m);
            snprintf(what, sizeof(what),
                     "%d declared, Multitap %s: as many tabs as cards", players,
                     multitap ? "on" : "off");
            expect_int(launcher_model_player_tabs(m), cards >= 2 ? cards : 0, what);
            free(m);
        }
    }
}

static void test_other_consoles_keep_the_grid(void) {
    expect_int(tabs(N64, 4, 0), 0, "N64, four players: no tab strip");
    expect_int(tabs(SNES, 2, 0), 0, "SNES, two players: no tab strip");
}

static void test_hidden_cards(void) {
    LauncherModel* m = session(PSX, 4, 1);
    if (!m) return;
    m->lock_device = true;
    expect_int(launcher_model_player_tabs(m), 0, "lock_device hides the cards: no tab strip");
    free(m);
}

static void test_open_tab(void) {
    LauncherModel* m = session(PSX, 4, 1);
    if (!m) return;
    expect_int(launcher_model_player_tab(m), 0, "the dashboard opens on Player 1");
    launcher_model_set_player_tab(m, 3);
    expect_int(launcher_model_player_tab(m), 3, "Player 4's tab can be opened");
    launcher_model_set_player_tab(m, 99);
    expect_int(launcher_model_player_tab(m), 3, "a tab past the strip reads as the last tab");
    launcher_model_set_player_tab(m, -1);
    expect_int(launcher_model_player_tab(m), 0, "a tab before the strip reads as the first tab");

    /* Multitap off hides seats 3 and 4 while Player 4's tab is open. */
    launcher_model_set_player_tab(m, 3);
    launcher_model_toggle_multitap(m);
    expect_int(launcher_model_player_tabs(m), 2, "Multitap off leaves two tabs");
    expect_int(launcher_model_player_tab(m), 1, "the open tab is then the last one left");
    launcher_model_toggle_multitap(m);
    expect_int(launcher_model_player_tab(m), 3, "and Player 4's tab again when Multitap is back");
    free(m);

    m = session(PSX, 1, 0);
    if (!m) return;
    launcher_model_set_player_tab(m, 1);
    expect_int(launcher_model_player_tab(m), 0, "a one-player title has only Player 1");
    free(m);
}

/* Opening a tab is a view change. It must not write a setting. */
static void test_open_tab_changes_no_setting(void) {
    LauncherModel* m = session(PSX, 4, 1);
    RecompLauncherCSettings before;
    if (!m) return;
    before = m->s;
    for (int p = 0; p < 4; ++p) launcher_model_set_player_tab(m, p);
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "opening tabs leaves the settings as they were");
    for (int slot = 0; slot < 2; ++slot) launcher_model_set_card_tab(m, slot);
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "opening card tabs leaves the settings as they were");
    free(m);
}

/* The tabbed dashboard is the console's choice, whatever the player count:
 * a one-player title has no player strip, and still has the card of the
 * tabbed dashboard and the memory cards as tabs. */
static void test_tabbed_dashboard(void) {
    static const int kConsoles[] = { PSX, N64, SNES };
    static const char* const kNames[] = { "PSX", "N64", "SNES" };
    for (int c = 0; c < 3; ++c) {
        for (int players = 1; players <= 8; players += 7) {
            LauncherModel* m = session(kConsoles[c], players, 1);
            char what[96];
            if (!m) return;
            snprintf(what, sizeof(what), "%s, %d player(s): tabbed dashboard", kNames[c], players);
            expect_int(launcher_model_dashboard_tabbed(m), kConsoles[c] == PSX, what);
            snprintf(what, sizeof(what), "%s, %d player(s): memory-card tabs", kNames[c], players);
            expect_int(launcher_model_card_tabs(m), kConsoles[c] == PSX ? 2 : 0, what);
            free(m);
        }
    }
    expect_int(launcher_model_dashboard_tabbed(NULL), 0, "no model: no tabbed dashboard");
    expect_int(launcher_model_card_tabs(NULL), 0, "no model: no card tabs");
}

static void test_open_card_tab(void) {
    LauncherModel* m = session(PSX, 2, 0);
    if (!m) return;
    expect_int(launcher_model_card_tab(m), 0, "Card 1 is open at the start");
    launcher_model_set_card_tab(m, 1);
    expect_int(launcher_model_card_tab(m), 1, "Card 2 opens");
    launcher_model_set_card_tab(m, 7);
    expect_int(launcher_model_card_tab(m), 1, "a card past the last one is the last card");
    launcher_model_set_card_tab(m, -3);
    expect_int(launcher_model_card_tab(m), 0, "a card before the first one is Card 1");
    launcher_model_set_card_tab(m, 1);
    launcher_model_set_player_tab(m, 1);
    expect_int(launcher_model_card_tab(m), 1, "opening a player tab leaves the card tab alone");
    free(m);

    m = session(SNES, 2, 0);
    if (!m) return;
    launcher_model_set_card_tab(m, 1);
    expect_int(launcher_model_card_tab(m), 0, "a console without card tabs has no open card tab");
    free(m);
}

int main(void) {
    test_tab_count();
    test_tabs_follow_the_cards();
    test_other_consoles_keep_the_grid();
    test_hidden_cards();
    test_open_tab();
    test_open_tab_changes_no_setting();
    test_tabbed_dashboard();
    test_open_card_tab();
    if (fails) { fprintf(stderr, "launcher_player_tabs_test: %d failure(s)\n", fails); return 1; }
    printf("launcher_player_tabs_test: all checks passed\n");
    return 0;
}
