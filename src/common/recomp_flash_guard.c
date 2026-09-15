/* recomp_flash_guard.c — see recomp_flash_guard.h. */

#include "recomp_flash_guard.h"

#include <stdlib.h>
#include <string.h>

/* How many consecutive limited frames before the guard gives up and lets the
 * frame through as drawn. This is what keeps a scene cut from becoming a
 * fade: two frames of ramp at 60 Hz is ~33 ms, which reads as a hard cut.
 *
 * It is deliberately short, because it is not the thing protecting against a
 * strobe — the reversal hold below is. Release is disabled entirely while
 * that hold is live, so shortening this cannot let a flash through. */
enum { kReleaseFrames = 2 };

/* How long a detected reversal keeps release disabled, in frames. 45 at
 * 60 Hz is 0.75 s.
 *
 * Sized from the far side of the problem: the risk band starts at 3 Hz, so
 * two flashes that matter can be a third of a second apart, and a hold that
 * expired between them would release exactly in the gap and then have to
 * re-detect on the next one — passing a full-amplitude frame each time. The
 * cost of holding too long is that the first scene cut within 0.75 s of a
 * flash ramps over two frames instead of cutting. That is not a cost worth
 * optimising against. */
enum { kHoldFrames = 45 };

/* Subsampling stride for the frame mean. The mean is a measure of the whole
 * picture's brightness, not a checksum: on a 342x224 frame a stride of 4 in x
 * and 2 in y still averages ~9,500 pixels, which pins the mean to well under
 * one 255th. Costs an eighth of the reads a full pass would.
 *
 * Strides are coprime with nothing in particular on purpose — a SNES frame is
 * built from 8-pixel tiles, and a stride of 8 would sample the same column of
 * every tile and could sit entirely on, say, a sprite outline. 4 and 2 both
 * divide 8, so they sample several positions within each tile. */
enum { kStrideX = 4, kStrideY = 2 };

struct RecompFlashGuard {
    uint32_t *prev;      /* the previously PRESENTED frame, tightly packed */
    size_t    capacity;  /* pixels prev can hold (kept, never shrunk) */
    int       width;
    int       height;
    int       valid;     /* 0 => prev holds nothing to mix with */

    int limit;           /* presented step limit, 1..255 */

    /* Frame means of the last PRESENTED frame, and of the last INCOMING one.
     * Both are needed and they are not the same number once the guard starts
     * limiting: the presented mean is what the next step is measured against,
     * the incoming mean is what tells a reversal from a continuing move. */
    int out_mean[3];
    int in_mean[3];

    int run;             /* consecutive limited frames */
    int last_sign;       /* -1/0/+1: direction of the last significant step */
    int hold;            /* frames left of the reversal hold */

    uint64_t frames;
    uint64_t limited;
    uint64_t events;
    int      peak_step;
};

RecompFlashGuard *recomp_flash_guard_create(void) {
    RecompFlashGuard *guard =
        (RecompFlashGuard *)calloc(1, sizeof(RecompFlashGuard));
    if (guard) guard->limit = kRecompFlashGuardStandard;
    return guard;
}

void recomp_flash_guard_destroy(RecompFlashGuard *guard) {
    if (!guard) return;
    free(guard->prev);
    free(guard);
}

void recomp_flash_guard_set_limit(RecompFlashGuard *guard, int limit) {
    if (!guard) return;
    if (limit < 0)   limit = 0;
    if (limit > 255) limit = 255;
    guard->limit = limit;
}

int recomp_flash_guard_get_limit(const RecompFlashGuard *guard) {
    return guard ? guard->limit : 0;
}

void recomp_flash_guard_reset(RecompFlashGuard *guard) {
    if (!guard) return;
    guard->valid = 0;
    guard->run = 0;
    guard->last_sign = 0;
    guard->hold = 0;
}

/* Grows (never shrinks) the kept frame to width*height, resetting whenever the
 * geometry changes: prev is indexed by the width it was captured at, so a kept
 * frame from a different width would mix misaligned rows. Same contract as
 * recomp_frame_blend's ensure_prev, for the same reason. */
