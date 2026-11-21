/* recomp_emoji_freetype.c — color emoji through FreeType on a bitmap color
 * font (Noto Color Emoji's CBDT strikes, Apple's sbix). Optional HarfBuzz
 * shapes multi-codepoint sequences into their ligature glyphs (flags, ZWJ
 * families, skin tones); without it each base codepoint renders on its own,
 * which is still color, just not joined. Compiled only when the build found
 * FreeType (RECOMP_UI_HAVE_FREETYPE). */
#include "recomp_emoji.h"

#if defined(RECOMP_UI_HAVE_FREETYPE)

#include <ft2build.h>
#include FT_FREETYPE_H

#if defined(RECOMP_UI_HAVE_HARFBUZZ)
#include <hb.h>
#include <hb-ft.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FT_Library g_lib;
static FT_Face    g_face;
static int        g_tried;
static int        g_strike_h;   /* pixel height of the selected strike */
#if defined(RECOMP_UI_HAVE_HARFBUZZ)
static hb_font_t* g_hb;
#endif

static const char* const kCandidates[] = {
    "/usr/share/fonts/noto/NotoColorEmoji.ttf",
    "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
    "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
    "/usr/share/fonts/google-noto/NotoColorEmoji.ttf",
    "/usr/share/fonts/TTF/NotoColorEmoji.ttf",
    "/usr/local/share/fonts/NotoColorEmoji.ttf",
    "/System/Library/Fonts/Apple Color Emoji.ttc",
    NULL,
};

static int open_font(void) {
    const char* env;
    int i;
    if (g_tried) return g_face != NULL;
    g_tried = 1;
    if (FT_Init_FreeType(&g_lib) != 0) return 0;
    env = getenv("RECOMP_UI_EMOJI_FONT");
    if (env && env[0] && FT_New_Face(g_lib, env, 0, &g_face) != 0) g_face = NULL;
    for (i = 0; !g_face && kCandidates[i]; ++i) {
        if (FT_New_Face(g_lib, kCandidates[i], 0, &g_face) != 0) g_face = NULL;
    }
    if (!g_face) return 0;
    /* Bitmap color strikes only: an outline-only font would just give us the
     * same monochrome shapes the atlas already has. */
    if (!FT_HAS_COLOR(g_face) || g_face->num_fixed_sizes <= 0) {
        FT_Done_Face(g_face);
        g_face = NULL;
        return 0;
    }
    {
        /* The largest strike downsamples best. */
        int best = 0;
        for (i = 1; i < g_face->num_fixed_sizes; ++i)
            if (g_face->available_sizes[i].height > g_face->available_sizes[best].height)
                best = i;
        if (FT_Select_Size(g_face, best) != 0) {
            FT_Done_Face(g_face);
            g_face = NULL;
            return 0;
        }
        g_strike_h = g_face->available_sizes[best].height;
        if (g_strike_h <= 0) g_strike_h = (int)(g_face->size->metrics.height >> 6);
        if (g_strike_h <= 0) g_strike_h = 109;
    }
#if defined(RECOMP_UI_HAVE_HARFBUZZ)
    g_hb = hb_ft_font_create_referenced(g_face);
#endif
    return 1;
}

const char* recomp_emoji_freetype_name(void) {
#if defined(RECOMP_UI_HAVE_HARFBUZZ)
    return "freetype+harfbuzz";
#else
    return "freetype";
#endif
}

/* Glyph run for the sequence: ids and advances in strike pixels. */
#define MAX_RUN 16
typedef struct { unsigned gid; int advance; } RunGlyph;

static size_t utf8_next(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (!len) return 0;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && len >= 2) { *cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3F); return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static int shape(const char* utf8, size_t len, RunGlyph* run, int cap) {
    int n = 0;
#if defined(RECOMP_UI_HAVE_HARFBUZZ)
    if (g_hb) {
        hb_buffer_t* buf = hb_buffer_create();
        unsigned int count = 0, i;
        hb_glyph_info_t* info;
        hb_glyph_position_t* pos;
        hb_buffer_add_utf8(buf, utf8, (int)len, 0, (int)len);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, HB_SCRIPT_COMMON);
        hb_buffer_set_language(buf, hb_language_from_string("en", -1));
        hb_shape(g_hb, buf, NULL, 0);
        info = hb_buffer_get_glyph_infos(buf, &count);
        pos = hb_buffer_get_glyph_positions(buf, &count);
        for (i = 0; i < count && n < cap; ++i) {
            if (info[i].codepoint == 0) continue; /* .notdef: a joiner the font swallowed */
            run[n].gid = info[i].codepoint;
            run[n].advance = (int)(pos[i].x_advance / 64);
            ++n;
        }
        hb_buffer_destroy(buf);
        return n;
    }
#endif
    /* No shaper: base codepoints only; joiners, selectors and modifiers are
     * dropped rather than drawn as boxes. */
    {
        size_t i = 0;
        while (i < len && n < cap) {
            uint32_t c;
            size_t k = utf8_next(utf8 + i, len - i, &c);
            if (!k) break;
            i += k;
            if (c == 0x200D || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) ||
                (c >= 0xE0020 && c <= 0xE007F))
                continue;
            {
                unsigned gid = FT_Get_Char_Index(g_face, c);
                if (!gid) continue;
                run[n].gid = gid;
                run[n].advance = 0; /* filled from the glyph metrics below */
                ++n;
            }
        }
    }
    return n;
}

