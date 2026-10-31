/* A desktop icon from a cover or from a save icon (src/common/launcher_icon.c):
 * the square picture, the .ico and .png files, and the icon of a save on a
 * memory-card image.
 *
 * The test builds its own pictures and its own card image, and reads the
 * files back with its own code: its own CRC-32 and Adler-32, and its own walk
 * of the stored deflate blocks. A file that only the writer can read would
 * pass a test that used the writer's helpers.
 *
 * What the card checks are for. A card holds saves of many games, and a save
 * of more than one block has "middle" and "last" directory frames that carry
 * no icon. The icon must come from the first block of the save of THIS game,
 * and the two pixels in a byte must come out in the right order.
 *
 * The fixture files go in the folder given as argv[1] (default: the current
 * folder).
 */
#include "launcher_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

/* ---- files ---- */

static char g_dir[400] = ".";

static const char* in_dir(const char* leaf) {
    static char paths[8][600];
    static int next;
    char* out = paths[next++ & 7];
    snprintf(out, sizeof(paths[0]), "%s/%s", g_dir, leaf);
    return out;
}

static int write_file(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    size_t n;
    if (!f) return 0;
    n = len ? fwrite(data, 1, len, f) : 0;
    return fclose(f) == 0 && n == len;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* The whole file in a block from malloc; NULL when it cannot be read. */
static unsigned char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    unsigned char* data;
    long size;
    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char*)malloc(size > 0 ? (size_t)size : 1);
    if (data && size > 0 && fread(data, 1, (size_t)size, f) == (size_t)size) *len = (size_t)size;
    fclose(f);
    return data;
}

static const char* part_of(const char* path) {
    static char part[700];
    snprintf(part, sizeof(part), "%s.part", path);
    return part;
}

/* ---- the test's own readers ---- */

static unsigned long le16(const unsigned char* p) { return (unsigned long)p[0] | ((unsigned long)p[1] << 8); }

static unsigned long le32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static unsigned long be32(const unsigned char* p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

/* CRC-32 one bit at a time, with no table. */
static unsigned long crc32_slow(const unsigned char* data, size_t len) {
    unsigned long crc = 0xFFFFFFFFUL;
    size_t i;
    int bit;
    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) crc = (crc & 1UL) ? (0xEDB88320UL ^ (crc >> 1)) : (crc >> 1);
    }
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

static unsigned long adler32(const unsigned char* data, size_t len) {
    unsigned long a = 1, b = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

/* ---- pictures the test builds itself ---- */

#define PIC_W 300
#define PIC_H 200

static unsigned char g_pic[PIC_W * PIC_H * 4];  /* a gradient with an opaque rectangle */
static unsigned char g_tall[PIC_W * PIC_H * 4]; /* the same, turned: 200 wide, 300 high */
static unsigned char g_pattern[16 * 16 * 4];

/* The 16x16 pattern, also the icon of the wanted save: index 0 is the
 * transparent colour. The 5 makes left and right different in every byte. */
static int pattern_index(int x, int y) { return (x * 5 + y * 3 + 1) & 15; }

static void pattern_pixel(int x, int y, unsigned char* p) {
    const int index = pattern_index(x, y);
    if (index == 0) {
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }
    p[0] = (unsigned char)(x * 16 + 7);
    p[1] = (unsigned char)(y * 16 + 3);
    p[2] = (unsigned char)(index * 17);
    p[3] = 255;
}

static void make_pictures(void) {
    int x, y;
    for (y = 0; y < PIC_H; ++y) {
        for (x = 0; x < PIC_W; ++x) {
            unsigned char* p = g_pic + (y * PIC_W + x) * 4;
            unsigned char* t = g_tall + (x * PIC_H + y) * 4;
            if (x >= 100 && x < 200 && y >= 50 && y < 150) {
                p[0] = 200; p[1] = 40; p[2] = 40; p[3] = 255;
            } else {
                p[0] = (unsigned char)(x * 255 / (PIC_W - 1));
                p[1] = (unsigned char)(y * 255 / (PIC_H - 1));
                p[2] = 64;
                p[3] = 128;
            }
            memcpy(t, p, 4);
        }
    }
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) pattern_pixel(x, y, g_pattern + (y * 16 + x) * 4);
    }
}

