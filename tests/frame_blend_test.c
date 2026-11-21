/* The shared presentation blend, including the holding variant a host with a
 * decoupled presentation clock needs.
 *
 * Why the holding variant exists: the pairing frame blending is about is two
 * consecutive GUEST frames, but such a host presents one guest frame more
 * than once. Letting every present advance the kept frame would average a
 * frame with an interpolated version of itself; blending only the fresh
 * presents and leaving the rest as drawn would make the picture alternate
 * blended and unblended. So every present blends, and only the one carrying a
 * new guest frame advances the reference.
 */
#include "recomp_frame_blend.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

#define W 4
#define H 2
#define PITCH ((size_t)W * 4u)

static void fill(uint32_t* f, uint32_t v) {
    for (int i = 0; i < W * H; ++i) f[i] = v;
}

static int all(const uint32_t* f, uint32_t v) {
    for (int i = 0; i < W * H; ++i) if (f[i] != v) return 0;
    return 1;
}

int main(void) {
    RecompFrameBlend* b = recomp_frame_blend_create();
    uint32_t f[W * H];
    expect(b != NULL, "the blend allocates");

    /* First frame after a reset is the reference: presented as drawn. */
    fill(f, 0xFF000000u);
    expect(recomp_frame_blend_apply(b, f, W, H, PITCH) == 0,
           "the first frame is not blended");
    expect(all(f, 0xFF000000u), "and is presented as drawn");

    /* Second frame averages with it, per byte. 0x00 with 0xFF is 0x7F. */
    fill(f, 0xFFFFFFFFu);
    expect(recomp_frame_blend_apply(b, f, W, H, PITCH) == 1,
           "the second frame is blended");
    expect(all(f, 0xFF7F7F7Fu), "and each byte is the mean of the two");

    /* The kept frame is the UNBLENDED second one, not the mix just produced:
     * a third identical frame comes back unchanged. */
    fill(f, 0xFFFFFFFFu);
    recomp_frame_blend_apply(b, f, W, H, PITCH);
    expect(all(f, 0xFFFFFFFFu), "the kept frame is the unblended one");

    /* Holding: blends, but leaves the reference where it was, so two holds in
     * a row against the same content give the same answer. */
    fill(f, 0xFF000000u);
    expect(recomp_frame_blend_apply_holding(b, f, W, H, PITCH) == 1,
           "a holding present is blended too");
    expect(all(f, 0xFF7F7F7Fu), "against the kept frame");
    fill(f, 0xFF000000u);
    recomp_frame_blend_apply_holding(b, f, W, H, PITCH);
    expect(all(f, 0xFF7F7F7Fu),
           "and a second holding present gets the same answer, not a new mix");

    /* The next non-holding present advances the reference again. */
    fill(f, 0xFF000000u);
    recomp_frame_blend_apply(b, f, W, H, PITCH);
    fill(f, 0xFF000000u);
    recomp_frame_blend_apply_holding(b, f, W, H, PITCH);
    expect(all(f, 0xFF000000u), "apply advances the reference, holding sees it");

    /* Reset: nothing kept. A holding present must not become the reference,
     * or the frame after a save-state load would be averaged into the new
     * scene. */
    recomp_frame_blend_reset(b);
    fill(f, 0xFFFFFFFFu);
    expect(recomp_frame_blend_apply_holding(b, f, W, H, PITCH) == 0,
           "holding with nothing kept does not blend");
    expect(all(f, 0xFFFFFFFFu), "and presents as drawn");
    fill(f, 0xFF000000u);
    expect(recomp_frame_blend_apply(b, f, W, H, PITCH) == 0,
           "and it did not become the reference either");

    /* A NULL handle is legal and inert. */
    fill(f, 0xFF123456u);
    expect(recomp_frame_blend_apply_holding(NULL, f, W, H, PITCH) == 0 &&
           all(f, 0xFF123456u), "a NULL handle is a no-op");

    recomp_frame_blend_destroy(b);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("frame_blend_test: ok");
    return 0;
}
