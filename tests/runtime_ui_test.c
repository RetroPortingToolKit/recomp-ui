#include "recomp_runtime_ui.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct TestState {
    int enabled;
    int level;
    int action_count;
    int launcher_count;
    int quit_count;
    int save_count;
    int visible;
    int dim;
    int opacity;
    int pause;
    int slot;
} TestState;

static int get_value(void *context, const RecompRuntimeUiItem *item, int *out) {
    TestState *state = (TestState *)context;
    if (!strcmp(item->key, "enabled")) *out = state->enabled;
    else if (!strcmp(item->key, "level")) *out = state->level;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VIEW_MODE)) *out = state->level;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_MENU_DIM)) *out = state->dim;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_MENU_OPACITY)) *out = state->opacity;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) *out = state->pause;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_STATE_SLOT)) *out = state->slot;
    else return 0;
    return 1;
}

static int set_value(void *context, const RecompRuntimeUiItem *item, int value) {
    TestState *state = (TestState *)context;
    if (!strcmp(item->key, "enabled")) state->enabled = value;
    else if (!strcmp(item->key, "level")) state->level = value;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VIEW_MODE)) state->level = value;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_MENU_DIM)) state->dim = value;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_MENU_OPACITY)) state->opacity = value;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) state->pause = value;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_STATE_SLOT)) state->slot = value;
    else return 0;
    return 1;
}

static void press(RecompRuntimeUi *ui, RecompRuntimeUiInput input) {
    assert(recomp_runtime_ui_handle_input(ui, input, 1, 0));
}

static int run_action(void *context, const RecompRuntimeUiItem *item) {
    TestState *state = (TestState *)context;
    if (!strcmp(item->key, "reset") || !strcmp(item->key, RECOMP_RUNTIME_UI_KEY_RESET))
        ++state->action_count;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_RETURN_TO_LAUNCHER))
        ++state->launcher_count;
    else if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_QUIT)) ++state->quit_count;
    else return 0;
    return 1;
}

static void save(void *context) { ++((TestState *)context)->save_count; }
static void visible(void *context, int open) {
    ((TestState *)context)->visible = open;
}

