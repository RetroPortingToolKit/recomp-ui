/* Live-device capture selection and GBA alternate persistence. No window. */
#include "launcher_input.h"
#include "launcher_platform.h"
#include "consoles/gba/gba_pad_binds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    LauncherPad pads[LNG_MAX_PADS] = {{0}};
    pads[0].id = 0; strcpy(pads[0].guid, "first");
    pads[1].id = 7; strcpy(pads[1].guid, "second");
    pads[2].id = 9; strcpy(pads[2].guid, "first");
    assert(launcher_input_find_selected_pad(pads, 3, 0, "first") == 0);
    assert(launcher_input_find_selected_pad(pads, 3, 9, "first") == 2);
    assert(launcher_input_find_selected_pad(pads, 3, 0, "second") == 1);
    assert(launcher_input_find_selected_pad(pads, 3, 99, "first") == 0);
    assert(launcher_input_find_selected_pad(pads, 3, 0, "missing") == -1);
    assert(launcher_input_find_selected_pad(pads, 3, 7, "") == 1);
    assert(launcher_input_find_selected_pad(pads, 0, 0, "first") == -1);
    assert(launcher_input_find_selected_pad(NULL, 3, 0, "first") == -1);
#if !defined(LNG_SDL3) && SDL_VERSION_ATLEAST(2, 0, 14)
    assert(argc == 2); /* fresh test-owned input.ini */
    assert(SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) == 0);
    int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    assert(device >= 0 && SDL_IsGameController(device));
    SDL_Joystick* joystick = SDL_JoystickOpen(device); assert(joystick);
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    assert(id >= 0);
    char guid[40]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof guid);
    int count = launcher_input_poll(pads, LNG_MAX_PADS, 0); assert(count >= 1);
    int selected = launcher_input_find_selected_pad(pads, count, (uint32_t)id, guid);
    assert(selected >= 0 && pads[selected].id == (uint32_t)id);
    const int capture_action = 2 + (kRuiGbaPadSpec.count > 10 ? 10 : 0);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    assert(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, -30000) == 0);
    SDL_JoystickUpdate();
    SDL_Event event; int found = 0;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_CONTROLLERAXISMOTION &&
            event.caxis.which == (SDL_JoystickID)pads[selected].id &&
            event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX && event.caxis.value <= -20000) {
            found = 1;
            rui_pad_binds_set(&kRuiGbaPadSpec, argv[1], pads[selected].guid,
                capture_action, 2, event.caxis.axis, -1);
        }
    }
    assert(found);
    assert(!launcher_input_gamepad_at_rest((uint32_t)id));
    char label[64];
    rui_pad_binds_init(&kRuiGbaPadSpec, argv[1]);
    rui_pad_binds_label(&kRuiGbaPadSpec, argv[1], pads[selected].guid,
                       capture_action, label, sizeof label);
    assert(strstr(label, "Left X") || strstr(label, "leftx"));
    rui_pad_binds_set(&kRuiGbaPadSpec, argv[1], pads[selected].guid,
                     capture_action, 0, 0, 0);
    rui_pad_binds_init(&kRuiGbaPadSpec, argv[1]);
    rui_pad_binds_label(&kRuiGbaPadSpec, argv[1], pads[selected].guid,
                       capture_action, label, sizeof label);
    assert(strstr(label, "unbound") || strstr(label, "Unbound"));
    /* Empty is an explicit clear, including a primary with nonempty defaults. */
    rui_pad_binds_set(&kRuiGbaPadSpec, argv[1], pads[selected].guid, 2, 0, 0, 0);
    rui_pad_binds_init(&kRuiGbaPadSpec, argv[1]);
    rui_pad_binds_label(&kRuiGbaPadSpec, argv[1], pads[selected].guid, 2, label, sizeof label);
    assert(!strcmp(label, "(unbound)"));
    rui_pad_binds_label(&kRuiGbaPadSpec, argv[1], "other-pad", 2, label, sizeof label);
    assert(!strcmp(label, "dpleft"));
    char global_path[1024]; snprintf(global_path, sizeof global_path, "%s-global", argv[1]);
    FILE* global = fopen(global_path, "w"); assert(global);
    fputs("[mapping]\nleft =\n", global); fclose(global);
    rui_pad_binds_init(&kRuiGbaPadSpec, global_path);
    rui_pad_binds_label(&kRuiGbaPadSpec, global_path, "", 2, label, sizeof label);
    assert(!strcmp(label, "(unbound)"));
    rui_pad_binds_remember(&kRuiGbaPadSpec, global_path, "new-pad", "New pad", -1);
    rui_pad_binds_init(&kRuiGbaPadSpec, global_path);
    rui_pad_binds_label(&kRuiGbaPadSpec, global_path, "new-pad", 2, label, sizeof label);
    assert(!strcmp(label, "(unbound)"));
    assert(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0) == 0);
    SDL_JoystickUpdate(); assert(launcher_input_gamepad_at_rest((uint32_t)id));
    launcher_input_shutdown(); SDL_JoystickClose(joystick);
    assert(SDL_JoystickDetachVirtual(device) == 0); SDL_Quit();
#endif
    puts("selected controller and GBA alternate capture/clear PASS"); return 0;
}
