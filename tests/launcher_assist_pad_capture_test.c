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
    const int keys[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS] = {0, 0, 0, 0};
    const int pads[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS] = {
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
    /* A cleared controller-only row must survive a commit/reload, rather
     * than being mistaken for an unspecified default. Its hidden keyboard
     * state remains owned by the separate keyboard editor. */
    game.assist_pad_only = 1;
    saved.assist_pad_bind[0] = 0;
    saved.assist_key_bind[0] = 99;
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_pad_bind[0] == pads[0]);
    launcher_model_begin_assist_capture(&model, 0, true);
    launcher_model_set_captured_pad(&model, 0);
    launcher_model_cancel_capture(&model);
    launcher_model_commit(&model, &saved);
    assert(saved.assist_pad_bind[0] == -1);
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_pad_bind[0] == -1);
    launcher_model_reset_assist_bindings(&model);
    assert(model.s.assist_pad_bind[0] == pads[0]);
    assert(model.s.assist_key_bind[0] == 99);
    return 0;
}