static int pixel_is(const unsigned char* p, int r, int g, int b, int a) {
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

/* ---- the square picture ---- */

static void test_square(void) {
    static unsigned char out[256 * 256 * 4];
    static unsigned char small[48 * 48 * 4];
    int x, y, ok;

    /* 300x200 in 256: 256 wide and 171 high, so 42 clear rows above and 43
     * below. */
    launcher_icon_square(g_pic, PIC_W, PIC_H, 256, 0, out);
    expect(out[3] == 0 && out[(255 * 256 + 255) * 4 + 3] == 0,
           "square: 300x200 leaves the top-left and bottom-right pixels transparent");
    ok = 1;
    for (y = 0; y < 256; ++y) {
        for (x = 0; x < 256; ++x) {
            const unsigned char* p = out + (y * 256 + x) * 4;
            const int bar = y < 42 || y > 212;
            if (bar && !pixel_is(p, 0, 0, 0, 0)) ok = 0;
            if (!bar && p[3] == 0) ok = 0;
        }
    }
    expect(ok, "square: rows 0-41 and 213-255 are clear bars, rows 42-212 hold the picture");
    expect(pixel_is(out + (128 * 256 + 128) * 4, 200, 40, 40, 255),
           "square: the centre is the opaque rectangle, colour kept");
    expect(out[(128 * 256 + 0) * 4 + 3] == 128 && out[(128 * 256 + 255) * 4 + 3] == 128 &&
           out[(128 * 256 + 0) * 4] < 8 && out[(128 * 256 + 255) * 4] > 247,
           "square: the whole width is kept, left edge to right edge");

    launcher_icon_square(g_tall, PIC_H, PIC_W, 256, 0, out);
    expect(out[(128 * 256 + 0) * 4 + 3] == 0 && out[(128 * 256 + 255) * 4 + 3] == 0 &&
           out[(0 * 256 + 128) * 4 + 3] == 128 && out[(255 * 256 + 128) * 4 + 3] == 128 &&
           pixel_is(out + (128 * 256 + 128) * 4, 200, 40, 40, 255),
           "square: 200x300 gets its bars left and right");

    /* Hard pixels. */
    launcher_icon_square(g_pattern, 16, 16, 256, 1, out);
    ok = 1;
    for (y = 0; y < 256; ++y) {
        for (x = 0; x < 256; ++x) {
            unsigned char want[4];
            pattern_pixel(x / 16, y / 16, want);
            if (memcmp(out + (y * 256 + x) * 4, want, 4)) ok = 0;
        }
    }
    expect(ok, "square: hard pixels scale 16x16 to 256 as exact 16x16 blocks");
    launcher_icon_square(g_pattern, 16, 16, 48, 1, small);
    ok = 1;
    for (y = 0; y < 48; ++y) {
        for (x = 0; x < 48; ++x) {
            unsigned char want[4];
            pattern_pixel(x / 3, y / 3, want);
            if (memcmp(small + (y * 48 + x) * 4, want, 4)) ok = 0;
        }
    }
    expect(ok, "square: hard pixels scale 16x16 to 48 as exact 3x3 blocks");
    launcher_icon_square(g_pattern, 16, 16, 16, 1, small);
    expect(!memcmp(small, g_pattern, sizeof(g_pattern)),
           "square: hard pixels at the same size copy the picture");
    launcher_icon_square(g_pattern, 16, 16, 16, 0, small);
    expect(!memcmp(small, g_pattern, sizeof(g_pattern)),
           "square: soft pixels at the same size copy the picture");

    /* Growing: bilinear. */
    {
        static const unsigned char two[2 * 2 * 4] = {0, 0, 0, 255, 200, 100, 0, 255,
                                                     0, 80, 40, 255, 40, 40, 40, 255};
        launcher_icon_square(two, 2, 2, 4, 0, small);
        expect(pixel_is(small + (0 * 4 + 0) * 4, 0, 0, 0, 255) &&
               pixel_is(small + (0 * 4 + 3) * 4, 200, 100, 0, 255) &&
               pixel_is(small + (3 * 4 + 0) * 4, 0, 80, 40, 255) &&
               pixel_is(small + (3 * 4 + 3) * 4, 40, 40, 40, 255),
               "square: a 2x2 picture grown to 4 keeps its four corners");
        expect(pixel_is(small + (0 * 4 + 1) * 4, 50, 25, 0, 255) &&
               pixel_is(small + (0 * 4 + 2) * 4, 150, 75, 0, 255),
               "square: between two pixels the colour is mixed 3 to 1 and 1 to 3");
    }

    /* Shrinking: the average of the area. */
    {
        unsigned char four[4 * 4 * 4];
        memset(four, 0, sizeof(four));
        for (y = 0; y < 2; ++y) { /* top-left block: 10, 20, 30, 40 */
            for (x = 0; x < 2; ++x) {
                unsigned char* p = four + (y * 4 + x) * 4;
                p[0] = (unsigned char)(10 + 10 * (y * 2 + x));
                p[1] = 100;
                p[2] = 0;
                p[3] = 255;
            }
        }
        for (y = 0; y < 2; ++y) { /* top-right block: one red pixel, three clear green ones */
            for (x = 2; x < 4; ++x) {
                unsigned char* p = four + (y * 4 + x) * 4;
                p[1] = 255;
            }
        }
        four[(0 * 4 + 2) * 4 + 0] = 255;
        four[(0 * 4 + 2) * 4 + 1] = 0;
        four[(0 * 4 + 2) * 4 + 3] = 255;
        launcher_icon_square(four, 4, 4, 2, 0, small);
        expect(pixel_is(small + 0, 25, 100, 0, 255), "square: 4x4 shrunk to 2 averages each 2x2 block");
        expect(pixel_is(small + 4, 255, 0, 0, 64),
               "square: a clear neighbour lowers the alpha and does not change the colour");
        expect(pixel_is(small + 8, 0, 0, 0, 0), "square: a block of clear pixels stays clear");
    }

    memset(small, 0xEE, sizeof(small));
    launcher_icon_square(NULL, 16, 16, 16, 0, small);
    for (ok = 1, x = 0; x < 16 * 16 * 4; ++x) {
        if (small[x]) ok = 0;
    }
    expect(ok && small[16 * 16 * 4] == 0xEE, "square: no picture gives a clear square and writes no more");
    memset(small, 0xEE, sizeof(small));
    launcher_icon_square(g_pic, PIC_W, PIC_H, 1, 0, small);
    expect(small[3] != 0 && small[4] == 0xEE, "square: a size of 1 gives one pixel");
}

/* ---- the .ico file ---- */

/* The header, the colour rows, and the mask rows padded to 32 bits. */
static size_t ico_image_bytes(int side) {
    const size_t mask_row = (size_t)((side + 31) / 32) * 4U;
    return 40U + (size_t)side * (size_t)side * 4U + mask_row * (size_t)side;
}

static void test_ico(void) {
    static const int sides[4] = {16, 32, 48, 256};
    static const unsigned char magic[6] = {0, 0, 1, 0, 4, 0};
    const char* path = in_dir("icon-pattern.ico");
    unsigned char* file;
    size_t size, offs[4] = {0, 0, 0, 0};
    size_t next = 6 + 4 * 16;
    int i, x, y, ok, dir_ok = 1, head_ok = 1;

    remove(path);
    expect(launcher_icon_write_ico(path, g_pattern, 16, 16, 1) == 1, "ico: the file is written");
    expect(!file_exists(part_of(path)), "ico: no .part is left");
    file = read_file(path, &size);
    if (!file || size < 70) {
        expect(0, "ico: the file can be read back");
        free(file);
        return;
    }
    expect(!memcmp(file, magic, sizeof(magic)), "ico: the file starts 00 00 01 00 04 00");
    for (i = 0; i < 4; ++i) {
        const unsigned char* e = file + 6 + 16 * i;
        const int want_side = sides[i] == 256 ? 0 : sides[i];
        offs[i] = (size_t)le32(e + 12);
        if (e[0] != want_side || e[1] != want_side || e[2] != 0 || e[3] != 0) dir_ok = 0;
        if (le16(e + 4) != 1 || le16(e + 6) != 32) dir_ok = 0;
        if (le32(e + 8) != ico_image_bytes(sides[i]) || offs[i] != next) dir_ok = 0;
        next += ico_image_bytes(sides[i]);
    }
    expect(dir_ok,
           "ico: four entries, sides 16 32 48 and 0 for 256, 1 plane, 32 bits, sizes and offsets in a row");
    expect(next == size && size == 285478U, "ico: the offsets and byte counts add up to the file size");
    if (!dir_ok || next != size) {
        free(file);
        return;
    }
    for (i = 0; i < 4; ++i) {
        const unsigned char* b = file + offs[i];
        if (le32(b) != 40 || le32(b + 4) != (unsigned long)sides[i] ||
            le32(b + 8) != (unsigned long)sides[i] * 2UL || le16(b + 12) != 1 || le16(b + 14) != 32 ||
            le32(b + 16) != 0)
            head_ok = 0;
    }
    expect(head_ok, "ico: each bitmap header is 40 bytes, height doubled, 32 bits, not compressed");

    /* The 16x16 entry: rows from the bottom up, each pixel blue green red alpha. */
    {
        const unsigned char* rows = file + offs[0] + 40;
        const unsigned char* mask = rows + 16 * 16 * 4;
        const unsigned char* p = rows + ((15 - 5) * 16 + 3) * 4;
        unsigned char want[4];
        pattern_pixel(3, 5, want);
        expect(want[3] == 255 && p[0] == want[2] && p[1] == want[1] && p[2] == want[0] && p[3] == 255,
               "ico: pixel (3,5) of the 16x16 entry reads back, bottom-up and BGRA");
        ok = 1;
        for (y = 0; y < 16; ++y) {
            for (x = 0; x < 16; ++x) {
                const unsigned char* q = rows + ((15 - y) * 16 + x) * 4;
                const int bit = (mask[(15 - y) * 4 + x / 8] >> (7 - (x & 7))) & 1;
                pattern_pixel(x, y, want);
                if (q[0] != want[2] || q[1] != want[1] || q[2] != want[0] || q[3] != want[3]) ok = 0;
                if (bit != (want[3] == 0)) ok = 0;
            }
        }
        expect(ok, "ico: all 256 pixels of the 16x16 entry match, mask bit set only on clear pixels");
        expect(pattern_index(3, 0) == 0 && (mask[15 * 4] & 0x10) && !(mask[15 * 4] & 0x80),
               "ico: the clear pixel (3,0) has its mask bit in the last mask row");
    }
    /* The 256 entry: 32 mask bytes a row. */
    {
        const unsigned char* rows = file + offs[3] + 40;
        const unsigned char* p = rows + ((255 - 200) * 256 + 40) * 4;
        unsigned char want[4];
        pattern_pixel(40 / 16, 200 / 16, want);
        expect(p[0] == want[2] && p[1] == want[1] && p[2] == want[0] && p[3] == want[3],
               "ico: pixel (40,200) of the 256 entry reads back");
    }
    free(file);

    /* From the cover: the bars are clear, and a second write replaces the file. */
    expect(launcher_icon_write_ico(path, g_pic, PIC_W, PIC_H, 0) == 1 && !file_exists(part_of(path)),
           "ico: a second write replaces the file");
    file = read_file(path, &size);
    if (file && size == 285478U) {
        const unsigned char* rows = file + le32(file + 6 + 16 * 3 + 12) + 40;
        const unsigned char* mask = rows + 256 * 256 * 4;
        const unsigned char* centre = rows + ((255 - 128) * 256 + 128) * 4;
        expect(rows[(255 * 256 + 0) * 4 + 3] == 0 && (mask[255 * 32] & 0x80) &&
               centre[3] == 255 && centre[2] == 200 && !(mask[(255 - 128) * 32 + 16] & 0x80),
               "ico: from the cover, the top-left pixel is clear and masked, the centre is opaque red");
    } else {
        expect(0, "ico: the replaced file can be read back");
    }
    free(file);

    expect(launcher_icon_write_ico(in_dir("no-such-folder/icon.ico"), g_pattern, 16, 16, 1) == 0,
           "ico: a path in a folder that does not exist returns 0");
    expect(launcher_icon_write_ico(NULL, g_pattern, 16, 16, 1) == 0 &&
           launcher_icon_write_ico(path, NULL, 16, 16, 1) == 0 &&
           launcher_icon_write_ico(path, g_pattern, 0, 16, 1) == 0 && file_exists(path),
           "ico: no path or no picture returns 0 and leaves the older file");
}

/* ---- the .png file ---- */

typedef struct {
    unsigned long len;
    char type[5];
    const unsigned char* data;
    int crc_ok;
} Chunk;

static int png_chunk(const unsigned char* file, size_t size, size_t* pos, Chunk* c) {
    if (*pos + 12 > size) return 0;
    c->len = be32(file + *pos);
    if (c->len > size || *pos + 12 + c->len > size) return 0;
    memcpy(c->type, file + *pos + 4, 4);
    c->type[4] = '\0';
    c->data = file + *pos + 8;
    c->crc_ok = be32(c->data + c->len) == crc32_slow(file + *pos + 4, c->len + 4);
    *pos += 12 + c->len;
    return 1;
}

/* Reads a zlib stream that holds stored blocks only. Checks the header, each
 * block's two length fields, that only the last block says so, and the
 * Adler-32 behind the last block. */
static int inflate_stored(const unsigned char* z, size_t zlen, unsigned char* out, size_t cap,
                          size_t* out_len, int* blocks) {
    size_t pos = 2, n = 0;
    int last = 0;

    *out_len = 0;
    *blocks = 0;
    if (zlen < 6 || z[0] != 0x78 || z[1] != 0x01) return 0;
    while (!last) {
        unsigned long len, nlen;
        if (pos + 5 > zlen || z[pos] > 1) return 0;
        last = z[pos];
        len = le16(z + pos + 1);
        nlen = le16(z + pos + 3);
        if ((len ^ nlen) != 0xFFFFUL) return 0;
        pos += 5;
        if (pos + len > zlen || n + len > cap) return 0;
        memcpy(out + n, z + pos, len);
        pos += len;
        n += len;
        ++*blocks;
    }
    if (pos + 4 != zlen || be32(z + pos) != adler32(out, n)) return 0;
    *out_len = n;
    return 1;
}

/* Reads one PNG written by launcher_icon_write_png into scanlines. Returns 1
 * when the signature, the three chunks, their CRCs, the IHDR fields and the
 * deflate stream are all as they must be. */
static int read_png(const char* path, int side, unsigned char* lines, size_t cap, size_t* lines_len,
                    int* blocks) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t size, pos = 8;
    unsigned char* file = read_file(path, &size);
    Chunk ihdr, idat, iend;
    int ok = 0;

    *lines_len = 0;
    *blocks = 0;
    if (file && size > 8 && !memcmp(file, sig, 8) && png_chunk(file, size, &pos, &ihdr) &&
        png_chunk(file, size, &pos, &idat) && png_chunk(file, size, &pos, &iend) && pos == size) {
        ok = !strcmp(ihdr.type, "IHDR") && ihdr.len == 13 && ihdr.crc_ok &&
             be32(ihdr.data) == (unsigned long)side && be32(ihdr.data + 4) == (unsigned long)side &&
             ihdr.data[8] == 8 && ihdr.data[9] == 6 && ihdr.data[10] == 0 && ihdr.data[11] == 0 &&
             ihdr.data[12] == 0;
        ok = ok && !strcmp(idat.type, "IDAT") && idat.crc_ok;
        ok = ok && !strcmp(iend.type, "IEND") && iend.len == 0 && iend.crc_ok;
        ok = ok && inflate_stored(idat.data, idat.len, lines, cap, lines_len, blocks);
    }
    free(file);
    return ok;
}

