/* Player 1's mouse source entry (GameInfo.has_mouse_controls).
 *
 * What this pins:
 *   - Snap-style mouse controls are unchanged: "Keyboard + Mouse", the
 *     mouse-aim defaults on a fresh struct, sensitivity clamping;
 *   - a host-emulated mouse device (mouse_source_label + mouse_source_only,
 *     psxrecomp's PlayStation Mouse) shows the host's name, keeps the
 *     host-seeded mouse_enabled (on or off) and seeds no mouse-aim defaults;
 *   - the source selection round-trips through mouse_enabled;
 *   - a game without has_mouse_controls keeps the single "Keyboard" entry
 *     even when a label is set.
 *
 * Includes the model translation unit (as launcher_pad_mode_test.c does).
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

static RecompLauncherCGameInfo g_game;
static RecompLauncherCSettings g_io;

static LauncherModel* session(int mouse_controls, const char* label,
                              int source_only, int p0_src, int mouse_enabled,
                              float sensitivity) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&g_game, 0, sizeof(g_game));
    launcher_profile_apply_psx(&g_game);
    g_game.name = "Mouse Source Fixture";
    g_game.num_players = 2;
    g_game.pad_mode_selectable = 1;
    g_game.has_mouse_controls = mouse_controls;
    g_game.mouse_source_label = label;
    g_game.mouse_source_only = source_only;
    memset(&g_io, 0, sizeof(g_io));
    g_io.player_src[0] = p0_src;
    g_io.mouse_enabled = mouse_enabled;
    g_io.mouse_sensitivity = sensitivity;
    launcher_model_init(m, &g_io, &g_game, NULL);
    return m;
}

static void test_snap_unchanged(void) {
    /* Fresh struct (sensitivity 0): the Snap defaults are seeded. */
    LauncherModel* m = session(1, NULL, 0, SRC_NONE, 0, 0.0f);
    if (!m) return;
    expect(m->s.player_src[0] == SRC_KEYBOARD && m->s.mouse_enabled == 1,
           "snap: a fresh struct defaults to Keyboard + Mouse");
    expect(m->s.mouse_sensitivity > 0.05f && m->s.mouse_sensitivity < 0.07f &&
           m->s.mouse_invert_y == 1,
           "snap: mouse-aim defaults are seeded");
    expect(strcmp(launcher_model_player_src_label(m, 0), "Keyboard + Mouse") == 0,
           "snap: the entry is labelled Keyboard + Mouse");
    expect(strcmp(launcher_model_mouse_source_label(m), "Keyboard + Mouse") == 0,
           "snap: the mouse entry name is Keyboard + Mouse");
    launcher_model_set_mouse_source(m, 0);
    expect(strcmp(launcher_model_player_src_label(m, 0), "Keyboard") == 0 &&
           m->s.mouse_enabled == 0, "snap: Keyboard turns mouse-aim off");
    free(m);

    /* Host-seeded values are only clamped. */
    m = session(1, NULL, 0, SRC_KEYBOARD, 0, 0.9f);
    if (!m) return;
    expect(m->s.mouse_enabled == 0 && m->s.mouse_sensitivity == 0.50f,
           "snap: a seeded sensitivity is clamped, mouse_enabled kept");
    free(m);

    /* An empty label is the default label. */
    m = session(1, "", 0, SRC_KEYBOARD, 1, 0.06f);
    if (!m) return;
    expect(strcmp(launcher_model_player_src_label(m, 0), "Keyboard + Mouse") == 0,
           "snap: an empty label keeps Keyboard + Mouse");
    free(m);
}

static void test_host_mouse_device(void) {
    /* Fresh struct: no mouse-aim defaults, the host's choice (off) stands. */
    LauncherModel* m = session(1, "PlayStation Mouse", 1, SRC_KEYBOARD, 0, 0.0f);
    if (!m) return;
    expect(m->s.player_src[0] == SRC_KEYBOARD && m->s.mouse_enabled == 0,
           "device: the host's Keyboard stays Keyboard");
    expect(m->s.mouse_sensitivity == 0.0f && m->s.mouse_invert_y == 0 &&
           m->s.mouse_bind[0] == 0 && m->s.mouse_bind[1] == 0,
           "device: no mouse-aim defaults are seeded");
    expect(strcmp(launcher_model_player_src_label(m, 0), "Keyboard") == 0,
           "device: label Keyboard while the mouse is off");
    expect(strcmp(launcher_model_mouse_source_label(m), "PlayStation Mouse") == 0,
           "device: the entry carries the host's name");

    /* Selecting the entry and back round-trips through mouse_enabled. */
    launcher_model_set_mouse_source(m, 1);
    expect(m->s.player_src[0] == SRC_KEYBOARD && m->s.mouse_enabled == 1,
           "device: selecting the entry sets mouse_enabled");
    expect(strcmp(launcher_model_player_src_label(m, 0), "PlayStation Mouse") == 0,
           "device: label PlayStation Mouse while selected");
    launcher_model_set_mouse_source(m, 0);
    expect(m->s.mouse_enabled == 0, "device: Keyboard clears mouse_enabled");
    launcher_model_set_source(m, 0, SRC_NONE, 0, NULL, NULL);
    expect(strcmp(launcher_model_player_src_label(m, 0), "None") == 0,
           "device: None is still None");
    free(m);

    /* A host-seeded mouse (e.g. p1_device = "mouse") is shown selected. */
    m = session(1, "PlayStation Mouse", 1, SRC_KEYBOARD, 7, 0.0f);
    if (!m) return;
    expect(m->s.mouse_enabled == 1, "device: mouse_enabled is normalized to 1");
    expect(strcmp(launcher_model_player_src_label(m, 0), "PlayStation Mouse") == 0,
           "device: a seeded mouse is shown selected");
    free(m);

    /* Only player 1 has the entry. */
    m = session(1, "PlayStation Mouse", 1, SRC_KEYBOARD, 1, 0.0f);
    if (!m) return;
    m->s.player_src[1] = SRC_KEYBOARD;
    expect(strcmp(launcher_model_player_src_label(m, 1), "Keyboard") == 0,
           "device: player 2 keyboard stays Keyboard");
    free(m);
}

static void test_no_mouse_controls(void) {
    LauncherModel* m = session(0, "PlayStation Mouse", 1, SRC_KEYBOARD, 1, 0.0f);
    if (!m) return;
    expect(strcmp(launcher_model_player_src_label(m, 0), "Keyboard") == 0,
           "off: without has_mouse_controls the entry is plain Keyboard");
    launcher_model_set_mouse_source(m, 0);
    expect(m->s.mouse_enabled == 1,
           "off: set_mouse_source is a no-op without has_mouse_controls");
    free(m);
}

int main(void) {
    test_snap_unchanged();
    test_host_mouse_device();
    test_no_mouse_controls();
    if (fails) {
        fprintf(stderr, "launcher_mouse_source_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("launcher_mouse_source_test: OK\n");
    return 0;
}
