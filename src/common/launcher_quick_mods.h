#pragma once
#include "launcher_mod_feature_actions.h"

/* The dashboard's Quick Mods card lists the features the provider marks quick
 * (the manifest's quick flag or the player's own choice) that the Mods page
 * also lists. Switching one uses launcher_mod_enable_feature, the same path as
 * the Mods page. */
static inline int launcher_quick_listed(const RecompLauncherCModProvider* p,
                                        const RecompLauncherCModFeature* f) {
    return p && f && f->quick && launcher_mod_feature_listed(p, f);
}
