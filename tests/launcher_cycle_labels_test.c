/* Every label a cycle-button Settings row can show.
 *
 * The Settings page sizes a button to the widest value it can show
 * (docs/SETTINGS_STANDARD.md: controls sized to their labels), so it needs the
 * whole value set, not only the current one: launcher_model_each_cycle_label
 * walks the row's own cycle on a scratch copy of the model. What this pins:
 *   - the walk starts at the current value and visits each distinct label once
 *     (stopping when one repeats), so the set is the row's real vocabulary;
 *   - a host-supplied vocabulary (texture filters, languages, layouts) is
 *     walked as declared;
 *   - an off-cycle starting value (a legacy antialiasing 1, "On") is visited
 *     and the walk still ends;
 *   - the model itself is never changed.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;
static void ok(int cond, const char* what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

typedef struct { char labels[LNG_CYCLE_LABELS_MAX][64]; int n; } Seen;
static void collect(const char* label, void* ctx) {
    Seen* s = (Seen*)ctx;
    if (s->n < LNG_CYCLE_LABELS_MAX)
        snprintf(s->labels[s->n++], sizeof s->labels[0], "%s", label);
}
static int has(const Seen* s, const char* label) {
    for (int i = 0; i < s->n; ++i)
        if (strcmp(s->labels[i], label) == 0) return 1;
    return 0;
}
static Seen walk(const LauncherModel* m, void (*cycle)(LauncherModel*),
                 const char* (*label)(const LauncherModel*)) {
    Seen s;
    memset(&s, 0, sizeof s);
    const int n = launcher_model_each_cycle_label(m, cycle, label, collect, &s);
    ok(n == s.n, "the return value counts the labels visited");
    return s;
}

static LauncherModel g_m;

int main(void) {
    LauncherModel* m = &g_m;
    memset(m, 0, sizeof *m);

    /* Antialiasing Off / 2x / 4x / 8x, from 4x: every value once. */
    m->s.antialiasing = 4;
    Seen aa = walk(m, launcher_model_cycle_aa, launcher_model_aa_label);
    ok(aa.n == 4 && has(&aa, "Off") && has(&aa, "2x") && has(&aa, "4x") && has(&aa, "8x"),
       "antialiasing: Off, 2x, 4x, 8x");
    ok(strcmp(aa.labels[0], "4x") == 0, "the walk starts at the current value");
    ok(m->s.antialiasing == 4, "the model is not changed");

    /* A legacy host value outside the cycle (1, "On") is shown, and the walk
     * still ends once the cycle repeats. */
    m->s.antialiasing = 1;
    aa = walk(m, launcher_model_cycle_aa, launcher_model_aa_label);
    ok(aa.n == 5 && has(&aa, "On") && has(&aa, "8x"), "legacy On is visited, the walk ends");
    ok(m->s.antialiasing == 1, "the model is not changed (legacy)");

    /* Host-supplied texture-filter vocabulary. */
    static const char* const tf_labels[] = {"Nearest", "Bilinear", "xBR (pixel art)"};
    static const int tf_values[] = {0, 1, 2};
    m->texture_filter_labels = tf_labels;
    m->texture_filter_values = tf_values;
    m->num_texture_filters = 3;
    m->s.texture_filter = 1;
    Seen tf = walk(m, launcher_model_toggle_texture_filter, launcher_model_texture_filter_label);
    ok(tf.n == 3 && has(&tf, "xBR (pixel art)"), "texture filtering walks the host's catalog");
    ok(m->s.texture_filter == 1, "the model is not changed (texture filter)");

    /* Languages and screen layouts come from the host too. */
    static const char* const langs[] = {"English", "Japanese", "Portuguese (Brazil)"};
    m->language_labels = langs;
    m->num_languages = 3;
    m->s.language_index = 2;
    Seen lang = walk(m, launcher_model_cycle_language, launcher_model_language_label);
    ok(lang.n == 3 && strcmp(lang.labels[0], "Portuguese (Brazil)") == 0,
       "languages: every entry, from the current one");
    static const char* const layouts[] = {"Default", "Side by side (wide)"};
    m->display_layout_labels = layouts;
    m->num_display_layouts = 2;
    Seen lay = walk(m, launcher_model_cycle_display_layout, launcher_model_display_layout_label);
    ok(lay.n == 2 && has(&lay, "Side by side (wide)"), "screen layouts: every entry");

    /* Formatted labels (one static buffer per function) are copied out. */
    m->s.audio_freq = 44100;
    Seen fr = walk(m, launcher_model_cycle_freq, launcher_model_freq_label);
    ok(fr.n >= 2 && has(&fr, "44100 Hz") && has(&fr, "48000 Hz"),
       "sample rates: distinct formatted labels");

    /* Nothing to walk: no label visited. */
    ok(launcher_model_each_cycle_label(m, NULL, launcher_model_aa_label, collect, &aa) == 0,
       "no cycle function: nothing visited");

    if (fails) {
        printf("launcher_cycle_labels_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("launcher_cycle_labels_test: all checks passed\n");
    return 0;
}
