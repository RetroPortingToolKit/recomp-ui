#ifdef NDEBUG
#undef NDEBUG
#endif
#include "launcher_audio_buffer.h"
#include <assert.h>
int main(void) {
    assert(launcher_audio_buffer_option("psx.audio.low-latency","low-latency-audio","target_ms"));
    assert(!launcher_audio_buffer_option("other","low-latency-audio","target_ms"));
    assert(launcher_audio_buffer_step(20)==0 && launcher_audio_buffer_step(180)==32);
    assert(launcher_audio_buffer_step(70)==10);
    assert(!strcmp(launcher_audio_buffer_note(70),"Recommended"));
    assert(!strcmp(launcher_audio_buffer_note(50),"Experimental"));
    assert(!strcmp(launcher_audio_buffer_note(80),"More reserve"));
}
