/* The real ordinary hotkey writer/reload path, without a launcher window. */
#include "../src/common/launcher_binds.c"
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    LauncherModel model;
    FILE *file = fopen(argv[1], "wb");
    assert(file);
    fputs("[Other]\nKeep = 17\n", file);
    fclose(file);
    g_launcher_config_path = argv[1];
    memset(&model, 0, sizeof model);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_SCREENSHOT], "PrintScreen"));
#if SDL_MAJOR_VERSION >= 3
    int control = SDL_KMOD_CTRL;
#else
    int control = KMOD_CTRL;
#endif
    launcher_binds_set_hotkey(&model, LNG_HK_SCREENSHOT, SDLK_F3, control);
    memset(&model, 0, sizeof model);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_SCREENSHOT], "Ctrl+F3"));
    long size;
    char *text = read_whole(argv[1], &size);
    assert(text && strstr(text, "Keep = 17") && strstr(text, "Screenshot = Ctrl+F3"));
    free(text);
    launcher_binds_set_hotkey(&model, LNG_HK_SCREENSHOT, 0, 0);
    memset(&model, 0, sizeof model);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_SCREENSHOT], "(unbound)"));
    assert(!strcmp(model.hotkeys[LNG_HK_FULLSCREEN], "Alt+Return"));
    remove(argv[1]);
    puts("PASS: screenshot default, combo persistence and saved unbound");
    return 0;
}
