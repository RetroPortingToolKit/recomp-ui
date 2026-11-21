/*
 * recomp_flash_guard: does it actually flatten a strobe, and does it leave
 * everything else alone?
 *
 * The second half matters as much as the first. A filter that dulled flashes
 * by dulling the whole game would pass any test that only looked at a strobe,
 * and would be found out by a player wondering why the game looks soft.
 *
 * Frames here are synthetic flat fills, because the guard's decision is taken
 * on the frame MEAN and a flat fill is the cleanest way to set one. The
 * amplitudes are not invented, though: FLASH_A and FLASH_B are the two
 * measured plateaus of the Gundam Wing end-of-fight flash, mean RGB
 * (31,26,140) and (152,153,150), a 68%-of-range step at 15 Hz.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_flash_guard.h"

#define W 64
#define H 32

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static void fill(uint32_t *f, int r, int g, int b)
{
    int i;
    for (i = 0; i < W * H; ++i)
        f[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
               (uint32_t)b;
}

/* Largest per-channel difference between two flat frames' first pixels. The
 * frames the guard emits from flat inputs stay flat, so pixel 0 is the mean. */
static int step_of(uint32_t a, uint32_t b)
{
    int i, mx = 0;
    for (i = 0; i < 3; ++i) {
        const int sa = (int)((a >> (i * 8)) & 0xFFu);
        const int sb = (int)((b >> (i * 8)) & 0xFFu);
        int d = sa - sb;
        if (d < 0) d = -d;
        if (d > mx) mx = d;
    }
    return mx;
}

/* The two measured plateaus, as R,G,B. */
#define FLASH_A  31,  26, 140
#define FLASH_B 152, 153, 150

