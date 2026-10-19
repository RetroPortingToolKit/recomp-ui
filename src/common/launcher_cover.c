/* launcher_cover.c -- the cover of the mounted disc. See launcher_cover.h. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* posix_spawn, nanosleep, kill */
#endif

#include "launcher_cover.h"
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
#include "launcher_module_runtime.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wchar.h>
#include <windows.h>
typedef wchar_t cover_tool_char;
#define COVER_TOOL_CAP 300
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
typedef char cover_tool_char;
#define COVER_TOOL_CAP 1024
#endif

#define COVER_URL_BASE "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/"
#define COVER_URL_CAP 160
#define COVER_DEST_MAX 1024                   /* the longest `dest` of a fetch */
#define COVER_PATH_CAP (COVER_DEST_MAX + 16)  /* room for ".part" and ".code" */
#define COVER_MIN_BYTES 1024L
#define COVER_MAX_BYTES (8L * 1024L * 1024L)
#define COVER_MIN_SIDE 64
#define COVER_MAX_SIDE 4096
#define COVER_WAIT_MS 30000
#define COVER_JOB_SERIALS 4

/* ---- serial and address ---- */

int launcher_cover_serial(const char* in, char out[16]) {
    char text[11];
    int digits = 0;
    int dot_seen = 0;
    int i;

    if (!in || !out) return 0;
    for (i = 0; i < 4; ++i, ++in) {
        char c = *in;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') return 0;
        text[i] = c;
    }
    if (*in == '-' || *in == '_') ++in;
    text[4] = '-';
    while (digits < 5) {
        if (*in >= '0' && *in <= '9') {
            text[5 + digits++] = *in++;
        } else if (*in == '.' && digits == 3 && !dot_seen) {
            /* The dot of the boot file name: SLUS_007.57 */
            dot_seen = 1;
            ++in;
        } else {
            return 0;
        }
    }
    if (*in) return 0;
    text[10] = '\0';
    memcpy(out, text, sizeof(text));
    return 1;
}

int launcher_cover_url(const char* serial, char* out, size_t cap) {
    char clean[16];
    int n;

    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!launcher_cover_serial(serial, clean)) return 0;
    n = snprintf(out, cap, COVER_URL_BASE "%s.jpg", clean);
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* ---- the picture check ---- */

static unsigned cover_be16(const unsigned char* p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned long cover_be32(const unsigned char* p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Walks the JPEG segments after the start marker to the first frame header.
 * A segment is FF, a marker byte, a 16-bit length that counts itself, and
 * the data. Returns 1 with the size of a baseline, extended or progressive
 * frame. Returns 0 for any other frame type, a scan with no frame header
 * before it, or a segment that does not fit the file. */
static int cover_jpeg_size(FILE* f, long size, unsigned long* w, unsigned long* h) {
    long pos = 2;
    int steps;

    for (steps = 0; steps < 4096; ++steps) {
        unsigned char b[8];
        unsigned marker, len;

        if (fseek(f, pos, SEEK_SET) != 0 || fread(b, 1, 2, f) != 2) return 0;
        if (b[0] != 0xFF) return 0;
        if (b[1] == 0xFF) { /* a fill byte before the marker */
            pos += 1;
            continue;
        }
        marker = b[1];
        pos += 2;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; /* no length */
        if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || marker == 0xDA) return 0;
        if (fread(b, 1, 2, f) != 2) return 0;
        len = cover_be16(b);
        if (len < 2 || pos + (long)len > size) return 0;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            /* precision, height, width, number of components */
            if (fread(b, 1, 6, f) != 6) return 0;
            if (b[5] < 1 || b[5] > 4 || len != 8U + 3U * b[5]) return 0;
            *h = cover_be16(b + 1);
            *w = cover_be16(b + 3);
            return 1;
        }
        /* C4, C8 and CC are tables. The rest of C0..CF are frame types that
         * the launcher does not show. */
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
            return 0;
        pos += (long)len;
    }
    return 0;
}

