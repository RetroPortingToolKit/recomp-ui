/* Internal resolution as a host-supplied LIST.
 *
 * A host (PSX) can replace the 1x..4x Supersampling cycle with its own
 * vocabulary -- Native / 720p / 1080p / 1440p / 4K / 5K / 8K / Match display /
 * Auto (-2, match display with dynamic resolution) -- stored as a value in
 * Settings.internal_resolution. The risks are the usual
 * ones for a borrowed vocabulary: a stale or unset value must not become a
 * selection the list cannot show, set must store the VALUE (not the index),
 * and a host that supplies nothing must keep exactly the legacy cycle.
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

int main(void) {
    static LauncherModel m;
    static const char* const kLabels[] = {
        "Native", "720p", "1080p", "1440p", "4K", "5K", "8K", "Match display",
        "Auto", "2x (480 lines)",
    };
    static const int kValues[] = { 1, 720, 1080, 1440, 2160, 2880, 4320, -1, -2, 480 };

    /* ---- no vocabulary: legacy cycle untouched -------------------------- */
    memset(&m, 0, sizeof(m));
    m.has_supersampling = true;
    ok(!launcher_model_internal_resolution_offered(&m), "no vocabulary => not offered");
    ok(launcher_model_internal_resolution_count(&m) == 0, "count 0");
    m.s.supersampling = 2;
    launcher_model_apply_internal_resolution_settings(&m);
    ok(m.s.internal_resolution == 0, "apply leaves an unset value alone without a vocabulary");
    launcher_model_cycle_supersampling(&m);
    ok(m.s.supersampling == 3 && strcmp(launcher_model_supersampling_label(&m), "3x") == 0,
       "legacy 1x..4x cycle unchanged");
    launcher_model_set_internal_resolution(&m, 2);
    ok(m.s.internal_resolution == 0, "set is a no-op without a vocabulary");

    /* ---- vocabulary without the capability flag: not offered ----------- */
    memset(&m, 0, sizeof(m));
    m.internal_resolution_labels = kLabels;
    m.internal_resolution_values = kValues;
    m.num_internal_resolutions = 10;
    ok(!launcher_model_internal_resolution_offered(&m),
       "a console without has_supersampling never draws the row");

    /* ---- host vocabulary --------------------------------------------- */
    memset(&m, 0, sizeof(m));
    m.has_supersampling = true;
    m.internal_resolution_labels = kLabels;
    m.internal_resolution_values = kValues;
    m.num_internal_resolutions = 10;
    m.internal_resolution_note = "8K = 16x on this GPU";
    ok(launcher_model_internal_resolution_offered(&m), "offered");
    ok(launcher_model_internal_resolution_count(&m) == 10, "count follows the host");
    ok(strcmp(launcher_model_internal_resolution_label_at(&m, 7), "Match display") == 0,
       "label_at speaks the host vocabulary");
    ok(strcmp(launcher_model_internal_resolution_label_at(&m, 99), "") == 0,
       "out-of-range label is empty, not garbage");

    /* unset seeds the first entry */
    m.s.internal_resolution = 0;
    launcher_model_apply_internal_resolution_settings(&m);
    ok(m.s.internal_resolution == 1 && launcher_model_internal_resolution_index(&m) == 0,
       "unset seeds the first entry (Native)");
    /* a stale value the host no longer lists also falls back */
    m.s.internal_resolution = 1234;
    launcher_model_apply_internal_resolution_settings(&m);
    ok(m.s.internal_resolution == 1, "unknown value falls back to the first entry");
    /* a listed value is kept */
    m.s.internal_resolution = 480;
    launcher_model_apply_internal_resolution_settings(&m);
    ok(m.s.internal_resolution == 480 &&
       strcmp(launcher_model_internal_resolution_label(&m), "2x (480 lines)") == 0,
       "a host-synthesized legacy entry round-trips");

    /* set stores the VALUE */
    launcher_model_set_internal_resolution(&m, 4);
    ok(m.s.internal_resolution == 2160, "set 4K stores 2160 lines");
    ok(strcmp(launcher_model_internal_resolution_label(&m), "4K") == 0, "and the label agrees");
    launcher_model_set_internal_resolution(&m, 7);
    ok(m.s.internal_resolution == -1, "Match display stores -1");
    launcher_model_set_internal_resolution(&m, 42);
    ok(m.s.internal_resolution == -1, "out-of-range index is refused");
    launcher_model_set_internal_resolution(&m, -3);
    ok(m.s.internal_resolution == -1, "negative index is refused");
    ok(strcmp(launcher_model_internal_resolution_note(&m), "8K = 16x on this GPU") == 0,
       "host note is passed through verbatim");

    /* Auto is one more entry of the host's list (there is no separate
     * Dynamic resolution row): it stores -2 and is kept across a reseed. */
    launcher_model_set_internal_resolution(&m, 8);
    ok(m.s.internal_resolution == -2 &&
       strcmp(launcher_model_internal_resolution_label(&m), "Auto") == 0,
       "Auto stores -2");
    launcher_model_apply_internal_resolution_settings(&m);
    ok(m.s.internal_resolution == -2, "a host-listed Auto survives the seed check");

    printf("%s\n", fails ? "FAILED" : "all internal-resolution checks passed");
    return fails ? 1 : 0;
}
