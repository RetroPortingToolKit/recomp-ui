#ifndef RUI_LAUNCHER_AUDIO_BUFFER_H
#define RUI_LAUNCHER_AUDIO_BUFFER_H
#include <string.h>
static inline int launcher_audio_buffer_option(const char *package, const char *feature,
                                                const char *option) {
    return !strcmp(package,"psx.audio.low-latency") &&
           !strcmp(feature,"low-latency-audio") && !strcmp(option,"target_ms");
}
static inline int launcher_audio_buffer_step(int target) {
    return target < 20 ? 0 : target > 180 ? 32 : (target - 20) / 5;
}
static inline const char *launcher_audio_buffer_note(int target) {
    return target == 20 ? "Experimental / Unstable" : target < 70 ? "Experimental" :
           target == 70 ? "Recommended" : "More reserve";
}
#endif
