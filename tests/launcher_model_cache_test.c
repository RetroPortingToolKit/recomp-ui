#include "launcher_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#define mkdir1(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir1(path) mkdir(path, 0777)
#endif

static int zapper_writes;
void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    ++zapper_writes;
    (void)mouse_enabled;
    (void)crosshair;
}

static int write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    if (fputs(text, f) < 0) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int expect_path(const char* got, const char* want, const char* label) {
    if (strcmp(got ? got : "", want ? want : "") == 0)
        return 1;
    fprintf(stderr, "%s: got '%s', want '%s'\n", label,
            got ? got : "(null)", want ? want : "(null)");
    return 0;
}

int main(int argc, char** argv) {
    const char* root = argc > 1 ? argv[1] : "launcher-model-cache-test";
    char rom[512];
    char stale[512];
    LauncherModel model;
    RecompLauncherCSettings settings;
    RecompLauncherCGameInfo game;

    mkdir1(root);
    if (chdir(root) != 0) {
        perror("chdir");
        return 2;
    }

    snprintf(rom, sizeof(rom), "%s", "picked.sfc");
    snprintf(stale, sizeof(stale), "%s", "missing.sfc");
    remove("rom.cfg");
    remove("disc.cfg");
    remove(rom);
    remove(stale);

    if (!write_text(rom, "rom") || !write_text("rom.cfg", rom)) {
        fprintf(stderr, "failed to create rom/cache fixtures\n");
        return 3;
    }

    memset(&settings, 0, sizeof(settings));
    memset(&game, 0, sizeof(game));
    game.name = "Cache Test";
    game.region = "Test";

    launcher_model_init(&model, &settings, &game, stale);
    if (!expect_path(launcher_model_rom_path(&model), rom,
                     "stale initial_rom should fall back to rom.cfg"))
        return 4;

    if (!write_text("rom.cfg", stale)) {
        fprintf(stderr, "failed to rewrite stale cache fixture\n");
        return 5;
    }
    launcher_model_init(&model, &settings, &game, "");
    if (!expect_path(launcher_model_rom_path(&model), "",
                     "stale rom.cfg should be ignored"))
        return 6;

    if (!write_text("disc.cfg", rom)) {
        fprintf(stderr, "failed to write disc.cfg fixture\n");
        return 7;
    }
    launcher_model_init(&model, &settings, &game, "");
    if (!expect_path(launcher_model_rom_path(&model), rom,
                     "disc.cfg should be fallback cache"))
        return 8;

    launcher_model_init(&model, &settings, &game, rom);
    if (!expect_path(launcher_model_rom_path(&model), rom,
                     "valid initial_rom should win"))
        return 9;

    /* A host that owns its bindings must receive Zapper edits through the
     * settings object. A legacy host still uses its native keybind writer. */
    model.settings_bindings = true;
    model.zapper = true;
    model.zapper_mouse = model.zapper_crosshair = true;
    launcher_model_toggle_zapper_mouse(&model);
    launcher_model_toggle_zapper_crosshair(&model);
    if (model.s.zapper_mouse != -1 || model.s.zapper_crosshair != -1 || zapper_writes) return 10;
    launcher_model_toggle_zapper_mouse(&model);
    launcher_model_toggle_zapper_crosshair(&model);
    if (model.s.zapper_mouse != 1 || model.s.zapper_crosshair != 1 || zapper_writes) return 11;
    model.settings_bindings = false;
    launcher_model_toggle_zapper_mouse(&model);
    launcher_model_toggle_zapper_crosshair(&model);
    if (zapper_writes != 2) return 12;
    return 0;
}
