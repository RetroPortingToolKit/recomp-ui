/* Named keyboard profiles (consoles/psx/psx_kb_profiles.h): save as, load,
 * rename, delete, the list, and the built-in "Default" that none of them may
 * change.
 *
 * Every call names its folder, a scratch folder of the build tree. The folder
 * of the user (rui_psx_kb_profiles_dir) is asked for, never read or written.
 */
#include "consoles/psx/psx_binds.h"
#include "consoles/psx/psx_kb_profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

/* Rebind-spec indices (psx_binds.c) and SDL scancodes. */
enum {
    B_UP = 0, B_DOWN = 1, B_LEFT = 2, B_RIGHT = 3, B_TRIANGLE = 4, B_CIRCLE = 5,
    B_CROSS = 6, B_SQUARE = 7, B_BUTTONS = 24,
    KEY_A = 4, KEY_D = 7, KEY_F = 9, KEY_S = 22, KEY_W = 26, KEY_X = 27,
    KEY_SPACE = 44, KEY_UP = 82, MOUSE1 = 513
};

#define NAMES 16
typedef char Names[NAMES][RUI_PSX_KB_PROFILE_NAME_MAX + 1];

static int fails, checks;
static char g_dir[900], g_keys[1024];

static void expect(int cond, const char* what) {
    ++checks;
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static void expect_int(int got, int want, const char* what) {
    ++checks;
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
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

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "FAIL: cannot write %s\n", path); ++fails; return; }
    fputs(text, f);
    fclose(f);
}

