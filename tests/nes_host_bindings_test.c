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

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void virtual_buttons(void) {
    static LauncherModel m;
    RecompLauncherCSettings io = {0};
    RecompLauncherCGameInfo gi = {0};
    const char* labels[] = {"Virtual L", "Virtual R"};
    launcher_profile_apply("gbc", &gi);
    gi.settings_bindings = 1;
    gi.native_player_count = gi.num_players = 2;
    gi.virtual_button_labels = labels;
    gi.virtual_button_count = 2;
    io.player_key_bind[0][8] = 20;
    io.player_key_bind[1][8] = 24;
    launcher_model_init(&m, &io, &gi, "");
    assert(m.profile->controller.button_count == 8);
    assert(launcher_model_active_button_count(&m, 0) == 10);
    assert(launcher_model_active_button_count(&m, 1) == 10);
    assert(!strcmp(launcher_model_button_label(&m, 7), "Select"));
    assert(!strcmp(launcher_model_button_label(&m, 8), "Virtual L"));
    assert(!strcmp(launcher_model_button_label(&m, 9), "Virtual R"));
    assert(!strcmp(launcher_model_button_label(&m, 10), "?"));
    m.cfg_player = 1;
    launcher_model_begin_pad_capture(&m, 9);
    assert(m.capturing && m.capture_pad && m.capture_btn == 9);
    launcher_model_set_captured_pad(&m, RECOMP_LAUNCHER_PAD_BUTTON(10));
    assert(m.s.player_pad_bind[1][9] == RECOMP_LAUNCHER_PAD_BUTTON(10));
    assert(m.s.player_pad_bind[0][9] == 0);
    launcher_model_cancel_capture(&m);
    launcher_model_begin_capture(&m, 8);
    launcher_model_set_captured_key(&m, 25);
    launcher_model_commit(&m, &io);
    assert(io.player_key_bind[0][8] == 20 && io.player_key_bind[1][8] == 25);
    gi.virtual_button_count = 0;
    launcher_model_init(&m, &io, &gi, "");
    assert(launcher_model_active_button_count(&m, 0) == 8);
    gi.virtual_button_count = 2;
    gi.settings_bindings = 0;
    launcher_model_init(&m, &io, &gi, "");
    assert(launcher_model_active_button_count(&m, 0) == 8);
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
    virtual_buttons();
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
