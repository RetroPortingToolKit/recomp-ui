/* The cover of the mounted disc: serial forms, the address, the picture
 * check, one whole fetch and the worker job (src/common/launcher_cover.c).
 *
 * No request leaves this test. Every fetch goes through a fake fetcher that
 * writes the file itself, and launcher_cover_fetch_curl is never called.
 *
 * What the checks are for. A serial is the only part of the address that
 * comes from a disc, and it ends on a command line, so every form with a
 * quote, a space, "..", "&" or extra text must be refused. A download is not
 * trusted either: a web page that came back in place of a picture must never
 * replace a cover that the player already has.
 *
 * The fixture files go in the folder given as argv[1] (default: the current
 * folder).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* nanosleep */
#endif

#include "launcher_cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static void make_dir(const char* path) { _mkdir(path); }
#else
#include <sys/stat.h>
#include <time.h>
static void sleep_ms(int ms) {
    struct timespec nap;
    nap.tv_sec = ms / 1000;
    nap.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&nap, NULL);
}
static void make_dir(const char* path) { mkdir(path, 0700); }
#endif

#define URL_SLUS "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/SLUS-00757.jpg"
#define URL_SLES "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/SLES-01234.jpg"

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

static int file_is(const char* path, const void* data, size_t len) {
    static unsigned char buf[16384];
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == len && !memcmp(buf, data, len);
}

/* ---- pictures the test writes itself ---- */

/* A JPEG as far as its headers go: start marker, APP0, then one frame header
 * of type `sof` with the given size. The rest is padding; it need not decode. */
static size_t make_jpeg(unsigned char* buf, size_t total, int sof, int w, int h) {
    static const unsigned char head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',  'F',  'I',  'F',
                                         0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    unsigned char frame[19] = {0xFF, 0xC0, 0x00, 0x11, 0x08, 0, 0, 0, 0, 0x03,
                               0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01};
    frame[1] = (unsigned char)sof;
    frame[5] = (unsigned char)(h >> 8);
    frame[6] = (unsigned char)(h & 0xFF);
    frame[7] = (unsigned char)(w >> 8);
    frame[8] = (unsigned char)(w & 0xFF);
    memset(buf, 0, total);
    memcpy(buf, head, sizeof(head));
    memcpy(buf + sizeof(head), frame, sizeof(frame));
    return total;
}

