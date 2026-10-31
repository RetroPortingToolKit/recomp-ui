/* The launcher's PSX keybinds writer keeps sections it does not own
 * (PS1B-305): the runtime keeps the GunCon controls in keybinds.ini
 * [guncon], and a rebind in the launcher rewrites the file. */
#include "consoles/psx/psx_binds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_CROSS = 6, TEST_KEY_K = 14 };

static void require(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static long read_text(const char *path, char *text, size_t cap)
{
    FILE *file = fopen(path, "r");
    size_t size;
    if (!file) return -1;
    size = fread(text, 1, cap - 1, file);
    fclose(file);
    text[size] = '\0';
    return (long)size;
}

static int count(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) ++n;
    return n;
}

int main(int argc, char **argv)
{
    static char first[16384], second[16384];
    const char *path;
    FILE *file;
    if (argc != 2) {
        fprintf(stderr, "usage: psx_binds_sections_test <temporary-ini>\n");
        return 2;
    }
    path = argv[1];
    remove(path);

    /* A runtime-written file: player sections, then [guncon] with its notes
     * inside the section, then a section a newer runtime might add. */
    file = fopen(path, "w");
    require(file != NULL, "can write the temporary ini");
    fputs("# runtime header\n\n"
          "[player1]\ncross     = X\ncircle    = S\n\n"
          "[guncon]\n"
          "# GunCon light gun notes\n"
          "trigger        = Mouse1\n"
          "a              = Mouse3, A\n"
          "offscreen_shot = W\n\n"
          "[future]\nkey = value\n", file);
    fclose(file);

    rui_psx_binds_init(path);
    rui_psx_binds_set_slot(path, 0, TEST_CROSS, 0, TEST_KEY_K);
    require(read_text(path, first, sizeof(first)) > 0, "the rebind wrote the file");
    require(strstr(first, "cross     = K") != NULL, "the rebind is on disk");
    require(strstr(first, "[guncon]\n# GunCon light gun notes\ntrigger        = Mouse1\n"
                          "a              = Mouse3, A\noffscreen_shot = W\n") != NULL,
            "[guncon] survives the rebind, notes and all");
    require(strstr(first, "[future]\nkey = value\n") != NULL,
            "an unknown section survives the rebind");
    require(count(first, "[player1]") == 1, "one [player1] section");
    require(count(first, "runtime header") == 0,
            "the header comment is the launcher's own");

    /* Writing again changes nothing: the kept text does not grow. */
    rui_psx_binds_save(path);
    require(read_text(path, second, sizeof(second)) > 0, "the save wrote the file");
    require(strcmp(first, second) == 0, "a second write is identical");
    require(count(second, "[guncon]") == 1, "[guncon] is not duplicated");

    remove(path);
    puts("PSX keybinds foreign-section tests passed");
    return 0;
}
