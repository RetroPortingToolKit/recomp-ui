/* Per-seat pad_mode resolution in launcher_model_init(), and the interaction
 * between a LOCKED mode and the keyboard-has-no-sticks presentation default.
 *
 * Regression it exists for: a PSX title whose game.toml declares
 * `default_mode = "analog"` + `lock_mode = true` (Ape Escape) booted its pad
 * DIGITAL on a fresh install. The lock IS honoured -- the !pad_mode_selectable
 * branch of the gate assigns locked_pad_mode -- but the keyboard coercion that
 * followed it in the same loop body had no selectable gate, and a release
 * install defaults Player 1's device to Keyboard. So pad_mode[0] came out 2
 * (D-Pad) on every first run of an Analog-locked title.
 *
 * That would be cosmetic if anything could undo it. Nothing can: lock_mode
 * hides the selector, and both launcher_model_set_pad_mode() and
 * apply_default_pad_mode_for_source() deliberately refuse to touch a locked
 * mode. The value stayed 2 after the player assigned a real DualShock, the
 * host read it back into player_mode[], and sio.c answered with the 4-byte
 * digital pad response: right stick dead, left stick folded onto the D-pad.
 *
 * The coercion is a PRESENTATION default only -- psxrecomp's
 * effective_player_mode() already reports DIGITAL for any keyboard seat
 * regardless of the seat's stored mode -- so gating it on pad_mode_selectable
 * costs nothing and keeps the locked mode intact for the pad that arrives later.
 *
 * Includes the model translation unit (as launcher_discs_test.c does) so the
 * static pad-mode helpers are reachable.
 */
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

/* ---- unlocked behaviour must be exactly what it was ---------------------- */
static void test_unlocked_keyboard_presents_dpad(void) {
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_KEYBOARD, PSX_ANALOG);
    if (!m) return;
    expect(m->pad_mode_selectable != 0, "no lock: the selector is live");
    expect_mode(m, 0, PSX_DIGITAL,
                "unlocked keyboard seat still presents D-Pad");
    /* Analog is genuinely unavailable while the keyboard drives the seat. */
    launcher_model_set_pad_mode(m, 0, PSX_ANALOG);
    expect_mode(m, 0, PSX_DIGITAL,
                "unlocked keyboard seat refuses an Analog pick");
    free(m);
}

static void test_unlocked_keyboard_to_pad_self_corrects(void) {
    /* Why the unlocked case was never as bad: assigning a pad goes through
     * launcher_model_set_source -> apply_default_pad_mode_for_source, which
     * is NOT gated off for an unlocked title and seeds Analog for a gamepad
     * seat. So the unlocked equivalent of the bug repairs itself the moment
     * the device is picked -- which is exactly the repair a locked title
     * cannot perform. */
    LauncherModel* m = psx_session(0, PSX_ANALOG, SRC_KEYBOARD, PSX_ANALOG);
    if (!m) return;
    expect_mode(m, 0, PSX_DIGITAL, "starts on D-Pad for the keyboard seat");
    launcher_model_set_source(m, 0, SRC_GAMEPAD, 7, "DualShock", kFixtureGuid);
    expect_mode(m, 0, PSX_ANALOG,
                "assigning a gamepad re-defaults an unlocked seat to Analog");
    /* And back: the presentation default follows the source both ways. */
    launcher_model_set_source(m, 0, SRC_KEYBOARD, 0, NULL, NULL);
    expect_mode(m, 0, PSX_DIGITAL,
                "switching back to keyboard re-presents D-Pad");
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

int main(void) {
    LauncherModel* shared = psx_session(1, PSX_DIGITAL, SRC_KEYBOARD, PSX_DIGITAL);
    expect(launcher_model_visible_player_count(shared) == 2,
           "two-player title exposes both local controller seats");
    launcher_model_set_source(shared, 0, SRC_GAMEPAD, 7, "DualShock", kFixtureGuid);
    expect(launcher_model_gamepad_claimed(shared, 1, kFixtureGuid),
           "normal picker reserves an assigned gamepad");
    shared->allow_shared_gamepad = true;
    expect(!launcher_model_gamepad_claimed(shared, 1, kFixtureGuid),
           "development picker permits the same gamepad for P2");
    launcher_model_set_source(shared, 1, SRC_GAMEPAD, 7, "DualShock", kFixtureGuid);
    expect(shared->s.player_gamepad_instance[0] == shared->s.player_gamepad_instance[1] &&
           strcmp(shared->s.player_gamepad_guid[0], shared->s.player_gamepad_guid[1]) == 0,
           "both seats retain the shared physical controller assignment");
    free(shared);
    test_locked_analog_keyboard_seat();
    test_locked_analog_poisoned_settings();
    test_locked_digital_still_digital();
    test_unlocked_keyboard_presents_dpad();
    test_unlocked_keyboard_to_pad_self_corrects();
    test_unlocked_hybrid_migrates();
    test_genesis_custom_mode_list();
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
