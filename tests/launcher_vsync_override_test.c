/* The VSync row while a mod forces driver vsync off.
 *
 * A host's frame-interpolation or high-rate pacing mod presents with vsync
 * off whatever the Display card says, so the row must not offer a choice that
 * would not apply (psxrecomp beads-eio.3.398). The mod provider reports such a
 * feature through RecompLauncherCModFeature.forces_vsync_off;
 * launcher_model_vsync_forced_off_by names the enabled one, which the row
 * shows as its reason. What this pins:
 *   - no provider, or no such feature enabled: NULL (the row is the player's);
 *   - an enabled forcing feature: its name, else its id;
 *   - a disabled forcing feature does not count;
 *   - the player's saved Settings.vsync is never rewritten.
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

typedef struct { const char* id; const char* name; int enabled; int forces; } FakeFeature;
static FakeFeature g_features[3];
static int g_feature_count;

static int fake_count(void* ctx) { (void)ctx; return g_feature_count; }
static int fake_get(void* ctx, int index, RecompLauncherCModFeature* out) {
    (void)ctx;
    if (index < 0 || index >= g_feature_count) return 0;
    snprintf(out->id, sizeof out->id, "%s", g_features[index].id);
    snprintf(out->name, sizeof out->name, "%s", g_features[index].name);
    out->enabled = g_features[index].enabled;
    out->forces_vsync_off = g_features[index].forces;
    return 1;
}

int main(void) {
    static LauncherModel m;
    static RecompLauncherCModProvider provider;
    memset(&m, 0, sizeof m);
    m.has_vsync = true;
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_VRR);
    ok(m.s.vsync != RECOMP_LAUNCHER_VSYNC_VRR, "older hosts do not offer VRR");
    m.has_vsync_vrr = true;
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_VRR);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_VRR && !strcmp(launcher_model_vsync_label(&m), "VRR"),
       "VRR has a distinct selectable value and label");
    launcher_model_cycle_vsync(&m);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_ON, "VRR cycles back to On");

    m.s.vsync = RECOMP_LAUNCHER_VSYNC_ON;

    ok(launcher_model_vsync_forced_off_by(&m) == NULL, "no mod provider: the row is the player's");
    ok(launcher_model_vsync_forced_off_by(NULL) == NULL, "NULL model is safe");

    memset(&provider, 0, sizeof provider);
    provider.feature_count = fake_count;
    provider.feature_get = fake_get;
    m.mods = &provider;
    g_features[0] = (FakeFeature){"crt-filter", "CRT Filter", 1, 0};
    g_features[1] = (FakeFeature){"smooth-motion", "Smooth Motion", 0, 1};
    g_feature_count = 2;
    ok(launcher_model_vsync_forced_off_by(&m) == NULL,
       "a forcing feature that is disabled does not override the row");

    g_features[1].enabled = 1;
    const char* by = launcher_model_vsync_forced_off_by(&m);
    ok(by && strcmp(by, "Smooth Motion") == 0, "an enabled forcing feature is named");
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_ON, "the saved VSync choice is left alone");

    g_features[1].name = "";
    by = launcher_model_vsync_forced_off_by(&m);
    ok(by && strcmp(by, "smooth-motion") == 0, "a nameless feature is named by its id");

    provider.feature_get = NULL;
    ok(launcher_model_vsync_forced_off_by(&m) == NULL,
       "a provider without the feature surface overrides nothing");

    printf("%s\n", fails ? "FAILED" : "all vsync-override checks passed");
    return fails ? 1 : 0;
}
