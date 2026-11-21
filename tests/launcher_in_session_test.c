/* The in-session launcher's model contract (GameInfo.in_session and
 * has_open_launcher_hotkey): both default off, so every host that predates
 * them opens exactly the launcher it always did; both carry through when a
 * host sets them; and the OpenLauncher hotkey has a label. */
#include "launcher_model.h"

#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    (void)mouse_enabled;
    (void)crosshair;
}

static int fails;
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

int main(void) {
    LauncherModel model;
    RecompLauncherCSettings settings;
    RecompLauncherCGameInfo game;

    memset(&settings, 0, sizeof(settings));
    memset(&game, 0, sizeof(game));
    game.name = "In-session test";
    launcher_model_init(&model, &settings, &game, NULL);
    check(!model.in_session, "a zeroed GameInfo is not in session");
    check(!model.has_open_launcher_hotkey,
          "a zeroed GameInfo does not offer the OpenLauncher hotkey");

    game.in_session = 1;
    game.has_open_launcher_hotkey = 1;
    launcher_model_init(&model, &settings, &game, NULL);
    check(model.in_session, "in_session carries into the model");
    check(model.has_open_launcher_hotkey,
          "has_open_launcher_hotkey carries into the model");

    for (int h = 0; h < LNG_HK_COUNT; ++h) {
        const char *name = launcher_hotkey_name((LngHotkey)h);
        check(name && name[0] && strcmp(name, "?") != 0,
              "every hotkey has a display label");
    }
    check(strcmp(launcher_hotkey_name(LNG_HK_OPEN_LAUNCHER), "Open launcher") == 0,
          "the OpenLauncher row is labelled \"Open launcher\"");

    if (fails) return 1;
    printf("ok: in-session launcher model contract\n");
    return 0;
}