static int file_contains(const char* path, const char* needle) {
    char text[8192];
    FILE* f = fopen(path, "r");
    size_t n;
    if (!f) return 0;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

static int key(int player, int b) { return rui_psx_binds_get_slot(g_keys, player, b, 0); }
static int alt(int player, int b) { return rui_psx_binds_get_slot(g_keys, player, b, 1); }

/* What every profile of this test is told apart by. */
static void expect_layout(int player, int up, int cross, int cross_alt, const char* what) {
    char line[160];
    snprintf(line, sizeof(line), "%s: Up", what);
    expect_int(key(player, B_UP), up, line);
    snprintf(line, sizeof(line), "%s: Cross", what);
    expect_int(key(player, B_CROSS), cross, line);
    snprintf(line, sizeof(line), "%s: Cross, alternate", what);
    expect_int(alt(player, B_CROSS), cross_alt, line);
}

static void set_wasd(int player) {
    rui_psx_binds_set_slot(g_keys, player, B_UP, 0, KEY_W);
    rui_psx_binds_set_slot(g_keys, player, B_DOWN, 0, KEY_S);
    rui_psx_binds_set_slot(g_keys, player, B_LEFT, 0, KEY_A);
    rui_psx_binds_set_slot(g_keys, player, B_RIGHT, 0, KEY_D);
    rui_psx_binds_set_slot(g_keys, player, B_CROSS, 0, KEY_SPACE);
    rui_psx_binds_set_slot(g_keys, player, B_CROSS, 1, MOUSE1);
}

static int list(Names names) { return rui_psx_kb_profiles_list(g_dir, names, NAMES); }

static const char* in_use(int player) {
    static char name[RUI_PSX_KB_PROFILE_NAME_MAX + 1];
    rui_psx_kb_profile_in_use(g_dir, g_keys, player, name, (int)sizeof(name));
    return name;
}

static int save_as(const char* name, int player, int replace) {
    return rui_psx_kb_profile_save_as(g_dir, name, g_keys, player, replace);
}
static int load(const char* name, int player) {
    return rui_psx_kb_profile_load(g_dir, name, g_keys, player);
}

static void test_empty_folder(void) {
    Names names;
    expect_int(list(names), 1, "a folder that is not there lists one profile");
    expect(!strcmp(names[0], "Default"), "and it is Default");
    expect(!strcmp(in_use(0), "Default"), "a new keybinds.ini has the Default keys in use");
    expect_int(load("WASD", 0), RUI_PSX_KBP_NOT_FOUND, "a profile that is not there does not load");
    expect_int(rui_psx_kb_profile_delete(g_dir, "WASD"), RUI_PSX_KBP_NOT_FOUND,
               "nor delete");
    expect_int(rui_psx_kb_profile_rename(g_dir, "WASD", "Other"), RUI_PSX_KBP_NOT_FOUND,
               "nor rename");
}

static void test_save_as(void) {
    Names names;
    set_wasd(0);
    expect(!strcmp(in_use(0), ""), "changed keys are no profile yet");
    expect_int(save_as("WASD", 0, 0), RUI_PSX_KBP_OK, "save as WASD");
    expect(exists(file_of("WASD")), "the profile is the file WASD.ini of the folder");
    expect(file_contains(file_of("WASD"), "[player1]\nup        = W\n"),
           "a plain text file: a [player1] section of key names");
    expect(file_contains(file_of("WASD"), "cross     = Space, Mouse1\n"),
           "with the alternate key after a comma");
    expect(!file_contains(file_of("WASD"), "[player2]"), "and one player's keys only");
    expect_int(list(names), 2, "the list has two profiles");
    expect(!strcmp(names[0], "Default") && !strcmp(names[1], "WASD"), "Default, then WASD");
    expect(!strcmp(in_use(0), "WASD"), "the keys in use are now the profile WASD");
    expect(!strcmp(in_use(1), "Default"), "Player 2 still has the Default keys");

    /* A profile saved from Player 3's seat is the same kind of file. */
    set_wasd(2);
    rui_psx_binds_set_slot(g_keys, 2, B_CROSS, 0, KEY_F);
    expect_int(save_as("From P3", 2, 0), RUI_PSX_KBP_OK, "save as from Player 3's seat");
    expect(file_contains(file_of("From P3"), "[player1]") &&
           !file_contains(file_of("From P3"), "[player3]"),
           "it is written as [player1] too, so it loads onto any player");
    rui_psx_binds_reset(g_keys, 2);
}

static void test_load(void) {
    expect_int(load("Default", 0), RUI_PSX_KBP_OK, "load Default");
    expect_layout(0, KEY_UP, KEY_X, 0, "Default on Player 1");
    expect(!strcmp(in_use(0), "Default"), "the keys in use are Default again");
    expect(file_contains(g_keys, "[player1]\nup        = Up\n"), "and keybinds.ini says so");

    expect_int(load("WASD", 0), RUI_PSX_KBP_OK, "load WASD");
    expect_layout(0, KEY_W, KEY_SPACE, MOUSE1, "WASD on Player 1");
    expect(file_contains(g_keys, "[player1]\nup        = W\n"),
           "the loaded keys are in keybinds.ini, which the game reads");

    expect_int(load("WASD", 1), RUI_PSX_KBP_OK, "load WASD onto Player 2");
    expect_layout(1, KEY_W, KEY_SPACE, MOUSE1, "WASD on Player 2");
    expect_layout(2, KEY_UP, KEY_X, 0, "Player 3 is untouched");
    expect_int(load("From P3", 0), RUI_PSX_KBP_OK, "load Player 3's profile onto Player 1");
    expect_layout(0, KEY_W, KEY_F, MOUSE1, "Player 3's profile on Player 1");
    expect_int(load("wasd", 0), RUI_PSX_KBP_OK, "a name is found whatever its letter case");
    expect_layout(0, KEY_W, KEY_SPACE, MOUSE1, "wasd on Player 1");
    expect_int(load("Default", 1), RUI_PSX_KBP_OK, "Player 2 back to Default");
}

static void test_save_over(void) {
    Names names;
    /* Player 1 has WASD. Change one key, then save over the profile. */
    rui_psx_binds_set_slot(g_keys, 0, B_SQUARE, 0, KEY_F);
    expect(!strcmp(in_use(0), ""), "one changed key: no profile holds these keys");
    expect_int(save_as("WASD", 0, 0), RUI_PSX_KBP_EXISTS,
               "save as a name that is there is refused without the word to replace");
    expect(file_contains(file_of("WASD"), "square    = Z\n"), "and the profile is as it was");
    expect_int(save_as("wasd", 0, 0), RUI_PSX_KBP_EXISTS, "another letter case is the same name");
    expect_int(save_as("wasd", 0, 1), RUI_PSX_KBP_OK, "with the word to replace it is saved");
    expect_int(list(names), 3, "still one WASD in the list");
    expect(!strcmp(names[2], "WASD"), "under the spelling it had");
    expect(file_contains(file_of("WASD"), "square    = F\n"), "with the new key");
    expect(!strcmp(in_use(0), "WASD"), "and the keys in use are WASD again");
}

static void test_rename(void) {
    Names names;
    expect_int(rui_psx_kb_profile_rename(g_dir, "WASD", "From P3"), RUI_PSX_KBP_EXISTS,
               "rename to another profile's name is refused");
    expect_int(rui_psx_kb_profile_rename(g_dir, "WASD", "from p3"), RUI_PSX_KBP_EXISTS,
               "in any letter case");
    expect(file_contains(file_of("WASD"), "cross     = Space, Mouse1\n") &&
           file_contains(file_of("From P3"), "cross     = F, Mouse1\n"),
           "and both profiles are as they were");
    expect_int(rui_psx_kb_profile_rename(g_dir, "WASD", "Arcade"), RUI_PSX_KBP_OK,
               "rename WASD to Arcade");
    expect(!exists(file_of("WASD")) && exists(file_of("Arcade")), "the file has the new name");
    expect_int(list(names), 3, "the list still has three profiles");
    expect(!strcmp(names[1], "Arcade") && !strcmp(names[2], "From P3"),
           "Default, Arcade, From P3");
    expect_int(load("WASD", 1), RUI_PSX_KBP_NOT_FOUND, "the old name is gone");
    expect_int(load("Arcade", 1), RUI_PSX_KBP_OK, "the new name loads");
    expect_layout(1, KEY_W, KEY_SPACE, MOUSE1, "the renamed profile holds the same keys");
    expect_int(key(1, B_SQUARE), KEY_F, "the renamed profile holds the same keys: Square");
    expect(!strcmp(in_use(0), "Arcade"), "the keys in use follow the new name");
    expect_int(rui_psx_kb_profile_rename(g_dir, "arcade", "ARCADE"), RUI_PSX_KBP_OK,
               "a rename may change the letter case alone");
    expect_int(list(names), 3, "which is still three profiles");
    expect(!strcmp(names[1], "ARCADE"), "with the new spelling");
    expect_int(rui_psx_kb_profile_rename(g_dir, "ARCADE", "Arcade"), RUI_PSX_KBP_OK, "and back");
    expect_int(load("Default", 1), RUI_PSX_KBP_OK, "Player 2 back to Default");
}

static void test_delete(void) {
    Names names;
    expect_int(rui_psx_kb_profile_delete(g_dir, "from p3"), RUI_PSX_KBP_OK, "delete From P3");
    expect(!exists(file_of("From P3")), "its file is gone");
    expect_int(rui_psx_kb_profile_delete(g_dir, "Arcade"), RUI_PSX_KBP_OK, "delete Arcade");
    expect_int(list(names), 1, "the list is Default alone again");
    expect_layout(0, KEY_W, KEY_SPACE, MOUSE1, "deleting a profile leaves the keys in use");
    expect(!strcmp(in_use(0), ""), "which no profile holds any more");
    expect_int(rui_psx_kb_profile_delete(g_dir, "Arcade"), RUI_PSX_KBP_NOT_FOUND,
               "a deleted profile cannot be deleted again");
}

static void test_default_is_protected(void) {
    static const char* const spellings[] = { "Default", "default", "DEFAULT", "dEfAuLt" };
    Names names;
    /* Player 1 has keys that are not the default map. */
    for (int i = 0; i < 4; ++i) {
        char what[96];
        snprintf(what, sizeof(what), "save as \"%s\" is refused", spellings[i]);
        expect_int(save_as(spellings[i], 0, 1), RUI_PSX_KBP_PROTECTED, what);
        snprintf(what, sizeof(what), "delete \"%s\" is refused", spellings[i]);
        expect_int(rui_psx_kb_profile_delete(g_dir, spellings[i]), RUI_PSX_KBP_PROTECTED, what);
        snprintf(what, sizeof(what), "rename \"%s\" is refused", spellings[i]);
        expect_int(rui_psx_kb_profile_rename(g_dir, spellings[i], "Mine"), RUI_PSX_KBP_PROTECTED, what);
        snprintf(what, sizeof(what), "check of the name \"%s\" says protected", spellings[i]);
        expect_int(rui_psx_kb_profile_check_name(spellings[i]), RUI_PSX_KBP_PROTECTED, what);
    }
    expect(!exists(file_of("Default")) && !exists(file_of("default")) && !exists(file_of("Mine")),
           "none of them wrote a file");
    expect_int(save_as("Mine", 0, 0), RUI_PSX_KBP_OK, "a profile Mine");
    expect_int(rui_psx_kb_profile_rename(g_dir, "Mine", "Default"), RUI_PSX_KBP_PROTECTED,
               "no profile can be renamed to Default");
    expect_int(rui_psx_kb_profile_rename(g_dir, "Mine", "default"), RUI_PSX_KBP_PROTECTED,
               "in any letter case");
    expect(exists(file_of("Mine")), "and it keeps its name");

    /* A file named Default.ini put into the folder by hand is not the Default. */
    write_text(file_of("Default"), "[player1]\nup = F\ncross = F\n");
    expect_int(list(names), 2, "a Default.ini in the folder adds nothing to the list");
    expect(!strcmp(names[0], "Default") && !strcmp(names[1], "Mine"), "Default once, then Mine");
    expect_int(load("Default", 0), RUI_PSX_KBP_OK, "load Default with that file there");
    expect_layout(0, KEY_UP, KEY_X, 0, "Default is still the built-in map");
    expect(!strcmp(in_use(0), "Default"), "and is named as the keys in use");
    expect_int(rui_psx_kb_profile_delete(g_dir, "Default"), RUI_PSX_KBP_PROTECTED,
               "delete Default is refused with that file there");
    expect(exists(file_of("Default")), "and the file is left alone");
    remove(file_of("Default"));
    expect_int(rui_psx_kb_profile_delete(g_dir, "Mine"), RUI_PSX_KBP_OK, "Mine deleted");
}

static void test_names(void) {
    static const char* const bad[] = {
        "", " ", " lead", "trail ", ".hidden", "dot.", "a/b", "a\\b", "..", "..\\escape",
        "../escape", "a:b", "a*b", "a?b", "a\"b", "a<b", "a|b", "tab\there", "CON", "nul",
        "Com1", "LPT9", "nul.txt", "caf\xC3\xA9",
        "123456789012345678901234567890123",   /* 33 characters */
    };
    static const char* const good[] = {
        "a", "WASD", "Arcade", "Left hand (v2)", "pad-like_keys+mouse", "v1.2", "console",
        "12345678901234567890123456789012",    /* 32 characters */
    };
    char outside[1024];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char what[128];
        snprintf(what, sizeof(what), "\"%s\" is not a name", bad[i]);
        expect_int(save_as(bad[i], 0, 1), RUI_PSX_KBP_BAD_NAME, what);
    }
    snprintf(outside, sizeof(outside), "%s" SEP ".." SEP "escape.ini", g_dir);
    expect(!exists(outside), "a name with a path in it wrote nothing outside the folder");
    expect_int(rui_psx_kb_profile_rename(g_dir, "..\\escape", "In"), RUI_PSX_KBP_BAD_NAME,
               "nor is a path taken as the profile to rename");
    expect_int(rui_psx_kb_profile_delete(g_dir, "../escape"), RUI_PSX_KBP_BAD_NAME, "or to delete");
    expect_int(load("../escape", 0), RUI_PSX_KBP_BAD_NAME, "or to load");
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        char what[128];
        snprintf(what, sizeof(what), "\"%s\" is a name", good[i]);
        expect_int(save_as(good[i], 0, 0), RUI_PSX_KBP_OK, what);
        snprintf(what, sizeof(what), "\"%s\" can be deleted again", good[i]);
        expect_int(rui_psx_kb_profile_delete(g_dir, good[i]), RUI_PSX_KBP_OK, what);
    }
    expect_int(save_as("Keep", 0, 0), RUI_PSX_KBP_OK, "a profile Keep");
    expect_int(rui_psx_kb_profile_rename(g_dir, "Keep", "a/b"), RUI_PSX_KBP_BAD_NAME,
               "a profile cannot be renamed to what is not a name");
    expect(exists(file_of("Keep")), "and it keeps its name");
    expect_int(rui_psx_kb_profile_delete(g_dir, "Keep"), RUI_PSX_KBP_OK, "Keep deleted");
}

