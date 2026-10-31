/* The "PS1 Mouse" input source (player_src 3, RECOMP_LAUNCHER_SRC_MOUSE).
 *
 * psxrecomp can put a Sony PS1 Mouse in a controller port, driven by the host
 * pointer. The launcher offers it as a fourth source, but only for the PSX
 * profile: no other console here has a mouse port device, so everywhere else
 * a 3 must read as None and never be stored.
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

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

enum { SRC_NONE = 0, SRC_KEYBOARD = 1, SRC_GAMEPAD = 2 };

static LauncherModel* session_players(int psx, int players, int p0_src, int p1_src) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    if (psx) launcher_profile_apply_psx(&game);
    else launcher_profile_apply_genesis(&game);
    game.name = "Mouse Source Fixture";
    game.num_players = players;
    game.pad_mode_supported = 1;
    game.pad_mode_selectable = 1;
    memset(&io, 0, sizeof(io));
    io.player_src[0] = p0_src;
    io.player_src[1] = p1_src;
    launcher_model_init(m, &io, &game, NULL);
    return m;
}

static LauncherModel* session(int psx, int p0_src) {
    return session_players(psx, 2, p0_src, SRC_NONE);
}

static void test_psx_offers_mouse(void) {
    LauncherModel* m = session(1, SRC_KEYBOARD);
    if (!m) return;
    expect(launcher_model_mouse_source_available(m), "PSX offers the mouse source");
    launcher_model_set_source(m, 0, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE, "set_source stores 3 on PSX");
    expect(strcmp(launcher_model_player_src_label(m, 0), "PS1 Mouse") == 0,
           "the label reads PS1 Mouse");
    expect(m->s.player_gamepad_guid[0][0] == '\0', "a mouse seat carries no pad GUID");
    launcher_model_set_source(m, 0, SRC_NONE, 0, NULL, NULL);
    expect(m->s.player_src[0] == SRC_NONE, "choosing None clears the mouse");
    free(m);
}

static void test_host_seeded_mouse_survives_init(void) {
    /* psxrecomp seeds player_src = 3 from settings.toml pN_device = "mouse". */
    LauncherModel* m = session(1, RECOMP_LAUNCHER_SRC_MOUSE);
    if (!m) return;
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE,
           "a host-seeded mouse seat survives launcher_model_init");
    expect(strcmp(launcher_model_player_src_label(m, 0), "PS1 Mouse") == 0,
           "and is labelled PS1 Mouse");
    free(m);
}

static void test_cycle_includes_mouse_on_psx(void) {
    LauncherModel* m = session(1, SRC_GAMEPAD);
    if (!m) return;
    launcher_model_cycle_player_src(m, 0);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE, "PSX cycle: Gamepad -> Mouse");
    launcher_model_cycle_player_src(m, 0);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_GUNCON, "PSX cycle: Mouse -> GunCon");
    launcher_model_cycle_player_src(m, 0);
    expect(m->s.player_src[0] == SRC_NONE, "PSX cycle: GunCon -> None");
    free(m);
}

/* ---- the GunCon source (player_src 4, PS1B-305) ---- */

static void test_psx_offers_guncon(void) {
    LauncherModel* m = session(1, SRC_KEYBOARD);
    if (!m) return;
    expect(launcher_model_guncon_source_available(m), "PSX offers the GunCon source");
    launcher_model_set_source(m, 0, RECOMP_LAUNCHER_SRC_GUNCON, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_GUNCON, "set_source stores 4 on PSX");
    expect(strcmp(launcher_model_player_src_label(m, 0), "GunCon") == 0,
           "the label reads GunCon");
    expect(m->s.player_gamepad_guid[0][0] == '\0', "a GunCon seat carries no pad GUID");
    expect(launcher_model_source_is_pointer(RECOMP_LAUNCHER_SRC_GUNCON) &&
           launcher_model_source_is_pointer(RECOMP_LAUNCHER_SRC_MOUSE) &&
           !launcher_model_source_is_pointer(SRC_GAMEPAD) &&
           !launcher_model_source_is_pointer(SRC_KEYBOARD),
           "the mouse and the GunCon are the pointer sources");
    free(m);

    m = session(1, RECOMP_LAUNCHER_SRC_GUNCON);
    if (!m) return;
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_GUNCON,
           "a host-seeded GunCon seat survives launcher_model_init");
    free(m);

    m = session(0, SRC_GAMEPAD);
    if (!m) return;
    expect(!launcher_model_guncon_source_available(m), "Genesis offers no GunCon source");
    launcher_model_set_source(m, 0, RECOMP_LAUNCHER_SRC_GUNCON, 0, NULL, NULL);
    expect(m->s.player_src[0] == SRC_NONE, "set_source maps 4 to None off PSX");
    m->s.player_src[1] = RECOMP_LAUNCHER_SRC_GUNCON;   /* a stray value from a host */
    expect(strcmp(launcher_model_player_src_label(m, 1), "None") == 0,
           "a stray 4 is labelled None off PSX");
    free(m);
}

