#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "launcher_input_report.h"
#include "launcher_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Uses the same SDL virtual joystick path as the GBA controller-menu contract,
 * without a host window or ROM. Both SDL backends run this fixture. */
int main(int argc,char** argv) {
    const char* path=argc>1?argv[1]:"input-report-test.jsonl";
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
#if defined(LNG_SDL3)
    assert(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_UNKNOWN;desc.naxes=2;desc.nbuttons=4;desc.nhats=1;
    desc.vendor_id=0xfeed;desc.product_id=0xbeee;desc.name="Unmapped input report test";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);
    SDL_Joystick* joystick=SDL_OpenJoystick(id);assert(joystick);
    char guid[40];SDL_GUIDToString(SDL_GetJoystickGUID(joystick),guid,sizeof(guid));
    assert(!SDL_IsGamepad(id));
#else
    SDL_SetMainReady();assert(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER)==0);
    SDL_VirtualJoystickDesc desc;memset(&desc,0,sizeof(desc));
    desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;desc.type=SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes=2;desc.nbuttons=4;desc.nhats=1;desc.vendor_id=0xfeed;desc.product_id=0xbeee;
    desc.name="Unmapped input report test";
    int device=SDL_JoystickAttachVirtualEx(&desc);assert(device>=0);
    SDL_Joystick* joystick=SDL_JoystickOpen(device);assert(joystick);
    SDL_JoystickID id=SDL_JoystickInstanceID(joystick);
    char guid[40];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick),guid,sizeof(guid));
    assert(!SDL_IsGameController(device));
#endif
    LauncherModel* model=(LauncherModel*)calloc(1,sizeof(LauncherModel));assert(model);
    model->s.player_src[0]=2;model->player_pad_id[0]=(uint32_t)id;
    snprintf(model->s.player_gamepad_guid[0],40,"%s",guid);
    snprintf(model->pad_binds[0][1],32,"x");model->s.player_pad_bind[0][1]=3;
    assert(launcher_input_report_start(path,model,"private config.ini","private keybinds.ini",1000));
    for(int down=1;down>=0;--down) {
#if defined(LNG_SDL3)
        assert(SDL_SetJoystickVirtualButton(joystick,1,down!=0));
        assert(SDL_SetJoystickVirtualAxis(joystick,0,down?30000:0));
        assert(SDL_SetJoystickVirtualHat(joystick,0,down?SDL_HAT_RIGHT:SDL_HAT_CENTERED));
        SDL_UpdateJoysticks();
#else
        assert(SDL_JoystickSetVirtualButton(joystick,1,(Uint8)down)==0);
        assert(SDL_JoystickSetVirtualAxis(joystick,0,down?30000:0)==0);
        assert(SDL_JoystickSetVirtualHat(joystick,0,down?SDL_HAT_RIGHT:SDL_HAT_CENTERED)==0);
        SDL_JoystickUpdate();
#endif
        SDL_Event event;while(SDL_PollEvent(&event)){}
    }
    char mapping[512];snprintf(mapping,sizeof(mapping),
        "%s,Report mapped,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,",guid);
#if defined(LNG_SDL3)
    assert(SDL_AddGamepadMapping(mapping)>=0);assert(SDL_IsGamepad(id));
#else
    assert(SDL_GameControllerAddMapping(mapping)>=0);assert(SDL_IsGameController(device));
#endif
    launcher_input_report_stop(model);assert(!launcher_input_report_active());
    FILE* file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
    char* text=(char*)calloc((size_t)size+1,1);assert(text);assert(fread(text,1,(size_t)size,file)==(size_t)size);fclose(file);
    assert(strstr(text,"\"mapped\":false") && strstr(text,"\"mapped\":true"));
    assert(strstr(text,"\"axes\":2,\"buttons\":4,\"hats\":1"));
    assert(strstr(text,"\"control\":\"button\"") && strstr(text,"\"control\":\"axis\"") && strstr(text,"\"control\":\"hat\""));
    assert(strstr(text,"\"index\":1,\"value\":1") && strstr(text,"\"index\":1,\"value\":0"));
    assert(strstr(text,"\"guid\":") && strstr(text,"\"source\":2") && strstr(text,"\"gamepad\":\"x\""));
    assert(strstr(text,"\"kind\":\"complete\"") && strstr(text,"\"dropped\":0"));
    free(text);free(model);
#if defined(LNG_SDL3)
    SDL_CloseJoystick(joystick);assert(SDL_DetachVirtualJoystick(id));
#else
    SDL_JoystickClose(joystick);assert(SDL_JoystickDetachVirtual(device)==0);
#endif
    SDL_Quit();return 0;
}