int main(void)
{
    static uint32_t frame[W * H];
    RecompFlashGuard *g = recomp_flash_guard_create();
    const size_t pitch = (size_t)W * 4u;
    const int limit = kRecompFlashGuardStandard;

    if (!g) { printf("create failed\n"); return 1; }
    recomp_flash_guard_set_limit(g, limit);

    printf("\n1. a NULL handle is legal and does nothing\n");
    fill(frame, 10, 20, 30);
    check("apply(NULL) returns 0",
          recomp_flash_guard_apply(NULL, frame, W, H, pitch) == 0);
    check("and did not touch the frame", frame[0] == (0xFF000000u | 0x0A141Eu));

    printf("\n2. ordinary play passes through byte-identical\n");
    {
        /* Steps of 4 per frame: a scrolling background, a fading sprite --
         * anything under the limit. Nothing may be mixed, and the pixels must
         * come back EXACTLY, not merely close: a filter that rounds every
         * frame of the game is a filter that degrades it. */
        int i, mixed = 0, exact = 1;
        recomp_flash_guard_reset(g);
        for (i = 0; i < 40; ++i) {
            const int v = 60 + (i * 4);
            fill(frame, v, v, v);
            if (recomp_flash_guard_apply(g, frame, W, H, pitch)) ++mixed;
            if (frame[0] != (0xFF000000u | ((uint32_t)v << 16) |
                             ((uint32_t)v << 8) | (uint32_t)v))
                exact = 0;
        }
        check("no frame was mixed", mixed == 0);
        check("every frame came back byte-identical", exact);
    }

    printf("\n3. a scene cut cuts, it does not fade\n");
    {
        /* One large step that does not come back. The guard is allowed to
         * ramp briefly, but it must arrive -- a permanent soft fade on every
         * stage load would be a worse bug than the one it fixes. */
        int i, settled = -1;
        recomp_flash_guard_reset(g);
        fill(frame, 20, 20, 20);
        recomp_flash_guard_apply(g, frame, W, H, pitch);
        for (i = 0; i < 10; ++i) {
            fill(frame, 220, 220, 220);
            recomp_flash_guard_apply(g, frame, W, H, pitch);
            if (settled < 0 && frame[0] == (0xFF000000u | 0xDCDCDCu))
                settled = i;
        }
        check("reached the new scene exactly", settled >= 0);
        check("within three frames of the cut", settled >= 0 && settled <= 2);
    }

    printf("\n4. the measured 15 Hz strobe is flattened\n");
    {
        /* Two guest frames per phase, which is what was measured -- and is
         * also why frame blending does not help: it averages a frame with its
         * own twin. */
        int i, worst_src = 0, worst_out = 0;
        uint32_t prev_out = 0, prev_src = 0;
        recomp_flash_guard_reset(g);
        for (i = 0; i < 80; ++i) {
            const int phase = (i / 2) & 1;
            uint32_t src;
            if (phase) fill(frame, FLASH_B); else fill(frame, FLASH_A);
            src = frame[0];
            recomp_flash_guard_apply(g, frame, W, H, pitch);
            if (i) {
                const int ss = step_of(src, prev_src);
                const int os = step_of(frame[0], prev_out);
                if (ss > worst_src) worst_src = ss;
                /* Skip the first few frames: the guard is entering the flash
                 * and legitimately still catching up. */
                if (i > 6 && os > worst_out) worst_out = os;
            }
            prev_src = src;
            prev_out = frame[0];
        }
        printf("     source step %d/255, guarded step %d/255\n",
               worst_src, worst_out);
        check("the source really is a violent strobe (>100/255)",
              worst_src > 100);
        check("the guarded step never exceeds the limit",
              worst_out <= limit);
        check("which is under the WCAG 10% flash threshold (26/255)",
              worst_out < 26);
    }

    printf("\n5. the guard says what it did\n");
    {
        RecompFlashGuardStats st;
        recomp_flash_guard_get_stats(g, &st);
        printf("     frames=%llu limited=%llu events=%llu peak=%d\n",
               (unsigned long long)st.frames, (unsigned long long)st.limited,
               (unsigned long long)st.events, st.peak_step);
        check("it counted frames", st.frames > 0);
        check("it counted the mixing it did", st.limited > 0);
        check("it detected a flash event", st.events >= 1);
        check("and reports the source amplitude it saw", st.peak_step > 100);
    }

    printf("\n6. a slower strobe is flattened too\n");
    {
        /* Four guest frames per phase -- 7.5 Hz, still inside the risk band.
         * This is the case a fixed "release after N frames" rule gets wrong:
         * the phase outlasts the release counter, so the guard would let the
         * strobe through if the reversal hold were not what disables release.
         */
        int i, worst_out = 0;
        uint32_t prev_out = 0;
        recomp_flash_guard_reset(g);
        for (i = 0; i < 96; ++i) {
            const int phase = (i / 4) & 1;
            if (phase) fill(frame, FLASH_B); else fill(frame, FLASH_A);
            recomp_flash_guard_apply(g, frame, W, H, pitch);
            if (i > 10) {
                const int os = step_of(frame[0], prev_out);
                if (os > worst_out) worst_out = os;
            }
            prev_out = frame[0];
        }
        printf("     guarded step %d/255\n", worst_out);
        check("still never exceeds the limit", worst_out <= limit);
    }

    printf("\n7. strength changes the amplitude, and off means off\n");
    {
        const int limits[3] = { kRecompFlashGuardMild,
                                kRecompFlashGuardStandard,
                                kRecompFlashGuardMaximum };
        int k;
        int ok = 1;
        for (k = 0; k < 3; ++k) {
            int i, worst = 0;
            uint32_t prev = 0;
            recomp_flash_guard_reset(g);
            recomp_flash_guard_set_limit(g, limits[k]);
            for (i = 0; i < 60; ++i) {
                const int phase = (i / 2) & 1;
                if (phase) fill(frame, FLASH_B); else fill(frame, FLASH_A);
                recomp_flash_guard_apply(g, frame, W, H, pitch);
                if (i > 6) {
                    const int os = step_of(frame[0], prev);
                    if (os > worst) worst = os;
                }
                prev = frame[0];
            }
            printf("     limit %3d -> guarded step %d/255\n", limits[k], worst);
            if (worst > limits[k]) ok = 0;
        }
        check("each strength honours its own limit", ok);

        recomp_flash_guard_set_limit(g, 0);
        recomp_flash_guard_reset(g);
        fill(frame, FLASH_A);
        recomp_flash_guard_apply(g, frame, W, H, pitch);
        fill(frame, FLASH_B);
        check("limit 0 leaves the frame completely alone",
              recomp_flash_guard_apply(g, frame, W, H, pitch) == 0 &&
              frame[0] == (0xFF000000u | 0x989996u));
    }

    recomp_flash_guard_destroy(g);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
