/* The launcher's side of the named keyboard profiles (launcher_binds.h): the
 * folder it reads per console, the key labels of the Controls page after a
 * load, and the profiles of every console that has a keyboard bind card, each
 * through its own bind store (PSX, PS2, NES, Genesis, GB, GBC, N64, SNES, GBA,
 * 3DO); and none for a host that keeps its bindings in its own settings.
 *
 * The folder is a scratch folder of the build tree, named by
 * RECOMP_UI_KEYBOARD_PROFILES_DIR (and PSXRECOMP_KEYBOARD_PROFILES_DIR for the
 * PlayStation alias): the user's own folder is never read or written.
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

enum { B_UP = 0, KEY_W = 26, KEY_F12 = 69 };   /* rebind-spec index, SDL scancodes */

static int fails, checks;
static LauncherModel m;
static char g_root[900], g_scratch[900];

static void expect(int cond, const char* what) {
    ++checks;
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static void say(const char* console, const char* what, int cond) {
    char line[200];
    snprintf(line, sizeof(line), "%s: %s", console, what);
    expect(cond, line);
}

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value[0]) setenv(name, value, 1); else unsetenv(name);
#endif
}

static int exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static const char* file_in(const char* dir, const char* name) {
    static char path[1100];
    snprintf(path, sizeof(path), "%s" SEP "%s.ini", dir, name);
    return path;
}

static void start(const char* console, const char* keybinds, int host_owned) {
    RecompLauncherCSettings io;
    RecompLauncherCGameInfo gi;
    memset(&io, 0, sizeof(io));
    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply(console, &gi);
    gi.name = "Profiles Fixture";
    gi.num_players = 2;
    gi.keybinds_path = keybinds;
    gi.settings_bindings = host_owned;
    memset(&m, 0, sizeof(m));
    launcher_model_init(&m, &io, &gi, "");
    launcher_binds_load(&m, NULL, keybinds);
}