static int ensure_prev(RecompFlashGuard *guard, int width, int height) {
    size_t want;
    if (guard->width == width && guard->height == height && guard->prev)
        return 1;
    want = (size_t)width * (size_t)height;
    if (want > guard->capacity) {
        uint32_t *grown = (uint32_t *)realloc(guard->prev,
                                              want * sizeof(*grown));
        if (!grown) return 0;
        guard->prev = grown;
        guard->capacity = want;
    }
    guard->width  = width;
    guard->height = height;
    guard->valid  = 0;
    guard->run = 0;
    guard->last_sign = 0;
    guard->hold = 0;
    return 1;
}

/* Mean B, G, R of the frame, subsampled. Gamma-space, not linearised: the
 * comparison that matters is between two means measured the same way, and a
 * linear-light mean would make the filter markedly more aggressive in the
 * shadows for no benefit the player can see. Stated because "WCAG relative
 * luminance" is linear, and someone will reasonably expect this to be too. */
static void frame_mean(const unsigned char *base, int width, int height,
                       size_t pitch_bytes, int out_mean[3]) {
    uint32_t sum[3] = { 0, 0, 0 };
    uint32_t n = 0;
    int y;

    for (y = 0; y < height; y += kStrideY) {
        const unsigned char *row = base + (size_t)y * pitch_bytes;
        int x;
        for (x = 0; x < width; x += kStrideX) {
            const unsigned char *px = row + (size_t)x * 4u;
            sum[0] += px[0];
            sum[1] += px[1];
            sum[2] += px[2];
            ++n;
        }
    }
    if (!n) n = 1;
    out_mean[0] = (int)(sum[0] / n);
    out_mean[1] = (int)(sum[1] / n);
    out_mean[2] = (int)(sum[2] / n);
}

/* The step between two frame means, as the largest per-channel difference.
 *
 * Per-channel rather than luminance because an equal-luminance colour strobe
 * is still a strobe — the ISO/IEC and ITU guidance singles out saturated red
 * transitions specifically, and a luminance-only metric scores those near
 * zero. Taking the max over channels costs nothing extra (the means are
 * already per-channel) and cannot score a flash LOWER than a luminance metric
 * would, which is the direction an accessibility filter should err in. */
static int mean_step(const int a[3], const int b[3], int *sign_out) {
    int i;
    int step = 0;
    int signed_sum = 0;
    for (i = 0; i < 3; ++i) {
        const int d = a[i] - b[i];
        const int m = d < 0 ? -d : d;
        if (m > step) step = m;
        signed_sum += d;
    }
    if (sign_out)
        *sign_out = signed_sum > 0 ? 1 : (signed_sum < 0 ? -1 : 0);
    return step;
}