int launcher_cover_check_file(const char* path, int* w, int* h) {
    static const unsigned char png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned char head[24];
    unsigned long pw = 0, ph = 0;
    size_t got;
    long size;
    int ok = 0;
    FILE* f;

    if (w) *w = 0;
    if (h) *h = 0;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size < COVER_MIN_BYTES || size > COVER_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    got = fread(head, 1, sizeof(head), f);
    if (got == sizeof(head) && !memcmp(head, png_sig, sizeof(png_sig))) {
        /* The first chunk must be IHDR: length 13, then width and height. */
        if (cover_be32(head + 8) == 13UL && !memcmp(head + 12, "IHDR", 4)) {
            pw = cover_be32(head + 16);
            ph = cover_be32(head + 20);
            ok = 1;
        }
    } else if (got >= 4 && head[0] == 0xFF && head[1] == 0xD8) {
        ok = cover_jpeg_size(f, size, &pw, &ph);
    }
    fclose(f);
    if (!ok) return 0;
    if (pw < COVER_MIN_SIDE || pw > COVER_MAX_SIDE || ph < COVER_MIN_SIDE || ph > COVER_MAX_SIDE)
        return 0;
    if (w) *w = (int)pw;
    if (h) *h = (int)ph;
    return 1;
}

/* ---- the default fetcher: the system's curl ---- */

/* Only a plain https address goes on a command line. The address this module
 * builds has this shape. */
static int cover_url_is_plain(const char* url) {
    size_t i;

    if (!url || strncmp(url, "https://", 8) != 0) return 0;
    for (i = 8; url[i]; ++i) {
        const char c = url[i];
        if (i >= COVER_URL_CAP - 1) return 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (c == '/' || c == '.' || c == '-' || c == '_') continue;
        return 0;
    }
    return i > 8;
}

/* The three digits that curl printed for -w %{http_code}; 0 for anything else. */
static int cover_read_code(const char* path) {
    char text[8];
    size_t got, i;
    int code = 0;
    FILE* f = fopen(path, "rb");

    if (!f) return 0;
    got = fread(text, 1, sizeof(text), f);
    fclose(f);
    if (got < 3 || (got > 3 && text[3] >= '0' && text[3] <= '9')) return 0;
    for (i = 0; i < 3; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        code = code * 10 + (text[i] - '0');
    }
    return code;
}

#ifdef _WIN32

static int cover_tool_path(wchar_t* out, size_t cap) {
    static const wchar_t leaf[] = L"\\curl.exe";
    const UINT n = GetSystemDirectoryW(out, (UINT)cap);
    DWORD attrs;

    if (n == 0 || (size_t)n + sizeof(leaf) / sizeof(leaf[0]) > cap) return 0;
    memcpy(out + n, leaf, sizeof(leaf));
    attrs = GetFileAttributesW(out);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Adds one argument to a command line in the form that the child's start-up
 * code reads back: in quotes, a quote escaped, and the backslashes in front
 * of a quote doubled. */
static int cover_cmd_arg(wchar_t* cmd, size_t cap, size_t* len, const wchar_t* arg) {
    size_t n = *len;
    size_t slashes = 0;
    size_t i;

    if (n + wcslen(arg) * 2 + 4 > cap) return 0;
    if (n) cmd[n++] = L' ';
    cmd[n++] = L'"';
    for (; *arg; ++arg) {
        if (*arg == L'\\') {
            ++slashes;
            cmd[n++] = L'\\';
            continue;
        }
        if (*arg == L'"') {
            for (i = 0; i < slashes + 1; ++i) cmd[n++] = L'\\';
        }
        slashes = 0;
        cmd[n++] = *arg;
    }
    for (i = 0; i < slashes; ++i) cmd[n++] = L'\\';
    cmd[n++] = L'"';
    cmd[n] = L'\0';
    *len = n;
    return 1;
}

/* Starts `tool` hidden with the fixed argument list and waits for it.
 * Returns 1 when it ran and ended with exit code 0. */
static int cover_run_tool(const wchar_t* tool, const char* url, const char* dest,
                          const char* code_path) {
    static const wchar_t* const fixed[] = {L"-s", L"-S", L"-L", L"--max-time", L"20", L"--proto",
                                           L"=https", L"--proto-redir", L"=https", L"-o"};
    wchar_t cmd[4096];
    wchar_t wdest[COVER_PATH_CAP + 8];
    wchar_t wurl[COVER_URL_CAP];
    size_t len = 0;
    size_t i;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE out_file, nul;
    DWORD exit_code = 1;
    int ok = 0;

    /* `dest` is read the way fopen reads it, so both name the same file. */
    if (!MultiByteToWideChar(AreFileApisANSI() ? CP_ACP : CP_OEMCP, 0, dest, -1, wdest,
                             (int)(sizeof(wdest) / sizeof(wdest[0]))))
        return 0;
    for (i = 0; url[i] && i + 1 < COVER_URL_CAP; ++i) wurl[i] = (wchar_t)(unsigned char)url[i];
    wurl[i] = L'\0';

    cmd[0] = L'\0';
    if (!cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, tool)) return 0;
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (!cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, fixed[i])) return 0;
    }
    if (!cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, wdest) ||
        !cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, L"-w") ||
        !cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, L"%{http_code}") ||
        !cover_cmd_arg(cmd, sizeof(cmd) / sizeof(cmd[0]), &len, wurl))
        return 0;

    /* The child gets three handles: nothing to read, the code file for its
     * standard output, and nowhere for its messages. */
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    out_file = CreateFileA(code_path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out_file == INVALID_HANDLE_VALUE) return 0;
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(out_file);
        return 0;
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out_file;
    si.hStdError = nul;
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(tool, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, COVER_WAIT_MS) == WAIT_OBJECT_0) {
            ok = GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0;
        } else {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(nul);
    CloseHandle(out_file);
    return ok;
}

