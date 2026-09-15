#ifndef RECOMP_FLASH_GUARD_H
#define RECOMP_FLASH_GUARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Presentation flash guard — cap how far the presented picture may move
 * between one displayed frame and the next.
 *
 * WHAT IT IS FOR
 * --------------
 * Console games flash the whole screen: a super move connects, a round ends,
 * a boss explodes, and the backdrop is slammed between two extreme colours for
 * half a second. On the original hardware that was a design choice. For a
 * player with photosensitive epilepsy — or with migraine, vestibular
 * sensitivity, or simply a low tolerance for strobing — it is the reason they
 * cannot play the game at all.
 *
 * The thresholds are not a matter of taste. WCAG 2.3.1 counts a *flash* as a
 * pair of opposing changes in relative luminance of 10% or more of the range,
 * and calls content unsafe at more than three such flashes per second over
 * more than 25% of the visible area. The risk band runs from about 3 Hz to
 * 60 Hz and peaks between 15 Hz and 25 Hz.
 *
 * MEASURED, on the sequence this was written for (Gundam Wing: Endless Duel,
 * the end-of-fight finisher, from a player capture — see the GWED repo's
 * docs/FLASH_GUARD.md):
 *
 *     backdrop alternates flat blue (mean RGB 31,26,140) and near-white
 *     (152,153,150) — a frame-mean luminance swing of ~119/255, about 47% of
 *     the range, over essentially the whole screen, at 15 Hz, for ~0.65 s.
 *
 * That is roughly five times the WCAG flash threshold, at the frequency where
 * the risk is highest. It is not a borderline case.
 *
 * WHY IT IS NOT A FRAME BLEND
 * ---------------------------
 * recomp_frame_blend.h, next door, averages every presented frame with the one
 * before it to reconstruct 30 Hz flicker transparency. It does not help here
 * and cannot: the flash measured above holds each phase for TWO guest frames,
 * so averaging adjacent frames mostly averages a frame with its own twin and
 * both plateaus survive intact. Confirmed on the capture — with blending on
 * (this title ships it on) the plateaus still read 42 and 161.
 *
 * A fixed blend is also the wrong shape for the job. It costs every frame of
 * the game half a frame of ghosting in order to fix the one second in a match
 * that is actually dangerous. This filter is idle — bit-identical output, no
 * blend at all — on any frame whose step is already under the limit, which in
 * ordinary play is all of them.
 *
 * WHAT IT DOES
 * ------------
 * One number: the largest per-channel change, frame mean to frame mean, that
 * may reach the display in a single frame. Give it a step limit L (0..255).
 *
 *   - Step within L: the frame is presented exactly as drawn. Not touched,
 *     not copied through any arithmetic, byte-identical.
 *   - Step over L: the frame is mixed with the previously PRESENTED frame at
 *     alpha = L/step, which by construction lands the presented step on
 *     exactly L.
 *
 * Because a strobe alternates, the swing the player sees becomes about 2L
 * instead of the source's. At L = 12 that is a ~24/255 swing — under the 10%
 * WCAG line — in place of the measured 119.
 *
 * Everything the filter presents is a convex mix of two frames the game
 * actually drew, so it cannot invent a colour, cannot clip, and cannot push
 * anything out of gamut. That matters more than elegance for a feature whose
 * failure mode is a seizure.
 *
 * CUTS ARE NOT FLASHES
 * --------------------
 * A round starting, a stage loading, a menu opening — each is one large step
 * that does not come back. Limiting those would put a soft fade on every
 * scene change in the game, which is both wrong and irritating. So the filter
 * releases: after kReleaseFrames consecutive limited frames it lets the frame
 * through as drawn, and a cut costs two frames of ramp (~33 ms) rather than a
 * fade.
 *
 * It must NOT release during a strobe, and the release counter alone cannot
 * tell the two apart — a slow enough strobe would out-wait any fixed count.
 * What distinguishes them is that a strobe REVERSES. The filter watches the
 * sign of the frame-mean step, and a reversal larger than the limit latches a
 * hold for kHoldFrames; while that hold is live, release is disabled and the
 * strobe stays pinned however long it runs and whatever its period.
 *
 * The first flash of a sequence is limited before any of this is known,
 * because the limiter is always armed and detection only decides whether to
 * stop releasing. A guard that waited to confirm a flash would have to let
 * the first one through at full amplitude, and the first one is the one that
 * is least expected.
 *
 * WHAT IT DOES NOT TOUCH
 * ----------------------
 * Pixels on their way to the screen, and nothing else. No CPU, WRAM, VRAM,
 * OAM, CGRAM, APU or save state — so a session with this on executes the
 * identical guest frames as one without, two netplay peers stay digest-equal
 * whichever of them is running it, and it is not something a peer can be
 * required to match. That independence is the point: the player who needs it
 * is not usually the player who picked the lobby.
 *
 * Inert until a host creates one and calls apply; a host that never does is
 * byte-identical to one built before this file existed. Whether it is on by
 * default, and at what limit, is game policy decided in the game repo — this
 * file has no default of its own.
 */