int recomp_emoji_render_freetype(const char* utf8, size_t len, int px, RecompEmojiBitmap* out) {
    RunGlyph run[MAX_RUN];
    int n, i, canvas_w = 0, canvas_h, pen = 0;
    unsigned char* canvas;
    float scale;
    int ow, oh, x, y;

    if (!out || !utf8 || !len || px <= 0) return 0;
    memset(out, 0, sizeof(*out));
    if (!open_font()) return 0;
    n = shape(utf8, len, run, MAX_RUN);
    if (n <= 0) return 0;

    /* Strike-size canvas, glyphs laid out left to right. */
    canvas_h = g_strike_h;
    for (i = 0; i < n; ++i) {
        if (FT_Load_Glyph(g_face, run[i].gid, FT_LOAD_COLOR | FT_LOAD_DEFAULT) != 0) return 0;
        if (run[i].advance <= 0) run[i].advance = (int)(g_face->glyph->advance.x >> 6);
        if (run[i].advance <= 0) run[i].advance = (int)g_face->glyph->bitmap.width;
        canvas_w += run[i].advance;
    }
    if (canvas_w <= 0 || canvas_h <= 0) return 0;
    canvas = (unsigned char*)calloc((size_t)canvas_w * (size_t)canvas_h, 4);
    if (!canvas) return 0;
    for (i = 0; i < n; ++i) {
        FT_Bitmap* bm;
        int gx, gy, bw, bh;
        if (FT_Load_Glyph(g_face, run[i].gid, FT_LOAD_COLOR | FT_LOAD_DEFAULT) != 0) continue;
        if (g_face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
            FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL) != 0)
            continue;
        bm = &g_face->glyph->bitmap;
        if (bm->pixel_mode != FT_PIXEL_MODE_BGRA) continue; /* not a color glyph */
        bw = (int)bm->width;
        bh = (int)bm->rows;
        gx = pen + g_face->glyph->bitmap_left;
        gy = (int)(g_face->size->metrics.ascender >> 6) - g_face->glyph->bitmap_top;
        if (gy < 0) gy = 0;
        for (y = 0; y < bh; ++y) {
            const unsigned char* src = bm->buffer + (size_t)y * (size_t)bm->pitch;
            int cy = gy + y;
            if (cy < 0 || cy >= canvas_h) continue;
            for (x = 0; x < bw; ++x) {
                int cx = gx + x;
                unsigned char* dst;
                if (cx < 0 || cx >= canvas_w) continue;
                dst = canvas + ((size_t)cy * canvas_w + cx) * 4;
                /* BGRA premultiplied in, kept premultiplied on the canvas
                 * (source-over is a straight add at premultiplied alpha). */
                {
                    unsigned a = src[x * 4 + 3];
                    unsigned inv = 255 - a;
                    dst[0] = (unsigned char)(src[x * 4 + 2] + (dst[0] * inv) / 255);
                    dst[1] = (unsigned char)(src[x * 4 + 1] + (dst[1] * inv) / 255);
                    dst[2] = (unsigned char)(src[x * 4 + 0] + (dst[2] * inv) / 255);
                    dst[3] = (unsigned char)(a + (dst[3] * inv) / 255);
                }
            }
        }
        pen += run[i].advance;
    }

    /* Box downscale to the requested height, then un-premultiply. */
    scale = (float)px / (float)canvas_h;
    oh = px;
    ow = (int)((float)canvas_w * scale + 0.5f);
    if (ow < 1) ow = 1;
    out->rgba = (unsigned char*)calloc((size_t)ow * (size_t)oh, 4);
    if (!out->rgba) { free(canvas); return 0; }
    for (y = 0; y < oh; ++y) {
        int sy0 = (int)((float)y / scale), sy1 = (int)((float)(y + 1) / scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > canvas_h) sy1 = canvas_h;
        for (x = 0; x < ow; ++x) {
            int sx0 = (int)((float)x / scale), sx1 = (int)((float)(x + 1) / scale);
            unsigned acc[4] = {0, 0, 0, 0}, cnt = 0, sx, sy;
            unsigned char* d = out->rgba + ((size_t)y * ow + x) * 4;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > canvas_w) sx1 = canvas_w;
            for (sy = (unsigned)sy0; sy < (unsigned)sy1; ++sy)
                for (sx = (unsigned)sx0; sx < (unsigned)sx1; ++sx) {
                    const unsigned char* s = canvas + ((size_t)sy * canvas_w + sx) * 4;
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                    ++cnt;
                }
            if (!cnt) continue;
            {
                unsigned a = acc[3] / cnt;
                if (a) {
                    unsigned r = acc[0] / cnt, g = acc[1] / cnt, b = acc[2] / cnt;
                    r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
                    d[0] = (unsigned char)(r > 255 ? 255 : r);
                    d[1] = (unsigned char)(g > 255 ? 255 : g);
                    d[2] = (unsigned char)(b > 255 ? 255 : b);
                }
                d[3] = (unsigned char)a;
            }
        }
    }
    free(canvas);
    out->w = ow;
    out->h = oh;
    return 1;
}

#endif /* RECOMP_UI_HAVE_FREETYPE */