#else /* not Windows */

static int cover_is_program(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static int cover_tool_path(char* out, size_t cap) {
#if defined(__APPLE__)
    static const char fixed[] = "/usr/bin/curl";

    if (cap < sizeof(fixed) || !cover_is_program(fixed)) return 0;
    memcpy(out, fixed, sizeof(fixed));
    return 1;
#else
    static const char leaf[] = "/curl";
    const char* p = getenv("PATH");

    while (p && *p) {
        const char* end = strchr(p, ':');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        /* Only an absolute folder. An empty or a relative entry means the
         * current folder, and no curl is started from there. */
        if (len > 0 && p[0] == '/' && len + sizeof(leaf) <= cap) {
            memcpy(out, p, len);
            memcpy(out + len, leaf, sizeof(leaf));
            if (cover_is_program(out)) return 1;
        }
        p = end ? end + 1 : NULL;
    }
    return 0;
#endif
}

/* The system's curl is a host program. An AppImage start script puts the
 * bundle's libraries first in LD_LIBRARY_PATH, and curl then loads ours and
 * not the ones it was built against. launcher_files.c has the same rule for
 * the file pickers. So the child gets our environment without those entries,
 * and with the host value when the start script kept one. One block: free()
 * the result. NULL when memory is short or on a system with no such rule; the
 * caller then passes environ. */
static char** cover_host_env(void) {
#if defined(__linux__)
    const char* host = getenv("RECOMP_HOST_LD_LIBRARY_PATH");
    const size_t line = (host && host[0]) ? sizeof("LD_LIBRARY_PATH=") + strlen(host) : 0;
    size_t n = 0, k = 0;
    char** e;
    char** out;

    for (e = environ; e && *e; ++e) ++n;
    out = (char**)malloc((n + 2) * sizeof(char*) + line);
    if (!out) return NULL;
    for (e = environ; e && *e; ++e) {
        if (!strncmp(*e, "LD_LIBRARY_PATH=", 16) || !strncmp(*e, "LD_PRELOAD=", 11) ||
            !strncmp(*e, "LD_AUDIT=", 9))
            continue;
        out[k++] = *e;
    }
    if (line) {
        char* text = (char*)(out + n + 2);
        snprintf(text, line, "LD_LIBRARY_PATH=%s", host);
        out[k++] = text;
    }
    out[k] = NULL;
    return out;
#else
    return NULL;
#endif
}

/* Starts `tool` with the fixed argument list and waits for it. Returns 1 when
 * it ran and ended with exit code 0. */
static int cover_run_tool(const char* tool, const char* url, const char* dest,
                          const char* code_path) {
    char* argv[16];
    posix_spawn_file_actions_t actions;
    char** env;
    pid_t pid = 0;
    int status = 0;
    int waited = 0;
    int rc;
    int i = 0;

    argv[i++] = (char*)"curl";
    argv[i++] = (char*)"-s";
    argv[i++] = (char*)"-S";
    argv[i++] = (char*)"-L";
    argv[i++] = (char*)"--max-time";
    argv[i++] = (char*)"20";
    argv[i++] = (char*)"--proto";
    argv[i++] = (char*)"=https";
    argv[i++] = (char*)"--proto-redir";
    argv[i++] = (char*)"=https";
    argv[i++] = (char*)"-o";
    argv[i++] = (char*)dest;
    argv[i++] = (char*)"-w";
    argv[i++] = (char*)"%{http_code}";
    argv[i++] = (char*)url;
    argv[i] = NULL;

    /* The child gets three files: nothing to read, the code file for its
     * standard output, and nowhere for its messages. */
    if (posix_spawn_file_actions_init(&actions) != 0) return 0;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, code_path,
                                         O_WRONLY | O_CREAT | O_TRUNC, 0600) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return 0;
    }
    env = cover_host_env();
    rc = posix_spawn(&pid, tool, &actions, NULL, argv, env ? env : environ);
    posix_spawn_file_actions_destroy(&actions);
    free(env);
    if (rc != 0) return 0;

    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        struct timespec nap;

        if (r == pid) break;
        if (r < 0 && errno != EINTR) return 0;
        if (waited >= COVER_WAIT_MS) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return 0;
        }
        nap.tv_sec = 0;
        nap.tv_nsec = 50L * 1000L * 1000L;
        nanosleep(&nap, NULL);
        waited += 50;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

