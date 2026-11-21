/* Dashboard ROM selections must survive both Play and Quit. No cache is
 * pre-seeded: exercise selection -> commit -> a fresh launcher model. */
#include "launcher_model.h"
#include "launcher_system.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static void require(int ok, const char* label) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", label); exit(1); }
}

static void write_text(const char* path, const char* body) {
    FILE* f = fopen(path, "wb");
    require(f != NULL, "create fixture");
    require(fputs(body, f) >= 0, "write fixture");
    require(fclose(f) == 0, "close fixture");
}

int main(int argc, char** argv) {
    require(argc == 2, "test directory argument");
    char rom[1024], other[1024], bad[1024], cache[1024], saved[1024];
    snprintf(rom, sizeof(rom), "%s/External ROM (USA).sfc", argv[1]);
    snprintf(other, sizeof(other), "%s/Replacement ROM.sfc", argv[1]);
    snprintf(bad, sizeof(bad), "%s/Invalid ROM.sfc", argv[1]);
    snprintf(cache, sizeof(cache), "%s/selected-rom.cfg", argv[1]);
#if defined(_WIN32)
    /* The cache canonicalizes Windows paths returned with forward slashes by
     * CMake, just as it does relative paths supplied by a host. */
    for (char* p = rom; *p; ++p) if (*p == '/') *p = '\\';
    for (char* p = other; *p; ++p) if (*p == '/') *p = '\\';
#endif
    remove(cache);
    write_text(rom, "valid ROM");
    write_text(other, "valid ROM");
    write_text(bad, "invalid ROM");

    RecompLauncherCSettings settings = {0};
    RecompLauncherCGameInfo game = {0};
    game.name = "ROM persistence test";
    game.rom_cache_path = cache;
    game.has_expected_crc = 1;
    game.expected_crc = recompui_crc32_compute((const uint8_t*)"valid ROM", 9);
    LauncherModel* m = calloc(1, sizeof(*m));
    require(m != NULL, "allocate model");
    launcher_model_init(m, &settings, &game, "");
    launcher_model_set_rom(m, rom);
    require(launcher_model_can_launch(m), "valid selection launches");
    /* Effective patched images are temporary; remember the user's source. */
    snprintf(m->rom_patch_prepared_path, sizeof(m->rom_patch_prepared_path),
             "%s/patched-cache.sfc", argv[1]);
    launcher_model_commit(m, &settings);
    FILE* f = fopen(cache, "rb");
    require(f != NULL, "selection creates the configured ROM cache");
    require(fgets(saved, sizeof(saved), f) != NULL, "cache contains a path");
    fclose(f);
    saved[strcspn(saved, "\r\n")] = '\0';
    require(strcmp(saved, rom) == 0, "cache contains source ROM, not patched output");
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), rom) == 0,
            "fresh launcher restores the selected ROM");
    require(launcher_model_can_launch(m), "restored ROM launches");

    launcher_model_set_rom(m, other);
    launcher_model_commit(m, NULL);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), other) == 0,
            "replacement survives commit without settings output");
    launcher_model_set_rom(m, bad);
    require(!launcher_model_can_launch(m), "invalid selection is rejected");
    launcher_model_commit(m, &settings);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), other) == 0,
            "invalid selection preserves the last valid cache");
    launcher_model_set_rom(m, "");
    launcher_model_commit(m, &settings);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), other) == 0,
            "empty selection preserves the last valid cache");

    launcher_model_set_rom(m, rom);
    m->setup_wizard_supported = true;
    launcher_model_commit(m, &settings);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), other) == 0,
            "wizard picks still require explicit confirmation");
    launcher_model_set_rom(m, rom);
    SystemProfile disc_profile = {0};
    disc_profile.verify.mode = 1;
    m->profile = &disc_profile;
    launcher_model_commit(m, &settings);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), other) == 0,
            "disc hosts retain their existing persistence flow");

    require(chdir(argv[1]) == 0, "enter fixture directory");
    launcher_model_set_rom(m, "External ROM (USA).sfc");
    launcher_model_commit(m, &settings);
    launcher_model_init(m, &settings, &game, "");
    require(strcmp(launcher_model_rom_path(m), rom) == 0,
            "relative ROM selection is cached as an absolute path");
    free(m);
    remove(cache); remove(rom); remove(other); remove(bad);
    puts("PASS: selected ROM persists across launcher instances");
    return 0;
}
