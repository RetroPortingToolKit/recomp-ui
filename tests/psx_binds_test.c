#include "consoles/psx/psx_binds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_TRIANGLE = 4,
    TEST_CIRCLE = 5,
    TEST_CROSS = 6,
    TEST_KEY_S = 22,   /* SDL_SCANCODE_S: Circle's default key */
    TEST_KEY_F = 9,    /* SDL_SCANCODE_F */
    TEST_KEY_R = 21,   /* SDL_SCANCODE_R */
    TEST_MOUSE1 = 513
};

static void require(int condition, const char *message);
static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    require(file != NULL, "can write a profile fixture");
    fputs(text, file);
    fclose(file);
}

static void require(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int file_contains(const char *path, const char *needle)
{
    /* This is a text INI assertion; translate Windows CRLF on read. */
    FILE *file = fopen(path, "r");
    char text[8192];
    size_t size;
    if (!file) return 0;
    size = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[size] = '\0';
    return strstr(text, needle) != NULL;
}

int main(int argc, char **argv)
{
    const char *path;
    int primary;
    if (argc != 2) {
        fprintf(stderr, "usage: psx_binds_test <temporary-ini>\n");
        return 2;
    }
    path = argv[1];
    remove(path);

    rui_psx_binds_init(path);
    primary = rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0);
    require(primary != 0, "Cross has a seeded primary binding");
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == 0,
            "Cross alternate starts unbound");

    rui_psx_binds_set_slot(path, 0, TEST_CROSS, 1, TEST_MOUSE1);
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0) == primary,
            "setting alternate preserves primary");
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == TEST_MOUSE1,
            "mouse pseudo-scancode round-trips in memory");
    require(file_contains(path, "Mouse1"),
            "mouse alternate persists by its runtime-compatible name");

    rui_psx_binds_set_slot(path, 0, TEST_CROSS, 0, primary);
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == TEST_MOUSE1,
            "rewriting primary does not erase alternate");
    require(file_contains(path, "Mouse1"),
            "rewriting primary preserves alternate on disk");

    rui_psx_binds_reset(path, 0);
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == 0,
            "reset clears alternate binding");

    /* A key MAY drive more than one input: S is Circle's default, and binding
     * it to Cross keeps it on Circle too (the controls page highlights the
     * duplicate instead of silently unbinding the other input). */
    require(rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 0) == TEST_KEY_S,
            "Circle defaults to S");
    rui_psx_binds_set_slot(path, 0, TEST_CROSS, 0, TEST_KEY_S);
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0) == TEST_KEY_S,
            "Cross now holds S");
    require(rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 0) == TEST_KEY_S,
            "binding S to Cross leaves it on Circle");
    require(file_contains(path, "circle    = S\ncross     = S"),
            "both inputs keep S on disk");
    /* The same key in both slots of ONE input is redundant: that input's
     * other slot is cleared, and nothing else. */
    rui_psx_binds_set_slot(path, 0, TEST_CROSS, 1, TEST_KEY_S);
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == TEST_KEY_S,
            "Cross's alternate holds S");
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0) == 0,
            "S as Cross's alternate clears Cross's primary");
    require(rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 0) == TEST_KEY_S,
            "Circle still holds S");
    /* Right-click clear: scancode 0 unbinds just that slot. */
    rui_psx_binds_set_slot(path, 0, TEST_CIRCLE, 0, 0);
    require(rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 0) == 0,
            "clearing Circle's primary unbinds it");
    require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 1) == TEST_KEY_S,
            "clearing Circle leaves Cross's alternate");
    require(file_contains(path, "circle    = None"),
            "the cleared Circle key is gone from disk too");

    /* Load Profile: a Player 1 profile with a mouse primary and an alternate. */
    {
        char profile[1024], foreign[1024];
        snprintf(profile, sizeof(profile), "%s.profile.ini", path);
        snprintf(foreign, sizeof(foreign), "%s.foreign.ini", path);
        write_text(profile, "[player1]\ncross     = Mouse1\ntriangle  = R, F\n");
        write_text(foreign, "[general]\nvolume = 7\n");

        require(rui_psx_binds_load_profile(path, 0, profile) == 1,
                "a keybinds profile loads");
        require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0) == TEST_MOUSE1,
                "loaded Cross is Mouse1");
        require(rui_psx_binds_get_slot(path, 0, TEST_TRIANGLE, 0) == TEST_KEY_R &&
                rui_psx_binds_get_slot(path, 0, TEST_TRIANGLE, 1) == TEST_KEY_F,
                "loaded Triangle keeps its alternate");
        require(rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 0) == TEST_KEY_S &&
                rui_psx_binds_get_slot(path, 0, TEST_CIRCLE, 1) == 0,
                "a key the profile omits takes the default");
        require(file_contains(path, "Mouse1"),
                "the loaded profile persists to keybinds.ini");

        /* No [player2] section: Player 2 takes the [player1] map. */
        require(rui_psx_binds_load_profile(path, 1, profile) == 1 &&
                rui_psx_binds_get_slot(path, 1, TEST_CROSS, 0) == TEST_MOUSE1,
                "a Player 1 profile loads onto Player 2");

        /* A foreign file is refused and changes nothing. */
        rui_psx_binds_set_slot(path, 0, TEST_CROSS, 0, TEST_KEY_S);
        require(rui_psx_binds_load_profile(path, 0, foreign) == 0,
                "a foreign ini is refused");
        require(rui_psx_binds_get_slot(path, 0, TEST_CROSS, 0) == TEST_KEY_S &&
                rui_psx_binds_get_slot(path, 1, TEST_CROSS, 0) == TEST_MOUSE1,
                "a refused load leaves every player unchanged");
        require(rui_psx_binds_load_profile(path, 0, "no-such-profile.ini") == 0,
                "a missing file is refused");
        remove(profile);
        remove(foreign);
    }

    remove(path);
    puts("PSX dual-bind persistence tests passed");
    return 0;
}
