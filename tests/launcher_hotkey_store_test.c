/* Actual hotkey writer and reload path, independent of a visible launcher. */
#include "../src/common/launcher_binds.c"

#include <assert.h>

int main(int argc, char **argv) {
    LauncherModel model;
    assert(argc == 2);
    remove(argv[1]);
    FILE *file = fopen(argv[1], "wb");
    assert(file);
    fputs("[Other]\nKeep = 17\n", file);
    fclose(file);
    g_launcher_config_path = argv[1];
    memset(&model, 0, sizeof model);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_SAVE], "F5"));
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_LOAD], "Shift+F5"));
    launcher_binds_set_hotkey(&model, LNG_HK_QUICK_SAVE, SDLK_F3, SDL_KMOD_CTRL);
    launcher_binds_set_hotkey(&model, LNG_HK_QUICK_LOAD, SDLK_F4, 0);
    memset(&model, 0, sizeof model);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_SAVE], "Ctrl+F3"));
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_LOAD], "F4"));
    long size;
    char *text = read_whole(argv[1], &size);
    assert(text && strstr(text, "Keep = 17") && strstr(text, "QuickSave") && strstr(text, "QuickLoad"));
    free(text);
    /* A conflict remains visible to the player; rebinding does not silently
     * erase the other action. The runtime refuses ambiguous quick actions. */
    launcher_binds_set_hotkey(&model, LNG_HK_QUICK_LOAD, SDLK_F3, SDL_KMOD_CTRL);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_SAVE], model.hotkeys[LNG_HK_QUICK_LOAD]));
    launcher_binds_set_hotkey(&model, LNG_HK_QUICK_SAVE, 0, 0);
    launcher_binds_set_hotkey(&model, LNG_HK_QUICK_LOAD, 0, 0);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_SAVE], "(unbound)"));
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_LOAD], "(unbound)"));
    remove(argv[1]);
    puts("PASS: quick hotkey defaults, persistence, conflict retention and unbind");
    return 0;
}
