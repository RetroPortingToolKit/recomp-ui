#include "launcher_input_report.h"
#include "launcher_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* report;
#if defined(LNG_SDL3)
static SDL_Mutex* report_mutex;
#else
static SDL_mutex* report_mutex;
#endif
static SDL_Joystick* raw_open[64];
static int raw_open_count;
static uint32_t started, duration, event_count, dropped;

static void quoted(FILE* out, const char* value) {
    fputc('"', out);
    for (const unsigned char* p=(const unsigned char*)(value?value:""); *p; ++p) {
        if (*p=='"' || *p=='\\') {fputc('\\',out);fputc(*p,out);}
        else if (*p<32) fprintf(out,"\\u%04x",(unsigned)*p);
        else fputc(*p,out);
    }
    fputc('"',out);
}

static void assignments(const LauncherModel* model, const char* phase) {
    fprintf(report,"{\"kind\":\"assignments\",\"phase\":");quoted(report,phase);
    fprintf(report,",\"model_available\":%s,\"players\":[",model?"true":"false");
    if (model) for (int p=0;p<LNG_MAX_PLAYERS;++p) {
        if(p)fputc(',',report);
        fprintf(report,"{\"player\":%d,\"source\":%d,\"instance\":%u,\"name\":",
                p+1,model->s.player_src[p],model->player_pad_id[p]);
        quoted(report,model->player_pad_name[p]);fprintf(report,",\"guid\":");
        quoted(report,model->s.player_gamepad_guid[p]);
        fprintf(report,",\"bindings\":[");
        for(int b=0;b<LNG_MAX_BUTTONS;++b) {
            if(b)fputc(',',report);
            fprintf(report,"{\"slot\":%d,\"keyboard\":",b);quoted(report,model->binds[p][b]);
            fprintf(report,",\"gamepad\":");quoted(report,model->pad_binds[p][b]);
            if(b<RECOMP_LAUNCHER_MAX_BINDINGS)
                fprintf(report,",\"key_value\":%d,\"pad_value\":%d",
                        model->s.player_key_bind[p][b],model->s.player_pad_bind[p][b]);
            fputc('}',report);
        }
        fprintf(report,"]}");
    }
    fprintf(report,"]}\n");
}

static void devices(int retain) {
#if defined(LNG_SDL3)
    int count=0;SDL_JoystickID* ids=SDL_GetJoysticks(&count);
#else
    int count=SDL_NumJoysticks();
#endif
    fprintf(report,"{\"kind\":\"enumeration\",\"raw_count\":%d}\n",count<0?0:count);
    for(int i=0;i<count;++i) {
        char guid[40];
#if defined(LNG_SDL3)
        SDL_JoystickID id=ids[i];
        const char* name=SDL_GetJoystickNameForID(id);
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id),guid,sizeof(guid));
        int mapped=SDL_IsGamepad(id);
        char* mapping=mapped?SDL_GetGamepadMappingForID(id):NULL;
        unsigned vendor=SDL_GetJoystickVendorForID(id),product=SDL_GetJoystickProductForID(id);
        SDL_Joystick* joystick=SDL_OpenJoystick(id);
#else
        SDL_JoystickID id=SDL_JoystickGetDeviceInstanceID(i);
        const char* name=SDL_JoystickNameForIndex(i);
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),guid,sizeof(guid));
        int mapped=SDL_IsGameController(i);
        char* mapping=mapped?SDL_GameControllerMappingForDeviceIndex(i):NULL;
        unsigned vendor=SDL_JoystickGetDeviceVendor(i),product=SDL_JoystickGetDeviceProduct(i);
        SDL_Joystick* joystick=SDL_JoystickOpen(i);
#endif
        fprintf(report,"{\"kind\":\"device\",\"instance\":%u,\"name\":",(unsigned)id);
        quoted(report,name);fprintf(report,",\"guid\":");quoted(report,guid);
        fprintf(report,",\"vendor\":%u,\"product\":%u,\"mapped\":%s,\"mapping\":",
                vendor,product,mapped?"true":"false");quoted(report,mapping);
        SDL_free(mapping);
        int axes=0,buttons=0,hats=0;
        if(joystick) {
#if defined(LNG_SDL3)
            axes=SDL_GetNumJoystickAxes(joystick);buttons=SDL_GetNumJoystickButtons(joystick);
            hats=SDL_GetNumJoystickHats(joystick);
#else
            axes=SDL_JoystickNumAxes(joystick);buttons=SDL_JoystickNumButtons(joystick);
            hats=SDL_JoystickNumHats(joystick);
#endif
        }
        fprintf(report,",\"axes\":%d,\"buttons\":%d,\"hats\":%d,\"axis_values\":[",axes,buttons,hats);
        for(int a=0;a<axes;++a) {
            if(a)fputc(',',report);
#if defined(LNG_SDL3)
            fprintf(report,"%d",SDL_GetJoystickAxis(joystick,a));
#else
            fprintf(report,"%d",SDL_JoystickGetAxis(joystick,a));
#endif
        }
        fprintf(report,"],\"button_values\":[");
        for(int b=0;b<buttons;++b) {
            if(b)fputc(',',report);
#if defined(LNG_SDL3)
            fprintf(report,"%d",SDL_GetJoystickButton(joystick,b)?1:0);
#else
            fprintf(report,"%d",SDL_JoystickGetButton(joystick,b)?1:0);
#endif
        }
        fprintf(report,"],\"hat_values\":[");
        for(int h=0;h<hats;++h) {
            if(h)fputc(',',report);
#if defined(LNG_SDL3)
            fprintf(report,"%u",SDL_GetJoystickHat(joystick,h));
#else
            fprintf(report,"%u",SDL_JoystickGetHat(joystick,h));
#endif
        }
        fprintf(report,"]}\n");
        if(joystick && retain && raw_open_count<64) {
            raw_open[raw_open_count++]=joystick;
        } else if(joystick) {
#if defined(LNG_SDL3)
            SDL_CloseJoystick(joystick);
#else
            SDL_JoystickClose(joystick);
#endif
        }
    }
