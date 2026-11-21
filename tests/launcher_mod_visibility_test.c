/* Hidden mod features. By default a hidden feature is listed only while it is
 * enabled (so it can be turned off) and every package is listed. A host that
 * sets hide_hidden_features never presents a hidden feature, enabled or not,
 * and omits a package whose every feature is hidden. */
#include "launcher_mod_visibility.h"

#include <stdio.h>
#include <string.h>

typedef struct FakeFeature {
    const char* package_id;
    const char* id;
    int hidden;
    int enabled;
} FakeFeature;

static const FakeFeature kFeatures[] = {
    /* A package that exists only to carry a hidden default-on feature
     * (WipEout 3's NTSC Mode). */
    {"game.ntsc", "ntsc", 1, 1},
    /* A package mixing a visible feature with a hidden one. */
    {"game.mixed", "visible", 0, 1},
    {"game.mixed", "secret", 1, 0},
    /* An ordinary package. */
    {"game.widescreen", "widescreen", 0, 0},
    /* Hidden and off. */
    {"game.off", "off", 1, 0},
};
#define FEATURE_N ((int)(sizeof(kFeatures) / sizeof(kFeatures[0])))

static int fake_feature_count(void* ctx) {
    (void)ctx;
    return FEATURE_N;
}

static int fake_feature_get(void* ctx, int index, RecompLauncherCModFeature* out) {
    (void)ctx;
    if (index < 0 || index >= FEATURE_N || !out) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->package_id, sizeof(out->package_id), "%s",
             kFeatures[index].package_id);
    snprintf(out->id, sizeof(out->id), "%s", kFeatures[index].id);
    out->hidden = kFeatures[index].hidden;
    out->enabled = kFeatures[index].enabled;
    return 1;
}

static int failures = 0;

static void expect(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static RecompLauncherCModFeature feature_at(int index) {
    RecompLauncherCModFeature feature;
    fake_feature_get(NULL, index, &feature);
    return feature;
}

int main(void) {
    RecompLauncherCModProvider mods;
    memset(&mods, 0, sizeof(mods));
    mods.feature_count = fake_feature_count;
    mods.feature_get = fake_feature_get;
    RecompLauncherCModFeature f;

    /* Default: unchanged behaviour for every title that does not opt in. */
    f = feature_at(0);
    expect(launcher_mod_feature_listed(&mods, &f), "default: hidden enabled is listed");
    expect(!launcher_mod_feature_concealed(&mods, &f), "default: nothing is concealed");
    f = feature_at(2);
    expect(!launcher_mod_feature_listed(&mods, &f), "default: hidden disabled is not listed");
    f = feature_at(3);
    expect(launcher_mod_feature_listed(&mods, &f), "default: visible disabled is listed");
    expect(launcher_mod_listed_feature_count(&mods) == 3, "default: three listed");
    expect(launcher_mod_package_listed(&mods, "game.ntsc"), "default: every package listed");
    expect(launcher_mod_package_listed(&mods, "game.off"), "default: every package listed");

    /* Opt-in: hidden features are never presented. */
    mods.hide_hidden_features = 1;
    f = feature_at(0);
    expect(!launcher_mod_feature_listed(&mods, &f), "opt-in: hidden enabled is not listed");
    expect(launcher_mod_feature_concealed(&mods, &f), "opt-in: hidden enabled is concealed");
    f = feature_at(2);
    expect(!launcher_mod_feature_listed(&mods, &f), "opt-in: hidden disabled is not listed");
    expect(launcher_mod_feature_concealed(&mods, &f), "opt-in: hidden disabled is concealed");
    f = feature_at(1);
    expect(launcher_mod_feature_listed(&mods, &f), "opt-in: visible enabled is listed");
    expect(!launcher_mod_feature_concealed(&mods, &f), "opt-in: visible is not concealed");
    f = feature_at(3);
    expect(launcher_mod_feature_listed(&mods, &f), "opt-in: visible disabled is listed");
    expect(!launcher_mod_feature_listed(&mods, NULL), "no feature is not listed");
    expect(launcher_mod_listed_feature_count(&mods) == 2, "opt-in: two listed");
    expect(!launcher_mod_package_listed(&mods, "game.ntsc"),
           "opt-in: package carrying only a hidden feature is not listed");
    expect(!launcher_mod_package_listed(&mods, "game.off"),
           "opt-in: package carrying only a hidden disabled feature is not listed");
    expect(launcher_mod_package_listed(&mods, "game.mixed"),
           "opt-in: package with a visible feature is listed");
    expect(launcher_mod_package_listed(&mods, "game.widescreen"), "opt-in: ordinary package");
    expect(launcher_mod_package_listed(&mods, "game.featureless"),
           "opt-in: package with no features is listed");

    /* A provider without the feature surface has no hidden flag. */
    RecompLauncherCModProvider legacy;
    memset(&legacy, 0, sizeof(legacy));
    legacy.hide_hidden_features = 1;
    expect(launcher_mod_package_listed(&legacy, "game.ntsc"),
           "legacy provider lists every package");
    expect(launcher_mod_listed_feature_count(&legacy) == 0,
           "legacy provider has no listed features");

    if (failures) return 1;
    printf("launcher_mod_visibility_test: ok\n");
    return 0;
}
