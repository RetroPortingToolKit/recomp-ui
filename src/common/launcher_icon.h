#ifndef LAUNCHER_ICON_H
#define LAUNCHER_ICON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A desktop icon for a title, made from its cover or from the 16x16 icon of
 * the game's own memory-card save. The files are written by hand: a Windows
 * .ico and a PNG with stored (not compressed) data. Nothing here draws. */

#define LAUNCHER_ICON_ICO "launcher-icon.ico"
#define LAUNCHER_ICON_PNG "launcher-icon.png"

/* Makes a square picture `size` x `size` (RGBA, 4 bytes per pixel, row by row
 * from the top) from any RGBA picture: the whole picture, centred, with
 * transparent bars where it is not square. hard_pixels != 0: nearest
 * neighbour (for a 16x16 save icon); else an area average when it shrinks and
 * bilinear when it grows. out holds size*size*4 bytes. With no picture, out
 * is all transparent. */
void launcher_icon_square(const unsigned char* rgba, int w, int h, int size, int hard_pixels,
                          unsigned char* out);

/* A Windows .ico with the sizes 16, 32, 48 and 256, each stored as a 32-bit
 * BGRA bitmap (BITMAPINFOHEADER with the doubled height, XOR rows bottom-up,
 * then the 1-bit AND mask rows padded to 32 bits; the directory writes 0 for
 * a 256 side). Returns 1 on success. Writes to "<path>.part" and renames, so
 * a failure leaves an older file alone. */
int launcher_icon_write_ico(const char* path, const unsigned char* rgba, int w, int h,
                            int hard_pixels);

/* One PNG, size x size (1..1024), 8-bit RGBA, with stored deflate blocks:
 * signature, IHDR, one IDAT (zlib header 78 01, stored blocks of at most
 * 65535 bytes, Adler-32), IEND. Returns 1 on success. Same .part rule. */
int launcher_icon_write_png(const char* path, const unsigned char* rgba, int w, int h, int size,
                            int hard_pixels);

/* The icon of a PlayStation save on a memory-card image.
 *
 * The file is 131072 bytes, or has a header of 64 or 3904 bytes in front
 * (.gme has 3904); the card is the LAST 131072 bytes, and starts with "MC".
 * The directory frame of block b (1..15) is at b*128 of the card:
 *   bytes 0-3    allocation state, little-endian; 0x51 = first block of a
 *                save, the only kind that carries an icon
 *   bytes 10-30  the file name, e.g. "BASLUS-00757QUAKE2": two region
 *                letters, the product code "SLUS-00757" (10 characters), then
 *                the game's own text
 * The save's first block is at b*8192:
 *   bytes 0-1    "SC"
 *   byte 2       the icon flag: 0x11, 0x12, 0x13 = 1, 2, 3 frames
 *   0x60-0x7F    sixteen 16-bit little-endian colours: bits 0-4 red, 5-9
 *                green, 10-14 blue; 0x0000 is fully transparent, every other
 *                value is opaque
 *   +128         the first icon frame: 16 rows of 8 bytes, 4 bits per pixel,
 *                the LOW nibble is the left pixel
 *
 * Finds the first block b = 1..15 in state 0x51 whose product code equals one
 * of serials[0..n-1] (letters and digits only, case-insensitive, so
 * "slus_007.57" matches), whose first block starts with "SC" and whose icon
 * flag is 0x11..0x13. Writes its first frame as 16x16 RGBA into out; a 5-bit
 * channel v becomes (v << 3) | (v >> 2). Returns 1 when found, 0 when the
 * card has no such save or the file is not a card. block_out (may be NULL)
 * gets b. */
int launcher_icon_from_memcard(const char* card_path, const char* const* serials, int n,
                               unsigned char out[16 * 16 * 4], int* block_out);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_ICON_H */
