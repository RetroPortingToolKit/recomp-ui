/* Controller selection: automatic local mode, legacy-setting migration,
 * explicit title locks, and independent console mode vocabularies. */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to pad modes. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static void expect_mode(const LauncherModel* m, int player, int want,
                        const char* what) {
    if (m->s.pad_mode[player] == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (pad_mode[%d] = %d, want %d)\n",
            what, player, m->s.pad_mode[player], want);
    ++fails;
}

/* PSX pad modes, mirroring the engine's PadMode enum
 * (recompiler/src/config_loader.h: ANALOG = 1, DIGITAL = 2). 0 is Hybrid,
 * which is mod-only and never selectable. */
enum { PSX_ANALOG = 1, PSX_DIGITAL = 2 };
/* recomp-ui player_src vocabulary: 0 None, 1 Keyboard, 2 Gamepad. */
enum { SRC_NONE = 0, SRC_KEYBOARD = 1, SRC_GAMEPAD = 2 };

static const char* kFixtureGuid = "030000004c0500006802000000000000";

/* One launcher session: a game spec + the settings the host handed in.
 * `locked` mirrors game.toml [controller] lock_mode; the host passes it as
 * pad_mode_selectable = !lock_mode plus locked_pad_mode = the game-declared
 * mode (see ae_fill_psx_launcher_game_info in psxrecomp runtime/src/main.cpp). */
static LauncherModel* psx_session(int locked, int locked_mode,
                                  int p0_src, int p0_persisted_mode) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }

    memset(&game, 0, sizeof(game));
    launcher_profile_apply_psx(&game);
    game.name = "Locked Mode Fixture";
    game.num_players = 2;
    game.pad_mode_selectable = locked ? 0 : 1;
    game.locked_pad_mode = locked_mode;

    memset(&io, 0, sizeof(io));
    io.player_src[0] = p0_src;
    io.pad_mode[0] = p0_persisted_mode;
    io.player_src[1] = SRC_NONE;
    io.pad_mode[1] = p0_persisted_mode;

    launcher_model_init(m, &io, &game, NULL);
    return m;
}

static LauncherModel* genesis_session(int p0_src, int p0_persisted_mode) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }

    memset(&game, 0, sizeof(game));
    launcher_profile_apply_genesis(&game);
    game.name = "Genesis Fixture";
    game.num_players = 2;

    memset(&io, 0, sizeof(io));
    io.player_src[0] = p0_src;
    io.pad_mode[0] = p0_persisted_mode;

    launcher_model_init(m, &io, &game, NULL);
    return m;
}

/* ---- the bug, exactly as the owner hit it -------------------------------- */
static void test_locked_analog_keyboard_seat(void) {
    /* Fresh release install of an Analog-locked title: Player 1's device
     * defaults to keyboard (psxrecomp runtime/src/main.cpp seeds
     * player_device[0] = "keyboard"), and settings.toml has nothing yet, so
     * the host seeds pad_mode 0. */
    LauncherModel* m = psx_session(1, PSX_ANALOG, SRC_KEYBOARD, 0);
    if (!m) return;
    expect(!m->pad_mode_selectable, "lock_mode hides the selector");
    expect_mode(m, 0, PSX_ANALOG,
                "locked ANALOG survives a keyboard seat (the reported bug)");
    expect_mode(m, 1, PSX_ANALOG, "locked ANALOG applies to every seat");

    /* The player now assigns a real DualShock. Nothing about that transition
     * may be needed to REPAIR the mode -- there is no UI that could. */
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "DualShock", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "still ANALOG once a gamepad is assigned");

    /* Locked means locked in BOTH directions: the hidden selector cannot be
     * driven, so a stray set_pad_mode must not push the seat to digital. */
    launcher_model_set_pad_mode(m, 0, PSX_DIGITAL);
    expect_mode(m, 0, PSX_ANALOG, "set_pad_mode is refused while locked");
    free(m);
}

static void test_locked_analog_poisoned_settings(void) {
    /* The corruption is durable: the host writes player_mode[] back to
     * settings.toml, so a build that already shipped the bug hands the
     * launcher a persisted DIGITAL for an ANALOG-locked title. The lock must
     * override the stale value rather than adopt it. */
    LauncherModel* m = psx_session(1, PSX_ANALOG, SRC_KEYBOARD, PSX_DIGITAL);
    if (!m) return;
    expect_mode(m, 0, PSX_ANALOG,
                "a settings.toml poisoned with DIGITAL is re-clamped to the lock");
    free(m);
}

static void test_locked_digital_still_digital(void) {
    /* The other direction of the same lock: digital-only titles (X4, Tomba 2)
     * must stay digital even for a gamepad seat, whose unlocked default would
     * be Analog. */
    LauncherModel* m = psx_session(1, PSX_DIGITAL, SRC_GAMEPAD, PSX_ANALOG);
    if (!m) return;
    expect_mode(m, 0, PSX_DIGITAL, "locked DIGITAL overrides a gamepad seat");
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    expect_mode(m, 0, PSX_DIGITAL,
                "set_pad_mode is refused while locked (digital)");
    free(m);
}

/* Automatic controller selection. */
static void test_auto_keyboard(void) {
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_KEYBOARD, PSX_ANALOG);
    if (!m) return;
    expect(m->pad_mode_selectable != 0, "no lock: the selector is live");
    expect_mode(m, 0, PSX_ANALOG,
                "unlocked keyboard seat presents Auto");
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    expect_mode(m, 0, PSX_ANALOG,
                "unlocked keyboard seat accepts Auto");
    free(m);
}

