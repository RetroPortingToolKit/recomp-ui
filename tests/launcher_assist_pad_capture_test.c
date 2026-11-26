#ifdef NDEBUG
#undef NDEBUG
#endif
#include "common/launcher_model.h"

#include <assert.h>
#include <string.h>

/* launcher_model.c contains the Zapper preference mutator even though this
 * test exercises only the generic assist binding model. */
void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    (void)mouse_enabled;
    (void)crosshair;
}

/* The documented count determines which host defaults belong to the model.
 * Extra sentinel values must never become defaults for unused rows. */
static int test_count_sized_defaults(void) {
    const int counts[] = {1, 4, RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS, 0};
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        int count = counts[n];
        int keys[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
        int pads[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
        RecompLauncherCGameInfo game = {0};
        RecompLauncherCSettings saved = {0};
        LauncherModel model;
        for (int i = 0; i < RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS; ++i) {
            keys[i] = 20 + i;
            pads[i] = RECOMP_LAUNCHER_PAD_BUTTON(i);
        }
        game.assist_binding_count = count;
        game.assist_default_key_bind = keys;
        game.assist_default_pad_bind = pads;
        launcher_model_init(&model, &saved, &game, NULL);
        /* Reset owns a copy, rather than retaining the host arrays. */
        memset(keys, 0, sizeof(keys));
        memset(pads, 0, sizeof(pads));
        memset(model.s.assist_key_bind, 0, sizeof(model.s.assist_key_bind));
        memset(model.s.assist_pad_bind, 0, sizeof(model.s.assist_pad_bind));
        launcher_model_reset_assist_bindings(&model);
        for (int i = 0; i < RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS; ++i) {
            int key = i < count ? 20 + i : 0;
            int pad = i < count ? RECOMP_LAUNCHER_PAD_BUTTON(i) : 0;
            if (model.default_assist_key_bind[i] != key ||
                model.default_assist_pad_bind[i] != pad ||
                model.s.assist_key_bind[i] != key ||
                model.s.assist_pad_bind[i] != pad)
                return 1;
        }
    }
    return 0;
}

int main(void) {
    if (test_count_sized_defaults()) return 1;
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
    /* Legacy hosts retain integer units. A fractional host explicitly opts in;
     * committing/reopening the common model must preserve 2.1x and Toggle. */
    assert(model.assist_fast_forward_divisor == 1);
    game.assist_fast_forward_min = 10;
    game.assist_fast_forward_max = 50;
    game.assist_fast_forward_divisor = 10;
    saved.assist_fast_forward_multiplier = 21;
    saved.fast_forward_toggle_mode = 1;
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.assist_fast_forward_divisor == 10);
    assert(model.s.assist_fast_forward_multiplier == 21);
    assert(model.s.fast_forward_toggle_mode == 1);
    launcher_model_commit(&model, &saved);
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_fast_forward_multiplier == 21);
    assert(model.s.fast_forward_toggle_mode == 1);
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
    /* A legacy-key-store host can mix a pad-only shortcut with an unbound
     * title action. Capturing the latter never edits the former's keys. */
    game.settings_bindings = 0;
    game.assist_pad_only = 0;
    game.assist_key_disabled_mask = 1u;
    saved.assist_key_bind[0] = 99;
    launcher_model_init(&model, &saved, &game, NULL);
    launcher_model_begin_assist_capture(&model, 0, false);
    assert(!model.capturing);
    launcher_model_begin_assist_capture(&model, 0, true);
    launcher_model_set_captured_pad(&model, 0);
    launcher_model_cancel_capture(&model);
    assert(model.s.assist_pad_bind[0] == -1);
    launcher_model_begin_assist_capture(&model, 3, false);
    assert(model.capturing && model.capture_assist && model.capture_mouse_armed);
    launcher_model_set_captured_key(&model, 513); /* Mouse1 */
    launcher_model_cancel_capture(&model);
    launcher_model_commit(&model, &saved);
    launcher_model_init(&model, &saved, &game, NULL);
    assert(model.s.assist_key_bind[0] == 99 && model.s.assist_key_bind[3] == 513);
    launcher_model_reset_assist_bindings(&model);
    assert(model.s.assist_key_bind[0] == 99 && model.s.assist_key_bind[3] == 0);
    return 0;
}
