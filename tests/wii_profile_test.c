#include "launcher_profile.h"
#include "launcher_system.h"

#include <assert.h>
#include <string.h>

static int has_panel(const char* const* panels, const char* name) {
    for (; *panels; ++panels) if (strcmp(*panels, name) == 0) return 1;
    return 0;
}

int main(void) {
    const char* aliases[] = { "wii", "WII", "NintendoWii" };
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        RecompLauncherCGameInfo game = {0};
        assert(launcher_profile_apply(aliases[i], &game));
        const SystemProfile* profile = launcher_system_infer(&game);
        assert(profile == launcher_system_by_id(aliases[i]));
        assert(strcmp(profile->id, "wii") == 0);
        assert(profile->controller.button_count == 11);
        assert(profile->controller.has_pad_mode == 0);
        for (int button = 0; button < profile->controller.button_count; ++button)
            assert(profile->controller.buttons[button].code == button);
        assert(strcmp(profile->controller.buttons[4].label, "1") == 0);
        assert(strcmp(profile->controller.buttons[5].label, "2") == 0);
        assert(strcmp(profile->controller.buttons[10].label, "HOME") == 0);
        assert(game.settings_bindings);
        assert(profile->save.kind == SAVE_NONE);
        assert(!has_panel(profile->panels_settings, "system"));
        assert(!has_panel(profile->panels_dashboard, "memcard"));
        assert(!game.has_bios && !game.has_renderer && !game.has_supersampling);
        assert(!game.widescreen_supported);
        assert(profile->video.hide_legacy_filter);
        // A title may add capabilities without being inferred as PSX.
        game.has_renderer = 1;
        game.has_supersampling = 1;
        assert(launcher_system_infer(&game) == profile);
    }
    return 0;
}
