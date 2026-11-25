#ifndef LAUNCHER_NETPLAY_DELAY_H
#define LAUNCHER_NETPLAY_DELAY_H

/* Delay-sync consumes confirmed input: the input travels to the peer, then
 * its confirmation returns. Budget the full RTT, plus scheduling/jitter room. */
static inline int launcher_delay_sync_frames_from_rtt_ms(int rtt_ms)
{
    int frames, delay;
    if (rtt_ms < 0) rtt_ms = 0;
    if (rtt_ms > 60000) rtt_ms = 60000;
    frames = (rtt_ms * 60 + 999) / 1000;
    if (frames < 1) frames = 1;
    delay = frames + 3;
    if (delay > 20) delay = 20;
    return delay;
}

#endif