/* A PNG as far as its first chunk goes: the signature and IHDR. */
static size_t make_png(unsigned char* buf, size_t total, unsigned long w, unsigned long h) {
    static const unsigned char head[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                         0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
    memset(buf, 0, total);
    memcpy(buf, head, sizeof(head));
    buf[16] = (unsigned char)(w >> 24);
    buf[17] = (unsigned char)(w >> 16);
    buf[18] = (unsigned char)(w >> 8);
    buf[19] = (unsigned char)w;
    buf[20] = (unsigned char)(h >> 24);
    buf[21] = (unsigned char)(h >> 16);
    buf[22] = (unsigned char)(h >> 8);
    buf[23] = (unsigned char)h;
    buf[24] = 8;
    buf[25] = 6;
    return total;
}

static unsigned char g_jpeg[2048]; /* 300 x 300 */
static unsigned char g_png[2048];  /* 256 x 256 */
static unsigned char g_html[1500]; /* a page, not a picture */
static unsigned char g_old[1300];  /* the cover the player already has */

static void make_fixtures(void) {
    static const char page[] = "<!DOCTYPE html><html><body>404: Not Found</body></html>";
    size_t i;
    make_jpeg(g_jpeg, sizeof(g_jpeg), 0xC0, 300, 300);
    make_png(g_png, sizeof(g_png), 256, 256);
    memset(g_html, ' ', sizeof(g_html));
    memcpy(g_html, page, sizeof(page) - 1);
    for (i = 0; i < sizeof(g_old); ++i) g_old[i] = (unsigned char)(i * 7 + 3);
}

/* ---- serial and address ---- */

static int serial_is(const char* in, const char* want) {
    char out[16] = "untouched";
    return launcher_cover_serial(in, out) == 1 && !strcmp(out, want);
}

static int serial_refused(const char* in) {
    char out[16] = "untouched";
    return launcher_cover_serial(in, out) == 0 && !strcmp(out, "untouched");
}

static void test_serial(void) {
    static const char* const bad[] = {
        "",                 /* nothing */
        "SLUS",             /* no digits */
        "SLUS-0075",        /* four digits */
        "SLUS-007570",      /* six digits */
        "SLU-00757",        /* three letters */
        "SLUSA-00757",      /* five letters */
        "SL1S-00757",       /* a digit among the letters */
        "SLUS-0075A",       /* a letter among the digits */
        " SLUS-00757",      /* a space in front */
        "SLUS-00757 ",      /* a space behind */
        "SLUS 00757",       /* a space in the middle */
        "\"SLUS-00757\"",   /* in quotes */
        "SLUS-00757\"",     /* a quote behind */
        "'SLUS-00757'",     /* in single quotes */
        "SLUS-0075&",       /* an ampersand for a digit */
        "SLUS-00757&calc",  /* a second command */
        "SLUS-00757|x",
        "SLUS-00757;1",     /* the version suffix of a boot file name */
        "SLUS-00757.jpg",
        "../SLUS-00757",    /* a way out of the folder */
        "SLUS-00757/../x",
        "SLUS_007..57",     /* two dots */
        "SLUS_0.0757",      /* the dot in another place */
        "SLUS_007.57.",
        "SLUS..00757",
        "SLUS--00757",      /* two separators */
        "SLUS-_00757",
        "SLUS.00757",       /* a dot as the separator */
        "SLUS-00757\n",
        "SLUS-00757%20",
        "-SLUS00757",
        "--output=x",
    };
    char longer[320];
    char out[16];
    size_t i;
    int all = 1;

    expect(serial_is("SLUS-00757", "SLUS-00757"), "serial: SLUS-00757 stays as it is");
    expect(serial_is("slus_007.57", "SLUS-00757"), "serial: the boot file name slus_007.57");
    expect(serial_is("SLUS00757", "SLUS-00757"), "serial: SLUS00757 gets its hyphen");
    expect(serial_is("SLUS_00757", "SLUS-00757"), "serial: SLUS_00757");
    expect(serial_is("sces-12345", "SCES-12345"), "serial: small letters become capitals");
    expect(launcher_cover_serial("SLPS-00001", out) == 1 && strlen(out) == 10 && out[10] == '\0',
           "serial: the result is 10 characters and a NUL");

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (!serial_refused(bad[i])) {
            fprintf(stderr, "  accepted or wrote out: [%s]\n", bad[i]);
            all = 0;
        }
    }
    expect(all, "serial: 32 forms with quotes, spaces, .., & or extra text are refused, out not written");

    memset(longer, '7', sizeof(longer) - 1);
    memcpy(longer, "SLUS-00757", 10);
    longer[sizeof(longer) - 1] = '\0';
    expect(serial_refused(longer), "serial: a 319-character string that starts as a serial is refused");
    expect(serial_refused(NULL) && launcher_cover_serial("SLUS-00757", NULL) == 0,
           "serial: NULL in or out is refused");
}

static void test_url(void) {
    char url[200] = "x";
    char tiny[40] = "x";
    size_t i;
    int plain = 1;

    expect(launcher_cover_url("SLUS-00757", url, sizeof(url)) == 1 && !strcmp(url, URL_SLUS),
           "url: the exact address for SLUS-00757");
    for (i = 8; url[i]; ++i) {
        const char c = url[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '-'))
            plain = 0;
    }
    expect(plain, "url: only letters, digits and / . - after https://");
    url[0] = 'x';
    expect(launcher_cover_url("slus_007.57", url, sizeof(url)) == 1 && !strcmp(url, URL_SLUS),
           "url: the boot file name gives the same address");
    url[0] = 'x';
    expect(launcher_cover_url("SLUS-00757\" -o \"x", url, sizeof(url)) == 0 && url[0] == '\0',
           "url: a serial that does not normalise gives no address");
    expect(launcher_cover_url("../../etc/passwd", url, sizeof(url)) == 0 && url[0] == '\0',
           "url: a path gives no address");
    expect(launcher_cover_url("SLUS-00757", tiny, sizeof(tiny)) == 0 && tiny[0] == '\0',
           "url: a buffer that is too small gives no cut-off address");
    expect(launcher_cover_url(NULL, url, sizeof(url)) == 0 &&
           launcher_cover_url("SLUS-00757", NULL, 0) == 0,
           "url: NULL serial or buffer gives no address");
}

/* ---- the picture check ---- */

