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

    game.disc_selection_locked=1;
    game.artwork_root="portable-artwork-root";
    game.dashboard_quick_controls=1;
    launcher_model_init(&model,&settings,&game,stale);
    if(!expect_path(model.rom_full,stale,"preselected path cannot be replaced by cached media") || !model.disc_selection_locked || model.artwork_root!=game.artwork_root || !model.dashboard_quick_controls)return 13;
    launcher_model_set_rom(&model,rom);
    if(!expect_path(model.rom_full,stale,"locked media cannot be changed"))return 14;

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

    /* Named host shortcuts must work without replacing a legacy game's
     * gameplay binding store. Exercise capture, commit/reload and reset. */
    static const char* labels[] = {"Quick Save", "Quick Load"};
    game.settings_bindings = 0;
    game.assist_binding_labels = labels;
    game.assist_binding_count = 2;
    settings.player_key_bind[0][0] = 17;
    launcher_model_init(&model, &settings, &game, rom);
    launcher_model_begin_assist_capture(&model, 0, false);
    if (!model.capturing || !model.capture_assist || model.capture_pad) return 13;
    launcher_model_set_captured_key(&model, 66);
    launcher_model_cancel_capture(&model);
    const int chord = RECOMP_LAUNCHER_PAD_BUTTON_COMBO((1u << 4) | (1u << 8));
    launcher_model_begin_assist_capture(&model, 1, true);
    if (!model.capturing || !model.capture_assist || !model.capture_pad) return 14;
    launcher_model_set_captured_pad(&model, chord);
    launcher_model_cancel_capture(&model);
    launcher_model_commit(&model, &settings);
    launcher_model_init(&model, &settings, &game, rom);
    if (model.s.assist_key_bind[0] != 66 || model.s.assist_pad_bind[1] != chord ||
        model.s.player_key_bind[0][0] != 17 || model.settings_bindings) return 15;
    launcher_model_set_captured_key(&model, 0); /* no active capture */
    if (model.s.assist_key_bind[0] != 66) return 16;
    launcher_model_begin_assist_capture(&model, -1, false);
    launcher_model_begin_assist_capture(&model, 2, false);
    launcher_model_begin_assist_capture(NULL, 0, false);
    if (model.capturing) return 17;
    model.s.assist_key_bind[0] = 0;
    model.s.assist_pad_bind[1] = 0;
    launcher_model_reset_assist_bindings(&model);
    if (model.s.assist_key_bind[0] != 66 || model.s.assist_pad_bind[1] != chord) return 18;
    return 0;
}