static void test_png(void) {
    static unsigned char lines[256 * (256 * 4 + 1)];
    const char* path = in_dir("icon-pattern.png");
    const char* cover = in_dir("icon-cover.png");
    size_t len;
    int blocks, x, y, ok;

    remove(path);
    expect(launcher_icon_write_png(path, g_pattern, 16, 16, 256, 1) == 1, "png: the file is written");
    expect(!file_exists(part_of(path)), "png: no .part is left");
    expect(read_png(path, 256, lines, sizeof(lines), &len, &blocks),
           "png: signature, IHDR 256x256 8-bit RGBA, one IDAT, IEND, three good CRCs, "
           "stored blocks with a good Adler-32");
    expect(len == sizeof(lines) && blocks == 5, "png: 262400 bytes of scanlines in five stored blocks");
    ok = len == sizeof(lines);
    for (y = 0; ok && y < 256; ++y) {
        const unsigned char* row = lines + (size_t)y * 1025;
        if (row[0] != 0) ok = 0;
        for (x = 0; x < 256; ++x) {
            unsigned char want[4];
            pattern_pixel(x / 16, y / 16, want);
            if (memcmp(row + 1 + x * 4, want, 4)) ok = 0;
        }
    }
    expect(ok, "png: the scanlines are filter byte 0 and the picture, row by row from the top");

    /* From the cover, one block. */
    remove(cover);
    expect(launcher_icon_write_png(cover, g_pic, PIC_W, PIC_H, 48, 0) == 1 &&
           read_png(cover, 48, lines, sizeof(lines), &len, &blocks) && len == 48 * 193 && blocks == 1,
           "png: a 48x48 picture from the cover is one stored block of 9264 bytes");
    expect(len == 48 * 193 && lines[0 * 193 + 1 + 3] == 0 && lines[24 * 193 + 1 + 24 * 4 + 3] == 255 &&
           lines[24 * 193 + 1 + 24 * 4] == 200,
           "png: from the cover, the top-left pixel is clear and the centre is opaque red");
    expect(launcher_icon_write_png(cover, g_pattern, 16, 16, 16, 1) == 1 &&
           read_png(cover, 16, lines, sizeof(lines), &len, &blocks) && len == 16 * 65,
           "png: a second write replaces the file");

    expect(launcher_icon_write_png(path, g_pattern, 16, 16, 0, 1) == 0 &&
           launcher_icon_write_png(path, g_pattern, 16, 16, 1025, 1) == 0 &&
           launcher_icon_write_png(path, NULL, 16, 16, 64, 1) == 0 &&
           launcher_icon_write_png(NULL, g_pattern, 16, 16, 64, 1) == 0 &&
           read_png(path, 256, lines, sizeof(lines), &len, &blocks),
           "png: a size of 0 or 1025, no picture or no path returns 0 and leaves the older file");
    expect(launcher_icon_write_png(in_dir("no-such-folder/icon.png"), g_pattern, 16, 16, 64, 1) == 0,
           "png: a path in a folder that does not exist returns 0");
}