static int check_bytes(const char* leaf, const unsigned char* data, size_t len, int* w, int* h) {
    const char* path = in_dir(leaf);
    if (!write_file(path, data, len)) {
        fprintf(stderr, "  could not write %s\n", path);
        return -1;
    }
    return launcher_cover_check_file(path, w, h);
}

static void test_check_file(void) {
    static unsigned char buf[4096];
    static unsigned char big[70000];
    int w = -1, h = -1;

    make_jpeg(buf, 2048, 0xC0, 300, 300);
    expect(check_bytes("check-jpeg.jpg", buf, 2048, &w, &h) == 1 && w == 300 && h == 300,
           "check: a 300x300 JPEG of 2 KiB passes, size read from its frame header");
    make_jpeg(buf, 2048, 0xC0, 640, 480);
    expect(check_bytes("check-jpeg-wide.jpg", buf, 2048, &w, &h) == 1 && w == 640 && h == 480,
           "check: width and height are not swapped (640x480)");
    expect(launcher_cover_check_file(in_dir("check-jpeg.jpg"), NULL, NULL) == 1,
           "check: w and h may be NULL");
    make_jpeg(buf, 2048, 0xC2, 512, 512);
    expect(check_bytes("check-jpeg-progressive.jpg", buf, 2048, &w, &h) == 1 && w == 512,
           "check: a progressive JPEG (SOF2) passes");
    make_png(buf, 2048, 256, 256);
    expect(check_bytes("check-png.png", buf, 2048, &w, &h) == 1 && w == 256 && h == 256,
           "check: a 256x256 PNG of 2 KiB passes, size read from IHDR");

    /* The sides. */
    make_jpeg(buf, 2048, 0xC0, 64, 64);
    expect(check_bytes("check-jpeg-64.jpg", buf, 2048, &w, &h) == 1, "check: 64x64 is the smallest side");
    make_jpeg(buf, 2048, 0xC0, 4096, 4096);
    expect(check_bytes("check-jpeg-4096.jpg", buf, 2048, &w, &h) == 1,
           "check: 4096x4096 is the largest side");
    w = h = -1;
    make_jpeg(buf, 2048, 0xC0, 32, 32);
    expect(check_bytes("check-jpeg-small.jpg", buf, 2048, &w, &h) == 0 && w == 0 && h == 0,
           "check: a 32x32 JPEG is too small, w and h are 0");
    make_jpeg(buf, 2048, 0xC0, 300, 63);
    expect(check_bytes("check-jpeg-flat.jpg", buf, 2048, &w, &h) == 0, "check: 300x63 fails on one side");
    make_jpeg(buf, 2048, 0xC0, 5000, 5000);
    expect(check_bytes("check-jpeg-large.jpg", buf, 2048, &w, &h) == 0,
           "check: a 5000x5000 JPEG is too large");
    make_png(buf, 2048, 32, 32);
    expect(check_bytes("check-png-small.png", buf, 2048, &w, &h) == 0, "check: a 32x32 PNG is too small");
    make_png(buf, 2048, 5000, 5000);
    expect(check_bytes("check-png-large.png", buf, 2048, &w, &h) == 0, "check: a 5000x5000 PNG is too large");
    make_png(buf, 2048, 0x80000100UL, 256);
    expect(check_bytes("check-png-huge.png", buf, 2048, &w, &h) == 0,
           "check: a PNG width with the top bit set does not pass as a small number");

    /* Not a picture. */
    expect(check_bytes("check-text.txt", g_html, sizeof(g_html), &w, &h) == 0, "check: a text file fails");
    make_jpeg(buf, 100, 0xC0, 300, 300);
    expect(check_bytes("check-100.jpg", buf, 100, &w, &h) == 0, "check: a 100-byte file fails");
    make_jpeg(buf, 1023, 0xC0, 300, 300);
    expect(check_bytes("check-1023.jpg", buf, 1023, &w, &h) == 0, "check: 1023 bytes is under 1 KiB");
    make_jpeg(buf, 1024, 0xC0, 300, 300);
    expect(check_bytes("check-1024.jpg", buf, 1024, &w, &h) == 1, "check: 1024 bytes is enough");
    expect(check_bytes("check-empty.jpg", buf, 0, &w, &h) == 0, "check: an empty file fails");
    remove(in_dir("check-missing.jpg"));
    expect(launcher_cover_check_file(in_dir("check-missing.jpg"), &w, &h) == 0,
           "check: a missing file fails");
    expect(launcher_cover_check_file(NULL, &w, &h) == 0 && launcher_cover_check_file("", &w, &h) == 0,
           "check: no path fails");

    /* The JPEG marker walk. */
    make_jpeg(buf, 2048, 0xC4, 300, 300); /* a table segment, then nothing */
    expect(check_bytes("check-jpeg-table.jpg", buf, 2048, &w, &h) == 0,
           "check: a Huffman table (C4) is not taken for a frame header");
    make_jpeg(buf, 2048, 0xC4, 300, 300);
    {
        unsigned char frame[19];
        memcpy(frame, buf + 20, sizeof(frame));
        frame[1] = 0xC0;
        frame[7] = 0x01; /* 400 wide */
        frame[8] = 0x90;
        memcpy(buf + 39, frame, sizeof(frame));
    }
    expect(check_bytes("check-jpeg-table-frame.jpg", buf, 2048, &w, &h) == 1 && w == 400 && h == 300,
           "check: the walk goes past a table to the frame header behind it");
    make_jpeg(buf, 2048, 0xC0, 300, 300);
    memmove(buf + 22, buf + 20, 19); /* two fill bytes in front of the frame marker */
    buf[20] = 0xFF;
    buf[21] = 0xFF;
    expect(check_bytes("check-jpeg-fill.jpg", buf, 2048, &w, &h) == 1 && w == 300,
           "check: fill bytes in front of a marker are skipped");
    make_jpeg(buf, 2048, 0xC3, 300, 300);
    expect(check_bytes("check-jpeg-lossless.jpg", buf, 2048, &w, &h) == 0,
           "check: a lossless frame (SOF3) fails");
    make_jpeg(buf, 2048, 0xDA, 300, 300);
    expect(check_bytes("check-jpeg-scan.jpg", buf, 2048, &w, &h) == 0,
           "check: a scan with no frame header before it fails");
    make_jpeg(buf, 2048, 0xC0, 300, 300);
    buf[4] = 0xFF; /* APP0 says it is 65535 bytes long */
    buf[5] = 0xFF;
    expect(check_bytes("check-jpeg-overrun.jpg", buf, 2048, &w, &h) == 0,
           "check: a segment longer than the file fails");
    make_jpeg(buf, 2048, 0xC0, 300, 300);
    buf[4] = 0x00; /* a length under 2 would never move on */
    buf[5] = 0x01;
    expect(check_bytes("check-jpeg-len1.jpg", buf, 2048, &w, &h) == 0, "check: a segment length of 1 fails");
    make_jpeg(buf, 2048, 0xC0, 300, 300);
    buf[20] = 0x00; /* no FF where the next marker must be */
    expect(check_bytes("check-jpeg-nomarker.jpg", buf, 2048, &w, &h) == 0,
           "check: bytes that are not a marker fail");
    make_jpeg(buf, 2048, 0xC0, 300, 300);
    buf[29] = 0x09; /* nine components do not fit the length */
    expect(check_bytes("check-jpeg-components.jpg", buf, 2048, &w, &h) == 0,
           "check: a frame header whose length does not fit its components fails");
    make_png(buf, 2048, 256, 256);
    memcpy(buf + 12, "IDAT", 4);
    expect(check_bytes("check-png-noihdr.png", buf, 2048, &w, &h) == 0,
           "check: a PNG whose first chunk is not IHDR fails");

    /* A long run of zero-length segments must end. */
    {
        size_t i;
        memset(big, 0, sizeof(big));
        big[0] = 0xFF;
        big[1] = 0xD8;
        for (i = 2; i + 4 <= sizeof(big); i += 4) {
            big[i] = 0xFF;
            big[i + 1] = 0xE1;
            big[i + 2] = 0x00;
            big[i + 3] = 0x02;
        }
        expect(check_bytes("check-jpeg-endless.jpg", big, sizeof(big), &w, &h) == 0,
               "check: a long run of empty segments with no frame header fails");
    }
}