/* One console, through its own store: save, change, load, rename, delete. */
static void walk(const char* console) {
    char names[8][RUI_KBP_NAME_MAX + 1];
    char in_use[RUI_KBP_NAME_MAX + 1];
    char keys[1000], want_dir[1000], mine[40], dflt[40];
    snprintf(keys, sizeof(keys), "%s" SEP "%s-keys.ini", g_scratch, console);
    remove(keys);
    start(console, keys, 0);

    say(console, "profiles are offered for Player 1", launcher_binds_kb_profiles_supported(&m, 1));
    snprintf(want_dir, sizeof(want_dir), "%s" SEP "%s", g_root, console);
    say(console, "in a folder of its own under the root",
        !strcmp(launcher_binds_kb_profiles_dir(&m), want_dir));
    say(console, "lists Default",
        launcher_binds_kb_profile_list(&m, names, 8) == 1 && !strcmp(names[0], "Default"));
    say(console, "with the Default keys in use",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Default"));
    snprintf(dflt, sizeof(dflt), "%s", m.binds[0][B_UP]);

    launcher_binds_set_button(&m, 1, B_UP, KEY_F12);
    snprintf(mine, sizeof(mine), "%s", m.binds[0][B_UP]);
    say(console, "Up rebound to F12 shows a new label", strcmp(mine, dflt) != 0);
    say(console, "and no profile holds these keys",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 0 &&
        in_use[0] == '\0');
    say(console, "save as Mine", launcher_binds_kb_profile_save_as(&m, 1, "Mine", 0) == RUI_KBP_OK);
    say(console, "the file is in the console's folder", exists(file_in(want_dir, "Mine")));
    say(console, "and is named as the keys in use",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Mine"));
    say(console, "asking again, as a frame does, says the same",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Mine"));

    say(console, "load Default", launcher_binds_kb_profile_load(&m, 1, "Default") == RUI_KBP_OK);
    say(console, "the page shows Default's key at once", !strcmp(m.binds[0][B_UP], dflt));
    say(console, "Default is in use",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Default"));
    say(console, "load Mine", launcher_binds_kb_profile_load(&m, 1, "Mine") == RUI_KBP_OK);
    say(console, "the page shows the profile's key at once", !strcmp(m.binds[0][B_UP], mine));
    say(console, "Mine is in use",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Mine"));

    say(console, "a player that is not there loads and saves nothing",
        launcher_binds_kb_profile_load(&m, 0, "Mine") == RUI_KBP_IO &&
        launcher_binds_kb_profile_load(&m, LNG_MAX_PLAYERS + 1, "Mine") == RUI_KBP_IO &&
        launcher_binds_kb_profile_save_as(&m, 0, "Yours", 0) == RUI_KBP_IO &&
        !exists(file_in(want_dir, "Yours")));
    say(console, "rename Mine to Yours",
        launcher_binds_kb_profile_rename(&m, "Mine", "Yours") == RUI_KBP_OK &&
        exists(file_in(want_dir, "Yours")) && !exists(file_in(want_dir, "Mine")));
    say(console, "the keys in use follow the new name",
        launcher_binds_kb_profile_in_use(&m, 1, in_use, (int)sizeof(in_use)) == 1 &&
        !strcmp(in_use, "Yours"));
    say(console, "Default is protected",
        launcher_binds_kb_profile_delete(&m, "Default") == RUI_KBP_PROTECTED);
    say(console, "delete Yours",
        launcher_binds_kb_profile_delete(&m, "Yours") == RUI_KBP_OK &&
        !exists(file_in(want_dir, "Yours")));

    /* Load from file: another game's bind file of the same format. */
    {
        char other[1100];
        snprintf(other, sizeof(other), "%s" SEP "%s-other.ini", g_scratch, console);
        say(console, "a file with no keys of the console is refused, the keys unchanged",
            (remove(other), launcher_binds_load_keyboard_file(&m, 1, other)) == 0 &&
            !strcmp(m.binds[0][B_UP], mine));
    }

    launcher_binds_reset_player(&m, 1);   /* leave the store, and a shared keybinds.ini, as found */
    remove(keys);
}

int main(int argc, char** argv) {
    static const char* const consoles[] = {
        "psx", "ps2", "nes", "genesis", "gb", "gbc", "n64", "snes", "gba", "3do" };
    char names[8][RUI_KBP_NAME_MAX + 1];
    char in_use[RUI_KBP_NAME_MAX + 1], alias[1000], keys[1000];
    if (argc != 2) {
        fprintf(stderr, "usage: launcher_kb_profiles_test <scratch-folder>\n");
        return 2;
    }
    snprintf(g_scratch, sizeof(g_scratch), "%s", argv[1]);
    snprintf(g_root, sizeof(g_root), "%s" SEP "kb-root", argv[1]);
    set_env("PSXRECOMP_KEYBOARD_PROFILES_DIR", "");
    set_env(RUI_KBP_ENV_ROOT, g_root);

    /* Start from an empty root: what an earlier run left is removed. */
    for (size_t i = 0; i < sizeof(consoles) / sizeof(consoles[0]); ++i) {
        char dir[1000];
        snprintf(dir, sizeof(dir), "%s" SEP "%s", g_root, consoles[i]);
        remove(file_in(dir, "Mine"));
        remove(file_in(dir, "Yours"));
    }

    for (size_t i = 0; i < sizeof(consoles) / sizeof(consoles[0]); ++i) walk(consoles[i]);

    /* One console's profiles are not another's. */
    start("snes", NULL, 0);
    expect(launcher_binds_kb_profile_list(&m, names, 8) == 1,
           "a console lists none of another console's profiles");

    /* PlayStation: Player 2 is a seat of its own, and the first release's
     * variable still names the folder. */
    snprintf(keys, sizeof(keys), "%s" SEP "psx2-keys.ini", argv[1]);
    snprintf(alias, sizeof(alias), "%s" SEP "psx-alias", argv[1]);
    remove(keys);
    remove(file_in(alias, "Mine"));
    set_env("PSXRECOMP_KEYBOARD_PROFILES_DIR", alias);
    start("psx", keys, 0);
    expect(!strcmp(launcher_binds_kb_profiles_dir(&m), alias),
           "PSXRECOMP_KEYBOARD_PROFILES_DIR still names the PlayStation folder");
    launcher_binds_set_button_slot(&m, 1, B_UP, 0, KEY_W);
    expect(launcher_binds_kb_profile_save_as(&m, 1, "Mine", 0) == RUI_KBP_OK &&
           exists(file_in(alias, "Mine")), "and a PlayStation profile is saved there");
    expect(!strcmp(m.binds[1][B_UP], "Up"), "Player 2's page is untouched");
    expect(launcher_binds_kb_profile_load(&m, 2, "Mine") == RUI_KBP_OK &&
           !strcmp(m.binds[1][B_UP], "W"), "load Mine onto Player 2: its page shows it");
    expect(launcher_binds_kb_profile_in_use(&m, 2, in_use, (int)sizeof(in_use)) == 1 &&
           !strcmp(in_use, "Mine"), "and Mine is named as Player 2's keys in use");
    expect(launcher_binds_kb_profile_delete(&m, "Mine") == RUI_KBP_OK, "delete Mine");
    launcher_binds_reset_player(&m, 1);
    launcher_binds_reset_player(&m, 2);
    remove(keys);
    set_env("PSXRECOMP_KEYBOARD_PROFILES_DIR", "");

    /* A host that keeps the bindings in its own settings has no profiles: the
     * keys are not in a store the launcher reads. */
    snprintf(keys, sizeof(keys), "%s" SEP "owned-keys.ini", argv[1]);
    remove(keys);
    start("nes", keys, 1);
    expect(!launcher_binds_kb_profiles_supported(&m, 1), "a host-owned store is not offered profiles");
    expect(launcher_binds_kb_profile_list(&m, names, 8) == 0, "it lists no profile");
    expect(launcher_binds_kb_profile_save_as(&m, 1, "Other", 0) == RUI_KBP_IO &&
           launcher_binds_kb_profile_load(&m, 1, "Default") == RUI_KBP_IO &&
           launcher_binds_kb_profile_rename(&m, "a", "b") == RUI_KBP_IO &&
           launcher_binds_kb_profile_delete(&m, "a") == RUI_KBP_IO,
           "and saves, loads, renames and deletes none");
    {
        char dir[1000];
        snprintf(dir, sizeof(dir), "%s" SEP "nes", g_root);
        expect(!exists(file_in(dir, "Other")), "nothing is written for it");
    }
    expect(!exists(keys), "and no keybinds.ini is made for the runner");

    set_env(RUI_KBP_ENV_ROOT, "");
    if (fails) { fprintf(stderr, "\n%d of %d FAILED\n", fails, checks); return 1; }
    printf("\nlauncher_kb_profiles_test: all %d checks passed\n", checks);
    return 0;
}