/* ---- the icon of a save on a memory card ---- */

#define CARD_BYTES 131072

/* The palette of the wanted save. Colour 0 is the transparent one. Colour 9
 * is black with the top bit set, which is opaque. */
static unsigned long wanted_colour(int index) {
    if (index == 0) return 0x0000UL;
    if (index == 9) return 0x8000UL;
    return (unsigned long)(index * 2) | ((unsigned long)(31 - index) << 5) |
           ((unsigned long)((index * 3) & 31) << 10) | (index == 5 ? 0x8000UL : 0UL);
}

static void wanted_pixel(int x, int y, unsigned char* p) {
    const unsigned long c = wanted_colour(pattern_index(x, y));
    const unsigned long r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (c == 0) {
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }
    p[0] = (unsigned char)((r << 3) | (r >> 2));
    p[1] = (unsigned char)((g << 3) | (g >> 2));
    p[2] = (unsigned char)((b << 3) | (b >> 2));
    p[3] = 255;
}

static void card_dir(unsigned char* card, int block, unsigned long state, const char* name) {
    unsigned char* d = card + block * 128;
    unsigned char sum = 0;
    int i;
    memset(d, 0, 128);
    d[0] = (unsigned char)(state & 0xFF);
    d[1] = (unsigned char)((state >> 8) & 0xFF);
    d[2] = (unsigned char)((state >> 16) & 0xFF);
    d[3] = (unsigned char)((state >> 24) & 0xFF);
    d[5] = 0x20; /* 8192 bytes */
    d[8] = 0xFF;
    d[9] = 0xFF;
    if (name) strncpy((char*)d + 10, name, 20);
    for (i = 0; i < 127; ++i) sum ^= d[i];
    d[127] = sum;
}

