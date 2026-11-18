// Per-game color overrides on top of a console theme. A game ships an INI
// file (recomp_ui_add ... THEME <ini>, staged as assets/theme_game.ini):
//
//   [colors]
//   accent = #ECC57D        ; #RRGGBB or #RRGGBBAA, keys = LauncherTheme fields
//
// Unset tokens keep the console theme; bad lines are skipped with a warning.
#ifndef LAUNCHER_THEME_OVERRIDES_H
#define LAUNCHER_THEME_OVERRIDES_H

#include <stdint.h>

#include "launcher_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAUNCHER_THEME_OVERRIDE_TOKENS 16

typedef struct {
    uint32_t set_mask;  // bit i set = color[i] overrides token i
    LngColor color[LAUNCHER_THEME_OVERRIDE_TOKENS];
} LauncherThemeOverrides;

// Returns the number of valid entries parsed into out.
int launcher_theme_overrides_parse(const char* text, LauncherThemeOverrides* out);
void launcher_theme_overrides_apply(const LauncherThemeOverrides* o, LauncherTheme* t);

// Process-wide game overrides. A missing file leaves them empty (returns 0).
int launcher_theme_load_game_overrides(const char* path);
const LauncherThemeOverrides* launcher_theme_game_overrides(void);
int launcher_theme_game_overrides_loaded(void);

// Applies the game overrides; on first use without an explicit load it reads
// <exe dir>/assets/theme_game.ini (launcher_theme_overrides_sdl.c).
void launcher_theme_apply_game_overrides(LauncherTheme* t);

#ifdef __cplusplus
}
#endif

#endif
