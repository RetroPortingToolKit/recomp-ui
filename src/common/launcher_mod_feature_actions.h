#pragma once
#include "launcher_model.h"
#include "launcher_mod_visibility.h"

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