/* The first block of a save. wanted != 0: the palette and the pattern that
 * the test looks for. Else another icon: all colours opaque and red. */
static void card_save(unsigned char* card, int block, int flag, int wanted) {
    unsigned char* s = card + (size_t)block * 8192;
    int i, x, y;
    memset(s, 0, 8192);
    s[0] = 'S';
    s[1] = 'C';
    s[2] = (unsigned char)flag;
    s[3] = 1;
    for (i = 0; i < 16; ++i) {
        const unsigned long c = wanted ? wanted_colour(i) : (0x001FUL | ((unsigned long)i << 5));
        s[0x60 + i * 2] = (unsigned char)(c & 0xFF);
        s[0x60 + i * 2 + 1] = (unsigned char)(c >> 8);
    }
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; x += 2) {
            const int left = wanted ? pattern_index(x, y) : ((x + y) & 15);
            const int right = wanted ? pattern_index(x + 1, y) : ((x + 1 + y) & 15);
            s[128 + y * 8 + x / 2] = (unsigned char)(left | (right << 4));
        }
    }
}

static unsigned char g_card[CARD_BYTES];

static void make_card(void) {
    int b;
    memset(g_card, 0, sizeof(g_card));
    g_card[0] = 'M';
    g_card[1] = 'C';
    g_card[127] = 0x0E;
    for (b = 1; b <= 15; ++b) card_dir(g_card, b, 0xA0, NULL); /* free */
    /* Block 1 stays free. */
    card_dir(g_card, 2, 0x51, "BESLES-01234GAMEONE");
    card_save(g_card, 2, 0x11, 0);
    /* A middle block with the wanted name and bytes that look like an icon. */
    card_dir(g_card, 3, 0x52, "BASLUS-00757QUAKE2");
    card_save(g_card, 3, 0x11, 0);
    /* A deleted save of the wanted game. */
    card_dir(g_card, 4, 0xA1, "BASLUS-00757QUAKE2");
    card_save(g_card, 4, 0x11, 0);
    /* The wanted save. */
    card_dir(g_card, 5, 0x51, "BASLUS-00757QUAKE2");
    card_save(g_card, 5, 0x12, 1);
}