/* One request with the given tool. The tool is a parameter so that this part
 * has no fixed path in it. */
static int cover_fetch_with(const cover_tool_char* tool, const char* url, const char* dest) {
    char code_path[COVER_PATH_CAP + 8];
    int ran, code;

    if (!cover_url_is_plain(url) || !dest || !dest[0] || strlen(dest) >= COVER_PATH_CAP) return 2;
    snprintf(code_path, sizeof(code_path), "%s.code", dest);
    /* curl writes no file for an empty answer. An old file must not pass for
     * a new one. */
    remove(dest);
    ran = cover_run_tool(tool, url, dest, code_path);
    code = cover_read_code(code_path);
    remove(code_path);
    if (ran && code == 200) return 0;
    remove(dest);
    return (ran && code == 404) ? 1 : 2;
}

int launcher_cover_fetch_curl(void* ctx, const char* url, const char* dest) {
    cover_tool_char tool[COVER_TOOL_CAP];

    (void)ctx;
    if (!cover_tool_path(tool, COVER_TOOL_CAP)) return 3;
    return cover_fetch_with(tool, url, dest);
}

int launcher_cover_tool_available(void) {
    cover_tool_char tool[COVER_TOOL_CAP];
    return cover_tool_path(tool, COVER_TOOL_CAP);
}

/* ---- one whole fetch ---- */

/* Puts the checked download in the place of the cover. A plain rename
 * replaces the old file where the system allows that. Windows does not, so
 * the old file goes first there. */
static int cover_replace(const char* part, const char* dest) {
    if (rename(part, dest) == 0) return 1;
    remove(dest);
    return rename(part, dest) == 0;
}