/* ---- the fake fetcher ---- */

enum { ANS_JPEG = 0, ANS_PNG, ANS_HTML, ANS_404, ANS_FAIL, ANS_NO_TOOL, ANS_NOTHING };

typedef struct {
    int answers[8]; /* one per call, in order */
    int calls;
    int delay_ms;
    char urls[8][200];
    char dests[8][700];
} Fake;

static int fake_fetch(void* ctx, const char* url, const char* dest) {
    Fake* f = (Fake*)ctx;
    const int call = f->calls++;
    const int answer = call < 8 ? f->answers[call] : ANS_FAIL;

    if (call < 8) {
        snprintf(f->urls[call], sizeof(f->urls[call]), "%s", url);
        snprintf(f->dests[call], sizeof(f->dests[call]), "%s", dest);
    }
    if (f->delay_ms) sleep_ms(f->delay_ms);
    switch (answer) {
    case ANS_JPEG:    return write_file(dest, g_jpeg, sizeof(g_jpeg)) ? 0 : 2;
    case ANS_PNG:     return write_file(dest, g_png, sizeof(g_png)) ? 0 : 2;
    case ANS_HTML:    return write_file(dest, g_html, sizeof(g_html)) ? 0 : 2;
    case ANS_404:     return 1;
    case ANS_NO_TOOL: return 3;
    case ANS_NOTHING: return 0; /* says saved, writes no file */
    default:
        /* A broken transfer can leave a piece of the file behind. */
        write_file(dest, "half a file", 11);
        return 2;
    }
}