static void test_a_file_put_into_the_folder(void) {
    Names names;
    /* A profile is a plain file: one written by hand, or shipped with a game
     * and copied here, is listed and loads. Keys it leaves out are Default's. */
    write_text(file_of("Shipped"),
               "# a layout shipped with a game\n[player1]\nup = W\ndown = S\nleft = A\n"
               "right = D\ncross = Space, Mouse1\n");
    write_text(file_of("notes"), "[general]\nvolume = 7\n");
    {
        char other[1024];
        snprintf(other, sizeof(other), "%s" SEP "readme.txt", g_dir);
        write_text(other, "not a profile\n");
        expect_int(list(names), 3, "the folder lists its .ini files and no other file");
        remove(other);
    }
    expect(!strcmp(names[1], "notes") && !strcmp(names[2], "Shipped"), "in alphabetical order");
    expect_int(load("Default", 0), RUI_PSX_KBP_OK, "Player 1 on Default");
    expect_int(load("Shipped", 0), RUI_PSX_KBP_OK, "a file put into the folder loads");
    expect_layout(0, KEY_W, KEY_SPACE, MOUSE1, "the shipped layout on Player 1");
    expect_int(key(0, B_TRIANGLE), KEY_A, "a key it leaves out is the Default's");
    expect(!strcmp(in_use(0), "Shipped"), "and it is named as the keys in use");
    expect_int(load("notes", 0), RUI_PSX_KBP_NOT_A_PROFILE,
               "an .ini that holds no keys is refused");
    expect_layout(0, KEY_W, KEY_SPACE, MOUSE1, "and the keys are unchanged");
    remove(file_of("Shipped"));
    remove(file_of("notes"));
}

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value[0]) setenv(name, value, 1); else unsetenv(name);
#endif
}

