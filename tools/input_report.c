#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "launcher_input_report.h"
#include "launcher_platform.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char** argv) {
    const char* path=argc>1?argv[1]:"controller-report.jsonl";
    unsigned ms=argc>2?(unsigned)strtoul(argv[2],NULL,10):10000u;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
#if defined(LNG_SDL3)
    if(!SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMEPAD)) {
#else
    SDL_SetMainReady();
    if(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER)!=0) {
#endif
        fprintf(stderr,"SDL input initialization failed: %s\n",SDL_GetError());return 1;
    }
    if(!launcher_input_report_start(path,NULL,NULL,NULL,ms)) {
        fprintf(stderr,"Cannot create input report: %s\n",path);SDL_Quit();return 1;
    }
    printf("Press and release controller buttons, move sticks and D-pad.\nReport: %s\n",path);
    while(launcher_input_report_active()) {
        SDL_Event event;while(SDL_PollEvent(&event)) {}
        launcher_input_report_tick(NULL);SDL_Delay(5);
    }
    SDL_Quit();return 0;
}
