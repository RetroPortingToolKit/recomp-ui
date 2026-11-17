/* The launcher's side of the named keyboard profiles (launcher_binds.h): the
 * folder it reads, the key labels of the Controls page after a load, and that
 * a console other than PlayStation has none.
 *
 * The folder is named by PSXRECOMP_KEYBOARD_PROFILES_DIR, a scratch folder of
 * the build tree: the user's own folder is never read or written.
 */
#include "launcher_model.h"
#include "launcher_binds.h"
#include "launcher_profile.h"
#include "launcher_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { B_UP = 0, KEY_W = 26 };   /* rebind-spec index, SDL scancode */

static int fails, checks;
static LauncherModel m;
static char g_dir[900], g_keys[1024], g_other[1024];

static void expect(int cond, const char* what) {
    ++checks;
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static const char* file_of(const char* name) {
    static char path[1024];
    snprintf(path, sizeof(path), "%s" SEP "%s.ini", g_dir, name);
    return path;
}

static int exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void start(const char* console, const char* keybinds) {
    RecompLauncherCSettings io;
    RecompLauncherCGameInfo gi;
    memset(&io, 0, sizeof(io));
    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply(console, &gi);
    gi.name = "Profiles Fixture";
    gi.num_players = 2;
    gi.keybinds_path = keybinds;
    memset(&m, 0, sizeof(m));
    launcher_model_init(&m, &io, &gi, "");
    launcher_binds_load(&m, NULL, keybinds);
}

int main(int argc, char** argv) {
    char names[8][RUI_PSX_KB_PROFILE_NAME_MAX + 1];
    char in_use[RUI_PSX_KB_PROFILE_NAME_MAX + 1];
    if (argc != 2) {
        fprintf(stderr, "usage: launcher_kb_profiles_test <scratch-folder>\n");
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s" SEP "shared", argv[1]);
    snprintf(g_keys, sizeof(g_keys), "%s" SEP "keybinds.ini", argv[1]);
    snprintf(g_other, sizeof(g_other), "%s" SEP "other-keybinds.ini", argv[1]);
    remove(file_of("Mine"));
    remove(file_of("Yours"));
    remove(file_of("Other"));
    remove(g_keys);
    remove(g_other);
#ifdef _WIN32
    _putenv_s("PSXRECOMP_KEYBOARD_PROFILES_DIR", g_dir);
#else
    setenv("PSXRECOMP_KEYBOARD_PROFILES_DIR", g_dir, 1);
#endif

    start("psx", g_keys);
    expect(!strcmp(launcher_binds_psx_kb_profiles_dir(), g_dir),
           "the launcher reads the folder the variable names");
    expect(launcher_binds_psx_kb_profile_list(&m, names, 8) == 1 && !strcmp(names[0], "Default"),
           "a PlayStation title lists Default");
    expect(launcher_binds_psx_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
           !strcmp(in_use, "Default"), "with the Default keys in use");

    launcher_binds_set_button_slot(&m, 1, B_UP, 0, KEY_W);
    expect(!strcmp(m.binds[0][B_UP], "W"), "Up rebound to W shows W");
    expect(launcher_binds_psx_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 0 &&
           in_use[0] == '\0', "and no profile holds these keys");
    expect(launcher_binds_psx_kb_profile_save_as(&m, 1, "Mine", 0) == RUI_PSX_KBP_OK,
           "save as Mine");
    expect(exists(file_of("Mine")), "the file is in the folder of the variable");
    expect(launcher_binds_psx_kb_profile_list(&m, names, 8) == 2 && !strcmp(names[1], "Mine"),
           "the list has Mine");

    expect(launcher_binds_psx_kb_profile_load(&m, 1, "Default") == RUI_PSX_KBP_OK, "load Default");
    expect(!strcmp(m.binds[0][B_UP], "Up"), "the page shows Default's key at once");
    expect(launcher_binds_psx_kb_profile_load(&m, 1, "Mine") == RUI_PSX_KBP_OK, "load Mine");
    expect(!strcmp(m.binds[0][B_UP], "W"), "the page shows the profile's key at once");
    expect(!strcmp(m.binds[1][B_UP], "Up"), "Player 2's page is untouched");
    expect(launcher_binds_psx_kb_profile_load(&m, 2, "Mine") == RUI_PSX_KBP_OK,
           "load Mine onto Player 2");
    expect(!strcmp(m.binds[1][B_UP], "W"), "Player 2's page shows it");
    expect(launcher_binds_psx_kb_profile_in_use(&m, 2, in_use, (int)sizeof(in_use)) == 1 &&
           !strcmp(in_use, "Mine"), "and Mine is named as Player 2's keys in use");

    expect(launcher_binds_psx_kb_profile_load(&m, 0, "Mine") == RUI_PSX_KBP_IO &&
           launcher_binds_psx_kb_profile_load(&m, LNG_MAX_PLAYERS + 1, "Mine") == RUI_PSX_KBP_IO &&
           launcher_binds_psx_kb_profile_save_as(&m, 0, "Yours", 0) == RUI_PSX_KBP_IO,
           "a player that is not there loads and saves nothing");
    expect(!exists(file_of("Yours")), "and no file is written for one");

    expect(launcher_binds_psx_kb_profile_rename(&m, "Mine", "Yours") == RUI_PSX_KBP_OK &&
           exists(file_of("Yours")) && !exists(file_of("Mine")), "rename Mine to Yours");
    expect(launcher_binds_psx_kb_profile_delete(&m, "Default") == RUI_PSX_KBP_PROTECTED,
           "Default is protected here too");
    expect(launcher_binds_psx_kb_profile_delete(&m, "Yours") == RUI_PSX_KBP_OK &&
           !exists(file_of("Yours")), "delete Yours");

    /* Keyboard profiles are a PlayStation store. Another console has none. */
    start("nes", g_other);
    expect(launcher_binds_psx_kb_profile_list(&m, names, 8) == 0, "another console lists no profile");
    expect(launcher_binds_psx_kb_profile_save_as(&m, 1, "Other", 0) == RUI_PSX_KBP_IO &&
           launcher_binds_psx_kb_profile_load(&m, 1, "Default") == RUI_PSX_KBP_IO &&
           launcher_binds_psx_kb_profile_rename(&m, "a", "b") == RUI_PSX_KBP_IO &&
           launcher_binds_psx_kb_profile_delete(&m, "a") == RUI_PSX_KBP_IO,
           "and saves, loads, renames and deletes none");
    expect(!exists(file_of("Other")), "nothing is written for it");

    remove(g_keys);
    remove(g_other);
    if (fails) { fprintf(stderr, "\n%d of %d FAILED\n", fails, checks); return 1; }
    printf("\nlauncher_kb_profiles_test: all %d checks passed\n", checks);
    return 0;
}
