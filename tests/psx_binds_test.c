#include "consoles/psx/psx_binds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_CIRCLE = 5,
    TEST_CROSS = 6,
    TEST_KEY_S = 22,   /* SDL_SCANCODE_S: Circle's default key */
    TEST_MOUSE1 = 513
};

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

    remove(path);
    puts("PSX dual-bind persistence tests passed");
    return 0;
}
