#ifndef RECOMP_FRAME_BLEND_H
#define RECOMP_FRAME_BLEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Presentation frame blending — average each presented frame with the one
 * before it.
 *
 * WHY THIS IS SHARED, AND NOT IN A GAME
 * -------------------------------------
 * Many console games fake translucency by drawing a sprite on alternate
 * frames only: thruster flames, explosions, shadows, HUD panels. On a CRT the
 * phosphor persistence averages the two phases and the player sees 50%
 * transparency. The trick relies on every guest frame being shown exactly
 * once, whole, in order -- and a desktop display breaks that both ways. With
 * vsync on, a 60.00 Hz panel against a 60.0988 Hz guest duplicates a frame
 * roughly every ten seconds, showing the same flicker phase twice. With vsync
 * off the scanout tears and splits the on-frame and the off-frame across one
 * visible field.
 *
 * Averaging the presented frame with the previous one puts both phases in
 * every displayed frame, so the flicker becomes steady translucency and stops
 * caring about pacing at all. It costs half a frame of motion ghosting.
 *
 * Nothing above is specific to a console, a game, or a renderer -- it is
 * arithmetic on two ARGB8888 buffers. It lives here, next to
 * RecompLauncherCSettings.frame_blend and the Display row that sets it
 * (GameInfo.has_frame_blend), so that every port gets one implementation
 * rather than a copy per game that cannot inherit a fix. The measured
 * write-combined-memory hazard documented under `apply` is exactly the kind
 * of fix a copy would miss.
 *
 * The capability is inert until a host creates one and calls apply: a host
 * that never does is byte-identical to one built before this file existed.
 * Whether it is on by default is GAME policy, decided in the game repo --
 * this file has no default of its own.
 */

/* Capability probe for hosts built against an unknown pin of this repo: the
 * holding entry point below arrived after the rest, and a host that wants it
 * must be able to compile without it. */
#define RECOMP_FRAME_BLEND_HAS_HOLDING 1

typedef struct RecompFrameBlend RecompFrameBlend;

/* NULL on allocation failure. A NULL handle is legal everywhere below and
 * makes every call a no-op, so a host may ignore the failure and simply
 * present unblended frames. */
RecompFrameBlend *recomp_frame_blend_create(void);
void recomp_frame_blend_destroy(RecompFrameBlend *blend);

/*
 * Forget the previous frame, so the next one is presented as drawn. Call it
 * wherever the frame that came before is not the frame the player was just
 * looking at: boot, reset, loading a save state, entering or leaving a mode
 * that changes the picture wholesale. Blending across such a cut mixes one
 * stale frame into the new scene.
 *
 * A frame size change resets automatically -- the kept frame is indexed by
 * the width it was captured at, so keeping it across a resize would blend
 * misaligned rows.
 */
void recomp_frame_blend_reset(RecompFrameBlend *blend);

/*
 * Blend `frame` (little-endian ARGB8888/BGRA, `height` rows of `width` pixels
 * `pitch_bytes` apart) with the previously applied frame, in place. Returns
 * non-zero when the frame was modified -- zero on the first frame after a
 * reset or a size change, which is captured and presented as drawn, and zero
 * for a NULL handle or NULL frame.
 *
 * `frame` MUST BE ORDINARY CACHED MEMORY, not a mapped texture. The blend is
 * a read-modify-write, and SDL_LockTexture on a streaming texture frequently
 * hands back write-combined, uncached driver memory: excellent for sequential
 * writes, pathological to read, because every load is an uncached fetch.
 * Whether the mapping is cached or write-combined depends on the backend and
 * the driver, so blending in a locked texture is fine on one machine and
 * ruins the frame rate on another -- the shape of a bug that never reproduces
 * for the developer. Blend into a staging frame here, then upload it with one
 * linear write-only copy, which is what write-combined memory is good at.
 *
 * All four bytes of each pixel are averaged, alpha included. Hosts whose
 * frames carry a meaningful alpha channel should be aware that it is
 * averaged too; a console framebuffer that leaves the top byte constant is
 * unaffected (constant averaged with itself is itself).
 */
int recomp_frame_blend_apply(RecompFrameBlend *blend, void *frame,
                             int width, int height, size_t pitch_bytes);

/*
 * As above, but WITHOUT replacing the kept frame: the next call still blends
 * against the same one. Same arguments, same return, and it never captures --
 * with nothing kept yet it leaves the frame as drawn and returns zero.
 *
 * For hosts whose presentation clock is decoupled from the simulation, which
 * present one guest frame more than once (interpolated, at the display's
 * rate). The pairing the effect is about is two consecutive GUEST frames, so
 * the kept frame must advance once per guest frame: `apply` on the present
 * that carries a new one, this on the presents in between. Blending only the
 * fresh presents and leaving the others as drawn would make the picture
 * alternate blended and unblended -- flicker, in a feature whose whole job is
 * to remove it.
 */
int recomp_frame_blend_apply_holding(RecompFrameBlend *blend, void *frame,
                                     int width, int height,
                                     size_t pitch_bytes);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_FRAME_BLEND_H */