static void fake_reset(Fake* f, int a0, int a1, int a2, int a3) {
    memset(f, 0, sizeof(*f));
    f->answers[0] = a0;
    f->answers[1] = a1;
    f->answers[2] = a2;
    f->answers[3] = a3;
}

/* The path of the download for a cover path. */
static const char* part_of(const char* dest) {
    static char part[700];
    snprintf(part, sizeof(part), "%s.part", dest);
    return part;
}

static void test_fetch_sync(void) {
    Fake f;
    char used[16];
    const char* dest;
    int r;

    /* Saved on the first serial. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        dest = in_dir("sync-first.jpg");
        remove(dest);
        fake_reset(&f, ANS_JPEG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        strcpy(used, "x");
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_SAVED && f.calls == 1, "sync: saved on the first serial with one request");
        expect(!strcmp(f.urls[0], URL_SLUS), "sync: the request is the exact address of the serial");
        expect(!strcmp(f.dests[0], part_of(dest)), "sync: the download goes to <dest>.part");
        expect(file_is(dest, g_jpeg, sizeof(g_jpeg)) && launcher_cover_check_file(dest, NULL, NULL),
               "sync: the cover is the checked download, byte for byte");
        expect(!file_exists(part_of(dest)), "sync: no .part is left after a save");
        expect(!strcmp(used, "SLUS-00757"), "sync: used is the serial that was saved");
    }

    /* 404 on the first, saved on the second. */
    {
        static const char* const serials[] = {"SLES-01234", "slus_007.57"};
        dest = in_dir("sync-second.jpg");
        remove(dest);
        fake_reset(&f, ANS_404, ANS_JPEG, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 2, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_SAVED && f.calls == 2 && !strcmp(f.urls[0], URL_SLES) &&
               !strcmp(f.urls[1], URL_SLUS),
               "sync: 404 on the first serial, then the second is asked and saved");
        expect(!strcmp(used, "SLUS-00757"), "sync: used is the second serial, normalised");
        expect(file_is(dest, g_jpeg, sizeof(g_jpeg)), "sync: the second answer is the cover");
    }

    /* 404 on all. */
    {
        static const char* const serials[] = {"SLES-01234", "SLUS-00757", "SCES-00001"};
        dest = in_dir("sync-none.jpg");
        remove(dest);
        fake_reset(&f, ANS_404, ANS_404, ANS_404, ANS_FAIL);
        strcpy(used, "x");
        r = launcher_cover_fetch_sync(serials, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NOT_FOUND && f.calls == 3, "sync: 404 on all three serials is NOT_FOUND");
        expect(!file_exists(dest) && !file_exists(part_of(dest)),
               "sync: NOT_FOUND leaves no cover and no .part");
        expect(used[0] == '\0', "sync: used is empty when nothing was saved");
    }

    /* A transfer failure keeps the old cover. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        dest = in_dir("sync-failed.jpg");
        write_file(dest, g_old, sizeof(g_old));
        fake_reset(&f, ANS_FAIL, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_FAILED && f.calls == 1, "sync: a failed transfer is FAILED");
        expect(file_is(dest, g_old, sizeof(g_old)), "sync: FAILED leaves the old cover byte for byte");
        expect(!file_exists(part_of(dest)), "sync: the piece of a broken transfer is removed");
    }

    /* A page in place of a picture keeps the old cover. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        dest = in_dir("sync-html.jpg");
        write_file(dest, g_old, sizeof(g_old));
        fake_reset(&f, ANS_HTML, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_BAD_IMAGE, "sync: a page that came back as the file is BAD_IMAGE");
        expect(file_is(dest, g_old, sizeof(g_old)), "sync: BAD_IMAGE leaves the old cover byte for byte");
        expect(!file_exists(part_of(dest)), "sync: the page is removed");

        fake_reset(&f, ANS_NOTHING, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_BAD_IMAGE && file_is(dest, g_old, sizeof(g_old)),
               "sync: a fetcher that says saved and writes nothing is BAD_IMAGE, old cover kept");
    }

    /* A second good fetch replaces the cover. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        dest = in_dir("sync-replace.jpg");
        remove(dest);
        fake_reset(&f, ANS_JPEG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_SAVED && file_is(dest, g_jpeg, sizeof(g_jpeg)),
               "sync: the first cover is in place");
        fake_reset(&f, ANS_PNG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_SAVED && file_is(dest, g_png, sizeof(g_png)),
               "sync: a second good fetch replaces the cover");
        expect(!file_exists(part_of(dest)), "sync: no .part is left after the replace");
    }

    /* No serial: nothing is asked. */
    {
        static const char* const serials[] = {"", "not a serial", NULL, "SLUS-00757\""};
        dest = in_dir("sync-noserial.jpg");
        remove(dest);
        fake_reset(&f, ANS_JPEG, ANS_JPEG, ANS_JPEG, ANS_JPEG);
        r = launcher_cover_fetch_sync(serials, 4, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NO_SERIAL && f.calls == 0,
               "sync: no usable serial is NO_SERIAL, fetcher never called");
        r = launcher_cover_fetch_sync(serials, 0, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NO_SERIAL && f.calls == 0, "sync: an empty list is NO_SERIAL");
        r = launcher_cover_fetch_sync(NULL, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NO_SERIAL && f.calls == 0 && !file_exists(dest),
               "sync: a NULL list is NO_SERIAL");
    }

    /* The tool is missing. */
    {
        static const char* const serials[] = {"SLUS-00757", "SLES-01234"};
        dest = in_dir("sync-notool.jpg");
        write_file(dest, g_old, sizeof(g_old));
        fake_reset(&f, ANS_NO_TOOL, ANS_JPEG, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 2, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NO_TOOL, "sync: a fetcher with no tool is NO_TOOL");
        expect(f.calls == 1, "sync: a missing tool is asked once, not once per serial");
        expect(file_is(dest, g_old, sizeof(g_old)), "sync: NO_TOOL leaves the old cover");
    }

    /* Duplicates are asked once. */
    {
        static const char* const serials[] = {"SLUS-00757", "slus_007.57", "SLUS00757", "SLES-01234",
                                              "SLUS_00757", "sles_012.34"};
        dest = in_dir("sync-duplicates.jpg");
        remove(dest);
        fake_reset(&f, ANS_404, ANS_404, ANS_JPEG, ANS_JPEG);
        r = launcher_cover_fetch_sync(serials, 6, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_NOT_FOUND && f.calls == 2,
               "sync: six spellings of two serials make two requests");
        expect(!strcmp(f.urls[0], URL_SLUS) && !strcmp(f.urls[1], URL_SLES),
               "sync: the two requests keep the order of the list");
    }

    /* Which result wins when nothing was saved. */
    {
        static const char* const serials[] = {"SLUS-00757", "SLES-01234", "SCES-00001"};
        dest = in_dir("sync-order.jpg");
        remove(dest);
        fake_reset(&f, ANS_FAIL, ANS_HTML, ANS_404, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_BAD_IMAGE && f.calls == 3, "sync: BAD_IMAGE wins over FAILED and NOT_FOUND");
        fake_reset(&f, ANS_404, ANS_FAIL, ANS_404, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_FAILED && f.calls == 3, "sync: FAILED wins over NOT_FOUND");
        fake_reset(&f, ANS_FAIL, ANS_NO_TOOL, ANS_404, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_FAILED, "sync: FAILED wins over NO_TOOL");
        fake_reset(&f, ANS_HTML, ANS_JPEG, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 3, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_SAVED && f.calls == 2 && !strcmp(used, "SLES-01234"),
               "sync: a bad answer for one serial does not stop a good one for the next");
        expect(!file_exists(part_of(dest)), "sync: no .part is left");
    }

    /* The cover cannot be replaced. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        static char long_dest[1100];
        char inside[700];
        dest = in_dir("sync-blocked.jpg");
        remove(dest);
        make_dir(dest); /* a folder with a file in it stands where the cover goes */
        snprintf(inside, sizeof(inside), "%s/keep.txt", dest);
        write_file(inside, "keep", 4);
        fake_reset(&f, ANS_JPEG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        strcpy(used, "x");
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_WRITE_FAILED && used[0] == '\0',
               "sync: a cover that cannot be replaced is WRITE_FAILED");
        expect(!file_exists(part_of(dest)) && file_is(inside, "keep", 4),
               "sync: WRITE_FAILED removes the .part and leaves what is in the way");

        fake_reset(&f, ANS_JPEG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        expect(launcher_cover_fetch_sync(serials, 1, NULL, fake_fetch, &f, used) == LNG_COVER_WRITE_FAILED &&
               launcher_cover_fetch_sync(serials, 1, "", fake_fetch, &f, used) == LNG_COVER_WRITE_FAILED &&
               f.calls == 0,
               "sync: no dest is WRITE_FAILED, nothing is asked");
        memset(long_dest, 'a', 1025);
        long_dest[1025] = '\0';
        r = launcher_cover_fetch_sync(serials, 1, long_dest, fake_fetch, &f, used);
        expect(r == LNG_COVER_WRITE_FAILED && f.calls == 0,
               "sync: a dest of 1025 characters is WRITE_FAILED, nothing is asked");
    }

    /* used may be NULL. */
    {
        static const char* const serials[] = {"SLUS-00757"};
        dest = in_dir("sync-nullused.jpg");
        remove(dest);
        fake_reset(&f, ANS_JPEG, ANS_FAIL, ANS_FAIL, ANS_FAIL);
        r = launcher_cover_fetch_sync(serials, 1, dest, fake_fetch, &f, NULL);
        expect(r == LNG_COVER_SAVED && file_exists(dest), "sync: used may be NULL");
    }
}

