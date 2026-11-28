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
static void fresh(void) {
    memset(&settings, 0, sizeof(settings));
    memset(&game, 0, sizeof(game));
    game.name = "Accessory fixture";
    game.platform = "NINTENDO 64";
    game.theme = "n64";
    game.num_players = 4;
    launcher_model_init(&model, &settings, &game, "");
}

int main(void) {
    /* Keep the established host-facing flag values unchanged. */
    _Static_assert(RECOMP_LAUNCHER_PAK_TRANSFER == 1, "Transfer Pak host flag");
    _Static_assert(RECOMP_LAUNCHER_PAK_RUMBLE == 2, "Rumble Pak host flag");
    const int transfer = RECOMP_LAUNCHER_PAK_TRANSFER;
    const int rumble = RECOMP_LAUNCHER_PAK_RUMBLE;
    recomp_launcher_set_n64_pak_options(0, NULL);
    fresh();
    EXPECT(launcher_model_pak_ports(&model) == 4);
    EXPECT(launcher_model_n64_pak_options() == (unsigned)transfer);
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == 0);
    launcher_model_set_tpak_rom(&model, 0, "retail.gb");
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == transfer);
    model.s.tpak_enabled[0] = -1;
    EXPECT(!launcher_model_tpak_enabled(&model, 0));
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == 0);

    int choices[RECOMP_LAUNCHER_MAX_TPAKS] = {0};
    recomp_launcher_set_n64_pak_options((unsigned)(transfer | rumble), choices);
    fresh();
    EXPECT(launcher_model_n64_pak_options() == (unsigned)(transfer | rumble));
    for (int p = 0; p < 4; ++p) {
        launcher_model_set_n64_pak_kind(&model, p, rumble);
        EXPECT(launcher_model_n64_pak_kind(&model, p) == rumble);
        EXPECT(!launcher_model_tpak_enabled(&model, p));
    }
    launcher_model_set_n64_pak_kind(&model, 1, 0);
    EXPECT(choices[0] == rumble && choices[1] == 0 && choices[2] == rumble && choices[3] == rumble);
    launcher_model_set_tpak_rom(&model, 0, "late-picker.gb");
    launcher_model_set_tpak_save(&model, 0, "late-picker.sav");
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == rumble);
    EXPECT(!launcher_model_tpak_enabled(&model, 0));
    launcher_model_toggle_tpak(&model, 0);
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == rumble);
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == rumble);
    EXPECT(!model.s.tpak_rom_path[0][0] && !model.s.tpak_save_path[0][0]);
    launcher_model_set_n64_pak_kind(&model, 0, transfer);
    EXPECT(launcher_model_tpak_enabled(&model, 0));
    launcher_model_set_tpak_rom(&model, 0, "picked.gb");
    launcher_model_clear_tpak(&model, 0);
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == transfer);
    launcher_model_set_n64_pak_kind(&model, 0, 0);
    launcher_model_set_tpak_rom(&model, 0, "inactive.gb");
    EXPECT(launcher_model_n64_pak_kind(&model, 0) == 0);
    EXPECT(!launcher_model_tpak_enabled(&model, 0));
    EXPECT(launcher_model_n64_pak_kind(&model, -1) == 0);
    EXPECT(launcher_model_n64_pak_kind(&model, 4) == 0);
    launcher_model_set_n64_pak_kind(&model, 4, rumble);
    EXPECT(choices[0] == 0 && choices[1] == 0 && choices[2] == rumble && choices[3] == rumble);
    EXPECT(launcher_model_n64_pak_kind(NULL, 0) == 0);
    settings = model.s;
    launcher_model_init(&model, &settings, &game, "");
    EXPECT(launcher_model_n64_pak_kind(&model, 2) == rumble);
    game.num_players = 1;
    launcher_model_init(&model, &settings, &game, "");
    EXPECT(launcher_model_pak_ports(&model) == 1);
    launcher_model_set_n64_pak_kind(&model, 1, rumble);
    EXPECT(choices[1] == 0);
    recomp_launcher_set_n64_pak_options(0, NULL);
    printf("Accessory production model PASS: %u checks\n", checks);
    return 0;
}
