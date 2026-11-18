// Default location of the game overrides: <exe dir>/assets/theme_game.ini,
// where recomp_ui_add(... THEME) stages the file. Kept apart from the parser
// so the parser builds and tests without SDL.
#include "launcher_theme_overrides.h"
#include "launcher_platform.h"

#include <stdio.h>

#define GAME_THEME_FILE "assets/theme_game.ini"
#define GAME_THEME_PATH_MAX 4096

static void load_default_game_overrides(void) {
#if defined(LNG_SDL3)
    const char* base = SDL_GetBasePath();
#else
    char* base = SDL_GetBasePath();
#endif
    char path[GAME_THEME_PATH_MAX];
    const int n = snprintf(path, sizeof path, "%s%s", base ? base : "", GAME_THEME_FILE);
#if !defined(LNG_SDL3)
    SDL_free(base);
#endif
    launcher_theme_load_game_overrides(n > 0 && n < (int)sizeof path ? path : NULL);
}

void launcher_theme_apply_game_overrides(LauncherTheme* t) {
    if (!launcher_theme_game_overrides_loaded()) load_default_game_overrides();
    launcher_theme_overrides_apply(launcher_theme_game_overrides(), t);
}
