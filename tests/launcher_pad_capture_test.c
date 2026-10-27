#include "common/launcher_pad_capture.h"
#include "launcher_profile.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

void launcher_binds_set_zapper(int mouse, int crosshair) {
    (void)mouse; (void)crosshair;
}

int main(void) {
    LauncherModel m;
    RecompLauncherCSettings saved = {0};
    RecompLauncherCGameInfo game = {0};
    LauncherPad pads[2] = {{0}};
    uint32_t id = 99;
    launcher_profile_apply_psx(&game);
    game.num_players = 2;
    launcher_model_init(&m, &saved, &game, NULL);
    pads[0].id = 0; pads[1].id = 7;
    strcpy(pads[0].guid, "identical");
    strcpy(pads[1].guid, "identical");

    launcher_model_set_source(&m, 0, 2, 0, "First controller", "identical");
    launcher_model_set_source(&m, 1, 2, 7, "Second controller", "identical");
    m.cfg_player = 0;
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == 1 && id == 0);
    m.cfg_player = 1;
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == 1 && id == 7);
    /* Reordering enumeration must not swap two identical devices. */
    LauncherPad reversed[2] = {pads[1], pads[0]};
    assert(launcher_pad_capture_resolve(&m, reversed, 2, &id) == 1 && id == 7);

    /* A restored GUID-only seat may resolve ID0, while excluding P2's unit. */
    m.cfg_player = 0;
    m.s.player_gamepad_instance[0] = m.player_pad_id[0] = 0;
    assert(launcher_pad_capture_resolve(&m, reversed, 2, &id) == 1 && id == 0);
    assert(launcher_pad_capture_resolve(&m, &pads[1], 1, &id) == -1);
    assert(launcher_pad_capture_resolve(&m, NULL, 0, &id) == -1);
    /* Reconnect to a new unclaimed instance of the saved profile. */
    pads[0].id = 12;
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == 1 && id == 12);
    strcpy(pads[0].guid, "other-profile");
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == -1);

    /* Generic consoles can still opt into any pad; keyboard and a concrete
     * disconnected selection cannot accidentally capture a different pad. */
    m.s.player_gamepad_guid[0][0] = 0;
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == 0);
    m.s.player_src[0] = 1;
    assert(launcher_pad_capture_resolve(&m, pads, 2, &id) == -1);
    return 0;
}