static void test_folder(void) {
    char dir[1024];
    set_env("PSXRECOMP_KEYBOARD_PROFILES_DIR", g_dir);
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 1 && !strcmp(dir, g_dir),
           "PSXRECOMP_KEYBOARD_PROFILES_DIR names the folder");
    set_env("PSXRECOMP_KEYBOARD_PROFILES_DIR", "");
#ifdef _WIN32
    set_env("APPDATA", "C:\\Users\\someone\\AppData\\Roaming");
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 1 &&
           !strcmp(dir, "C:\\Users\\someone\\AppData\\Roaming\\psxrecomp\\keyboard-profiles"),
           "else it is psxrecomp\\keyboard-profiles under %APPDATA%");
    set_env("APPDATA", "");
#elif defined(__APPLE__)
    set_env("HOME", "/Users/someone");
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 1 &&
           !strcmp(dir, "/Users/someone/Library/Application Support/psxrecomp/keyboard-profiles"),
           "else it is psxrecomp/keyboard-profiles under Application Support");
    set_env("HOME", "");
#else
    set_env("XDG_CONFIG_HOME", "/home/someone/cfg");
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 1 &&
           !strcmp(dir, "/home/someone/cfg/psxrecomp/keyboard-profiles"),
           "else it is psxrecomp/keyboard-profiles under $XDG_CONFIG_HOME");
    set_env("XDG_CONFIG_HOME", "");
    set_env("HOME", "/home/someone");
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 1 &&
           !strcmp(dir, "/home/someone/.config/psxrecomp/keyboard-profiles"),
           "or under ~/.config");
    set_env("HOME", "");
