/*
 * launcher_icons.h - "Add to Steam" and "create a desktop shortcut" for the
 * launcher's main page.
 *
 * Both act on the image the player has selected, so the caller passes the
 * program path, the arguments that start that image, and a display name.
 * Windows: a real .lnk on the Desktop, and an entry appended to Steam's
 * shortcuts.vdf. Other platforms report that the action is unavailable.
 */
#ifndef LAUNCHER_ICONS_H
#define LAUNCHER_ICONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a desktop shortcut that starts `exe_path` with `args`.
 * 1 = created, 0 = failed (err holds the reason). */
int launcher_desktop_shortcut(const char *exe_path, const char *args,
                              const char *name, char *err, size_t err_cap);

/* Add a non-Steam shortcut to the user's Steam library.
 * Refuses while Steam is running (it would overwrite the file on exit),
 * backs up the current shortcuts.vdf, and is idempotent: an entry with the
 * same name and program is not added twice.
 * 1 = added (or already present), 0 = failed (err holds the reason). */
int launcher_steam_shortcut(const char *exe_path, const char *args,
                            const char *name, const char *icon_path,
                            char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_ICONS_H */