int main(void) {
    static const RecompRuntimeUiItem items[] = {
        { "enabled", "Video", "Enabled", "Toggle the feature.",
          RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
        { "level", "Video", "Level", "Adjust the level.",
          RECOMP_RUNTIME_UI_INT, 0, 10, 2, NULL, 0, NULL },
        { "reset", "System", "Reset", "Run the action.",
          RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    };
    TestState state = { 0 };
    RecompRuntimeUiConfig config = {0};
    config.title = "Runtime UI";
    config.subtitle = "TEST";
    config.items = items;
    config.item_count = sizeof(items) / sizeof(items[0]);
    config.callbacks = (RecompRuntimeUiCallbacks){
        &state, get_value, set_value, run_action, NULL, save, visible
    };
    config.theme = "n64";
    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    assert(ui != NULL);
    assert(!recomp_runtime_ui_is_open(ui));

    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_TOGGLE, 1, 0));
    assert(recomp_runtime_ui_is_open(ui) && state.visible);
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0));
    assert(state.enabled == 1 && state.save_count == 1);
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_RIGHT, 1, 0));
    assert(state.level == 2 && state.save_count == 2);

    uint32_t frame[256 * 224];
    memset(frame, 0x7f, sizeof(frame));
    uint32_t before = frame[0];
    recomp_runtime_ui_render_argb8888(ui, frame, 256, 224, 256 * 4);
    assert(frame[0] != before);
    assert(frame[112 * 256 + 128] != 0x7f7f7f7fU);

    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_BACK, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0));
    /* Actions own any persistence they need; value persistence is not rerun. */
    assert(state.action_count == 1 && state.save_count == 2);
    recomp_runtime_ui_close(ui);
    assert(!state.visible);
    recomp_runtime_ui_destroy(ui);

    /* Sparse universal view modes retain their semantic values (0 -> 2). */
    state.level = RECOMP_RUNTIME_UI_VIEW_NATIVE;
    RecompRuntimeUiStandardConfig standard = {0};
    standard.menu.title = "Standard settings";
    standard.menu.theme = "gba";
    standard.menu.callbacks = config.callbacks;
    standard.features = RECOMP_RUNTIME_UI_STANDARD_VIEW_MODE;
    standard.view_modes = RECOMP_RUNTIME_UI_VIEW_MODE_NATIVE |
                          RECOMP_RUNTIME_UI_VIEW_MODE_ADAPTIVE;
    RecompRuntimeUi *standard_ui = recomp_runtime_ui_create_standard(&standard);
    assert(standard_ui != NULL);
    recomp_runtime_ui_open(standard_ui);
    assert(recomp_runtime_ui_handle_input(
        standard_ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0));
    assert(recomp_runtime_ui_handle_input(
        standard_ui, RECOMP_RUNTIME_UI_INPUT_RIGHT, 1, 0));
    assert(state.level == RECOMP_RUNTIME_UI_VIEW_ADAPTIVE);
    recomp_runtime_ui_destroy(standard_ui);

    /* recomp_runtime_ui_confirm: a second press on the same row while its
     * prompt is up. Another row, another status, the prompt running out, or
     * closing the menu all withdraw it. */
    {
        RecompRuntimeUi *c = recomp_runtime_ui_create(&config);
        const RecompRuntimeUiItem *reset = &items[2], *level = &items[1];
        static uint32_t pic[256 * 224];
        recomp_runtime_ui_open(c);
        assert(!recomp_runtime_ui_confirm(c, reset, "Press again to reset"));
        assert(recomp_runtime_ui_confirm(c, reset, "Press again to reset"));
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));   /* starts over */
        assert(!recomp_runtime_ui_confirm(c, level, NULL));   /* another row */
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));
        recomp_runtime_ui_set_status(c, "Slot 2 is empty");
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));   /* replaced */
        for (int i = 0; i < 179; ++i)
            recomp_runtime_ui_render_argb8888(c, pic, 256, 224, 256 * 4);
        assert(recomp_runtime_ui_confirm(c, reset, NULL));    /* last frame */
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));
        for (int i = 0; i < 180; ++i)
            recomp_runtime_ui_render_argb8888(c, pic, 256, 224, 256 * 4);
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));   /* ran out */
        recomp_runtime_ui_close(c);
        recomp_runtime_ui_open(c);
        assert(!recomp_runtime_ui_confirm(c, reset, NULL));   /* closed */
        assert(recomp_runtime_ui_confirm(c, reset, NULL));
        recomp_runtime_ui_destroy(c);
    }

    /* The standard exits ask for the second press themselves: run_action only
     * hears the confirmed one, and a held button never confirms. Reset keeps
     * its single press. */
    {
        RecompRuntimeUiStandardConfig exits = {0};
        exits.menu.title = "Exits";
        exits.menu.callbacks = config.callbacks;
        exits.features = RECOMP_RUNTIME_UI_STANDARD_RESET |
                         RECOMP_RUNTIME_UI_STANDARD_RETURN_TO_LAUNCHER |
                         RECOMP_RUNTIME_UI_STANDARD_QUIT;
        RecompRuntimeUi *e = recomp_runtime_ui_create_standard(&exits);
        assert(e != NULL);
        state.action_count = 0;
        recomp_runtime_ui_open(e);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        assert(state.action_count == 1);                       /* Reset */
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 1);
        assert(state.launcher_count == 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        assert(state.launcher_count == 1);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_UP, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        assert(state.quit_count == 0 && state.launcher_count == 1);
        recomp_runtime_ui_handle_input(e, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        assert(state.quit_count == 1 && state.launcher_count == 1);
        recomp_runtime_ui_destroy(e);
    }

    /* The standard backdrop, state slot and pause rows read and write the
     * host's values within the host's slot count. The backdrop is two rows
     * from one bit, and extras still follow them. */
    {
        static const RecompRuntimeUiItem extra[] = {
            { "level", "Extra", "Level", "After the standard rows.",
              RECOMP_RUNTIME_UI_INT, 0, 10, 2, NULL, 0, NULL },
        };
        RecompRuntimeUiStandardConfig rows = {0};
        rows.menu.title = "Menu settings";
        rows.menu.callbacks = config.callbacks;
        rows.features = RECOMP_RUNTIME_UI_STANDARD_MENU_BACKDROP |
                        RECOMP_RUNTIME_UI_STANDARD_STATE_SLOT |
                        RECOMP_RUNTIME_UI_STANDARD_PAUSE_IN_MENU;
        rows.state_slot_count = 3;
        rows.extra_items = extra;
        rows.extra_item_count = 1;
        RecompRuntimeUi *m = recomp_runtime_ui_create_standard(&rows);
        assert(m != NULL);
        state.dim = RECOMP_RUNTIME_UI_DEFAULT_DIM_PERCENT;
        state.opacity = 100;
        state.pause = 1;
        state.slot = 1;
        state.level = 0;
        recomp_runtime_ui_open(m);
        press(m, RECOMP_RUNTIME_UI_INPUT_ACCEPT);                /* Display */
        press(m, RECOMP_RUNTIME_UI_INPUT_RIGHT);
        assert(state.dim == 70);
        press(m, RECOMP_RUNTIME_UI_INPUT_DOWN);
        press(m, RECOMP_RUNTIME_UI_INPUT_RIGHT);
        assert(state.opacity == 100);                            /* top */
        press(m, RECOMP_RUNTIME_UI_INPUT_LEFT);
        assert(state.opacity == 90);
        press(m, RECOMP_RUNTIME_UI_INPUT_BACK);
        press(m, RECOMP_RUNTIME_UI_INPUT_DOWN);
        press(m, RECOMP_RUNTIME_UI_INPUT_ACCEPT);                /* System */
        for (int i = 0; i < 4; ++i) press(m, RECOMP_RUNTIME_UI_INPUT_RIGHT);
        assert(state.slot == 3);
        press(m, RECOMP_RUNTIME_UI_INPUT_DOWN);
        press(m, RECOMP_RUNTIME_UI_INPUT_ACCEPT);
        assert(state.pause == 0);
        press(m, RECOMP_RUNTIME_UI_INPUT_BACK);
        press(m, RECOMP_RUNTIME_UI_INPUT_DOWN);
        press(m, RECOMP_RUNTIME_UI_INPUT_ACCEPT);                /* Extra */
        press(m, RECOMP_RUNTIME_UI_INPUT_RIGHT);
        assert(state.level == 2);
        recomp_runtime_ui_destroy(m);
    }

    /* Toasts: drawn with the menu closed, only in the frame's top rows, and
     * nothing at all once cleared. The model keeps no clock of its own. */
    {
        RecompRuntimeUi *t = recomp_runtime_ui_create(&config);
        static uint32_t pic[256 * 240], ref[256 * 240];
        for (int i = 0; i < 256 * 240; ++i) pic[i] = ref[i] = 0xff102030u + (uint32_t)(i % 7);
        assert(!recomp_runtime_ui_toast_visible(t));
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        assert(!memcmp(pic, ref, sizeof(pic)));             /* closed, no toast: untouched */
        recomp_runtime_ui_set_toast(t, "DISK 1 SIDE A", "MOTOR OFF\nD AGAIN: DISK 1 SIDE B");
        assert(recomp_runtime_ui_toast_visible(t) && !recomp_runtime_ui_is_open(t));
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        int changed = 0, lowest = 0;
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 256; ++x)
                if (pic[y * 256 + x] != ref[y * 256 + x]) { ++changed; lowest = y; }
        assert(changed > 0 && lowest < 48);                  /* three lines, near the top */
        /* the widest line decides the panel: a wider body widens it */
        memcpy(pic, ref, sizeof(pic));
        recomp_runtime_ui_set_toast(t, "A", "B");
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        int narrow = 0;
        for (int x = 0; x < 256; ++x) narrow += pic[10 * 256 + x] != ref[10 * 256 + x];
        assert(narrow > 0 && narrow < changed);
        /* NULLs clear it; an overlong text is clipped, never overrun */
        recomp_runtime_ui_set_toast(t, NULL, NULL);
        assert(!recomp_runtime_ui_toast_visible(t));
        memcpy(pic, ref, sizeof(pic));
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        assert(!memcmp(pic, ref, sizeof(pic)));
        char big[2000];
        memset(big, 'W', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        recomp_runtime_ui_set_toast(t, big, big);
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        /* over an open menu it still draws */
        recomp_runtime_ui_set_toast(t, "DISK 1 SIDE B", NULL);
        recomp_runtime_ui_open(t);
        memcpy(pic, ref, sizeof(pic));
        recomp_runtime_ui_render_argb8888(t, pic, 256, 240, 256 * 4);
        assert(memcmp(pic, ref, sizeof(pic)));
        recomp_runtime_ui_destroy(t);
    }
    puts("runtime UI tests passed");
    return 0;
}