#endif
    expect(rui_psx_kb_profiles_dir(dir, (int)sizeof(dir)) == 0 && dir[0] == '\0',
           "a system that names no settings folder has no profile folder");
    expect_int(rui_psx_kb_profile_save_as("", "Nowhere", g_keys, 0, 0), RUI_PSX_KBP_IO,
               "and save as says it could not write");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: psx_kb_profiles_test <scratch-folder>\n");
        return 2;
    }
    /* The profile folder is two levels under the scratch folder and is not
     * there yet: the first save as makes it. */
    snprintf(g_keys, sizeof(g_keys), "%s" SEP "kb-profiles-keybinds.ini", argv[1]);
    snprintf(g_dir, sizeof(g_dir), "%s" SEP "kb-profiles" SEP "shared", argv[1]);
    {
        /* What an earlier run that stopped half way left behind. */
        Names names;
        for (int guard = 0; guard < 64 && list(names) > 1; ++guard) remove(file_of(names[1]));
        remove(file_of("Default"));
        remove(g_keys);
    }
    rui_psx_binds_init(g_keys);

    test_empty_folder();
    test_save_as();
    test_load();
    test_save_over();
    test_rename();
    test_delete();
    test_default_is_protected();
    test_names();
    test_a_file_put_into_the_folder();
    test_folder();

    remove(g_keys);
    if (fails) { fprintf(stderr, "\n%d of %d FAILED\n", fails, checks); return 1; }
    printf("\npsx_kb_profiles_test: all %d checks passed\n", checks);
    return 0;
}
