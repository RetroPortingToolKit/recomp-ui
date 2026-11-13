#include "launcher_profile.h"
#include "launcher_system.h"
#include "launcher_theme.h"
#include <assert.h>
#include <string.h>
int main(void) {
    RecompLauncherCGameInfo game={0};
    assert(launcher_profile_apply("CD-I",&game));
    const SystemProfile *profile=launcher_system_infer(&game);
    assert(profile==launcher_system_by_id("cdi"));
    assert(!strcmp(profile->id,"cdi"));
    assert(game.has_bios && game.settings_bindings && game.num_players==1);
    assert(!game.netplay_supported && !game.has_turbo_loads);
    assert(profile->controller.button_count==6 && profile->controller.max_players==1);
    assert(profile->rom_filter.pattern_count==2);
    assert(!strcmp(profile->rom_filter.patterns[0],"*.cue"));
    assert(profile->hide_audio_freq && profile->save.kind==SAVE_NONE);
    LauncherTheme theme=launcher_theme_by_name("cdi");
    assert(!theme.scanlines && theme.accent.g>theme.accent.r && theme.accent2.r>theme.accent2.b);
    assert(launcher_system_by_id("psx")!=profile);
    return 0;
}
