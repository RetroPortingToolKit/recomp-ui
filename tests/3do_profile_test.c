#include "launcher_profile.h"
#include "launcher_system.h"
#include <assert.h>
#include <string.h>
int main(void) {
    RecompLauncherCGameInfo game;
    memset(&game, 0, sizeof(game));
    assert(launcher_profile_apply("3do", &game));
    assert(game.has_bios && game.num_players == 1);
    assert(!strcmp(game.platform, "3DO"));
    assert(!game.pad_mode_supported && !game.has_supersampling);
    assert(launcher_system_infer(&game) == &kSystemProfile3do);
    assert(launcher_system_by_id("3DO") == &kSystemProfile3do);
    assert(kSystemProfile3do.controller.button_count == 11);
    assert(!strcmp(k3doPadButtons[6].label, "C"));
    assert(k3doKbIndex[6] == 2 && k3doKbIndex[9] == 6 && k3doKbIndex[10] == 7);
    return 0;
}