typedef struct RecompFlashGuard RecompFlashGuard;

/* Presented-step limits, as the largest per-channel frame-mean change allowed
 * per displayed frame. A strobe's visible swing settles at about twice these.
 *
 * kRecompFlashGuardStandard is the one to reach for: 12/255 per step is a
 * ~9.4% swing, just inside the WCAG 2.3.1 10% flash threshold. Mild trades
 * some of that margin for a livelier picture; Maximum is for players who want
 * the effect gone rather than merely compliant. */
enum {
    kRecompFlashGuardMild     = 20,
    kRecompFlashGuardStandard = 12,
    kRecompFlashGuardMaximum  = 6
};

/* NULL on allocation failure. A NULL handle is legal everywhere below and
 * makes every call a no-op, so a host may ignore the failure and present
 * unguarded frames — the same contract as recomp_frame_blend. */
RecompFlashGuard *recomp_flash_guard_create(void);
void recomp_flash_guard_destroy(RecompFlashGuard *guard);

/* The step limit, 1..255. Values outside the range are clamped. 0 disables
 * the filter (apply becomes a no-op that still tracks nothing); prefer simply
 * not calling apply. Safe to change mid-session — it takes effect on the next
 * frame and does not disturb the kept frame. */
void recomp_flash_guard_set_limit(RecompFlashGuard *guard, int limit);
int recomp_flash_guard_get_limit(const RecompFlashGuard *guard);

/*
 * Forget the previous frame and drop the flash hold, so the next frame is
 * presented as drawn. Call it wherever the frame before is not the frame the
 * player was looking at: boot, reset, loading a save state, rewind, entering
 * or leaving the launcher. Carrying state across such a cut would mix a stale
 * frame into a new scene and could hold a phantom flash open.
 *
 * A frame size change resets automatically — the kept frame is indexed by the
 * width it was captured at.
 */
void recomp_flash_guard_reset(RecompFlashGuard *guard);

/*
 * Limit `frame` (little-endian ARGB8888/BGRA, `height` rows of `width` pixels
 * `pitch_bytes` apart) in place. Returns non-zero when the frame was actually
 * mixed — that is, when a flash was being suppressed this frame — and zero
 * when it passed through untouched, which is the ordinary case.
 *
 * `frame` MUST BE ORDINARY CACHED MEMORY, not a mapped texture, for the same
 * measured reason recomp_frame_blend.h gives at length: this is a
 * read-modify-write, SDL_LockTexture often hands back write-combined driver
 * memory, and reading it is pathological on some drivers and fine on others.
 * Blend into a staging frame, then upload it with one linear write-only copy.
 *
 * Only the low three bytes of each pixel are mixed. The top byte is taken
 * from the incoming frame unchanged, because a console framebuffer's top byte
 * is padding rather than coverage and some of them emit zero (see the GWED
 * black-window finding) — averaging it would be arithmetic on a value that
 * means nothing.
 */
int recomp_flash_guard_apply(RecompFlashGuard *guard, void *frame,
                             int width, int height, size_t pitch_bytes);

/*
 * What the guard has actually done, for a diagnostics line or a probe.
 *
 * A filter whose whole job is to not be noticed is a filter that cannot be
 * confirmed working by looking at it, so it has to be able to say. `limited`
 * counting zero over a session where the player saw a flash is the signal
 * that the guard is not wired into the path that presents.
 */
typedef struct RecompFlashGuardStats {
    uint64_t frames;         /* frames seen by apply */
    uint64_t limited;        /* of those, frames actually mixed */
    uint64_t events;         /* distinct flash sequences detected */
    int      peak_step;      /* largest per-channel source step seen, 0..255 */
    int      holding;        /* non-zero while a detected flash is held down */
} RecompFlashGuardStats;

void recomp_flash_guard_get_stats(const RecompFlashGuard *guard,
                                  RecompFlashGuardStats *out);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_FLASH_GUARD_H */
