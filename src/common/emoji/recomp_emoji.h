/* recomp_emoji.h — color emoji for the launcher's text.
 *
 * Dear ImGui rasterizes fonts with stb_truetype, which only understands
 * outline glyphs; the launcher's emoji have therefore been OpenMoji's black
 * outlines. This module renders emoji SEQUENCES (one visual emoji, one or
 * more codepoints: skin tones, ZWJ families, flags, keycaps) to RGBA sprites
 * through whatever the platform has, so the atlas can carry them as custom
 * glyphs:
 *
 *   Windows : DirectWrite + Direct2D on Segoe UI Emoji (the OS look, shaped)
 *   Linux   : FreeType on the system Noto Color Emoji (+HarfBuzz for shaping
 *             when the build has it; without it, sequences render base-by-base)
 *   other   : no provider -> recomp_emoji_render() fails and the caller keeps
 *             the outline glyphs. That is the fallback, and a console port
 *             lands on it with no code change.
 *
 * FLAGS are the exception: Segoe UI Emoji has none (Windows shows two boxed
 * letters), so a regional-indicator pair is served from a bundled sprite
 * sheet (recomp_emoji_flags_load) on every platform, provider or not.
 *
 * Everything here is plain C so the ImGui side and any host can call it. */
#ifndef RECOMP_EMOJI_H
#define RECOMP_EMOJI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompEmojiBitmap {
    int            w;
    int            h;
    unsigned char* rgba;   /* straight (non-premultiplied) alpha; free with recomp_emoji_free */
} RecompEmojiBitmap;

/* Name of the provider that will answer render calls: "directwrite",
 * "freetype+harfbuzz", "freetype", or "none". Probes lazily on first call. */
const char* recomp_emoji_backend_name(void);
int         recomp_emoji_backend_available(void);

/* Render one emoji sequence (UTF-8, `len` bytes) to a sprite about `px`
 * pixels tall. 1 on success (out filled), 0 when no provider could. */
int  recomp_emoji_render(const char* utf8, size_t len, int px, RecompEmojiBitmap* out);
void recomp_emoji_free(RecompEmojiBitmap* bm);

/* Find the next emoji sequence in `utf8` at or after byte `pos`. 1 when one
 * was found (start/seq_len set, in bytes); 0 when none remain. Text-default
 * symbols (©, ®, ™, ‼ …) only count with a VS16 after them, so ordinary
 * punctuation is never pulled out of a sentence. */
int  recomp_emoji_scan(const char* utf8, size_t len, size_t pos,
                       size_t* start, size_t* seq_len);

/* Country flags: a 26x26 sprite sheet (assets/img/flags.png, built by
 * tools/gen_flag_sheet.py). Load it once at startup; recomp_emoji_render
 * then serves any regional-indicator pair from it ahead of the provider.
 * `has` says whether the sheet carries a code, so a UI can decide before
 * drawing whether it gets a flag or should print the letters. */
int  recomp_emoji_flags_load(const char* png_path);
int  recomp_emoji_flags_available(void);
int  recomp_emoji_flags_has(char first, char second);

/* Providers (internal). Each returns 1 / 0 like recomp_emoji_render. */
int  recomp_emoji_flags_code(const char* utf8, size_t len, char* first, char* second);
int  recomp_emoji_flags_render(char first, char second, int px, RecompEmojiBitmap* out);
int recomp_emoji_render_freetype(const char* utf8, size_t len, int px, RecompEmojiBitmap* out);
const char* recomp_emoji_freetype_name(void);
int recomp_emoji_render_win32(const char* utf8, size_t len, int px, RecompEmojiBitmap* out);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EMOJI_H */
