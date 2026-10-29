#include "common/launcher_model.h"

#include <assert.h>
#include <string.h>

/* launcher_model.c contains the Zapper preference mutator even though this
 * test exercises only the generic assist binding model. */
void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    (void)mouse_enabled;
    (void)crosshair;
}

int main(void) {
    const int keys[4] = {0, 0, 0, 0};
    const int pads[4] = {
        RECOMP_LAUNCHER_PAD_BUTTON_COMBO(1u << 3),
        RECOMP_LAUNCHER_PAD_BUTTON_COMBO((1u << 4) | (1u << 8)),
        RECOMP_LAUNCHER_PAD_BUTTON_COMBO((1u << 4) | (1u << 10)),
        0,
    };
    RecompLauncherCGameInfo game;
    RecompLauncherCSettings saved;
    LauncherModel model;
    memset(&game, 0, sizeof(game));
    memset(&saved, 0, sizeof(saved));
    game.settings_bindings = 1;
    game.assist_binding_count = 4;
    game.assist_default_key_bind = keys;
    game.assist_default_pad_bind = pads;
    game.assist_direct_pad_bind_action = 1; /* Rewind, index 0 */

    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.assist_direct_pad_bind_action == 0);
    assert(model.s.assist_pad_bind[0] == pads[0]);
    assert(launcher_model_assist_pad_button_capture_binding(
               &model, 0, 3) == RECOMP_LAUNCHER_PAD_BUTTON_COMBO(1u << 3));
    assert(launcher_model_assist_pad_button_capture_binding(
               &model, 1, 3) == RECOMP_LAUNCHER_PAD_BUTTON(3));

    launcher_model_commit(&model, &saved);
    memset(&model, 0, sizeof(model));
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_pad_bind[0] == pads[0]);

    /* A previously saved custom Select+R3 binding wins over the modern
     * default on both the first model load and a settings reload. */
    saved.assist_pad_bind[0] = RECOMP_LAUNCHER_PAD_BUTTON_COMBO(
        (1u << 4) | (1u << 8));
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_pad_bind[0] == saved.assist_pad_bind[0]);
    launcher_model_commit(&model, &saved);
    memset(&model, 0, sizeof(model));
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_pad_bind[0] == saved.assist_pad_bind[0]);

    /* Older one-button encodings remain distinguishable from the new direct
     * one-button combo, so the runtime can keep their implicit Select chord. */
    assert(RECOMP_LAUNCHER_PAD_BUTTON(3) < 1000);
    assert(RECOMP_LAUNCHER_PAD_BUTTON_COMBO(1u << 3) == 1008);
    /* Named shortcuts also work for hosts such as SNES which retain their
     * own gameplay mapper. Quick Save (action0) must never overwrite Up. */
    for (int settings_owned = 0; settings_owned <= 1; ++settings_owned) {
        game.settings_bindings = settings_owned;
        launcher_model_init(&model, &saved, &game, NULL);
        const int up_key = model.s.player_key_bind[0][0];
        const int up_pad = model.s.player_pad_bind[0][0];
        launcher_model_begin_assist_capture(&model, 0, true);
        launcher_model_set_captured_pad(&model, RECOMP_LAUNCHER_PAD_BUTTON(2));
        launcher_model_cancel_capture(&model);
        launcher_model_begin_assist_capture(&model, 1, false);
        launcher_model_set_captured_key(&model, 61);
        launcher_model_cancel_capture(&model);
        assert(model.s.assist_pad_bind[0] == RECOMP_LAUNCHER_PAD_BUTTON(2));
        assert(model.s.assist_key_bind[1] == 61);
        assert(model.s.player_key_bind[0][0] == up_key);
        assert(model.s.player_pad_bind[0][0] == up_pad);
        launcher_model_commit(&model, &saved);
        launcher_model_init(&model, &saved, &game, NULL);
        assert(model.s.assist_pad_bind[0] == RECOMP_LAUNCHER_PAD_BUTTON(2));
        assert(model.s.assist_key_bind[1] == 61);
    }
    return 0;
}
