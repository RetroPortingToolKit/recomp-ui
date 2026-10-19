#ifndef LAUNCHER_COVER_H
#define LAUNCHER_COVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cover of the mounted PlayStation disc.
 *
 * The player asks for it with a button. One request then goes to a public
 * repository, and the picture is kept in the title's own writable folder.
 * Nothing here draws and nothing here runs without that request. The UI
 * starts a job, polls it every frame, and reloads the picture when it ends. */

/* In the title's writable folder. The name says .jpg; the check also lets a
 * PNG through, so load the file by its content. */
#define LAUNCHER_COVER_FILE "launcher-cover.jpg"
#define LAUNCHER_COVER_SOURCE "github.com/xlenore/psx-covers"

typedef enum {
    LNG_COVER_IDLE = 0,
    LNG_COVER_BUSY,        /* a fetch runs */
    LNG_COVER_SAVED,       /* fetched, checked and saved */
    LNG_COVER_NOT_FOUND,   /* the source has no cover for the serial(s) */
    LNG_COVER_FAILED,      /* no network, a time-out, any other transfer failure */
    LNG_COVER_BAD_IMAGE,   /* what came back is not a picture of a sane size */
    LNG_COVER_NO_SERIAL,   /* no usable serial to ask for */
    LNG_COVER_NO_TOOL,     /* this system has no curl to make the request with */
    LNG_COVER_WRITE_FAILED /* the file could not be written or replaced */
} LauncherCoverState;

/* "SLUS-00757" from any of "SLUS-00757", "slus_007.57", "SLUS00757",
 * "SLUS_00757". Returns 1 and writes out (10 characters and the NUL) only for
 * 4 letters + 5 digits; else 0, and out is not written. Nothing may come
 * before or after the serial. */
int launcher_cover_serial(const char* in, char out[16]);

/* https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/<SERIAL>.jpg
 * Returns 1, or 0 when the serial does not normalise (so nothing but
 * [A-Z0-9-] can ever reach a command line) or cap is too small. */
int launcher_cover_url(const char* serial, char* out, size_t cap);

/* 1 when the file is a JPEG or a PNG by its own header, each side 64..4096
 * pixels, file size 1 KiB..8 MiB. Reads headers only (JPEG: walks the markers
 * to the first SOF0/SOF1/SOF2; PNG: IHDR). w/h may be NULL. */
int launcher_cover_check_file(const char* path, int* w, int* h);

/* Gets `url` into the file `dest`. Returns 0 saved, 1 the server says the
 * file does not exist (HTTP 404), 2 any other failure, 3 the tool is
 * missing. */
typedef int (*LauncherCoverFetchFn)(void* ctx, const char* url, const char* dest);

/* The default fetcher: the system's curl, started hidden, no shell.
 * Windows: curl.exe in the system folder (%SystemRoot%\System32).
 * macOS: /usr/bin/curl. Linux: curl found on PATH.
 * Arguments: -s -S -L --max-time 20 --proto =https --proto-redir =https
 *            -o <dest> -w %{http_code} <url>
 * Standard output goes to the file "<dest>.code", which is read and removed:
 * 200 = saved, 404 = not found, anything else or a non-zero exit = failure.
 * `dest` is removed unless the answer is 200. The wait ends after 30 s, and
 * the process is ended then. Takes only a plain https address: letters,
 * digits and / . - _ after "https://". `ctx` is not used. */
int launcher_cover_fetch_curl(void* ctx, const char* url, const char* dest);

/* 1 when launcher_cover_fetch_curl has a curl to start on this system. No
 * process is started to find out: Windows and macOS test the file, Linux
 * searches PATH for an executable file named curl. */
int launcher_cover_tool_available(void);

/* One whole fetch, on the calling thread. Tries serials[0..n-1] in order and
 * skips the ones that do not normalise and the duplicates. The first that is
 * saved AND passes launcher_cover_check_file wins. The download goes to
 * "<dest>.part"; only a checked picture replaces `dest`, so on any failure
 * the old cover stays. fetch == NULL means launcher_cover_fetch_curl.
 *
 * Returns a LauncherCoverState other than IDLE/BUSY. `used` (may be NULL,
 * 16 bytes) gets the serial that was saved, else an empty string.
 * When nothing was saved: BAD_IMAGE if any answer was not a picture, else
 * FAILED if any transfer failed, else NO_TOOL if the fetcher said so, else
 * NOT_FOUND. NO_SERIAL when no serial normalised: the fetcher is not called.
 * WRITE_FAILED when `dest` is empty, longer than 1024 characters, or cannot
 * be replaced. A missing tool ends the run; later serials are not asked. */
int launcher_cover_fetch_sync(const char* const* serials, int n, const char* dest,
                              LauncherCoverFetchFn fetch, void* ctx, char* used);

/* The same on a worker thread, one job at a time. start copies its arguments
 * (the first 4 different serials that normalise, dest up to 1024 characters).
 * Returns 1 when a job was started. Returns 0 when one already runs, when
 * dest is empty or too long, or when the thread cannot start. A list with no
 * usable serial still starts a job; its result is LNG_COVER_NO_SERIAL.
 * fetch == NULL means launcher_cover_fetch_curl. `ctx` must stay valid until
 * the job ends. */
int launcher_cover_job_start(const char* const* serials, int n, const char* dest,
                             LauncherCoverFetchFn fetch, void* ctx);

/* LNG_COVER_IDLE when no job was started, LNG_COVER_BUSY while one runs, then
 * its result. The result stays readable until the next start. `used` as
 * above. Call from the UI thread every frame. */
int launcher_cover_job_poll(char* used);

/* 1 once after a job has ended, so the caller reloads the picture one time. */
int launcher_cover_job_take_finished(void);

#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
/* Blocking module teardown: all worker code/context is gone before return. */
void launcher_cover_job_wait(void);
#endif

/* One short line for the player per state, in plain English. "" for
 * LNG_COVER_IDLE and for a value that is not a state. */
const char* launcher_cover_state_text(int state);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_COVER_H */
