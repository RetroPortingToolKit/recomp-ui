/* launcher_icon.c -- a desktop icon from a cover or from a save icon.
 * See launcher_icon.h. */

#include "launcher_icon.h"

#include "crc32.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_PATH_CAP 1100
#define ICON_ICO_COUNT 4
#define ICON_ICO_MAX_SIDE 256
#define ICON_PNG_MAX_SIDE 1024
#define ICON_CARD_BYTES 131072L

static void icon_le16(unsigned char* p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void icon_le32(unsigned char* p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void icon_be32(unsigned char* p, unsigned long v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

/* ---- the square picture ---- */

/* A weighted sum of pixels. Each colour is multiplied by its alpha first, so
 * a transparent neighbour does not darken an edge. */
typedef struct {
    double r, g, b, a, weight;
} IconSum;

static void icon_sum_add(IconSum* s, const unsigned char* p, double weight) {
    const double a = (double)p[3] * weight;
    s->r += (double)p[0] * a;
    s->g += (double)p[1] * a;
    s->b += (double)p[2] * a;
    s->a += a;
    s->weight += weight;
}

static unsigned char icon_round(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (unsigned char)(v + 0.5);
}

static void icon_sum_store(const IconSum* s, unsigned char* d) {
    if (s->a <= 0.0 || s->weight <= 0.0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    d[0] = icon_round(s->r / s->a);
    d[1] = icon_round(s->g / s->a);
    d[2] = icon_round(s->b / s->a);
    d[3] = icon_round(s->a / s->weight);
}

/* The average of the source area that one output pixel covers. A source pixel
 * that is only partly inside counts for that part. */
static void icon_area(const unsigned char* rgba, int w, int h, int dw, int dh, int x, int y,
                      unsigned char* d) {
    const double x0 = (double)x * w / dw, x1 = (double)(x + 1) * w / dw;
    const double y0 = (double)y * h / dh, y1 = (double)(y + 1) * h / dh;
    IconSum s = {0.0, 0.0, 0.0, 0.0, 0.0};
    int sx, sy;

    for (sy = (int)y0; sy < h && (double)sy < y1; ++sy) {
        const double wy = ((double)(sy + 1) < y1 ? (double)(sy + 1) : y1) -
                          ((double)sy > y0 ? (double)sy : y0);
        for (sx = (int)x0; sx < w && (double)sx < x1; ++sx) {
            const double wx = ((double)(sx + 1) < x1 ? (double)(sx + 1) : x1) -
                              ((double)sx > x0 ? (double)sx : x0);
            if (wx > 0.0 && wy > 0.0)
                icon_sum_add(&s, rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4U, wx * wy);
        }
    }
    icon_sum_store(&s, d);
}

/* The four source pixels around the centre of one output pixel. */
static void icon_bilinear(const unsigned char* rgba, int w, int h, int dw, int dh, int x, int y,
                          unsigned char* d) {
    double fx = ((double)x + 0.5) * w / dw - 0.5;
    double fy = ((double)y + 0.5) * h / dh - 0.5;
    IconSum s = {0.0, 0.0, 0.0, 0.0, 0.0};
    int x0, y0, x1, y1;
    double tx, ty;

    if (fx < 0.0) fx = 0.0;
    if (fx > (double)(w - 1)) fx = (double)(w - 1);
    if (fy < 0.0) fy = 0.0;
    if (fy > (double)(h - 1)) fy = (double)(h - 1);
    x0 = (int)fx;
    y0 = (int)fy;
    x1 = x0 + 1 < w ? x0 + 1 : x0;
    y1 = y0 + 1 < h ? y0 + 1 : y0;
    tx = fx - (double)x0;
    ty = fy - (double)y0;
    icon_sum_add(&s, rgba + ((size_t)y0 * (size_t)w + (size_t)x0) * 4U, (1.0 - tx) * (1.0 - ty));
    icon_sum_add(&s, rgba + ((size_t)y0 * (size_t)w + (size_t)x1) * 4U, tx * (1.0 - ty));
    icon_sum_add(&s, rgba + ((size_t)y1 * (size_t)w + (size_t)x0) * 4U, (1.0 - tx) * ty);
    icon_sum_add(&s, rgba + ((size_t)y1 * (size_t)w + (size_t)x1) * 4U, tx * ty);
    icon_sum_store(&s, d);
}

void launcher_icon_square(const unsigned char* rgba, int w, int h, int size, int hard_pixels,
                          unsigned char* out) {
    int dw, dh, ox, oy, x, y;

    if (!out || size <= 0) return;
    memset(out, 0, (size_t)size * (size_t)size * 4U);
    if (!rgba || w <= 0 || h <= 0) return;

    /* The longer side fills the square. The other side keeps the shape. */
    if (w >= h) {
        dw = size;
        dh = (int)(((long long)h * size + w / 2) / w);
    } else {
        dh = size;
        dw = (int)(((long long)w * size + h / 2) / h);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    ox = (size - dw) / 2;
    oy = (size - dh) / 2;

    for (y = 0; y < dh; ++y) {
        for (x = 0; x < dw; ++x) {
            unsigned char* d = out + ((size_t)(oy + y) * (size_t)size + (size_t)(ox + x)) * 4U;
            if (hard_pixels) {
                /* The source pixel under the centre of the output pixel. */
                const long long sx = (((long long)x * 2 + 1) * w) / ((long long)dw * 2);
                const long long sy = (((long long)y * 2 + 1) * h) / ((long long)dh * 2);
                memcpy(d, rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4U, 4);
            } else if (dw < w || dh < h) {
                icon_area(rgba, w, h, dw, dh, x, y, d);
            } else {
                icon_bilinear(rgba, w, h, dw, dh, x, y, d);
            }
        }
    }
}

/* ---- the files ---- */

/* Writes "<path>.part" and puts it in the place of `path`. A plain rename
 * replaces the old file where the system allows that. Windows does not, so
 * the old file goes first there. */
static int icon_save(const char* path, const unsigned char* data, size_t len) {
    char part[ICON_PATH_CAP + 8];
    int ok;
    FILE* f;

    if (!path || !path[0] || strlen(path) >= ICON_PATH_CAP) return 0;
    snprintf(part, sizeof(part), "%s.part", path);
    f = fopen(part, "wb");
    if (!f) return 0;
    ok = fwrite(data, 1, len, f) == len;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(part, path) != 0) {
        remove(path);
        ok = rename(part, path) == 0;
    }
    if (!ok) remove(part);
    return ok;
}

/* One picture of an .ico: the header, the colour rows, the mask rows. */
static size_t icon_ico_image_bytes(int side) {
    const size_t mask_row = (size_t)((side + 31) / 32) * 4U;
    return 40U + (size_t)side * (size_t)side * 4U + mask_row * (size_t)side;
}

int launcher_icon_write_ico(const char* path, const unsigned char* rgba, int w, int h,
                            int hard_pixels) {
    static const int sides[ICON_ICO_COUNT] = {16, 32, 48, ICON_ICO_MAX_SIDE};
    size_t total = 6U + 16U * ICON_ICO_COUNT;
    size_t off = total;
    unsigned char* file;
    unsigned char* square;
    int i, x, y, ok;

    if (!path || !path[0] || !rgba || w <= 0 || h <= 0) return 0;
    for (i = 0; i < ICON_ICO_COUNT; ++i) total += icon_ico_image_bytes(sides[i]);
    file = (unsigned char*)calloc(1, total);
    square = (unsigned char*)malloc((size_t)ICON_ICO_MAX_SIDE * ICON_ICO_MAX_SIDE * 4U);
    if (!file || !square) {
        free(file);
        free(square);
        return 0;
    }

    /* ICONDIR: reserved 0, type 1 (icon), the number of pictures. */
    icon_le16(file + 2, 1);
    icon_le16(file + 4, ICON_ICO_COUNT);
    for (i = 0; i < ICON_ICO_COUNT; ++i) {
        const int s = sides[i];
        const size_t mask_row = (size_t)((s + 31) / 32) * 4U;
        const size_t bytes = icon_ico_image_bytes(s);
        unsigned char* entry = file + 6 + 16 * i;
        unsigned char* bmp = file + off;
        unsigned char* xor_rows = bmp + 40;
        unsigned char* and_rows = xor_rows + (size_t)s * (size_t)s * 4U;

        launcher_icon_square(rgba, w, h, s, hard_pixels, square);

        /* ICONDIRENTRY: width, height (0 means 256), colours 0, reserved 0,
         * planes, bits per pixel, byte count, offset. */
        entry[0] = (unsigned char)(s == 256 ? 0 : s);
        entry[1] = entry[0];
        icon_le16(entry + 4, 1);
        icon_le16(entry + 6, 32);
        icon_le32(entry + 8, (unsigned long)bytes);
        icon_le32(entry + 12, (unsigned long)off);

        /* BITMAPINFOHEADER: the height counts the colour rows and the mask
         * rows, so it is doubled. No compression. */
        icon_le32(bmp, 40);
        icon_le32(bmp + 4, (unsigned long)s);
        icon_le32(bmp + 8, (unsigned long)s * 2UL);
        icon_le16(bmp + 12, 1);
        icon_le16(bmp + 14, 32);
        icon_le32(bmp + 20, (unsigned long)(bytes - 40U));

        for (y = 0; y < s; ++y) {
            /* The bottom row comes first. */
            const unsigned char* src = square + (size_t)(s - 1 - y) * (size_t)s * 4U;
            unsigned char* dst = xor_rows + (size_t)y * (size_t)s * 4U;
            unsigned char* mask = and_rows + (size_t)y * mask_row;
            for (x = 0; x < s; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2]; /* blue */
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0]; /* red */
                dst[x * 4 + 3] = src[x * 4 + 3];
                /* A set mask bit means: this pixel is not drawn. */
                if (src[x * 4 + 3] == 0) mask[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
            }
        }
        off += bytes;
    }
    ok = icon_save(path, file, total);
    free(file);
    free(square);
    return ok;
}

static unsigned long icon_adler32(const unsigned char* data, size_t len) {
    unsigned long a = 1, b = 0;
    size_t i;

    for (i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

/* Closes one PNG chunk whose type and data are in place: writes the length in
 * front, the type, and the CRC of type and data behind. Returns the place of
 * the next chunk. */
static unsigned char* icon_png_chunk(unsigned char* chunk, const char* type, size_t data_len) {
    icon_be32(chunk, (unsigned long)data_len);
    memcpy(chunk + 4, type, 4);
    icon_be32(chunk + 8 + data_len,
              (unsigned long)recompui_crc32_compute((const uint8_t*)(chunk + 4), data_len + 4U));
    return chunk + 12 + data_len;
}

int launcher_icon_write_png(const char* path, const unsigned char* rgba, int w, int h, int size,
                            int hard_pixels) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t row, raw_len, blocks, zlen, total, done;
    unsigned char* file;
    unsigned char* square;
    unsigned char* raw;
    unsigned char* p;
    unsigned char* z;
    int y, ok;

    if (!path || !path[0] || !rgba || w <= 0 || h <= 0) return 0;
    if (size < 1 || size > ICON_PNG_MAX_SIDE) return 0;
    row = (size_t)size * 4U + 1U;
    raw_len = row * (size_t)size;
    blocks = (raw_len + 65534U) / 65535U;
    zlen = 2U + blocks * 5U + raw_len + 4U;
    total = 8U + (12U + 13U) + (12U + zlen) + 12U;
    file = (unsigned char*)malloc(total);
    square = (unsigned char*)malloc((size_t)size * (size_t)size * 4U);
    raw = (unsigned char*)malloc(raw_len);
    if (!file || !square || !raw) {
        free(file);
        free(square);
        free(raw);
        return 0;
    }

    /* The scanlines: one filter byte (0 = none), then the RGBA row. */
    launcher_icon_square(rgba, w, h, size, hard_pixels, square);
    for (y = 0; y < size; ++y) {
        raw[(size_t)y * row] = 0;
        memcpy(raw + (size_t)y * row + 1, square + (size_t)y * (size_t)size * 4U, (size_t)size * 4U);
    }

    p = file;
    memcpy(p, sig, sizeof(sig));
    p += sizeof(sig);

    /* IHDR: width, height, 8 bits, colour type 6 (RGBA), no interlace. */
    icon_be32(p + 8, (unsigned long)size);
    icon_be32(p + 12, (unsigned long)size);
    p[16] = 8;
    p[17] = 6;
    p[18] = 0;
    p[19] = 0;
    p[20] = 0;
    p = icon_png_chunk(p, "IHDR", 13);

    /* IDAT: a zlib stream of stored blocks. Each block is one byte (1 on the
     * last block), the length, the length with all bits turned, the bytes. */
    z = p + 8;
    *z++ = 0x78;
    *z++ = 0x01;
    for (done = 0; done < raw_len;) {
        const size_t n = raw_len - done < 65535U ? raw_len - done : 65535U;
        z[0] = (unsigned char)(done + n == raw_len ? 1 : 0);
        icon_le16(z + 1, (unsigned long)n);
        icon_le16(z + 3, (unsigned long)n ^ 0xFFFFUL);
        memcpy(z + 5, raw + done, n);
        z += 5 + n;
        done += n;
    }
    icon_be32(z, icon_adler32(raw, raw_len));
    p = icon_png_chunk(p, "IDAT", zlen);
    icon_png_chunk(p, "IEND", 0);

    ok = icon_save(path, file, total);
    free(file);
    free(square);
    free(raw);
    return ok;
}

/* ---- the icon of a save on a memory card ---- */

/* The letters and digits of `in`, in capitals: "slus_007.57" and
 * "SLUS-00757" both give "SLUS00757". Reads at most in_len characters.
 * Returns the length, or 0 when nothing is left or it does not fit. */
static size_t icon_plain_code(const char* in, size_t in_len, char* out, size_t cap) {
    size_t i, k = 0;

    for (i = 0; i < in_len && in[i]; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) continue;
        if (k + 1 >= cap) return 0;
        out[k++] = c;
    }
    out[k] = '\0';
    return k;
}

static int icon_code_listed(const char* code, const char* const* serials, int n) {
    char want[16];
    int i;

    for (i = 0; i < n; ++i) {
        if (!serials[i] || !icon_plain_code(serials[i], (size_t)-1, want, sizeof(want))) continue;
        if (!strcmp(want, code)) return 1;
    }
    return 0;
}

static void icon_decode_frame(const unsigned char* save, unsigned char* out) {
    const unsigned char* palette = save + 0x60;
    const unsigned char* frame = save + 128;
    int x, y;

    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            const unsigned pair = frame[y * 8 + x / 2];
            /* The low nibble is the left pixel. */
            const unsigned index = (x & 1) ? (pair >> 4) : (pair & 0x0F);
            const unsigned colour =
                (unsigned)palette[index * 2] | ((unsigned)palette[index * 2 + 1] << 8);
            const unsigned r = colour & 31, g = (colour >> 5) & 31, b = (colour >> 10) & 31;
            unsigned char* d = out + (y * 16 + x) * 4;

            if (colour == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            d[0] = (unsigned char)((r << 3) | (r >> 2));
            d[1] = (unsigned char)((g << 3) | (g >> 2));
            d[2] = (unsigned char)((b << 3) | (b >> 2));
            d[3] = 255;
        }
    }
}

int launcher_icon_from_memcard(const char* card_path, const char* const* serials, int n,
                               unsigned char out[16 * 16 * 4], int* block_out) {
    unsigned char* card;
    long size, header;
    int found = 0;
    int b;
    FILE* f;

    if (!card_path || !card_path[0] || !serials || n <= 0 || !out) return 0;
    f = fopen(card_path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    header = size - ICON_CARD_BYTES;
    if (header != 0 && header != 64 && header != 3904) {
        fclose(f);
        return 0;
    }
    card = (unsigned char*)malloc((size_t)ICON_CARD_BYTES);
    if (!card || fseek(f, header, SEEK_SET) != 0 ||
        fread(card, 1, (size_t)ICON_CARD_BYTES, f) != (size_t)ICON_CARD_BYTES) {
        free(card);
        fclose(f);
        return 0;
    }
    fclose(f);

    for (b = 1; b <= 15 && card[0] == 'M' && card[1] == 'C'; ++b) {
        const unsigned char* dir = card + b * 128;
        const unsigned char* save = card + (size_t)b * 8192U;
        const char* name = (const char*)dir + 10;
        char code[16];

        /* Only the first block of a save carries the icon. */
        if (dir[0] != 0x51 || dir[1] != 0 || dir[2] != 0 || dir[3] != 0) continue;
        /* Two region letters, then the ten characters of the product code. */
        if (memchr(name, 0, 12) || !icon_plain_code(name + 2, 10, code, sizeof(code))) continue;
        if (!icon_code_listed(code, serials, n)) continue;
        if (save[0] != 'S' || save[1] != 'C' || save[2] < 0x11 || save[2] > 0x13) continue;
        icon_decode_frame(save, out);
        if (block_out) *block_out = b;
        found = 1;
        break;
    }
    free(card);
    return found;
}