int launcher_cover_fetch_sync(const char* const* serials, int n, const char* dest,
                              LauncherCoverFetchFn fetch, void* ctx, char* used) {
    char part[COVER_PATH_CAP];
    char url[COVER_URL_CAP];
    char serial[16];
    int asked = 0, bad_image = 0, failed = 0, no_tool = 0;
    int i, j;

    if (used) used[0] = '\0';
    if (!dest || !dest[0] || strlen(dest) > COVER_DEST_MAX) return LNG_COVER_WRITE_FAILED;
    if (!fetch) fetch = launcher_cover_fetch_curl;
    snprintf(part, sizeof(part), "%s.part", dest);

    for (i = 0; serials && i < n && !no_tool; ++i) {
        int seen = 0;
        int r;

        if (!serials[i] || !launcher_cover_serial(serials[i], serial)) continue;
        for (j = 0; j < i && !seen; ++j) {
            char earlier[16];
            if (serials[j] && launcher_cover_serial(serials[j], earlier) && !strcmp(earlier, serial))
                seen = 1;
        }
        if (seen || !launcher_cover_url(serial, url, sizeof(url))) continue;
        ++asked;
        remove(part);
        r = fetch(ctx, url, part);
        if (r == 0) {
            if (launcher_cover_check_file(part, NULL, NULL)) {
                if (!cover_replace(part, dest)) {
                    remove(part);
                    return LNG_COVER_WRITE_FAILED;
                }
                if (used) memcpy(used, serial, 11);
                return LNG_COVER_SAVED;
            }
            bad_image = 1;
        } else if (r == 3) {
            no_tool = 1; /* the next serial would get the same answer */
        } else if (r != 1) {
            failed = 1;
        }
        remove(part);
    }
    if (!asked) return LNG_COVER_NO_SERIAL;
    if (bad_image) return LNG_COVER_BAD_IMAGE;
    if (failed) return LNG_COVER_FAILED;
    if (no_tool) return LNG_COVER_NO_TOOL;
    return LNG_COVER_NOT_FOUND;
}

/* ---- the same on a worker thread (one job at a time) ---- */

typedef struct {
    char serials[COVER_JOB_SERIALS][16];
    int n;
    char dest[COVER_DEST_MAX + 1];
    LauncherCoverFetchFn fetch;
    void* ctx;
    char used[16];
    int result;
    int state;    /* 0 no job yet, 1 a job runs, 2 the job has ended */
    int finished; /* set when a job ends, cleared by take_finished */
} CoverJob;

static CoverJob g_cover_job;

/* The lock guards state, result, used and finished. The worker reads the
 * arguments without it: they are written before the thread starts and stay
 * as they are while state is 1. The first lock call comes from the UI thread,
 * before any worker exists. */
#ifdef _WIN32
static CRITICAL_SECTION g_cover_lock;
static int g_cover_lock_ready = 0;
static void cover_lock(void) {
    if (!g_cover_lock_ready) {
        InitializeCriticalSection(&g_cover_lock);
        g_cover_lock_ready = 1;
    }
    EnterCriticalSection(&g_cover_lock);
}
static void cover_unlock(void) { LeaveCriticalSection(&g_cover_lock); }
#else
static pthread_mutex_t g_cover_lock = PTHREAD_MUTEX_INITIALIZER;
static void cover_lock(void) { pthread_mutex_lock(&g_cover_lock); }
static void cover_unlock(void) { pthread_mutex_unlock(&g_cover_lock); }
#endif

