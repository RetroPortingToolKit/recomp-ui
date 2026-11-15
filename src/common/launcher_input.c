// launcher_input.c — live gamepad enumeration (SDL2 + SDL3).

#include "launcher_input.h"
#include "launcher_platform.h"   // pulls the right SDL header for this build
#include "sha256.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void launcher_input_identity(char out[65],const char* guid,const char* path,const char* serial) {
    out[0]=0;
    /* SDL can expose a transient slot (e.g. XInput#0 or event13) as a path.
     * Persist only a physical OS path, otherwise require a device serial. */
    const int physical=path && (!strncmp(path,"\\\\?\\",4) ||
        !strncmp(path,"\\\\.\\",4) || !strncmp(path,"/dev/input/by-id/",17) ||
        !strncmp(path,"/sys/devices/",13));
    const char* value=physical?path:serial;
    if(!value || !*value || !guid)return;
    const size_t g=strlen(guid),n=strlen(value);
    if(g>40 || n>32768)return;
    unsigned char* text=malloc(g+n+2);if(!text)return;
    memcpy(text,guid,g);text[g]=0;text[g+1]=physical?'p':'s';memcpy(text+g+2,value,n);
    unsigned char hash[32];recompui_sha256_compute(text,g+n+2,hash);free(text);
    static const char hex[]="0123456789abcdef";
    for(int i=0;i<32;++i){out[2*i]=hex[hash[i]>>4];out[2*i+1]=hex[hash[i]&15];}out[64]=0;
}

typedef struct {
    uint32_t id;
#if defined(LNG_SDL3)
    SDL_Gamepad* handle;
#else
    SDL_GameController* handle;
#endif
    int has_gyro;
    char identity[65];
} OpenPad;

static OpenPad s_open[LNG_MAX_PADS];

static OpenPad* find_open(uint32_t id) {
    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (s_open[i].handle && s_open[i].id == id) return &s_open[i];
    return NULL;
}

static OpenPad* free_open_slot(void) {
    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (!s_open[i].handle) return &s_open[i];
    return NULL;
}

static void log_opened_pad(const LauncherPad* pad, const OpenPad* opened) {
    if (!pad || !opened || !opened->handle) return;
#if defined(LNG_SDL3)
    char* mapping = SDL_GetGamepadMapping(opened->handle);
#else
    char* mapping = SDL_GameControllerMapping(opened->handle);
#endif
    fprintf(stderr,
            "[launcher] gamepad opened: id=%u name=\"%s\" guid=%s "
            "mapping=\"%s\"\n",
            pad->id, pad->name[0] ? pad->name : "Gamepad",
            pad->guid[0] ? pad->guid : "unknown",
            mapping ? mapping : "");
    if (mapping) SDL_free(mapping);
}

static int id_is_live(const LauncherPad* pads, int count, uint32_t id) {
    for (int i = 0; i < count; ++i)
        if (pads[i].id == id) return 1;
    return 0;
}

static void close_open(OpenPad* pad) {
    if (!pad || !pad->handle) return;
#if defined(LNG_SDL3)
    if (pad->has_gyro)
        SDL_SetGamepadSensorEnabled(pad->handle, SDL_SENSOR_GYRO, false);
    SDL_CloseGamepad(pad->handle);
#else
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (pad->has_gyro)
        SDL_GameControllerSetSensorEnabled(
            pad->handle, SDL_SENSOR_GYRO, SDL_FALSE);
#endif
    SDL_GameControllerClose(pad->handle);
#endif
    memset(pad, 0, sizeof(*pad));
}

void launcher_input_shutdown(void) {
    for (int i = 0; i < LNG_MAX_PADS; ++i) close_open(&s_open[i]);
}

int launcher_input_gamepad_at_rest(uint32_t id) {
    OpenPad* pad = find_open(id);
    if (!pad || !pad->handle) return 1;  // no handle → don't block capture
    // Stick rest band; well below the capture commit threshold (20000).
    const int kRest = 8000;
#if defined(LNG_SDL3)
    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        if (SDL_GetGamepadButton(pad->handle, (SDL_GamepadButton)b))
            return 0;
    }
    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const int v = (int)SDL_GetGamepadAxis(pad->handle, (SDL_GamepadAxis)a);
        if (v <= -kRest || v >= kRest) return 0;
    }
#else
    for (int b = 0; b < (int)SDL_CONTROLLER_BUTTON_MAX; ++b) {
        if (SDL_GameControllerGetButton(
                pad->handle, (SDL_GameControllerButton)b))
            return 0;
    }
    for (int a = 0; a < (int)SDL_CONTROLLER_AXIS_MAX; ++a) {
        const int v = (int)SDL_GameControllerGetAxis(
            pad->handle, (SDL_GameControllerAxis)a);
        if (v <= -kRest || v >= kRest) return 0;
    }
#endif
    return 1;
}

uint32_t launcher_input_gamepad_button_mask(uint32_t id) {
    OpenPad* pad = find_open(id);
    uint32_t mask = 0;
    if (!pad || !pad->handle) return 0;
#if defined(LNG_SDL3)
    const int max_buttons = (int)SDL_GAMEPAD_BUTTON_COUNT;
    for (int b = 0; b < max_buttons && b < 32; ++b) {
        if (SDL_GetGamepadButton(pad->handle, (SDL_GamepadButton)b))
            mask |= (uint32_t)1u << b;
    }
#else
    const int max_buttons = (int)SDL_CONTROLLER_BUTTON_MAX;
    for (int b = 0; b < max_buttons && b < 32; ++b) {
        if (SDL_GameControllerGetButton(
                pad->handle, (SDL_GameControllerButton)b))
            mask |= (uint32_t)1u << b;
    }
#endif
    return mask;
}

