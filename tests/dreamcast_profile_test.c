#include "launcher_profile.h"
#include "launcher_system.h"
#include <assert.h>
#include <string.h>

int main(void) {
    RecompLauncherCGameInfo game = {0};
    assert(launcher_profile_apply("DC", &game));
    const SystemProfile* profile = launcher_system_infer(&game);
    assert(profile == launcher_system_by_id("Dreamcast"));
    assert(strcmp(profile->id, "dreamcast") == 0);
    assert(strcmp(game.rom_noun, "Disc") == 0);
    assert(game.num_players == 4);
    assert(game.settings_bindings && game.has_bios && game.has_deadzone_pct);
    assert(!game.pad_mode_supported && !game.pad_mode_selectable);
    assert(!game.has_renderer && !game.has_turbo_loads && !game.has_skip_fmv);
    assert(profile->save.kind == SAVE_NONE);
    assert(profile->controller.button_count == 15);
    assert(strcmp(profile->controller.buttons[10].label, "Start") == 0);
    assert(kDreamcastKbIndex[8] == 4 && kDreamcastKbIndex[9] == 5);
    assert(kDreamcastKbIndex[10] == 6);
    assert(kDreamcastKbIndex[14] == 15);
    assert(profile->rom_filter.pattern_count == 3);
    return 0;
}
