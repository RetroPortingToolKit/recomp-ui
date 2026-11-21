/* The read-only view API, open_section, and NAVIGATION_SKIP_DISABLED. */
#include "recomp_runtime_ui.h"

#include <assert.h>
#include <string.h>

typedef struct {
    int widescreen, hud, fps, aspect;
    int first_disabled;   /* makes the first row report disabled */
    int back_count;
} State;

static const char *const kAspects[] = { "Fit", "16:9", "21:9" };

static const RecompRuntimeUiItem kItems[] = {
    { "widescreen", "Options", "Widescreen", NULL,
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "hud", "Options", "HUD anchoring", NULL,
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "fps", "Options", "Presentation FPS", NULL,
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "aspect", "Options", "Aspect ratio", NULL,
      RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kAspects, 3, NULL },
    { "back", "Options", "Back", NULL,
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    { "other", "Other", "Other", NULL,
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
};

static int *field(State *s, const RecompRuntimeUiItem *item) {
    if (!strcmp(item->key, "widescreen")) return &s->widescreen;
    if (!strcmp(item->key, "hud")) return &s->hud;
    if (!strcmp(item->key, "fps")) return &s->fps;
    if (!strcmp(item->key, "aspect")) return &s->aspect;
    return NULL;
}

static int get_value(void *ctx, const RecompRuntimeUiItem *item, int *out) {
    int *f = field((State *)ctx, item);
    if (!f) return 0;
    *out = *f;
    return 1;
}

static int set_value(void *ctx, const RecompRuntimeUiItem *item, int value) {
    int *f = field((State *)ctx, item);
    if (!f) return 0;
    *f = value;
    return 1;
}

static int run_action(void *ctx, const RecompRuntimeUiItem *item) {
    (void)item;
    ++((State *)ctx)->back_count;
    return 1;
}

static int is_enabled(void *ctx, const RecompRuntimeUiItem *item) {
    State *s = (State *)ctx;
    if (!strcmp(item->key, "hud")) return s->widescreen;
    if (!strcmp(item->key, "widescreen")) return !s->first_disabled;
    return 1;
}

static RecompRuntimeUi *make(State *s, uint32_t navigation_flags) {
    RecompRuntimeUiConfig config;
    memset(&config, 0, sizeof(config));
    config.title = "View";
    config.items = kItems;
    config.item_count = sizeof(kItems) / sizeof(kItems[0]);
    config.callbacks.context = s;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    config.callbacks.run_action = run_action;
    config.callbacks.is_enabled = is_enabled;
    config.navigation_flags = navigation_flags;
    return recomp_runtime_ui_create(&config);
}

static void press(RecompRuntimeUi *ui, RecompRuntimeUiInput input) {
    assert(recomp_runtime_ui_handle_input(ui, input, 1, 0));
}

static size_t selected(const RecompRuntimeUi *ui) {
    return recomp_runtime_ui_view_selected_row(ui);
}

int main(void) {
    /* Default navigation is unchanged: a disabled row still takes the
     * selection. */
    {
        State s = { 0 };
        RecompRuntimeUi *ui = make(&s, 0);
        assert(ui);
        assert(!recomp_runtime_ui_open_section(ui, "Missing"));
        assert(!recomp_runtime_ui_is_open(ui));
        assert(recomp_runtime_ui_open_section(ui, "Options"));
        assert(recomp_runtime_ui_is_open(ui));
        assert(selected(ui) == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);
        assert(selected(ui) == 1);
        recomp_runtime_ui_destroy(ui);
    }

    /* SKIP_DISABLED passes over the disabled HUD row in both directions and
     * wraps; enabling widescreen makes it selectable again. */
    {
        State s = { 0 };
        RecompRuntimeUi *ui = make(&s, RECOMP_RUNTIME_UI_NAVIGATION_SKIP_DISABLED);
        assert(ui && recomp_runtime_ui_open_section(ui, "Options"));
        assert(selected(ui) == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);
        assert(selected(ui) == 2);
        press(ui, RECOMP_RUNTIME_UI_INPUT_UP);
        assert(selected(ui) == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_UP);
        assert(selected(ui) == 4);
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);
        assert(selected(ui) == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT);
        assert(s.widescreen == 1);
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);
        assert(selected(ui) == 1);

        /* The view reports what the built-in presentations show. */
        RecompRuntimeUiRowView row;
        assert(recomp_runtime_ui_view_row_count(ui) == 5);
        assert(recomp_runtime_ui_view_row(ui, 0, &row));
        assert(row.item == &kItems[0] && row.enabled);
        assert(!strcmp(row.label, "Widescreen") && !strcmp(row.value, "On"));
        assert(recomp_runtime_ui_view_row(ui, 2, &row));
        assert(!strcmp(row.value, "Off"));
        s.aspect = 1;
        assert(recomp_runtime_ui_view_row(ui, 3, &row));
        assert(!strcmp(row.value, "16:9"));
        assert(recomp_runtime_ui_view_row(ui, 4, &row));
        assert(row.item->type == RECOMP_RUNTIME_UI_ACTION && !strcmp(row.value, ">"));
        assert(!recomp_runtime_ui_view_row(ui, 5, &row));
        s.widescreen = 0;
        assert(recomp_runtime_ui_view_row(ui, 1, &row));
        assert(!row.enabled);

        /* Left/right step a choice; accept runs an action. */
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);   /* 1 -> 2 (fps) */
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);   /* -> 3 (aspect) */
        assert(selected(ui) == 3);
        press(ui, RECOMP_RUNTIME_UI_INPUT_LEFT);
        assert(s.aspect == 0);
        press(ui, RECOMP_RUNTIME_UI_INPUT_LEFT);
        assert(s.aspect == 2);
        press(ui, RECOMP_RUNTIME_UI_INPUT_DOWN);
        press(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT);
        assert(s.back_count == 1);

        /* Back at the page root lists the sections. */
        press(ui, RECOMP_RUNTIME_UI_INPUT_BACK);
        assert(recomp_runtime_ui_is_open(ui));
        assert(recomp_runtime_ui_view_row_count(ui) == 2);
        assert(recomp_runtime_ui_view_row(ui, 1, &row));
        assert(row.item == NULL && !strcmp(row.label, "Other") && !row.value[0]);
        recomp_runtime_ui_destroy(ui);
    }

    /* Entering a page whose first row is disabled starts on the next one. */
    {
        State s = { 0 };
        s.first_disabled = 1;
        RecompRuntimeUi *ui = make(&s, RECOMP_RUNTIME_UI_NAVIGATION_SKIP_DISABLED);
        assert(ui && recomp_runtime_ui_open_section(ui, "Options"));
        assert(selected(ui) == 2);
        recomp_runtime_ui_destroy(ui);
    }
    return 0;
}