int recomp_flash_guard_apply(RecompFlashGuard *guard, void *frame,
                             int width, int height, size_t pitch_bytes) {
    unsigned char *base = (unsigned char *)frame;
    int in_mean[3];
    int step, sign;
    int alpha;      /* 0..256, the weight given to the incoming frame */
    int y;

    if (!guard || !frame || width <= 0 || height <= 0) return 0;
    if (pitch_bytes < (size_t)width * 4u) return 0;
    if (guard->limit <= 0) return 0;
    if (!ensure_prev(guard, width, height)) return 0;

    ++guard->frames;
    frame_mean(base, width, height, pitch_bytes, in_mean);

    /* Detection runs against the INCOMING frames, so that what is being
     * classified is what the game drew — not what this filter has already
     * softened. Feeding the decision its own output is how a limiter talks
     * itself out of limiting. */
    if (guard->valid) {
        int in_sign = 0;
        const int in_step = mean_step(in_mean, guard->in_mean, &in_sign);
        if (in_step > guard->peak_step)
            guard->peak_step = in_step;
        if (in_step >= guard->limit && in_sign != 0) {
            if (guard->last_sign != 0 && in_sign != guard->last_sign) {
                /* A reversal: the picture has moved a long way and come back.
                 * That is the definition of a flash, and one is enough — the
                 * guard does not wait to count three per second the way the
                 * standard does, because the cost of engaging on something
                 * that turns out not to be a flash is one softened frame
                 * nobody notices. */
                if (!guard->hold)
                    ++guard->events;
                guard->hold = kHoldFrames;
            }
            guard->last_sign = in_sign;
        }
    }
    if (guard->hold > 0)
        --guard->hold;
    memcpy(guard->in_mean, in_mean, sizeof(in_mean));

    /* First frame after a reset or a resize: nothing to mix with. Capture and
     * present as drawn. */
    if (!guard->valid) {
        for (y = 0; y < height; ++y)
            memcpy(guard->prev + (size_t)y * (size_t)width,
                   base + (size_t)y * pitch_bytes, (size_t)width * 4u);
        memcpy(guard->out_mean, in_mean, sizeof(in_mean));
        guard->valid = 1;
        guard->run = 0;
        return 0;
    }

    step = mean_step(in_mean, guard->out_mean, &sign);

    if (step <= guard->limit) {
        /* The ordinary case, and the one that has to be free: the picture is
         * moving no faster than the limit, so it is presented exactly as
         * drawn — not mixed, not rounded, byte-identical. */
        guard->run = 0;
        goto pass_through;
    }

    if (guard->run >= kReleaseFrames && guard->hold == 0) {
        /* A cut, not a flash: a large step, sustained in one direction, with
         * no reversal recently. Snap to it rather than fading into it. */
        guard->run = 0;
        goto pass_through;
    }

    /* Limit. alpha = limit/step in 1/256ths, so the presented mean moves by
     * exactly the limit and no further. Clamped to at least 1 so a colossal
     * step still advances the picture instead of freezing it. */
    alpha = (guard->limit * 256) / step;
    if (alpha < 1)   alpha = 1;
    if (alpha > 255) alpha = 255;   /* step > limit, so this is a guard only */

    for (y = 0; y < height; ++y) {
        uint32_t *row  = (uint32_t *)(void *)(base + (size_t)y * pitch_bytes);
        uint32_t *prev = guard->prev + (size_t)y * (size_t)width;
        int x;
        for (x = 0; x < width; ++x) {
            const uint32_t cur = row[x];
            const uint32_t old = prev[x];
            /* Per channel: old + alpha*(cur - old), in 1/256ths. Done on the
             * three colour bytes only; the top byte is carried from the
             * incoming pixel untouched (see the header — it is padding, and
             * on at least one PPU it is zero). */
            const int b = (int)((old >>  0) & 0xFFu) +
                          (((int)((cur >>  0) & 0xFFu) -
                            (int)((old >>  0) & 0xFFu)) * alpha >> 8);
            const int g = (int)((old >>  8) & 0xFFu) +
                          (((int)((cur >>  8) & 0xFFu) -
                            (int)((old >>  8) & 0xFFu)) * alpha >> 8);
            const int r = (int)((old >> 16) & 0xFFu) +
                          (((int)((cur >> 16) & 0xFFu) -
                            (int)((old >> 16) & 0xFFu)) * alpha >> 8);
            const uint32_t out = (cur & 0xFF000000u) |
                                 ((uint32_t)r << 16) |
                                 ((uint32_t)g <<  8) |
                                 (uint32_t)b;
            row[x] = out;
            prev[x] = out;
        }
    }

    /* The kept frame is the PRESENTED one, not the incoming one — the
     * opposite of recomp_frame_blend, which keeps the unblended frame so its
     * mix cannot feed back on itself. Here the feedback IS the mechanism: the
     * limit is on the step the display shows, so the next step has to be
     * measured from what was actually shown. */
    frame_mean(base, width, height, pitch_bytes, guard->out_mean);
    ++guard->run;
    ++guard->limited;
    return 1;

pass_through:
    for (y = 0; y < height; ++y)
        memcpy(guard->prev + (size_t)y * (size_t)width,
               base + (size_t)y * pitch_bytes, (size_t)width * 4u);
    memcpy(guard->out_mean, in_mean, sizeof(in_mean));
    return 0;
}

void recomp_flash_guard_get_stats(const RecompFlashGuard *guard,
                                  RecompFlashGuardStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!guard) return;
    out->frames    = guard->frames;
    out->limited   = guard->limited;
    out->events    = guard->events;
    out->peak_step = guard->peak_step;
    out->holding   = guard->hold > 0;
}
