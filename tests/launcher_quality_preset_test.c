/* Graphics preset row (Settings.quality_preset, GameInfo.quality_*).
 *
 * The host owns detection and what each preset sets (quality_apply); the
 * launcher picks presets, re-detects, and turns the state into Custom (5)
 * as soon as the player changes a field the preset set, keeping the preset
 * it started from in quality_base. Hosts without the fields hide the row.
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

static int applied;
static void host_apply(int preset, RecompLauncherCSettings* s) {
    applied = preset;
    s->frame_generation = preset >= 3;
    s->dynamic_resolution_min = preset == 1 ? 1 : 720;
}
static int host_redetect(void) { return 2; }

int main(void) {
    static RecompLauncherCSettings io, out;
    static RecompLauncherCGameInfo gi;
    static LauncherModel lm;
    memset(&io, 0, sizeof(io));
    memset(&gi, 0, sizeof(gi));

    launcher_model_init(&lm, &io, &gi, NULL);
    ok(!launcher_model_quality_offered(&lm), "host without presets => row hidden");

    io.quality_preset = 4;              /* host seeds Ultra in force */
    io.quality_base = 4;
    io.frame_generation = 1;
    io.dynamic_resolution_min = 720;
    gi.quality_offered_mask = 0xF;
    gi.quality_detected = 4;
    gi.quality_summary = "Apple M4";
    gi.quality_apply = host_apply;
    gi.quality_redetect = host_redetect;
    launcher_model_init(&lm, &io, &gi, NULL);
    ok(launcher_model_quality_offered(&lm), "presets offered");
    ok(lm.s.quality_preset == 4, "seeded preset reaches the model");

    launcher_model_quality_track(&lm);   /* first frame: snapshot */
    launcher_model_quality_track(&lm);
    ok(lm.s.quality_preset == 4, "no change => preset stays");

    launcher_model_quality_select(&lm, 1);
    ok(applied == 1 && lm.s.quality_preset == 1 && lm.s.frame_generation == 0 &&
       lm.s.dynamic_resolution_min == 1, "selecting Low applies the host's values");
    launcher_model_quality_track(&lm);
    ok(lm.s.quality_preset == 1, "preset values themselves are not a customisation");

    lm.s.frame_generation = 1;          /* player turns Smooth motion back on */
    launcher_model_quality_track(&lm);
    ok(lm.s.quality_preset == 5 && lm.s.quality_base == 1,
       "changing a governed row => Custom, based on Low");
    lm.s.frame_generation = 0;
    launcher_model_quality_track(&lm);
    ok(lm.s.quality_preset == 5, "Custom stays Custom until a preset is picked");

    lm.s.window_width = 1920;           /* not a preset field */
    launcher_model_quality_select(&lm, 3);
    lm.s.window_width = 2560;
    launcher_model_quality_track(&lm);
    ok(lm.s.quality_preset == 3, "rows a preset does not govern never make it Custom");

    launcher_model_quality_redetect(&lm);
    ok(lm.s.quality_preset == 2 && lm.quality_detected == 2 && applied == 2,
       "Re-detect applies the detected preset");

    gi.quality_offered_mask = 0x9;      /* Low + Ultra only */
    launcher_model_init(&lm, &io, &gi, NULL);
    applied = 0;
    launcher_model_quality_select(&lm, 2);
    ok(applied == 0 && lm.s.quality_preset == 4, "a preset the host does not offer is ignored");

    memset(&out, 0, sizeof(out));
    launcher_model_quality_select(&lm, 1);
    launcher_model_commit(&lm, &out);
    ok(out.quality_preset == 1 && out.quality_base == 1, "the pick commits to the host");

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
