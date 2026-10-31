/* The "NeGcon" pad type (pad_mode 3, RECOMP_LAUNCHER_PAD_MODE_NEGCON).
 *
 * psxrecomp can present a Namco neGcon in a controller port, driven by the
 * seat's pad or keyboard (PS1B-304). The launcher offers it as a third pad
 * type next to Analog and D-Pad, but only for the PSX profile. Unlike Analog,
 * a keyboard may drive it (stick binds twist it), and it is the device in the
 * port, so changing the seat's input source must not reset it.
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

enum { PSX_ANALOG = 1, PSX_DIGITAL = 2, PSX_NEGCON = RECOMP_LAUNCHER_PAD_MODE_NEGCON };
enum { SRC_NONE = 0, SRC_KEYBOARD = 1, SRC_GAMEPAD = 2 };

static LauncherModel* session(int psx, int locked, int p0_src, int p0_mode) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    if (psx) launcher_profile_apply_psx(&game);
    else launcher_profile_apply_genesis(&game);
    game.name = "NeGcon Fixture";
    game.num_players = 2;
    game.pad_mode_supported = 1;
    game.pad_mode_selectable = locked ? 0 : 1;
    game.locked_pad_mode = PSX_DIGITAL;
    memset(&io, 0, sizeof(io));
    io.player_src[0] = p0_src;
    io.pad_mode[0] = p0_mode;
    io.player_src[1] = SRC_NONE;
    io.pad_mode[1] = PSX_ANALOG;
    launcher_model_init(m, &io, &game, NULL);
    return m;
}

static void test_psx_offers_negcon(void) {
    LauncherModel* m = session(1, 0, SRC_GAMEPAD, PSX_ANALOG);
    if (!m) return;
    expect(launcher_model_negcon_mode_available(m), "PSX offers the neGcon pad type");
    launcher_model_set_pad_mode(m, 0, PSX_NEGCON);
    expect(m->s.pad_mode[0] == PSX_NEGCON, "a gamepad seat can pick neGcon");
    launcher_model_set_pad_mode(m, 0, PSX_DIGITAL);
    expect(m->s.pad_mode[0] == PSX_DIGITAL, "and go back to D-Pad");
    free(m);
}

static void test_keyboard_can_drive_negcon(void) {
    LauncherModel* m = session(1, 0, SRC_KEYBOARD, PSX_DIGITAL);
    if (!m) return;
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    expect(m->s.pad_mode[0] == PSX_DIGITAL, "a keyboard still cannot pick Analog");
    launcher_model_set_pad_mode(m, 0, PSX_NEGCON);
    expect(m->s.pad_mode[0] == PSX_NEGCON, "a keyboard can pick neGcon");
    free(m);

    m = session(1, 0, SRC_KEYBOARD, PSX_NEGCON);
    if (!m) return;
    expect(m->s.pad_mode[0] == PSX_NEGCON,
           "a host-seeded keyboard neGcon survives init (not forced to D-Pad)");
    free(m);

    m = session(1, 0, SRC_KEYBOARD, PSX_ANALOG);
    if (!m) return;
    expect(m->s.pad_mode[0] == PSX_DIGITAL,
           "a keyboard Analog seat is still presented as D-Pad");
    free(m);
}

static void test_source_change_keeps_negcon(void) {
    LauncherModel* m = session(1, 0, SRC_GAMEPAD, PSX_NEGCON);
    if (!m) return;
    expect(m->s.pad_mode[0] == PSX_NEGCON, "a host-seeded gamepad neGcon survives init");
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect(m->s.pad_mode[0] == PSX_NEGCON, "switching to the keyboard keeps neGcon");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 4, "Pad", "guid-pad");
    expect(m->s.pad_mode[0] == PSX_NEGCON, "switching to a pad keeps neGcon");
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect(m->s.pad_mode[0] == PSX_DIGITAL, "an Analog seat still becomes D-Pad on the keyboard");
    free(m);
}

static void test_locked_title(void) {
    LauncherModel* m = session(1, 1, SRC_GAMEPAD, PSX_NEGCON);
    if (!m) return;
    expect(m->s.pad_mode[0] == PSX_DIGITAL, "a locked title keeps its locked mode");
    launcher_model_set_pad_mode(m, 0, PSX_NEGCON);
    expect(m->s.pad_mode[0] == PSX_DIGITAL, "and refuses neGcon");
    free(m);
}

static void test_other_profiles_refuse_negcon(void) {
    LauncherModel* m = session(0, 0, SRC_GAMEPAD, 0);
    if (!m) return;
    expect(!launcher_model_negcon_mode_available(m), "Genesis offers no neGcon pad type");
    /* Genesis lists its own modes (0 = 3-Button, 1 = 6-Button); 3 is not one. */
    launcher_model_set_pad_mode(m, 0, PSX_NEGCON);
    expect(m->s.pad_mode[0] == 0, "set_pad_mode(3) is refused on Genesis");
    free(m);
}

int main(void) {
    test_psx_offers_negcon();
    test_keyboard_can_drive_negcon();
    test_source_change_keeps_negcon();
    test_locked_title();
    test_other_profiles_refuse_negcon();
    if (fails) { fprintf(stderr, "launcher_negcon_mode_test: %d failure(s)\n", fails); return 1; }
    printf("launcher_negcon_mode_test: all checks passed\n");
    return 0;
}
