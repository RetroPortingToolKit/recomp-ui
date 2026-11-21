/* A host that owns its bindings (GameInfo.settings_bindings) under the NES
 * profile: loading and refreshing the Controller page must not create, seed or
 * rewrite the function-level runner's keybinds.ini, which that host never
 * reads (nesrecomp's cycle host keeps its bindings in its own config.ini).
 * The same calls without settings_bindings still create the runner's file,
 * so the guard is what makes the difference. Also: the GameInfo ROM-picker
 * override reaches the model. */
#include "launcher_model.h"
#include "launcher_binds.h"
#include "launcher_profile.h"
#include "launcher_system.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void run(const char *keybinds, int host_owned) {
    static LauncherModel m;
    RecompLauncherCSettings io;
    memset(&io, 0, sizeof(io));
    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply("nes", &gi);
    gi.platform = "FAMICOM DISK SYSTEM";   /* a disk title's subtitle: still the NES row */
    gi.name = "Test";
    gi.settings_bindings = host_owned;
    gi.keybinds_path = keybinds;
    static const char *const patterns[] = { "*.fds", "*.qd" };
    gi.rom_patterns = patterns;
    gi.num_rom_patterns = 2;
    gi.rom_filter_desc = "Famicom Disk System image (.fds, .qd)";
    memset(&m, 0, sizeof(m));
    launcher_model_init(&m, &io, &gi, "");
    assert(m.profile && !strcmp(((const SystemProfile *)m.profile)->id, "nes"));
    assert(m.settings_bindings == (host_owned != 0));
    assert(m.rom_patterns == patterns && m.num_rom_patterns == 2);
    assert(!strcmp(m.rom_filter_desc, "Famicom Disk System image (.fds, .qd)"));
    launcher_binds_load(&m, NULL, keybinds);
    launcher_binds_refresh(&m);
    launcher_binds_refresh_camera(&m);
    launcher_binds_set_camera(&m, 0, 4);
    launcher_binds_reset_camera(&m);
}

int main(int argc, char **argv) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/nes_host_keybinds.ini", argc > 1 ? argv[1] : ".");
    remove(path);
    run(path, 1);
    assert(!exists(path));        /* host-owned: no runner file appears */
    run(path, 0);
    assert(exists(path));         /* the runner's store still seeds its file */
    remove(path);
    puts("nes host bindings test passed");
    return 0;
}
