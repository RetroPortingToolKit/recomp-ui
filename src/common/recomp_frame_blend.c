/* recomp_frame_blend.c — see recomp_frame_blend.h. */

#include "recomp_frame_blend.h"

#include <stdlib.h>
#include <string.h>

struct RecompFrameBlend {
    uint32_t *prev;      /* the previous frame, UNBLENDED, tightly packed */
    size_t    capacity;  /* pixels prev can hold (kept, never shrunk) */
    int       width;
    int       height;
    int       valid;     /* 0 => prev holds nothing to blend with */
};

RecompFrameBlend *recomp_frame_blend_create(void) {
    return (RecompFrameBlend *)calloc(1, sizeof(RecompFrameBlend));
}

void recomp_frame_blend_destroy(RecompFrameBlend *blend) {
    if (!blend) return;
    free(blend->prev);
    free(blend);
}

void recomp_frame_blend_reset(RecompFrameBlend *blend) {
    if (blend) blend->valid = 0;
}

/* Grows (never shrinks) the kept frame to width*height, resetting whenever
 * the geometry changes: prev is indexed by the width it was captured at, so
 * a kept frame from a different width would blend misaligned rows. */
static int ensure_prev(RecompFrameBlend *blend, int width, int height) {
    size_t want;
    if (blend->width == width && blend->height == height && blend->prev)
        return 1;
    want = (size_t)width * (size_t)height;
    if (want > blend->capacity) {
        uint32_t *grown = (uint32_t *)realloc(blend->prev,
                                              want * sizeof(*grown));
        if (!grown) return 0;
        blend->prev = grown;
        blend->capacity = want;
    }
    blend->width  = width;
    blend->height = height;
    blend->valid  = 0;
    return 1;
}

/* One pass for both entry points. `keep` non-zero leaves the kept frame
 * alone, so several presents can be blended against the same one. */
static int blend_frame(RecompFrameBlend *blend, void *frame, int width,
                       int height, size_t pitch_bytes, int keep) {
    unsigned char *base = (unsigned char *)frame;
    int blended;
    int y;

    if (!blend || !frame || width <= 0 || height <= 0) return 0;
    if (pitch_bytes < (size_t)width * 4u) return 0;
    if (!ensure_prev(blend, width, height)) return 0;
    /* Nothing kept yet and not allowed to keep: there is no frame to average
     * with and this call must not become the reference. Present as drawn. */
    if (keep && !blend->valid) return 0;

    /* prev keeps the UNBLENDED frame, so the mix never feeds back on itself
     * and the source buffer the host drew from stays pure for thumbnails and
     * captures. Both are read and written in one pass. */
    blended = blend->valid;
    for (y = 0; y < height; ++y) {
        uint32_t *row  = (uint32_t *)(void *)(base + (size_t)y * pitch_bytes);
        uint32_t *prev = blend->prev + (size_t)y * (size_t)width;
        int x;
        for (x = 0; x < width; ++x) {
            uint32_t cur = row[x];
            /* Per-byte average without widening: (a&b) + ((a^b)>>1) is the
             * mean of each byte, and masking the shift with 0x7F7F7F7F drops
             * the bit that crossed a byte boundary. Each byte result is at
             * most 255, so the add cannot carry between bytes either. */
            if (blended)
                row[x] = (cur & prev[x]) +
                         (((cur ^ prev[x]) >> 1) & 0x7F7F7F7Fu);
            if (!keep) prev[x] = cur;
        }
    }
    if (!keep) blend->valid = 1;
    return blended;
}

int recomp_frame_blend_apply(RecompFrameBlend *blend, void *frame,
                             int width, int height, size_t pitch_bytes) {
    return blend_frame(blend, frame, width, height, pitch_bytes, 0);
}

int recomp_frame_blend_apply_holding(RecompFrameBlend *blend, void *frame,
                                     int width, int height,
                                     size_t pitch_bytes) {
    return blend_frame(blend, frame, width, height, pitch_bytes, 1);
}