static void test_auto_source_changes(void) {
    /* Input-source changes do not choose the live controller mode. */
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_KEYBOARD, PSX_ANALOG);
    if (!m) return;
    expect_mode(m, 0, PSX_ANALOG, "starts on Auto for the keyboard seat");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "DualShock", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG,
                "assigning a gamepad keeps Auto");
    /* And back: the presentation default follows the source both ways. */
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect_mode(m, 0, PSX_ANALOG,
                "switching back to keyboard preserves Auto");
    free(m);
}

static void test_unlocked_hybrid_migrates(void) {
    /* Hybrid (0) is mod-only and never selectable; a stale persisted 0 on a
     * gamepad seat migrates to Analog. Unchanged by this fix, pinned because
     * it shares the same gate. */
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_GAMEPAD, 0);
    if (!m) return;
    expect_mode(m, 0, PSX_ANALOG, "stale Hybrid migrates to Analog");
    free(m);
}

static void test_legacy_digital_migrates_to_auto(void) {
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_GAMEPAD, PSX_DIGITAL);
    if (!m) return;
    expect_mode(m, 0, PSX_ANALOG, "saved D-Pad setting migrates to Auto");
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect_mode(m, 0, PSX_ANALOG, "migrated Auto survives keyboard reassignment");
    free(m);
}

/* ---- a console with its own mode vocabulary is untouched ----------------- */
static void test_genesis_custom_mode_list(void) {
    /* Genesis modes are 0 = 3-Button, 1 = 6-Button -- NOT the PSX
     * Analog/D-Pad pair. The keyboard coercion has always skipped consoles
     * with a custom mode list (a keyboard can drive a 6-button pad fine), and
     * gating it on pad_mode_selectable must not change that. */
    LauncherModel* m = genesis_session(SRC_KEYBOARD, 1);
    if (!m) return;
    expect_mode(m, 0, 1, "Genesis keyboard seat keeps 6-Button");
    free(m);

    m = genesis_session(SRC_KEYBOARD, 0);
    if (!m) return;
    expect_mode(m, 0, 0, "Genesis keyboard seat keeps 3-Button");
    free(m);

    /* An out-of-vocabulary value (e.g. a PSX 2 written by a shared
     * settings.toml) snaps to the first listed mode, not to D-Pad. */
    m = genesis_session(SRC_KEYBOARD, PSX_DIGITAL);
    if (!m) return;
    expect_mode(m, 0, kGenesisPadModes[0].mode,
                "an unlisted Genesis mode snaps to the first listed mode");
    free(m);
}


/* Dino uses existing PSX title metadata; the public module ABI stays frozen. */
static void test_dino_default_and_analog_note(void) {
    RecompLauncherCGameInfo game = {0};
    RecompLauncherCSettings io = {0};
    launcher_profile_apply_psx(&game);
    game.name = "Dino Crisis";
    game.pad_mode_selectable = 1;
    io.player_src[0] = SRC_GAMEPAD;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(*m));
    if (!m) { ++fails; return; }
    launcher_model_init(m, &io, &game, NULL);
    expect_mode(m, 0, PSX_ANALOG, "Dino unset gamepad defaults to Auto");
    expect(!strcmp(launcher_model_analog_note(m),
                   "this game does not use the analog stick"), "exact approved Dino Analog note");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "Gamepad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "Dino gamepad assignment keeps Auto");
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    expect_mode(m, 0, PSX_ANALOG, "Dino Auto remains selectable");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 8, "Other pad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "Dino reassignment preserves Auto");
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect_mode(m, 0, PSX_ANALOG, "Dino keyboard selection preserves Auto");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 8, "Gamepad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "Dino gamepad return preserves Auto");
    io = m->s;
    launcher_model_init(m, &io, &game, NULL);
    expect_mode(m, 0, PSX_ANALOG, "Dino Auto survives launcher reload");
    launcher_model_set_pad_mode(m, 0, PSX_DIGITAL);
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "Gamepad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "removed D-Pad selection cannot replace Auto");
    io = m->s;
    io.player_src[0] = SRC_KEYBOARD;
    io.pad_mode[0] = PSX_ANALOG;
    launcher_model_init(m, &io, &game, NULL);
    expect_mode(m, 0, PSX_ANALOG, "Dino keeps Auto while keyboard drives the seat");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "Gamepad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "Dino source change keeps Auto");
    game.name = "Unchanged other title";
    io.pad_mode[0] = PSX_DIGITAL;
    launcher_model_init(m, &io, &game, NULL);
    expect(launcher_model_analog_note(m) == NULL, "other title has no Dino Analog note");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "Gamepad", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG, "other PSX gamepad uses Auto");
    launcher_profile_apply_genesis(&game);
    game.name = "Dino Crisis";
    launcher_model_init(m, &io, &game, NULL);
    expect(launcher_model_analog_note(m) == NULL, "same name on another console gets no Dino note");
    free(m);
}

int main(void) {
    test_dino_default_and_analog_note();
    test_locked_analog_keyboard_seat();
    test_locked_analog_poisoned_settings();
    test_locked_digital_still_digital();
    test_auto_keyboard();
    test_auto_source_changes();
    test_unlocked_hybrid_migrates();
    test_legacy_digital_migrates_to_auto();
    test_genesis_custom_mode_list();
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