int launcher_input_poll(LauncherPad* out, int max, int enable_gyro) {
    int n = 0;
    if (!out || max <= 0) return 0;
    if (max > LNG_MAX_PADS) max = LNG_MAX_PADS;
    memset(out, 0, (size_t)max * sizeof(*out));

#if defined(LNG_SDL3)
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count && n < max; ++i) {
            out[n].id = (uint32_t)ids[i];
            const char* nm = SDL_GetGamepadNameForID(ids[i]);
            snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
            out[n].guid[0] = '\0';
            {
                SDL_GUID g = SDL_GetGamepadGUIDForID(ids[i]);
                SDL_GUIDToString(g, out[n].guid, (int)sizeof(out[n].guid));
            }
            OpenPad* opened = find_open(out[n].id);
            if (!opened) {
                opened = free_open_slot();
                if (opened) {
                    opened->handle = SDL_OpenGamepad(ids[i]);
                    opened->id = out[n].id;
                    if(opened->handle)launcher_input_identity(opened->identity,out[n].guid,
                        SDL_GetGamepadPath(opened->handle),SDL_GetGamepadSerial(opened->handle));
                    log_opened_pad(&out[n], opened);
                    if (enable_gyro && opened->handle &&
                        SDL_GamepadHasSensor(opened->handle, SDL_SENSOR_GYRO) &&
                        SDL_SetGamepadSensorEnabled(
                            opened->handle, SDL_SENSOR_GYRO, true)) {
                        opened->has_gyro = 1;
                    }
                }
            }
            if (!enable_gyro && opened && opened->handle && opened->has_gyro) {
                SDL_SetGamepadSensorEnabled(
                    opened->handle, SDL_SENSOR_GYRO, false);
                opened->has_gyro = 0;
            } else if (enable_gyro && opened && opened->handle &&
                       !opened->has_gyro &&
                       SDL_GamepadHasSensor(
                           opened->handle, SDL_SENSOR_GYRO) &&
                       SDL_SetGamepadSensorEnabled(
                           opened->handle, SDL_SENSOR_GYRO, true)) {
                opened->has_gyro = 1;
            }
            if (enable_gyro && opened && opened->handle && opened->has_gyro) {
                float rate[3] = {0};
                if (SDL_GetGamepadSensorData(
                        opened->handle, SDL_SENSOR_GYRO, rate, 3)) {
                    out[n].has_gyro = 1;
                    out[n].gyro_z = rate[2];
                }
            }
            if(opened)snprintf(out[n].identity,sizeof out[n].identity,"%s",opened->identity);
            ++n;
        }
        SDL_free(ids);
    }
#else
    // SDL2 uses a device-index model. SDL_NumJoysticks() re-scans on each call
    // (fed by SDL's event pump), so a pad powered on after launch shows up here
    // without a relaunch — same hot-plug behaviour as the SDL3 path.
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count && n < max; ++i) {
        if (!SDL_IsGameController(i)) continue;   // mapped gamepads only
        // Report the stable instance id, not the volatile device index, so a
        // selection survives other pads connecting/disconnecting.
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        if (inst < 0) continue;

        out[n].id = (uint32_t)inst;
        const char* nm = SDL_GameControllerNameForIndex(i);
        snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
        out[n].guid[0] = '\0';
        {
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
            SDL_JoystickGetGUIDString(g, out[n].guid, (int)sizeof(out[n].guid));
        }
        OpenPad* opened = find_open(out[n].id);
        if (!opened) {
            opened = free_open_slot();
            if (opened) {
                opened->handle = SDL_GameControllerOpen(i);
                opened->id = out[n].id;
                if(opened->handle) {
                    const char* path=NULL;const char* serial=NULL;
#if SDL_VERSION_ATLEAST(2,24,0)
                    path=SDL_GameControllerPath(opened->handle);
#endif
#if SDL_VERSION_ATLEAST(2,0,14)
                    serial=SDL_GameControllerGetSerial(opened->handle);
#endif
                    launcher_input_identity(opened->identity,out[n].guid,path,serial);
                }
                log_opened_pad(&out[n], opened);
#if SDL_VERSION_ATLEAST(2, 0, 14)
                if (enable_gyro && opened->handle &&
                    SDL_GameControllerHasSensor(
                        opened->handle, SDL_SENSOR_GYRO) &&
                    SDL_GameControllerSetSensorEnabled(
                        opened->handle, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
                    opened->has_gyro = 1;
                }
#endif
            }
        }
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (!enable_gyro && opened && opened->handle && opened->has_gyro) {
            SDL_GameControllerSetSensorEnabled(
                opened->handle, SDL_SENSOR_GYRO, SDL_FALSE);
            opened->has_gyro = 0;
        } else if (enable_gyro && opened && opened->handle &&
                   !opened->has_gyro &&
                   SDL_GameControllerHasSensor(
                       opened->handle, SDL_SENSOR_GYRO) &&
                   SDL_GameControllerSetSensorEnabled(
                       opened->handle, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
            opened->has_gyro = 1;
        }
        if (enable_gyro && opened && opened->handle && opened->has_gyro) {
            float rate[3] = {0};
            if (SDL_GameControllerGetSensorData(
                    opened->handle, SDL_SENSOR_GYRO, rate, 3) == 0) {
                out[n].has_gyro = 1;
                out[n].gyro_z = rate[2];
            }
        }
#endif
        if(opened)snprintf(out[n].identity,sizeof out[n].identity,"%s",opened->identity);
        ++n;
    }
#endif

    for (int i = 0; i < LNG_MAX_PADS; ++i)
        if (s_open[i].handle && !id_is_live(out, n, s_open[i].id))
            close_open(&s_open[i]);

    return n;
}