/* ---- the worker job ---- */

static Fake g_job_fake;

static int wait_for_job(char* used) {
    int i, state = LNG_COVER_BUSY;
    for (i = 0; i < 2000 && state == LNG_COVER_BUSY; ++i) {
        sleep_ms(5);
        state = launcher_cover_job_poll(used);
    }
    return state;
}

static void test_job(void) {
    static const char* const serials[] = {"SLES-01234", "slus_007.57", "SLES-01234", "SCES-00001",
                                          "SCES-00002", "SCES-00003"};
    static const char* const none[] = {"nothing here"};
    static char long_dest[1100];
    char used[16];
    const char* dest = in_dir("job.jpg");
    const char* other = in_dir("job-other.jpg");
    int started, second, busy, state;

    strcpy(used, "x");
    expect(launcher_cover_job_poll(used) == LNG_COVER_IDLE && used[0] == '\0',
           "job: IDLE before the first start");
    expect(launcher_cover_job_take_finished() == 0, "job: nothing has finished before the first start");

    expect(launcher_cover_job_start(serials, 6, NULL, fake_fetch, &g_job_fake) == 0 &&
           launcher_cover_job_start(serials, 6, "", fake_fetch, &g_job_fake) == 0,
           "job: no dest does not start a job");
    memset(long_dest, 'a', 1025);
    long_dest[1025] = '\0';
    expect(launcher_cover_job_start(serials, 6, long_dest, fake_fetch, &g_job_fake) == 0 &&
           launcher_cover_job_poll(NULL) == LNG_COVER_IDLE,
           "job: a dest of 1025 characters does not start a job");

    remove(dest);
    remove(other);
    fake_reset(&g_job_fake, ANS_404, ANS_JPEG, ANS_FAIL, ANS_FAIL);
    g_job_fake.delay_ms = 200;
    started = launcher_cover_job_start(serials, 6, dest, fake_fetch, &g_job_fake);
    busy = launcher_cover_job_poll(used);
    second = launcher_cover_job_start(serials, 6, other, fake_fetch, &g_job_fake);
    expect(started == 1, "job: start returns 1");
    expect(busy == LNG_COVER_BUSY && used[0] == '\0', "job: poll says BUSY while the fetch runs");
    expect(second == 0, "job: a second start while one runs returns 0");
    expect(launcher_cover_job_take_finished() == 0, "job: nothing has finished while it runs");

    state = wait_for_job(used);
    expect(state == LNG_COVER_SAVED, "job: poll says SAVED when the fetch has ended");
    expect(!strcmp(used, "SLUS-00757"), "job: used is the serial that was saved");
    expect(g_job_fake.calls == 2 && !strcmp(g_job_fake.urls[0], URL_SLES) &&
           !strcmp(g_job_fake.urls[1], URL_SLUS),
           "job: the serials were copied in order, the duplicate left out");
    expect(file_is(dest, g_jpeg, sizeof(g_jpeg)) && !file_exists(other),
           "job: the cover is saved at the first dest, the refused start wrote nothing");
    expect(launcher_cover_job_take_finished() == 1, "job: take_finished is 1 after the job has ended");
    expect(launcher_cover_job_take_finished() == 0, "job: take_finished is 1 only once");
    strcpy(used, "x");
    expect(launcher_cover_job_poll(used) == LNG_COVER_SAVED && !strcmp(used, "SLUS-00757"),
           "job: the result stays readable");

    /* Only the first four different serials are copied. */
    fake_reset(&g_job_fake, ANS_404, ANS_404, ANS_404, ANS_404);
    g_job_fake.answers[4] = ANS_JPEG;
    remove(other);
    started = launcher_cover_job_start(serials, 6, other, fake_fetch, &g_job_fake);
    state = wait_for_job(used);
    expect(started == 1 && state == LNG_COVER_NOT_FOUND && g_job_fake.calls == 4,
           "job: a new start works after the end, and asks for four serials at most");
    expect(launcher_cover_job_take_finished() == 1 && !file_exists(other),
           "job: a job that saved nothing also finishes once");

    /* No serial is a result, not a refused start. */
    fake_reset(&g_job_fake, ANS_JPEG, ANS_JPEG, ANS_JPEG, ANS_JPEG);
    started = launcher_cover_job_start(none, 1, other, fake_fetch, &g_job_fake);
    state = wait_for_job(used);
    expect(started == 1 && state == LNG_COVER_NO_SERIAL && g_job_fake.calls == 0,
           "job: a list with no usable serial ends as NO_SERIAL, fetcher never called");
    started = launcher_cover_job_start(NULL, 0, other, fake_fetch, &g_job_fake);
    state = wait_for_job(used);
    expect(started == 1 && state == LNG_COVER_NO_SERIAL && launcher_cover_job_take_finished() == 1,
           "job: a NULL list ends as NO_SERIAL");
}