#ifdef _WIN32
static DWORD WINAPI cover_thread_main(LPVOID arg) {
#else
static void* cover_thread_main(void* arg) {
#endif
    CoverJob* j = (CoverJob*)arg;
    const char* list[COVER_JOB_SERIALS];
    char used[16];
    int result;
    int i;

    memset(used, 0, sizeof(used));
    for (i = 0; i < j->n; ++i) list[i] = j->serials[i];
    result = launcher_cover_fetch_sync(list, j->n, j->dest, j->fetch, j->ctx, used);
    cover_lock();
    j->result = result;
    memcpy(j->used, used, sizeof(j->used));
    j->finished = 1;
    j->state = 2;
    cover_unlock();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
#ifdef _WIN32
static HANDLE g_cover_thread;
#else
static pthread_t g_cover_thread;
static int g_cover_thread_joinable;
#endif
void launcher_cover_job_wait(void) {
#ifdef _WIN32
    if (g_cover_thread) {
        const DWORD wait_status = WaitForSingleObject(g_cover_thread, INFINITE);
        if (wait_status != WAIT_OBJECT_0)
            launcher_v1_worker_join_failed("cover", (uint32_t)wait_status);
        CloseHandle(g_cover_thread);
        g_cover_thread = NULL;
    }
#else
    if (g_cover_thread_joinable) {
        const int wait_status = pthread_join(g_cover_thread, NULL);
        if (wait_status)
            launcher_v1_worker_join_failed("cover", (uint32_t)wait_status);
        g_cover_thread_joinable = 0;
    }
#endif
}
void launcher_cover_job_finish(void) {
    launcher_cover_job_wait();
    memset(&g_cover_job, 0, sizeof(g_cover_job));
#ifdef _WIN32
    if (g_cover_lock_ready) {
        DeleteCriticalSection(&g_cover_lock);
        g_cover_lock_ready = 0;
    }
#endif
}
#endif

static int cover_spawn_thread(void) {
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
    launcher_cover_job_wait();
#endif
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, cover_thread_main, &g_cover_job, 0, NULL);
    if (!th) return 0;
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
    g_cover_thread = th;
#else
    CloseHandle(th);
#endif
#else
    pthread_t th;
    if (pthread_create(&th, NULL, cover_thread_main, &g_cover_job) != 0) return 0;
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
    g_cover_thread = th;
    g_cover_thread_joinable = 1;
#else
    pthread_detach(th);
#endif
#endif
    return 1;
}

int launcher_cover_job_start(const char* const* serials, int n, const char* dest,
                             LauncherCoverFetchFn fetch, void* ctx) {
    CoverJob* j = &g_cover_job;
    char serial[16];
    int i, k;

    if (!dest || !dest[0] || strlen(dest) > COVER_DEST_MAX) return 0;
    cover_lock();
    if (j->state == 1) {
        cover_unlock();
        return 0;
    }
    j->n = 0;
    for (i = 0; serials && i < n && j->n < COVER_JOB_SERIALS; ++i) {
        int seen = 0;
        if (!serials[i] || !launcher_cover_serial(serials[i], serial)) continue;
        for (k = 0; k < j->n; ++k) {
            if (!strcmp(j->serials[k], serial)) seen = 1;
        }
        if (!seen) memcpy(j->serials[j->n++], serial, sizeof(serial));
    }
    memcpy(j->dest, dest, strlen(dest) + 1);
    j->fetch = fetch ? fetch : launcher_cover_fetch_curl;
    j->ctx = ctx;
    j->used[0] = '\0';
    j->result = LNG_COVER_FAILED;
    j->finished = 0;
    j->state = 1;
    cover_unlock();

    if (!cover_spawn_thread()) {
        cover_lock();
        j->state = 0;
        cover_unlock();
        return 0;
    }
    return 1;
}

int launcher_cover_job_poll(char* used) {
    const CoverJob* j = &g_cover_job;
    int state = LNG_COVER_IDLE;

    if (used) used[0] = '\0';
    cover_lock();
    if (j->state == 1) {
        state = LNG_COVER_BUSY;
    } else if (j->state == 2) {
        state = j->result;
        if (used) memcpy(used, j->used, sizeof(j->used));
    }
    cover_unlock();
    return state;
}

int launcher_cover_job_take_finished(void) {
    int finished;

    cover_lock();
    finished = g_cover_job.finished;
    g_cover_job.finished = 0;
    cover_unlock();
    return finished;
}

const char* launcher_cover_state_text(int state) {
    switch (state) {
    case LNG_COVER_BUSY:         return "Fetching the cover...";
    case LNG_COVER_SAVED:        return "Cover saved.";
    case LNG_COVER_NOT_FOUND:    return "No cover for this disc at the source.";
    case LNG_COVER_FAILED:       return "The cover could not be fetched (no network?).";
    case LNG_COVER_BAD_IMAGE:    return "The file that came back is not a picture.";
    case LNG_COVER_NO_SERIAL:    return "This disc has no serial to look up.";
    case LNG_COVER_NO_TOOL:      return "This system has no curl to fetch the cover with.";
    case LNG_COVER_WRITE_FAILED: return "The cover could not be saved in the game folder.";
    default:                     return "";
    }
}
