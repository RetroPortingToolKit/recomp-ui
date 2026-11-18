/* The in-game runtime UI's window scale row is windowed-only: inert in
 * borderless and exclusive fullscreen, live again in windowed mode. */
#include "recomp_runtime_ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { int fullscreen, scale; } State;

static int get_value(void *ctx, const RecompRuntimeUiItem *item, int *out) {
    State *s = (State *)ctx;
    if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) { *out = s->fullscreen; return 1; }
    if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) { *out = s->scale; return 1; }
    return 0;
}

static int set_value(void *ctx, const RecompRuntimeUiItem *item, int value) {
    State *s = (State *)ctx;
    if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) { s->fullscreen = value; return 1; }
    if (!strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) { s->scale = value; return 1; }
    return 0;
}

static int scale_row_enabled(RecompRuntimeUi *ui) {
    RecompRuntimeUiRowView v;
    for (size_t i = 0; recomp_runtime_ui_view_row(ui, i, &v); ++i)
        if (v.item && !strcmp(v.item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE))
            return v.enabled;
    return -1;
}

static void press(RecompRuntimeUi *ui, RecompRuntimeUiInput in) {
    recomp_runtime_ui_handle_input(ui, in, 1, 0);
}

int main(void) {
    State s = { 0, 2 };
    RecompRuntimeUiStandardConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.menu.title = "Settings";
    cfg.menu.callbacks.context = &s;
    cfg.menu.callbacks.get_value = get_value;
    cfg.menu.callbacks.set_value = set_value;
    cfg.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN | RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE;
    cfg.window_scale_max = 4;
    RecompRuntimeUi *ui = recomp_runtime_ui_create_standard(&cfg);
    assert(ui);
    recomp_runtime_ui_open(ui);
    assert(recomp_runtime_ui_open_section(ui, "Display"));

    assert(scale_row_enabled(ui) == 1);
    press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);          /* onto the scale row */
    press(ui, RECOMP_RUNTIME_UI_INPUT_RIGHT);
    assert(s.scale == 3);

    for (int mode = 1; mode <= 2; ++mode) {           /* borderless, exclusive */
        s.fullscreen = mode;
        assert(scale_row_enabled(ui) == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_RIGHT);
        press(ui, RECOMP_RUNTIME_UI_INPUT_LEFT);
        assert(s.scale == 3);
    }

    s.fullscreen = 0;
    assert(scale_row_enabled(ui) == 1);
    press(ui, RECOMP_RUNTIME_UI_INPUT_RIGHT);
    assert(s.scale == 4);

    recomp_runtime_ui_destroy(ui);
    puts("PASSED");
    return 0;
}