static void test_state_text(void) {
    static const int states[] = {LNG_COVER_BUSY,      LNG_COVER_SAVED,     LNG_COVER_NOT_FOUND,
                                 LNG_COVER_FAILED,    LNG_COVER_BAD_IMAGE, LNG_COVER_NO_SERIAL,
                                 LNG_COVER_NO_TOOL,   LNG_COVER_WRITE_FAILED};
    size_t i, k;
    int all = 1;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        const char* text = launcher_cover_state_text(states[i]);
        if (!text || !text[0] || strlen(text) > 60 || strchr(text, '\n')) all = 0;
        for (k = 0; k < i && text; ++k) {
            if (!strcmp(text, launcher_cover_state_text(states[k]))) all = 0;
        }
    }
    expect(all, "text: every state has its own short line");
    expect(!strcmp(launcher_cover_state_text(LNG_COVER_IDLE), "") &&
           !strcmp(launcher_cover_state_text(999), "") && !strcmp(launcher_cover_state_text(-1), ""),
           "text: IDLE and a value that is not a state give an empty line");
    expect(!strcmp(launcher_cover_state_text(LNG_COVER_SAVED), "Cover saved.") &&
           !strcmp(launcher_cover_state_text(LNG_COVER_NOT_FOUND), "No cover for this disc at the source."),
           "text: the lines for SAVED and NOT_FOUND");
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0]) snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    make_fixtures();

    test_serial();
    test_url();
    test_check_file();
    test_fetch_sync();
    test_job();
    test_state_text();

    /* Not a check: the answer depends on the system. No process is started. */
    printf("note: launcher_cover_tool_available() = %d\n", launcher_cover_tool_available());

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("launcher_cover_test: all checks passed\n");
    return 0;
}