static int icon_matches_wanted(const unsigned char* icon) {
    int x, y;
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            unsigned char want[4];
            wanted_pixel(x, y, want);
            if (memcmp(icon + (y * 16 + x) * 4, want, 4)) return 0;
        }
    }
    return 1;
}

static void test_memcard(void) {
    static const char* const exact[] = {"SLUS-00757"};
    static const char* const boot_name[] = {"slus_007.57"};
    static const char* const second[] = {"SCUS-99999", "SLUS-00757"};
    static const char* const with_null[] = {NULL, "", "SLUS00757"};
    static const char* const other_game[] = {"SLES-01234"};
    static const char* const absent[] = {"SLPS-00001"};
    static const char* const shorter[] = {"SLUS-0075"};
    static const char* const longer[] = {"SLUS-007570"};
    static unsigned char file[CARD_BYTES + 3904];
    static unsigned char copy[CARD_BYTES];
    unsigned char icon[16 * 16 * 4];
    unsigned char want[4];
    const char* card = in_dir("card.mcd");
    const char* path;
    int block;

    make_card();
    write_file(card, g_card, sizeof(g_card));

    block = -1;
    memset(icon, 0xEE, sizeof(icon));
    expect(launcher_icon_from_memcard(card, exact, 1, icon, &block) == 1 && block == 5,
           "card: SLUS-00757 is found in block 5, not in the middle block 3 or the deleted block 4");
    wanted_pixel(0, 0, want);
    expect(want[3] == 255 && !memcmp(icon, want, 4), "card: pixel (0,0) is the low nibble of the first byte");
    wanted_pixel(1, 0, want);
    expect(!memcmp(icon + 4, want, 4), "card: pixel (1,0) is the high nibble of the first byte");
    expect(pattern_index(3, 0) == 0 && pixel_is(icon + 3 * 4, 0, 0, 0, 0),
           "card: pixel (3,0) has colour 0x0000 and is transparent");
    wanted_pixel(7, 9, want);
    expect(pattern_index(7, 9) == 15 && !memcmp(icon + (9 * 16 + 7) * 4, want, 4) &&
           pixel_is(want, 247, 132, 107, 255),
           "card: pixel (7,9) has the widened colour 15 of the palette");
    expect(pattern_index(0, 8) == 9 && pixel_is(icon + (8 * 16 + 0) * 4, 0, 0, 0, 255),
           "card: colour 0x8000 is black and opaque, not transparent");
    expect(icon_matches_wanted(icon), "card: all 256 pixels match the pattern");

    block = -1;
    memset(icon, 0xEE, sizeof(icon));
    expect(launcher_icon_from_memcard(card, boot_name, 1, icon, &block) == 1 && block == 5 &&
           icon_matches_wanted(icon),
           "card: the boot file name slus_007.57 finds the same save");
    block = -1;
    expect(launcher_icon_from_memcard(card, second, 2, icon, &block) == 1 && block == 5,
           "card: a list whose second serial is on the card finds it");
    block = -1;
    expect(launcher_icon_from_memcard(card, with_null, 3, icon, &block) == 1 && block == 5,
           "card: NULL and empty entries in the list are passed over");
    expect(launcher_icon_from_memcard(card, exact, 1, icon, NULL) == 1, "card: block_out may be NULL");
    block = -1;
    expect(launcher_icon_from_memcard(card, other_game, 1, icon, &block) == 1 && block == 2 &&
           pixel_is(icon, 255, 0, 0, 255),
           "card: SLES-01234 finds the other save in block 2");

    block = -1;
    expect(launcher_icon_from_memcard(card, absent, 1, icon, &block) == 0 && block == -1,
           "card: SLPS-00001 is not on the card, block_out is not written");
    expect(launcher_icon_from_memcard(card, shorter, 1, icon, NULL) == 0 &&
           launcher_icon_from_memcard(card, longer, 1, icon, NULL) == 0,
           "card: a serial one digit shorter or longer does not match");
    expect(launcher_icon_from_memcard(card, exact, 0, icon, NULL) == 0 &&
           launcher_icon_from_memcard(card, NULL, 1, icon, NULL) == 0 &&
           launcher_icon_from_memcard(NULL, exact, 1, icon, NULL) == 0,
           "card: no list or no path returns 0");

    /* Not a card. */
    path = in_dir("card-100.bin");
    write_file(path, g_card, 100);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0, "card: a 100-byte file returns 0");
    path = in_dir("card-missing.mcd");
    remove(path);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0, "card: a missing file returns 0");
    memcpy(file, g_card, 100);
    memcpy(file + 100, g_card, CARD_BYTES);
    path = in_dir("card-odd.bin");
    write_file(path, file, CARD_BYTES + 100);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0,
           "card: a file of 131072 + 100 bytes returns 0");
    memcpy(copy, g_card, CARD_BYTES);
    copy[0] = 0;
    copy[1] = 0;
    path = in_dir("card-nomc.mcd");
    write_file(path, copy, CARD_BYTES);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0,
           "card: 131072 bytes that do not start with MC return 0");

    /* A save with no icon. */
    memcpy(copy, g_card, CARD_BYTES);
    copy[5 * 8192] = 0;
    copy[5 * 8192 + 1] = 0;
    path = in_dir("card-nosc.mcd");
    write_file(path, copy, CARD_BYTES);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0,
           "card: a save block with no SC returns 0");
    memcpy(copy, g_card, CARD_BYTES);
    copy[5 * 8192 + 2] = 0x00;
    path = in_dir("card-noicon.mcd");
    write_file(path, copy, CARD_BYTES);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0,
           "card: an icon flag of 0x00 returns 0");
    copy[5 * 8192 + 2] = 0x14;
    write_file(path, copy, CARD_BYTES);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 0,
           "card: an icon flag of 0x14 returns 0");
    copy[5 * 8192 + 2] = 0x13;
    write_file(path, copy, CARD_BYTES);
    expect(launcher_icon_from_memcard(path, exact, 1, icon, NULL) == 1,
           "card: an icon flag of 0x13 (three frames) is read");

    /* A header in front of the card. */
    memset(file, 0x55, 64);
    memcpy(file + 64, g_card, CARD_BYTES);
    path = in_dir("card-header64.vgs");
    write_file(path, file, CARD_BYTES + 64);
    block = -1;
    memset(icon, 0xEE, sizeof(icon));
    expect(launcher_icon_from_memcard(path, exact, 1, icon, &block) == 1 && block == 5 &&
           icon_matches_wanted(icon),
           "card: a 64-byte header in front is read the same");
    memset(file, 0x55, 3904);
    memcpy(file + 3904, g_card, CARD_BYTES);
    path = in_dir("card-header3904.gme");
    write_file(path, file, CARD_BYTES + 3904);
    block = -1;
    memset(icon, 0xEE, sizeof(icon));
    expect(launcher_icon_from_memcard(path, exact, 1, icon, &block) == 1 && block == 5 &&
           icon_matches_wanted(icon),
           "card: a 3904-byte header in front is read the same");
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0]) snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    make_pictures();

    test_square();
    test_ico();
    test_png();
    test_memcard();

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("launcher_icon_test: all checks passed\n");
    return 0;
}