#if defined(LNG_SDL3)
    SDL_free(ids);
#endif
}

#if defined(LNG_SDL3)
static bool SDLCALL watch(void* context, SDL_Event* event) {
#define REPORT_JOY_AXIS SDL_EVENT_JOYSTICK_AXIS_MOTION
#define REPORT_JOY_DOWN SDL_EVENT_JOYSTICK_BUTTON_DOWN
#define REPORT_JOY_UP SDL_EVENT_JOYSTICK_BUTTON_UP
#define REPORT_JOY_HAT SDL_EVENT_JOYSTICK_HAT_MOTION
#else
static int SDLCALL watch(void* context, SDL_Event* event) {
#define REPORT_JOY_AXIS SDL_JOYAXISMOTION
#define REPORT_JOY_DOWN SDL_JOYBUTTONDOWN
#define REPORT_JOY_UP SDL_JOYBUTTONUP
#define REPORT_JOY_HAT SDL_JOYHATMOTION
#endif
    (void)context;
    const char* kind=NULL;unsigned id=0,index=0;int value=0;
    switch(event->type) {
        case REPORT_JOY_AXIS:kind="axis";id=event->jaxis.which;index=event->jaxis.axis;value=event->jaxis.value;break;
        case REPORT_JOY_DOWN:case REPORT_JOY_UP:kind="button";id=event->jbutton.which;index=event->jbutton.button;value=event->type==REPORT_JOY_DOWN;break;
        case REPORT_JOY_HAT:kind="hat";id=event->jhat.which;index=event->jhat.hat;value=event->jhat.value;break;
        default:break;
    }
    if(kind) {
        SDL_LockMutex(report_mutex);
        if(report && (uint32_t)((uint32_t)SDL_GetTicks()-started)<duration) {
            if(event_count<4096) {
                fprintf(report,"{\"kind\":\"event\",\"elapsed_ms\":%u,\"control\":\"%s\",\"instance\":%u,\"index\":%u,\"value\":%d}\n",
                        (uint32_t)((uint32_t)SDL_GetTicks()-started),kind,id,index,value);
                ++event_count;
            } else ++dropped;
        }
        SDL_UnlockMutex(report_mutex);
    }
    return 1;
}

int launcher_input_report_start(const char* path,const LauncherModel* model,
                               const char* config_path,const char* keybinds_path,
                               uint32_t duration_ms) {
    if(report || !path || !path[0])return 0;
    if(!report_mutex)report_mutex=SDL_CreateMutex();
    if(!report_mutex)return 0;
    report=fopen(path,"wb");if(!report)return 0;
    started=(uint32_t)SDL_GetTicks();event_count=dropped=0;
    duration=duration_ms<100?100:duration_ms>30000?30000:duration_ms;
    fprintf(report,"{\"kind\":\"header\",\"schema\":\"recomp-input-report/1\",\"sdl_major\":%d,\"duration_ms\":%u,\"config_path\":",SDL_MAJOR_VERSION,duration);
    quoted(report,config_path);fprintf(report,",\"keybinds_path\":");quoted(report,keybinds_path);
    fprintf(report,"}\n");devices(1);assignments(model,"begin");fflush(report);
    SDL_AddEventWatch(watch,NULL);
    return 1;
}

int launcher_input_report_active(void) {return report!=NULL;}

void launcher_input_report_stop(const LauncherModel* model) {
    if(!report)return;
#if defined(LNG_SDL3)
    SDL_RemoveEventWatch(watch,NULL);
#else
    SDL_DelEventWatch(watch,NULL);
#endif
    SDL_LockMutex(report_mutex);
    assignments(model,"end");devices(0);
    fprintf(report,"{\"kind\":\"complete\",\"elapsed_ms\":%u,\"events\":%u,\"dropped\":%u}\n",
            (uint32_t)((uint32_t)SDL_GetTicks()-started),event_count,dropped);
    fclose(report);report=NULL;
    for(int i=0;i<raw_open_count;++i) {
#if defined(LNG_SDL3)
        SDL_CloseJoystick(raw_open[i]);
#else
        SDL_JoystickClose(raw_open[i]);
#endif
    }
    raw_open_count=0;
    SDL_UnlockMutex(report_mutex);
}

void launcher_input_report_tick(const LauncherModel* model) {
    if(report && (uint32_t)((uint32_t)SDL_GetTicks()-started)>=duration)
        launcher_input_report_stop(model);
}
