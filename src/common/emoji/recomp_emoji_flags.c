/* recomp_emoji_flags.c — country flags from a bundled sprite sheet.
 *
 * Segoe UI Emoji has no flag glyphs: on Windows a regional-indicator pair
 * comes back from DirectWrite as two boxed letters, "successfully". A
 * platform with no emoji provider has nothing at all. So flags do not go
 * through the provider: they come from assets/img/flags.png, a 26x26 grid of
 * cells indexed by the two letters of the region code (row = first letter,
 * column = second), rendered from Noto Color Emoji by tools/gen_flag_sheet.py.
 * Every platform draws the same flag for the same country.
 *
 * The sheet is decoded once; a render samples the cell down to the requested
 * height with a box filter in premultiplied space (no dark fringes). */
#include "recomp_emoji.h"

#include <stdlib.h>
#include <string.h>

/* A private, static stb_image (PNG only), as launcher_gl.c keeps its own:
 * a host that embeds stb_image too must never see a second public impl. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include "third_party/stb_image.h"

#define FLAG_GRID 26

static unsigned char* g_sheet = NULL;   /* RGBA, straight alpha */
static int g_sheet_w = 0, g_sheet_h = 0;
static int g_cell_w = 0, g_cell_h = 0;
static unsigned char g_present[FLAG_GRID * FLAG_GRID];

static int letter_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return -1;
    return c - 'A';
}

int recomp_emoji_flags_load(const char* png_path) {
    int w = 0, h = 0, comp = 0;
    unsigned char* px;
    if (!png_path || !png_path[0]) return 0;
    px = stbi_load(png_path, &w, &h, &comp, 4);
    if (!px) return 0;
    if (w < FLAG_GRID || h < FLAG_GRID || (w % FLAG_GRID) != 0 || (h % FLAG_GRID) != 0) {
        stbi_image_free(px);
        return 0;
    }
    free(g_sheet);
    g_sheet = px;
    g_sheet_w = w;
    g_sheet_h = h;
    g_cell_w = w / FLAG_GRID;
    g_cell_h = h / FLAG_GRID;
    /* A cell with any coverage is a flag; the rest of the grid is empty. */
    memset(g_present, 0, sizeof(g_present));
    for (int r = 0; r < FLAG_GRID; r++) {
        for (int c = 0; c < FLAG_GRID; c++) {
            int any = 0;
            for (int y = 0; y < g_cell_h && !any; y++) {
                const unsigned char* row =
                    g_sheet + ((size_t)(r * g_cell_h + y) * (size_t)g_sheet_w + (size_t)c * g_cell_w) * 4;
                for (int x = 0; x < g_cell_w; x++) {
                    if (row[x * 4 + 3]) { any = 1; break; }
                }
            }
            g_present[r * FLAG_GRID + c] = (unsigned char)any;
        }
    }
    return 1;
}

int recomp_emoji_flags_available(void) { return g_sheet != NULL; }

int recomp_emoji_flags_has(char first, char second) {
    const int a = letter_index(first), b = letter_index(second);
    if (!g_sheet || a < 0 || b < 0) return 0;
    return g_present[a * FLAG_GRID + b] != 0;
}

/* A sequence that is exactly two regional indicators, as letters. */
int recomp_emoji_flags_code(const char* utf8, size_t len, char* first, char* second) {
    const unsigned char* p = (const unsigned char*)utf8;
    if (!utf8 || len != 8) return 0;
    /* U+1F1E6..U+1F1FF are F0 9F 87 A6..BF. */
    if (p[0] != 0xF0 || p[1] != 0x9F || p[2] != 0x87 || p[3] < 0xA6 || p[3] > 0xBF) return 0;
    if (p[4] != 0xF0 || p[5] != 0x9F || p[6] != 0x87 || p[7] < 0xA6 || p[7] > 0xBF) return 0;
    if (first) *first = (char)('A' + (p[3] - 0xA6));
    if (second) *second = (char)('A' + (p[7] - 0xA6));
    return 1;
}

int recomp_emoji_flags_render(char first, char second, int px, RecompEmojiBitmap* out) {
    const int a = letter_index(first), b = letter_index(second);
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!recomp_emoji_flags_has(first, second) || px <= 0) return 0;
    {
        const int oh = px;
        const int ow = (int)((float)g_cell_w * (float)px / (float)g_cell_h + 0.5f);
        const float sx = (float)g_cell_w / (float)ow;
        const float sy = (float)g_cell_h / (float)oh;
        unsigned char* dst;
        if (ow <= 0) return 0;
        dst = (unsigned char*)malloc((size_t)ow * (size_t)oh * 4);
        if (!dst) return 0;
        for (int y = 0; y < oh; y++) {
            int y0 = (int)((float)y * sy), y1 = (int)((float)(y + 1) * sy);
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > g_cell_h) y1 = g_cell_h;
            for (int x = 0; x < ow; x++) {
                int x0 = (int)((float)x * sx), x1 = (int)((float)(x + 1) * sx);
                float r = 0, g = 0, bl = 0, al = 0;
                int n = 0;
                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > g_cell_w) x1 = g_cell_w;
                for (int yy = y0; yy < y1; yy++) {
                    const unsigned char* row =
                        g_sheet + ((size_t)(a * g_cell_h + yy) * (size_t)g_sheet_w + (size_t)b * g_cell_w) * 4;
                    for (int xx = x0; xx < x1; xx++) {
                        const unsigned char* s = row + xx * 4;
                        const float sa = (float)s[3] / 255.0f;
                        r += (float)s[0] * sa;
                        g += (float)s[1] * sa;
                        bl += (float)s[2] * sa;
                        al += sa;
                        n++;
                    }
                }
                {
                    unsigned char* d = dst + ((size_t)y * (size_t)ow + (size_t)x) * 4;
                    if (n == 0 || al <= 0.0f) {
                        d[0] = d[1] = d[2] = d[3] = 0;
                    } else {
                        /* Un-premultiply: the atlas wants straight alpha. */
                        d[0] = (unsigned char)(r / al + 0.5f);
                        d[1] = (unsigned char)(g / al + 0.5f);
                        d[2] = (unsigned char)(bl / al + 0.5f);
                        d[3] = (unsigned char)(al / (float)n * 255.0f + 0.5f);
                    }
                }
            }
        }
        out->w = ow;
        out->h = oh;
        out->rgba = dst;
    }
    return 1;
}
