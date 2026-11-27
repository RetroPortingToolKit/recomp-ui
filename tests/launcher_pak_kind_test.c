/* Exercise the production accessory model without opening a window. */
#include "launcher_model.c"
#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }
static unsigned checks;
#define EXPECT(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static LauncherModel model;
static RecompLauncherCSettings settings;
static RecompLauncherCGameInfo game;
static void fresh(unsigned mask) {
    memset(&settings, 0, sizeof(settings));
    memset(&game, 0, sizeof(game));
    game.name = "Accessory fixture";
    game.platform = "NINTENDO 64";
    game.theme = "n64";
    game.num_players = 4;
    game.pak_kind_mask = mask;
    launcher_model_init(&model, &settings, &game, "");
}

int main(void) {
    const int none = RECOMP_LAUNCHER_PAK_NONE;
    const int transfer = RECOMP_LAUNCHER_PAK_TRANSFER;
    const int rumble = RECOMP_LAUNCHER_PAK_RUMBLE;
    const unsigned all = (1u << none) | (1u << transfer) | (1u << rumble);
    fresh(0);
    EXPECT(launcher_model_pak_ports(&model) == 4);
    EXPECT(launcher_model_pak_kind_supported(&model, none));
    EXPECT(launcher_model_pak_kind_supported(&model, transfer));
    EXPECT(!launcher_model_pak_kind_supported(&model, rumble));
    EXPECT(!launcher_model_pak_kind_supported(&model, -1));
    EXPECT(!launcher_model_pak_kind_supported(&model, 99));
    EXPECT(launcher_model_pak_kind(&model, 0) == none);
    launcher_model_set_tpak_rom(&model, 0, "retail.gb");
    EXPECT(launcher_model_pak_kind(&model, 0) == transfer);
    EXPECT(model.s.pak_kind[0] == RECOMP_LAUNCHER_PAK_UNSET);
    model.s.tpak_enabled[0] = -1;
    EXPECT(launcher_model_pak_kind(&model, 0) == none);
    model.s.tpak_enabled[0] = 1;
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_set_pak_kind(&model, 0, rumble);
    EXPECT(model.s.pak_kind[0] == RECOMP_LAUNCHER_PAK_UNSET);
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == none);
    EXPECT(model.s.pak_kind[0] == RECOMP_LAUNCHER_PAK_UNSET);

    fresh(all);
    for (int p = 0; p < 4; ++p) {
        launcher_model_set_pak_kind(&model, p, rumble);
        EXPECT(launcher_model_pak_kind(&model, p) == rumble);
        EXPECT(!launcher_model_tpak_enabled(&model, p));
        EXPECT(model.s.tpak_enabled[p] == -1);
    }
    launcher_model_set_pak_kind(&model, 1, none);
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    EXPECT(launcher_model_pak_kind(&model, 1) == none);
    EXPECT(launcher_model_pak_kind(&model, 2) == rumble);
    EXPECT(launcher_model_pak_kind(&model, 3) == rumble);
    launcher_model_set_tpak_rom(&model, 0, "late-picker.gb");
    launcher_model_set_tpak_save(&model, 0, "late-picker.sav");
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    EXPECT(!launcher_model_tpak_enabled(&model, 0));
    launcher_model_toggle_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    EXPECT(!model.s.tpak_rom_path[0][0] && !model.s.tpak_save_path[0][0]);
    launcher_model_set_pak_kind(&model, 0, transfer);
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_set_tpak_rom(&model, 0, "picked.gb");
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_toggle_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == none);
    EXPECT(!launcher_model_tpak_enabled(&model, 0));
    launcher_model_toggle_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == transfer);
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_pak_kind(&model, 0) == transfer);
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_set_pak_kind(&model, 0, none);
    launcher_model_set_tpak_rom(&model, 0, "inactive.gb");
    EXPECT(launcher_model_pak_kind(&model, 0) == none);
    EXPECT(!launcher_model_tpak_enabled(&model, 0));

    fresh((1u << none) | (1u << rumble));
    EXPECT(model.pak_kind_mask == ((1u << none) | (1u << rumble)));
    EXPECT(!launcher_model_pak_kind_supported(&model, transfer));
    launcher_model_set_pak_kind(&model, 0, rumble);
    launcher_model_set_pak_kind(&model, 0, transfer);
    launcher_model_set_tpak_rom(&model, 0, "unsupported.gb");
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    EXPECT(!model.s.tpak_rom_path[0][0]);
    model.s.pak_kind[1] = transfer;
    model.s.tpak_enabled[1] = 1;
    strcpy(model.s.tpak_rom_path[1], "old.gb");
    EXPECT(launcher_model_pak_kind(&model, 1) == none);
    EXPECT(!launcher_model_tpak_enabled(&model, 1));
    model.s.pak_kind[1] = 99;
    EXPECT(launcher_model_pak_kind(&model, 1) == none);
    EXPECT(launcher_model_pak_kind(&model, -1) == none);
    EXPECT(launcher_model_pak_kind(&model, 4) == none);
    launcher_model_set_pak_kind(&model, 4, rumble);
    EXPECT(launcher_model_pak_kind(NULL, 0) == none);
    EXPECT(!launcher_model_pak_kind_supported(NULL, rumble));

    settings = model.s;
    settings.pak_kind[1] = none;
    settings.pak_kind[2] = rumble;
    settings.pak_kind[3] = none;
    launcher_model_init(&model, &settings, &game, "");
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    EXPECT(launcher_model_pak_kind(&model, 1) == none);
    EXPECT(launcher_model_pak_kind(&model, 2) == rumble);
    EXPECT(launcher_model_pak_kind(&model, 3) == none);
    game.num_players = 1;
    for (int p = 0; p < 4; ++p) settings.pak_kind[p] = rumble;
    launcher_model_init(&model, &settings, &game, "");
    EXPECT(launcher_model_pak_ports(&model) == 1);
    EXPECT(launcher_model_pak_kind(&model, 0) == rumble);
    for (int p = 1; p < 4; ++p) EXPECT(model.s.pak_kind[p] == rumble);
    launcher_model_set_pak_kind(&model, 1, none);
    EXPECT(model.s.pak_kind[1] == rumble);
    printf("Accessory production model PASS: %u checks\n", checks);
    return 0;
}
