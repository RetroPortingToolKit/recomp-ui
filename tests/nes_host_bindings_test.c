/* A host that owns its bindings (GameInfo.settings_bindings) under the NES
 * profile: loading and refreshing the Controller page must not create, seed or
 * rewrite the function-level runner's keybinds.ini, which that host never
 * reads (nesrecomp's cycle host keeps its bindings in its own config.ini).
 * The same calls without settings_bindings still create the runner's file,
 * so the guard is what makes the difference. Also: the GameInfo ROM-picker
 * override reaches the model. Wii's host-owned mode must also avoid creating
 * either the custom-path or generic fallback binding file. */
#include "launcher_model.h"
#include "launcher_binds.h"
#include "launcher_profile.h"
#include "launcher_system.h"
#include "launcher_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void run(const char *keybinds, int host_owned, int wii) {
    static LauncherModel m;
    RecompLauncherCSettings io;
    memset(&io, 0, sizeof(io));
    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply(wii ? "wii" : "nes", &gi);
    if (!wii) gi.platform = "FAMICOM DISK SYSTEM";
    gi.name = "Test";
    gi.settings_bindings = host_owned;
    gi.keybinds_path = keybinds;
    io.player_key_bind[0][4] = 27;
    gi.default_settings = &io;
    static const char *const patterns[] = { "*.fds", "*.qd" };
    gi.rom_patterns = patterns;
    gi.num_rom_patterns = 2;
    gi.rom_filter_desc = "Famicom Disk System image (.fds, .qd)";
    memset(&m, 0, sizeof(m));
    launcher_model_init(&m, &io, &gi, "");
    assert(m.profile && !strcmp(((const SystemProfile *)m.profile)->id, wii ? "wii" : "nes"));
    assert(m.settings_bindings == (host_owned != 0));
    assert(m.rom_patterns == patterns && m.num_rom_patterns == 2);
    assert(!strcmp(m.rom_filter_desc, "Famicom Disk System image (.fds, .qd)"));
    launcher_binds_load(&m, NULL, keybinds);
    launcher_binds_refresh(&m);
    launcher_binds_refresh_camera(&m);
    launcher_binds_set_camera(&m, 0, 4);
    launcher_binds_reset_camera(&m);
    if (wii && host_owned) {
        /* These are the real handlers the UI event path calls. */
        launcher_binds_set_button(&m, 1, 4, 9);
        launcher_binds_set_pad_button(&m, 1, 4, LNG_PADBIND_AXIS, 0, -1);
        assert(m.s.player_key_bind[0][4] == 9);
        assert(m.s.player_pad_bind[0][4] == RECOMP_LAUNCHER_PAD_AXIS(0, 0));
        launcher_binds_set_button(&m, 1, 4, 0);
        launcher_binds_set_pad_button(&m, 1, 4, LNG_PADBIND_NONE, 0, 0);
        assert(!m.s.player_key_bind[0][4] && !m.s.player_pad_bind[0][4]);
        launcher_binds_reset_player(&m, 1);
        assert(m.s.player_key_bind[0][4] == 27);
    }
}

int main(int argc, char **argv) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/nes_host_keybinds.ini", argc > 1 ? argv[1] : ".");
    remove(path);
    run(path, 1, 0);
    assert(!exists(path));        /* host-owned: no runner file appears */
    run(path, 0, 0);
    assert(exists(path));         /* the runner's store still seeds its file */
    remove(path);
    char generic[2048];
#if defined(LNG_SDL3)
    snprintf(generic, sizeof(generic), "%skeybinds.ini", SDL_GetBasePath());
#else
    char* base = SDL_GetBasePath();
    snprintf(generic, sizeof(generic), "%skeybinds.ini", base);
    SDL_free(base);
#endif
    int had_generic = exists(generic);
    run(path, 1, 1);
    assert(!exists(path) && exists(generic) == had_generic);
    puts("NES/Wii host binding stores test passed");
    return 0;
}
