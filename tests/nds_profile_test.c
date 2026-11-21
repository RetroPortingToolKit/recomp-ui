#include "recomp_launcher.h"
#include "consoles/nds/nds_profile.h"

#include <assert.h>
#include <string.h>

int main(void) {
    RecompLauncherCGameInfo game;
    memset(&game, 0, sizeof(game));

    launcher_profile_apply_nds(&game);

    assert(game.has_virtual_stylus == 1);
    assert(game.settings_bindings == 1);
    assert(game.assist_binding_count == 2);
    assert(strcmp(game.assist_binding_labels[0], "Virtual Stylus") == 0);
    assert(strcmp(game.assist_binding_labels[1], "Virtual Stylus Tap") == 0);

    assert(game.assist_default_key_bind[0] == 43);
    assert(game.assist_default_key_bind[1] == 0);
    assert(game.assist_default_pad_bind[0] == RECOMP_LAUNCHER_PAD_AXIS(4, 1));
    assert(game.assist_default_pad_bind[1] == RECOMP_LAUNCHER_PAD_BUTTON(0));

    return 0;
}
