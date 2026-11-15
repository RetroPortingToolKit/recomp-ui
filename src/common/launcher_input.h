// launcher_input.h — live gamepad enumeration for the launcher (shared core).
//
// Polls SDL for currently-connected gamepads every frame, so a controller
// turned on AFTER the launcher opens shows up without a relaunch (and one
// turned off disappears). Game-agnostic; both backends use it to populate the
// per-player input-source dropdown with real device names.

#ifndef LAUNCHER_NG_INPUT_H
#define LAUNCHER_NG_INPUT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LNG_MAX_PADS 8

typedef struct LauncherPad {
    uint32_t id;        // SDL_JoystickID (stable while connected)
    char     name[64];  // e.g. "PS5 Controller", "Xbox Series Controller"
    char     guid[40];  // SDL_JoystickGetGUIDString (persists across reconnects)
    int      has_gyro;  // bool: angular-rate sensor opened and enabled
    float    gyro_z;    // radians/second around the controller face normal
    char     identity[65]; // model + OS path/serial hash; empty if unavailable
} LauncherPad;

/* Exact live choices win. A saved physical identity must have one match;
 * missing/ambiguous devices never fall back to a different same-model pad. */
static inline int launcher_input_resolve(const LauncherPad* pads, int count,
                                        const char* guid, const char* identity,
                                        uint32_t instance) {
    if (!pads || count < 0) return -1;
    if (count > LNG_MAX_PADS) count = LNG_MAX_PADS;
    const int manual = identity && !strcmp(identity, "manual");
    for (int i=0; instance && i<count; ++i)
        if (pads[i].id == instance-1 && (!guid || !*guid || !strcmp(guid,pads[i].guid)) &&
            (!identity || !*identity || manual || !strcmp(identity,pads[i].identity))) return i;
    if (manual || (instance && (!identity || !*identity))) return -1;
    int found=-1;
    for (int i=0; i<count; ++i) {
        if (guid && *guid && strcmp(guid,pads[i].guid)) continue;
        if (identity && *identity && strcmp(identity,pads[i].identity)) continue;
        if (found>=0) return -1;
        found=i;
    }
    return found;
}
static inline void launcher_input_pad_label(const LauncherPad* pads,int count,int index,
                                           char* out,unsigned capacity) {
    int total=0,number=0;
    for (int i=0;i<count;++i) if (!strcmp(pads[i].name,pads[index].name)) {
        ++total;if(i<=index)++number;
    }
    if(total>1)snprintf(out,capacity,"%s (%d)",pads[index].name,number);
    else snprintf(out,capacity,"%s",pads[index].name[0]?pads[index].name:"Gamepad");
}
void launcher_input_identity(char out[65],const char* guid,const char* path,const char* serial);

// Fill `out` (capacity `max`) with the gamepads SDL currently sees. Returns the
// count. `enable_gyro` is a game/build capability: when 0, sensors are neither
// enabled nor read and the gyro fields remain zero.
int launcher_input_poll(LauncherPad* out, int max, int enable_gyro);

// 1 if every button is up and every axis is near rest on the open pad with
// this SDL instance id. Used by bind capture to wait for a full release before
// accepting the next mapping (sticks otherwise re-fire on every axis tick).
int launcher_input_gamepad_at_rest(uint32_t id);

// Bit mask of currently held standard game-controller buttons on the open pad.
// Bit N corresponds to SDL button N. Unknown/missing id returns 0.
uint32_t launcher_input_gamepad_button_mask(uint32_t id);

// Close controller handles retained for live input/sensor polling. Call before
// the launcher tears down SDL.
void launcher_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_INPUT_H