static void test_guncon_leaves_mouse_pair(void) {
    /* One-card title, mouse in port 2 with a pad in port 1: picking the
     * GunCon for Player 1 ends the pair and empties port 2. */
    LauncherModel* m = session_players(1, 1, SRC_GAMEPAD, RECOMP_LAUNCHER_SRC_MOUSE);
    if (!m) return;
    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_GUNCON, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_GUNCON && m->s.player_src[1] == SRC_NONE,
           "GunCon for Player 1 replaces the mouse pair");
    expect(launcher_model_mouse_pair_port(m) == 0, "and there is no mouse pair");
    free(m);
}

static void test_other_profiles_refuse_mouse(void) {
    LauncherModel* m = session(0, SRC_GAMEPAD);
    if (!m) return;
    expect(!launcher_model_mouse_source_available(m), "Genesis offers no mouse source");
    launcher_model_set_source(m, 0, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[0] == SRC_NONE, "set_source maps 3 to None off PSX");
    launcher_model_cycle_player_src(m, 0);
    launcher_model_cycle_player_src(m, 0);
    launcher_model_cycle_player_src(m, 0);
    expect(m->s.player_src[0] == SRC_NONE, "non-PSX cycle stays None/Kbd/Pad");
    m->s.player_src[1] = RECOMP_LAUNCHER_SRC_MOUSE;   /* a stray value from a host */
    expect(strcmp(launcher_model_player_src_label(m, 1), "None") == 0,
           "a stray 3 is labelled None off PSX");
    free(m);
}

/* ---- PS1 Mouse + pad in a one-card title (PS1B-279) ---- */

static void test_pair_only_for_one_card_psx(void) {
    LauncherModel* m = session_players(1, 1, SRC_KEYBOARD, SRC_NONE);
    if (!m) return;
    expect(launcher_model_mouse_pair_available(m), "a one-player PSX title offers mouse + pad");
    expect(launcher_model_mouse_pair_port(m) == 0, "no mouse yet: pair port 0");
    expect(launcher_model_mouse_pair_pad_seat(m) == -1, "no mouse yet: no pad seat");
    free(m);
    m = session_players(1, 2, SRC_KEYBOARD, SRC_NONE);
    if (!m) return;
    expect(!launcher_model_mouse_pair_available(m),
           "a two-player title has a Player 2 card instead");
    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE &&
           m->s.player_src[1] == SRC_NONE,
           "there the primary pick is a plain seat 0 pick");
    free(m);
    m = session_players(0, 1, SRC_KEYBOARD, SRC_NONE);
    if (!m) return;
    expect(!launcher_model_mouse_pair_available(m), "Genesis offers no mouse + pad");
    free(m);
}

static void test_mouse_keeps_keyboard_in_port_2(void) {
    LauncherModel* m = session_players(1, 1, SRC_KEYBOARD, SRC_NONE);
    if (!m) return;
    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE, "PS1 Mouse goes to port 1");
    expect(m->s.player_src[1] == SRC_KEYBOARD, "the keyboard Player 1 had moves to port 2");
    expect(m->s.pad_mode[1] == 2, "and stays digital");
    expect(launcher_model_mouse_pair_port(m) == 1, "pair port reads 1");
    expect(launcher_model_mouse_pair_pad_seat(m) == 1, "pad seat reads 1");

    launcher_model_set_mouse_pair_port(m, 2);
    expect(m->s.player_src[0] == SRC_KEYBOARD && m->s.player_src[1] == RECOMP_LAUNCHER_SRC_MOUSE,
           "mouse in port 2 swaps the seats");
    expect(m->s.pad_mode[0] == 2, "the pad type moves with the keyboard");
    expect(launcher_model_mouse_pair_port(m) == 2 && launcher_model_mouse_pair_pad_seat(m) == 0,
           "pair port 2, pad seat 0");
    launcher_model_set_mouse_pair_port(m, 2);
    expect(m->s.player_src[1] == RECOMP_LAUNCHER_SRC_MOUSE, "picking the same port changes nothing");

    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[1] == RECOMP_LAUNCHER_SRC_MOUSE && m->s.player_src[0] == SRC_KEYBOARD,
           "picking PS1 Mouse again keeps the layout");

    launcher_model_set_primary_source(m, SRC_KEYBOARD, 0, NULL, NULL);
    expect(m->s.player_src[0] == SRC_KEYBOARD && m->s.player_src[1] == SRC_NONE,
           "leaving the mouse from port 2 empties port 2");
    expect(launcher_model_mouse_pair_port(m) == 0, "and ends the pair");
    free(m);
}

