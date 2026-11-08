#pragma once
#include "launcher_model.h"
#include "launcher_mod_visibility.h"

#include <string.h>

/* Existing built-in preferences keep their provider and persisted values;
 * their controls belong to Settings, independent of installed game mods. */
static inline const char* launcher_settings_group(const char* package) {
    if (!package) return NULL;
    if (!strcmp(package, "psx.enhancement.crt-filter") ||
        !strcmp(package, "psx.enhancement.image-filtering") ||
        !strcmp(package, "psx.enhancement.internal-resolution") ||
        !strcmp(package, "psx.enhancement.pgxp")) return "Display";
    if (!strcmp(package, "psx.enhancement.enhanced-audio") ||
        !strcmp(package, "psx.feature.rewind")) return "General";
    if (!strcmp(package, "psx.runtime.settings")) return "Runtime";
    return NULL;
}

/* One provider operation for the full Mods page and dashboard shortcuts.
 * The provider owns dependencies, validation and persisted feature state. */
static inline int launcher_mod_enable_feature(LauncherModel* model,
        const RecompLauncherCModFeature* feature,int enabled) {
    const RecompLauncherCModProvider* mods=model?model->mods:NULL;
    if(!feature || !mods || !mods->feature_enable ||
       !launcher_model_can_open_mods(model) ||
       !launcher_mod_feature_listed(mods,feature))return 0;
    return mods->feature_enable(mods->ctx,feature->package_id,feature->id,enabled);
}
