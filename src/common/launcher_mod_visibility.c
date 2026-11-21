#include "launcher_mod_visibility.h"

#include <string.h>

int launcher_mod_feature_concealed(const RecompLauncherCModProvider* mods,
                                   const RecompLauncherCModFeature* feature) {
    return feature && feature->hidden && mods && mods->hide_hidden_features;
}

int launcher_mod_feature_listed(const RecompLauncherCModProvider* mods,
                                const RecompLauncherCModFeature* feature) {
    if (!feature) return 0;
    if (!feature->hidden) return 1;
    if (launcher_mod_feature_concealed(mods, feature)) return 0;
    return feature->enabled != 0;   /* default: shown while enabled */
}

static int has_feature_surface(const RecompLauncherCModProvider* mods) {
    return mods && mods->feature_count && mods->feature_get;
}

int launcher_mod_package_listed(const RecompLauncherCModProvider* mods,
                                const char* package_id) {
    if (!has_feature_surface(mods) || !package_id || !mods->hide_hidden_features)
        return 1;
    const int count = mods->feature_count(mods->ctx);
    int owned = 0;
    for (int index = 0; index < count; ++index) {
        RecompLauncherCModFeature feature;
        memset(&feature, 0, sizeof(feature));
        if (!mods->feature_get(mods->ctx, index, &feature)) continue;
        if (strcmp(feature.package_id, package_id) != 0) continue;
        if (!launcher_mod_feature_concealed(mods, &feature)) return 1;
        owned = 1;
    }
    return !owned;
}

int launcher_mod_listed_feature_count(const RecompLauncherCModProvider* mods) {
    if (!has_feature_surface(mods)) return 0;
    const int count = mods->feature_count(mods->ctx);
    int listed = 0;
    for (int index = 0; index < count; ++index) {
        RecompLauncherCModFeature feature;
        memset(&feature, 0, sizeof(feature));
        if (mods->feature_get(mods->ctx, index, &feature) &&
            launcher_mod_feature_listed(mods, &feature))
            ++listed;
    }
    return listed;
}
