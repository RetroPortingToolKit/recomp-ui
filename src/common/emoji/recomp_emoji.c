/* recomp_emoji.c — sequence scanner + provider dispatch. See recomp_emoji.h. */
#include "recomp_emoji.h"

#include <stdlib.h>
#include <string.h>

/* ---- UTF-8 ------------------------------------------------------------ */

static size_t utf8_decode(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (len == 0) { *cp = 0; return 0; }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && len >= 2 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && len >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && len >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

/* ---- Emoji classes (Unicode Emoji data, the parts that matter here) ---- */

#define CP_ZWJ     0x200Du
#define CP_VS16    0xFE0Fu
#define CP_KEYCAP  0x20E3u

static int in(uint32_t c, uint32_t lo, uint32_t hi) { return c >= lo && c <= hi; }
static int is_modifier(uint32_t c) { return in(c, 0x1F3FB, 0x1F3FF); }
static int is_regional(uint32_t c) { return in(c, 0x1F1E6, 0x1F1FF); }
static int is_tag(uint32_t c) { return in(c, 0xE0020, 0xE007E); }
static int is_tag_end(uint32_t c) { return c == 0xE007F; }
static int is_keycap_base(uint32_t c) { return c == '#' || c == '*' || in(c, '0', '9'); }

/* Emoji-presentation by default: an emoji on its own, no VS16 needed. */
static int is_emoji_default(uint32_t c) {
    return in(c, 0x1F300, 0x1F5FF) || in(c, 0x1F600, 0x1F64F) || in(c, 0x1F680, 0x1F6FF) ||
           in(c, 0x1F900, 0x1F9FF) || in(c, 0x1FA70, 0x1FAFF) || in(c, 0x1F7E0, 0x1F7EB) ||
           in(c, 0x2614, 0x2615) || in(c, 0x2648, 0x2653) || c == 0x267F || c == 0x2693 ||
           c == 0x26A1 || in(c, 0x26AA, 0x26AB) || in(c, 0x26BD, 0x26BE) || in(c, 0x26C4, 0x26C5) ||
           c == 0x26CE || c == 0x26D4 || c == 0x26EA || in(c, 0x26F2, 0x26F3) || c == 0x26F5 ||
           c == 0x26FA || c == 0x26FD || c == 0x2705 || in(c, 0x270A, 0x270B) || c == 0x2728 ||
           c == 0x274C || c == 0x274E || in(c, 0x2753, 0x2755) || c == 0x2757 ||
           in(c, 0x2795, 0x2797) || c == 0x27B0 || c == 0x27BF || in(c, 0x2B1B, 0x2B1C) ||
           c == 0x2B50 || c == 0x2B55 || in(c, 0x231A, 0x231B) || in(c, 0x23E9, 0x23EC) ||
           c == 0x23F0 || c == 0x23F3 || in(c, 0x25FD, 0x25FE) || c == 0x1F004 || c == 0x1F0CF ||
           c == 0x1F18E || in(c, 0x1F191, 0x1F19A) || c == 0x1F201 || c == 0x1F21A ||
           c == 0x1F22F || in(c, 0x1F232, 0x1F236) || in(c, 0x1F238, 0x1F23A) ||
           in(c, 0x1F250, 0x1F251);
}

/* Text-presentation by default: emoji only when VS16 follows (☺️, ❤️, ©️). */
static int is_emoji_text_default(uint32_t c) {
    return c == 0x00A9 || c == 0x00AE || c == 0x203C || c == 0x2049 || c == 0x2122 ||
           c == 0x2139 || in(c, 0x2194, 0x2199) || in(c, 0x21A9, 0x21AA) || c == 0x2328 ||
           c == 0x23CF || in(c, 0x23ED, 0x23EF) || in(c, 0x23F1, 0x23F2) ||
           in(c, 0x23F8, 0x23FA) || c == 0x24C2 || in(c, 0x25AA, 0x25AB) || c == 0x25B6 ||
           c == 0x25C0 || in(c, 0x25FB, 0x25FC) || in(c, 0x2600, 0x2604) || c == 0x260E ||
           c == 0x2611 || c == 0x2618 || c == 0x261D || c == 0x2620 || in(c, 0x2622, 0x2623) ||
           c == 0x2626 || c == 0x262A || in(c, 0x262E, 0x262F) || in(c, 0x2638, 0x263A) ||
           c == 0x2640 || c == 0x2642 || in(c, 0x265F, 0x2660) || c == 0x2663 ||
           in(c, 0x2665, 0x2666) || c == 0x2668 || c == 0x267B || c == 0x267E ||
           in(c, 0x2692, 0x2697) || c == 0x2699 || in(c, 0x269B, 0x269C) || in(c, 0x26A0, 0x26A7) ||
           in(c, 0x26B0, 0x26B1) || in(c, 0x26C8, 0x26CF) || c == 0x26D1 || c == 0x26D3 ||
           in(c, 0x26E9, 0x26F1) || c == 0x26F4 || in(c, 0x26F7, 0x26F9) || in(c, 0x2702, 0x2704) ||
           c == 0x2708 || c == 0x2709 || in(c, 0x270C, 0x270D) || c == 0x270F || c == 0x2712 ||
           c == 0x2714 || c == 0x2716 || c == 0x271D || c == 0x2721 || c == 0x2733 || c == 0x2734 ||
           c == 0x2744 || c == 0x2747 || c == 0x2763 || c == 0x2764 || in(c, 0x27A1, 0x27A1) ||
           in(c, 0x2934, 0x2935) || in(c, 0x2B05, 0x2B07) || c == 0x3030 || c == 0x303D ||
           c == 0x3297 || c == 0x3299 || in(c, 0x1F170, 0x1F171) || in(c, 0x1F17E, 0x1F17F) ||
           c == 0x1F202 || c == 0x1F237;
}

/* One element of a sequence starting at `p`: base (+VS16)(+modifier), a
 * regional-indicator pair, a keycap, or a tag sequence. Returns bytes
 * consumed, 0 if `p` does not start an emoji element. */
static size_t scan_element(const char* s, size_t len) {
    uint32_t c, n;
    size_t i = utf8_decode(s, len, &c);
    if (!i) return 0;
    if (is_regional(c)) {
        size_t j = utf8_decode(s + i, len - i, &n);
        return (j && is_regional(n)) ? i + j : i;  /* a lone RI still renders as itself */
    }
    if (is_keycap_base(c)) {
        size_t j = i, k;
        k = utf8_decode(s + j, len - j, &n);
        if (k && n == CP_VS16) j += k;
        k = utf8_decode(s + j, len - j, &n);
        if (k && n == CP_KEYCAP) return j + k;
        return 0;
    }
    if (is_emoji_default(c) || is_modifier(c)) {
        size_t j = i, k;
        k = utf8_decode(s + j, len - j, &n);
        if (k && n == CP_VS16) { j += k; k = utf8_decode(s + j, len - j, &n); }
        if (k && is_modifier(n) && !is_modifier(c)) { j += k; k = utf8_decode(s + j, len - j, &n); }
        /* Tag sequence (subdivision flags): base + tags + cancel tag. */
        if (k && is_tag(n)) {
            size_t t = j;
            while (k && is_tag(n)) { t += k; k = utf8_decode(s + t, len - t, &n); }
            if (k && is_tag_end(n)) return t + k;
        }
        return j;
    }
    if (is_emoji_text_default(c)) {
        size_t k = utf8_decode(s + i, len - i, &n);
        if (k && n == CP_VS16) {
            size_t j = i + k;
            k = utf8_decode(s + j, len - j, &n);
            if (k && is_modifier(n)) j += k;
            return j;
        }
        return 0;
    }
    return 0;
}

int recomp_emoji_scan(const char* utf8, size_t len, size_t pos,
                      size_t* start, size_t* seq_len) {
    if (!utf8 || !start || !seq_len) return 0;
    while (pos < len) {
        size_t e = scan_element(utf8 + pos, len - pos);
        if (e) {
            size_t end = pos + e;
            /* ZWJ joins: keep going while ZWJ + element follows. */
            for (;;) {
                uint32_t n;
                size_t k = utf8_decode(utf8 + end, len - end, &n);
                if (!k || n != CP_ZWJ) break;
                size_t e2 = scan_element(utf8 + end + k, len - end - k);
                if (!e2) break;
                end += k + e2;
            }
            *start = pos;
            *seq_len = end - pos;
            return 1;
        }
        uint32_t c;
        size_t k = utf8_decode(utf8 + pos, len - pos, &c);
        pos += k ? k : 1;
    }
    return 0;
}

/* ---- Providers -------------------------------------------------------- */

#if defined(_WIN32)
#define RECOMP_EMOJI_HAVE_WIN32 1
#else
#define RECOMP_EMOJI_HAVE_WIN32 0
#endif

static int g_probed = 0;
static int g_backend = 0; /* 0 none, 1 win32, 2 freetype */

static void probe(void) {
    if (g_probed) return;
    g_probed = 1;
#if RECOMP_EMOJI_HAVE_WIN32
    {
        RecompEmojiBitmap t;
        if (recomp_emoji_render_win32("\xF0\x9F\x98\x80", 4, 16, &t)) {
            recomp_emoji_free(&t);
            g_backend = 1;
            return;
        }
    }
#endif
#if defined(RECOMP_UI_HAVE_FREETYPE)
    {
        RecompEmojiBitmap t;
        if (recomp_emoji_render_freetype("\xF0\x9F\x98\x80", 4, 16, &t)) {
            recomp_emoji_free(&t);
            g_backend = 2;
            return;
        }
    }
#endif
    g_backend = 0;
}

const char* recomp_emoji_backend_name(void) {
    probe();
    if (g_backend == 1) return "directwrite";
#if defined(RECOMP_UI_HAVE_FREETYPE)
    if (g_backend == 2) return recomp_emoji_freetype_name();
#endif
    return "none";
}

int recomp_emoji_backend_available(void) {
    probe();
    return g_backend != 0;
}

int recomp_emoji_render(const char* utf8, size_t len, int px, RecompEmojiBitmap* out) {
    if (!utf8 || !len || px <= 0 || !out) return 0;
    memset(out, 0, sizeof(*out));
    /* A flag comes from the sheet wherever the sheet has it; the provider is
     * only asked for one the sheet lacks (which on Windows draws letters). */
    {
        char a, b;
        if (recomp_emoji_flags_code(utf8, len, &a, &b) && recomp_emoji_flags_has(a, b))
            return recomp_emoji_flags_render(a, b, px, out);
    }
    probe();
#if RECOMP_EMOJI_HAVE_WIN32
    if (g_backend == 1) return recomp_emoji_render_win32(utf8, len, px, out);
#endif
#if defined(RECOMP_UI_HAVE_FREETYPE)
    if (g_backend == 2) return recomp_emoji_render_freetype(utf8, len, px, out);
#endif
    return 0;
}

void recomp_emoji_free(RecompEmojiBitmap* bm) {
    if (!bm) return;
    free(bm->rgba);
    bm->rgba = NULL;
    bm->w = bm->h = 0;
}

/* Stubs so the dispatcher links whichever providers the build left out. */
#if !RECOMP_EMOJI_HAVE_WIN32
int recomp_emoji_render_win32(const char* utf8, size_t len, int px, RecompEmojiBitmap* out) {
    (void)utf8; (void)len; (void)px; (void)out;
    return 0;
}
#endif
#if !defined(RECOMP_UI_HAVE_FREETYPE)
int recomp_emoji_render_freetype(const char* utf8, size_t len, int px, RecompEmojiBitmap* out) {
    (void)utf8; (void)len; (void)px; (void)out;
    return 0;
}
const char* recomp_emoji_freetype_name(void) { return "none"; }
#endif
