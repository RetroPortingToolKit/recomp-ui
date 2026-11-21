/* Which mod features and packages the launcher presents to the player.
 *
 * Default (provider hide_hidden_features == 0, every existing title): a
 * feature marked `hidden` is listed only while it is enabled, so it can be
 * turned off again, and every package is listed.
 *
 * Opt-in (hide_hidden_features != 0, set by the host for its title): a hidden
 * feature is never presented -- not in the Mods list, the lobby picker or its
 * summary, not as the detail pane's selection -- and "Disable all"
 * leaves it alone. It still runs exactly as its package and the
 * player's saved state say, so a hidden default-on feature is simply active.
 * A package whose every feature is hidden is not listed under "Installed
 * packages" either. Providers without the feature surface have no hidden
 * flag, so all of their packages are presented. */
#ifndef LAUNCHER_MOD_VISIBILITY_H
#define LAUNCHER_MOD_VISIBILITY_H

#include "recomp_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hidden, and the host opted into never presenting hidden features. */
int launcher_mod_feature_concealed(const RecompLauncherCModProvider* mods,
                                   const RecompLauncherCModFeature* feature);

int launcher_mod_feature_listed(const RecompLauncherCModProvider* mods,
                                const RecompLauncherCModFeature* feature);

int launcher_mod_package_listed(const RecompLauncherCModProvider* mods,
                                const char* package_id);

/* Number of features launcher_mod_feature_listed() accepts. */
int launcher_mod_listed_feature_count(const RecompLauncherCModProvider* mods);

#ifdef __cplusplus
}
#endif

#endif
