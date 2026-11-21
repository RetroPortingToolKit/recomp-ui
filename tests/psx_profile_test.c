#include "recomp_launcher.h"
#include "consoles/psx/psx_profile.h"

#include <assert.h>
#include <string.h>

int main(void) {
    RecompLauncherCGameInfo game;
    memset(&game, 0, sizeof(game));

    launcher_profile_apply_psx(&game);

    /* PSX display enhancements are mod-owned, never generic launcher rows. */
    assert(game.widescreen_supported == 0);
    assert(game.aspect_mask == 0);
    assert(game.has_frame_interp == 0);
    assert(game.has_skip_fmv == 0);

    /* The game's look is psxrecomp's built-in mods (CRT Filter, Image
     * Filtering, Internal Resolution, Enhanced Audio, Rewind), not Settings
     * rows (docs/SETTINGS_STANDARD.md). */
    assert(game.has_screen_kind == 0);
    assert(game.has_scanlines == 0);
    assert(game.has_antialiasing == 0);
    assert(game.has_texture_filter == 0);
    assert(game.has_fmv_filter == 0);
    assert(game.has_spu_hq == 0);
    assert(game.has_rewind_depth == 0);
    assert(game.linear_filter_hidden == 1);
    assert(game.has_supersampling == 0);            /* Internal Resolution mod */
    assert(kSystemProfilePsx.video.supersampling == 0);

    /* Audio: Volume only. The runtime opens the device at its built-in rate,
     * so there is no Sample rate row. */
    assert(kSystemProfilePsx.hide_audio_freq == 1);

    /* The PC/system Display capabilities remain available. */
    assert(game.has_window_size == 1);
    assert(game.has_renderer == 1);
    assert(game.frame_generation_hidden == 1);
    assert(game.has_vsync == 1);

    return 0;
}
