/* Actual hotkey writer and reload path, independent of a visible launcher. */
#include "../src/common/launcher_binds.c"

#include <assert.h>
#include "../src/launcher_system.h"
#if defined(RUI_TEST_PSX_HOST_KEYMAP)
#include "host_menu_shortcut.h"
static int menu_open;
static void set_menu(void* ctx,int open) { (void)ctx; menu_open=open; }
static int menu_key(SDL_Keycode key,SDL_Scancode scan,int modifiers,int* conflict) {
    PsxMenuProvider provider={0};
    SDL_Event event={0};
    provider.set_open=set_menu;
    event.type=SDL_EVENT_KEY_DOWN;
    event.key.key=key; event.key.scancode=scan; event.key.mod=(SDL_Keymod)modifiers;
    return psx_host_menu_shortcut(&provider,&event,menu_open,0,conflict);
}
#endif


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
    model.profile = &kSystemProfilePsx;
    assert(launcher_binds_hotkeys_equal("Ctrl+Alt+F1","Alt+Ctrl+F1"));
    assert(!launcher_binds_hotkeys_equal("None","None"));
    assert(!launcher_binds_hotkeys_equal("Ctrl+F1","Alt+F1"));
    const uint32_t mask = kSystemProfilePsx.hotkeys_mask | (1u << LNG_HK_OPEN_LAUNCHER);
    launcher_binds_reset_hotkeys(&model, mask);
    assert(!strcmp(model.hotkeys[LNG_HK_FULLSCREEN], "F11,Alt+Return,Ctrl+F"));
    assert(!strcmp(model.hotkeys[LNG_HK_SCANLINES], "F6"));
    assert(!strcmp(model.hotkeys[LNG_HK_REPLAY_RECORD], "F12"));
    assert(!strcmp(model.hotkeys[LNG_HK_ANALOG_MODE], "F3"));
    launcher_binds_set_hotkey_slot(&model, LNG_HK_FULLSCREEN, 0, SDLK_F4, SDL_KMOD_CTRL);
    launcher_binds_set_hotkey_slot(&model, LNG_HK_FULLSCREEN, 3, SDLK_F2, 0);
    reload_hotkey_display(&model);
    char slots[4][64];
    assert(launcher_binds_hotkey_slots(&model,LNG_HK_FULLSCREEN,slots)==4);
    assert(!strcmp(slots[0],"Ctrl+F4") && !strcmp(slots[1],"Alt+Return") && !strcmp(slots[2],"Ctrl+F") && !strcmp(slots[3],"F2"));
    launcher_binds_set_hotkey_slot(&model,LNG_HK_FULLSCREEN,1,0,0);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_FULLSCREEN],"Ctrl+F4,Ctrl+F,F2"));
    for (int i=0;i<3;++i) launcher_binds_set_hotkey_slot(&model,LNG_HK_FULLSCREEN,0,0,0);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_FULLSCREEN],"(unbound)"));
    launcher_binds_reset_hotkeys(&model,mask);
    reload_hotkey_display(&model);
    assert(!strcmp(model.hotkeys[LNG_HK_FULLSCREEN],"F11,Alt+Return,Ctrl+F"));
    assert((mask & (1u<<LNG_HK_OPEN_LAUNCHER)) != 0);
    assert(!strcmp(model.hotkeys[LNG_HK_OPEN_LAUNCHER],"Ctrl+L,F10"));
#if defined(RUI_TEST_PSX_HOST_KEYMAP)
    host_keymap_load(argv[1]);
    assert(host_keymap_match(HOST_KEYMAP_FULLSCREEN,SDLK_F11,0));
    assert(host_keymap_match(HOST_KEYMAP_FULLSCREEN,SDLK_RETURN,SDL_KMOD_ALT));
    assert(host_keymap_match(HOST_KEYMAP_FULLSCREEN,SDLK_F,SDL_KMOD_CTRL));
    assert(host_keymap_match(HOST_KEYMAP_QUICK_SAVE,SDLK_F5,0));
    assert(host_keymap_match(HOST_KEYMAP_QUICK_LOAD,SDLK_F5,SDL_KMOD_SHIFT));
    assert(host_keymap_match(HOST_KEYMAP_SCANLINES,SDLK_F6,0));
    assert(host_keymap_match(HOST_KEYMAP_REPLAY_RECORD,SDLK_F12,0));
    assert(host_keymap_match(HOST_KEYMAP_RUNTIME_MENU,SDLK_F1,0));
    assert(host_keymap_match(HOST_KEYMAP_OPEN_LAUNCHER,SDLK_F10,0));
    assert(host_keymap_match(HOST_KEYMAP_OPEN_LAUNCHER,SDLK_L,SDL_KMOD_CTRL));
    int conflict=0;
    menu_open=0;
    assert(menu_key(SDLK_F1,SDL_SCANCODE_F1,0,&conflict)==1 && menu_open && !conflict);
    assert(menu_key(SDLK_F10,SDL_SCANCODE_F10,0,&conflict)==1 && !menu_open);
    assert(menu_key(SDLK_L,SDL_SCANCODE_L,SDL_KMOD_CTRL,&conflict)==1 && menu_open);
    launcher_binds_set_hotkey(&model,LNG_HK_RUNTIME_MENU,SDLK_F2,SDL_KMOD_CTRL);
    launcher_binds_set_hotkey(&model,LNG_HK_OPEN_LAUNCHER,0,0);
    host_keymap_load(argv[1]);
    menu_open=0;
    assert(menu_key(SDLK_F1,SDL_SCANCODE_F1,0,&conflict)==0 && !menu_open);
    assert(menu_key(SDLK_F10,SDL_SCANCODE_F10,0,&conflict)==0 && !menu_open);
    assert(menu_key(SDLK_F2,SDL_SCANCODE_F2,SDL_KMOD_CTRL,&conflict)==1 && menu_open);
    launcher_binds_reset_hotkeys(&model,mask);
    launcher_binds_set_hotkey(&model,LNG_HK_QUICK_LOAD,SDLK_F10,0);
    host_keymap_load(argv[1]);
    menu_open=0;
    assert(menu_key(SDLK_F10,SDL_SCANCODE_F10,0,&conflict)==1 && !menu_open && conflict);
    // Neither the menu nor a quick load executes an ambiguous F10.
    assert(!strcmp(model.hotkeys[LNG_HK_QUICK_LOAD],"F10"));
    launcher_binds_reset_hotkeys(&model,mask);
    host_keymap_load(argv[1]);
    assert(menu_key(SDLK_F10,SDL_SCANCODE_F10,0,&conflict)==1 && menu_open && !conflict);
    puts("PASS: actual GUI writer/reset -> runtime parser -> provider handoff,aliases,remap,unbind,conflict refusal");
#else
    puts("SKIP: matching PSX host keymap fixture unavailable; launcher persistence still checked");
#endif
    text=read_whole(argv[1],&size);
    assert(text && strstr(text,"Keep = 17"));
    free(text);
    remove(argv[1]);
    puts("PASS: quick defaults, persistence, conflicts, four bindings, individual removal, explicit unbind and reset defaults");
    return 0;
}
