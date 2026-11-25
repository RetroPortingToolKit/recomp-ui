#include "launcher_netplay_delay.h"
#include <assert.h>
#include <limits.h>

int main(void)
{
    assert(launcher_delay_sync_frames_from_rtt_ms(-1) == 4);
    assert(launcher_delay_sync_frames_from_rtt_ms(0) == 4);
    assert(launcher_delay_sync_frames_from_rtt_ms(50) == 6);
    assert(launcher_delay_sync_frames_from_rtt_ms(100) == 9);
    assert(launcher_delay_sync_frames_from_rtt_ms(200) == 15);
    assert(launcher_delay_sync_frames_from_rtt_ms(INT_MAX) == 20);
    for (int ms = 1; ms <= 200; ++ms) {
        const int delay = launcher_delay_sync_frames_from_rtt_ms(ms);
        /* Never undersize the confirmed-input round trip at 60 Hz. */
        assert((delay - 3) * 1000 >= ms * 60);
        assert(delay >= launcher_delay_sync_frames_from_rtt_ms(ms - 1));
    }
    return 0;
}