static void test_mouse_keeps_gamepad_in_port_2(void) {
    LauncherModel* m = session_players(1, 1, SRC_NONE, SRC_NONE);
    if (!m) return;
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "Xbox One Controller", "guid-xbox");
    m->s.deadzone[0] = 15;
    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[1] == SRC_GAMEPAD &&
           strcmp(m->s.player_gamepad_guid[1], "guid-xbox") == 0,
           "the pad Player 1 had moves to port 2 with its GUID");
    expect(strcmp(m->player_pad_name[1], "Xbox One Controller") == 0 && m->player_pad_id[1] == 7,
           "with its name and device id");
    expect(m->s.deadzone[1] == 15, "and its deadzone");
    expect(m->s.player_gamepad_guid[0][0] == '\0' && m->player_pad_id[0] == 0,
           "the mouse seat carries no pad");

    /* Leaving the mouse by picking that same pad puts it back in port 1 once. */
    launcher_model_set_primary_source(m, SRC_GAMEPAD, 7, "Xbox One Controller", "guid-xbox");
    expect(m->s.player_src[0] == SRC_GAMEPAD &&
           strcmp(m->s.player_gamepad_guid[0], "guid-xbox") == 0,
           "picking the port 2 pad as Player 1 moves it to port 1");
    expect(m->s.player_src[1] == SRC_NONE, "and never leaves it in two ports");
    free(m);
}

static void test_leaving_mouse_in_port_1_keeps_port_2_pad(void) {
    LauncherModel* m = session_players(1, 1, RECOMP_LAUNCHER_SRC_MOUSE, SRC_NONE);
    if (!m) return;
    launcher_model_set_source(m, 1, SRC_GAMEPAD, 3, "Pad", "guid-pad");
    launcher_model_set_primary_source(m, SRC_KEYBOARD, 0, NULL, NULL);
    expect(m->s.player_src[0] == SRC_KEYBOARD, "the pick replaces the mouse in port 1");
    expect(m->s.player_src[1] == SRC_GAMEPAD, "a different pad in port 2 stays");
    free(m);
}

static void test_mouse_does_not_evict_port_2(void) {
    LauncherModel* m = session_players(1, 1, SRC_KEYBOARD, SRC_NONE);
    if (!m) return;
    launcher_model_set_source(m, 1, SRC_GAMEPAD, 3, "Pad", "guid-pad");
    launcher_model_set_primary_source(m, RECOMP_LAUNCHER_SRC_MOUSE, 0, NULL, NULL);
    expect(m->s.player_src[0] == RECOMP_LAUNCHER_SRC_MOUSE &&
           m->s.player_src[1] == SRC_GAMEPAD &&
           strcmp(m->s.player_gamepad_guid[1], "guid-pad") == 0,
           "a device already in port 2 stays; the mouse takes port 1");
    free(m);
}

static void test_host_seeded_mouse_in_port_2(void) {
    /* A settings.toml with p1 = pad and p2 = "mouse" (Final Doom layout). */
    LauncherModel* m = session_players(1, 1, SRC_GAMEPAD, RECOMP_LAUNCHER_SRC_MOUSE);
    if (!m) return;
    expect(launcher_model_mouse_pair_port(m) == 2, "a seeded port 2 mouse reads as pair port 2");
    expect(launcher_model_mouse_pair_pad_seat(m) == 0, "with the pad in port 1");
    launcher_model_set_mouse_pair_port(m, 3);
    expect(launcher_model_mouse_pair_port(m) == 2, "port 3 is refused");
    free(m);
}

int main(void) {
    test_psx_offers_mouse();
    test_host_seeded_mouse_survives_init();
    test_cycle_includes_mouse_on_psx();
    test_other_profiles_refuse_mouse();
    test_pair_only_for_one_card_psx();
    test_mouse_keeps_keyboard_in_port_2();
    test_mouse_keeps_gamepad_in_port_2();
    test_leaving_mouse_in_port_1_keeps_port_2_pad();
    test_mouse_does_not_evict_port_2();
    test_host_seeded_mouse_in_port_2();
    test_psx_offers_guncon();
    test_guncon_leaves_mouse_pair();
    if (fails) { fprintf(stderr, "launcher_mouse_source_test: %d failure(s)\n", fails); return 1; }
    printf("launcher_mouse_source_test: all checks passed\n");
    return 0;
}
