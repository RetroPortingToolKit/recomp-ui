// launcher_files.c — native file/folder pickers.
//
// Windows / macOS: tinyfiledialogs (GetOpenFileName / osascript).
// Linux: posix_spawn of zenity or kdialog (no shell, no popen/vfork).
//   tinyfd's Linux path uses popen/vfork from the UI thread while SDL has
//   already spawned audio/GL threads — that SIGSEGVs in libc on modern
//   Arch/CachyOS/Fedora (Browse BIOS / Change ROM). posix_spawn is safe
//   from a multithreaded process; we also skip zenity --attach=$(xprop…).

#include "launcher_files.h"

#include "third_party/tinyfiledialogs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* True if path's filename matches any "*.ext" / "*.*" pattern (case-insensitive).
 * Used after native pick so typed paths / residual All-Files bypasses cannot
 * accept a PSX track .bin when the picker asked for *.cue only. */
static int launcher_path_matches_patterns(const char* path,
                                          const char* const* patterns,
                                          int num_patterns) {
    if (!path || !path[0]) return 0;
    if (!patterns || num_patterns <= 0) return 1;
    const char* slash = strrchr(path, '/');
#if defined(_WIN32)
    {
        const char* bslash = strrchr(path, '\\');
        if (!slash || (bslash && bslash > slash)) slash = bslash;
    }
#endif
    const char* name = slash ? slash + 1 : path;
    const char* dot = strrchr(name, '.');
    for (int i = 0; i < num_patterns; i++) {
        const char* pat = patterns[i];
        if (!pat || !pat[0]) continue;
        if (strcmp(pat, "*") == 0 || strcmp(pat, "*.*") == 0) return 1;
        if (pat[0] == '*' && pat[1] == '.' && pat[2] != '\0') {
            if (!dot) continue;
            const char* want = pat + 1; /* ".cue" */
            const char* have = dot;
            int ok = 1;
            while (*want && *have) {
                if (tolower((unsigned char)*want) != tolower((unsigned char)*have)) {
                    ok = 0;
                    break;
                }
                want++;
                have++;
            }
            if (ok && !*want && !*have) return 1;
        }
    }
    return 0;
}

static int launcher_reject_if_pattern_mismatch(const char* title,
                                               const char* const* patterns,
                                               int num_patterns,
                                               char* out_path) {
    if (num_patterns <= 0 || !patterns) return 0;
    if (launcher_path_matches_patterns(out_path, patterns, num_patterns))
        return 0;
    tinyfd_messageBox(
        title && title[0] ? title : "Select file",
        "That file type is not allowed for this picker.\n"
        "Please choose a file matching the filter (for PlayStation discs, "
        "select the .cue sheet).",
        "ok", "warning", 1);
    out_path[0] = '\0';
    return 1;
}

#if defined(__linux__)
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

/* --- environment handed to the spawned picker -----------------------------
 * zenity and kdialog are HOST programs: they link the host's GTK/Qt stack.
 * An AppImage AppRun prepends the bundle's usr/lib to LD_LIBRARY_PATH, and a
 * child spawned with our own environ inherits it — so the host zenity loads
 * OUR glib/pcre2/freetype/png, which do not match the host GTK it is linked
 * against, and it dies before it can draw anything. linuxdeploy's plugin-path
 * variables (GTK_PATH, GDK_PIXBUF_MODULE_FILE, QT_PLUGIN_PATH, ...) break the
 * same way, and LD_PRELOAD/LD_AUDIT are never ours to pass on.
 *
 * So: copy our environment MINUS those, and restore the pre-AppRun value of
 * LD_LIBRARY_PATH when the launcher script saved one for us in
 * RECOMP_HOST_LD_LIBRARY_PATH. A host tool gets a host environment. */
static const char* const kLinuxSpawnDropPrefixes[] = {
    "LD_LIBRARY_PATH=",
    "LD_PRELOAD=",
    "LD_AUDIT=",
    "GTK_PATH=",
    "GTK_EXE_PREFIX=",
    "GTK_DATA_PREFIX=",
    "GTK_IM_MODULE_FILE=",
    "GDK_PIXBUF_MODULE_FILE=",
    "GDK_PIXBUF_MODULEDIR=",
    "GIO_MODULE_DIR=",
    "GSETTINGS_SCHEMA_DIR=",
    "GST_PLUGIN_SYSTEM_PATH=",
    "GST_PLUGIN_SYSTEM_PATH_1_0=",
    "GST_PLUGIN_PATH=",
    "QT_PLUGIN_PATH=",
    "QML2_IMPORT_PATH=",
    "QML_IMPORT_PATH=",
    "FONTCONFIG_FILE=",
    "FONTCONFIG_PATH=",
    "PYTHONHOME=",
    "PYTHONPATH=",
    "PERLLIB=",
    "PERL5LIB=",
    /* injected by the AppImage runtime / our AppRun */
    "APPDIR=",
    "APPIMAGE=",
    "ARGV0=",
    "OWD=",
    "RECOMP_HOST_LD_LIBRARY_PATH=",
};

#define LINUX_HOST_LDPATH_VAR "RECOMP_HOST_LD_LIBRARY_PATH"

static int linux_env_should_drop(const char* entry) {
    if (!entry) return 1;
    const size_t n = sizeof(kLinuxSpawnDropPrefixes) /
                     sizeof(kLinuxSpawnDropPrefixes[0]);
    for (size_t i = 0; i < n; i++) {
        const char* pre = kLinuxSpawnDropPrefixes[i];
        if (strncmp(entry, pre, strlen(pre)) == 0) return 1;
    }
    return 0;
}

static void linux_free_spawn_env(char** env) {
    if (!env) return;
    for (char** e = env; *e; e++) free(*e);
    free(env);
}

/* NULL on allocation failure — callers then fall back to `environ`, which is
 * still better than not opening a dialog at all. */
static char** linux_build_spawn_env(void) {
    size_t n = 0;
    for (char** e = environ; e && *e; e++) n++;

    char** out = (char**)calloc(n + 2, sizeof(char*));
    if (!out) return NULL;

    size_t k = 0;
    for (char** e = environ; e && *e; e++) {
        if (linux_env_should_drop(*e)) continue;
        out[k] = strdup(*e);
        if (!out[k]) {
            linux_free_spawn_env(out);
            return NULL;
        }
        k++;
    }

    const char* host = getenv(LINUX_HOST_LDPATH_VAR);
    if (host && host[0]) {
        const size_t len = strlen("LD_LIBRARY_PATH=") + strlen(host) + 1;
        char* v = (char*)malloc(len);
        if (!v) {
            linux_free_spawn_env(out);
            return NULL;
        }
        snprintf(v, len, "LD_LIBRARY_PATH=%s", host);
        out[k++] = v;
    }

    out[k] = NULL;
    return out;
}

/* One line, on stderr, whenever a backend is declared unusable — so a bug
 * report carries the reason instead of "the button does nothing". */
static void linux_picker_unusable(const char* backend, const char* why,
                                  const char* detail) {
    fprintf(stderr,
            "[launcher] file picker: %s %s; falling back to the built-in "
            "browser%s%s\n",
            backend ? backend : "(picker)", why ? why : "is unusable",
            (detail && detail[0]) ? " — " : "",
            (detail && detail[0]) ? detail : "");
    fflush(stderr);
}

static int linux_str_has_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return 0;
    for (const char* p = hay; *p; p++) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b) {
            const int ca = tolower((unsigned char)*a);
            const int cb = tolower((unsigned char)*b);
            if (ca != cb) break;
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static int linux_desktop_prefers_kdialog(void) {
    const char* d = getenv("XDG_SESSION_DESKTOP");
    if (!d || !d[0]) d = getenv("XDG_CURRENT_DESKTOP");
    if (!d) return 0;
    /* Plasma: XDG_SESSION_DESKTOP=KDE|plasma, XDG_CURRENT_DESKTOP=KDE:…. */
    if (linux_str_has_ci(d, "KDE") || linux_str_has_ci(d, "plasma") ||
        linux_str_has_ci(d, "LXQt"))
        return 1;
    return 0;
}

static int linux_have_cmd(const char* name) {
    const char* path = getenv("PATH");
    if (!path || !name || !name[0]) return 0;
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* tok = strtok(buf, ":"); tok; tok = strtok(NULL, ":")) {
        char cand[512];
        int n = snprintf(cand, sizeof(cand), "%s/%s", tok, name);
        if (n <= 0 || (size_t)n >= sizeof(cand)) continue;
        if (access(cand, X_OK) == 0) return 1;
    }
    return 0;
}

static void linux_trim_trailing_ws(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* Drain both pipes until EOF. Polling rather than reading stdout to EOF first
 * matters: a backend that writes a long diagnostic to stderr would otherwise
 * block on a full stderr pipe while we waited for a stdout line that never
 * comes. */
static void linux_drain_pipes(int out_fd, int err_fd,
                              char* sout, size_t sout_cap, size_t* sout_n,
                              char* serr, size_t serr_cap, size_t* serr_n) {
    struct pollfd pfd[2];
    pfd[0].fd = out_fd;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = err_fd;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;

    int open_fds = 2;
    while (open_fds > 0) {
        const int pr = poll(pfd, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (pfd[i].fd < 0) continue;
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[1024];
            const ssize_t r = read(pfd[i].fd, buf, sizeof(buf));
            if (r > 0) {
                char* dst = (i == 0) ? sout : serr;
                const size_t cap = (i == 0) ? sout_cap : serr_cap;
                size_t* used = (i == 0) ? sout_n : serr_n;
                /* Keep the FIRST bytes: the opening line of a loader failure
                 * ("zenity: symbol lookup error: ...") is the diagnostic. */
                size_t room = (cap ? cap - 1 : 0) - *used;
                if (room > (size_t)r) room = (size_t)r;
                if (room) {
                    memcpy(dst + *used, buf, room);
                    *used += room;
                    dst[*used] = '\0';
                }
            } else if (r == 0) {
                pfd[i].fd = -1;
                open_fds--;
            } else if (errno != EINTR && errno != EAGAIN) {
                pfd[i].fd = -1;
                open_fds--;
            }
        }
    }
}

/* Spawn argv[0] via posix_spawnp with a sanitised (host) environment and
 * capture its first stdout line.
 *
 * Returns:
 *    1  path selected (out filled)
 *    0  the dialog really ran and the user cancelled — stop, do NOT try
 *       another backend, because the user already said no
 *   -1  the backend is UNUSABLE: exec failed, it died on a signal, it exited
 *       with anything other than 0 or 1, or it "succeeded" while printing no
 *       path. Only -1 falls through to another backend / the built-in browser.
 *
 * Exit 1 is the documented cancel code for both zenity and kdialog, and it is
 * the ONLY non-zero code that means cancel. Treating every non-zero exit as a
 * cancel (as this did before) turns a backend that cannot start at all — e.g.
 * a host zenity poisoned by an AppImage LD_LIBRARY_PATH, which exits 127 — into
 * a silent no-op: the click appears to do nothing and the built-in browser is
 * never reached. */
static int linux_spawn_capture(char* const argv[], char* out, size_t out_cap) {
    const char* backend = (argv && argv[0]) ? argv[0] : "(picker)";
    if (out && out_cap) out[0] = '\0';

    int op[2] = {-1, -1};
    int ep[2] = {-1, -1};
    if (pipe(op) != 0) {
        linux_picker_unusable(backend, "could not be piped", strerror(errno));
        return -1;
    }
    if (pipe(ep) != 0) {
        const int e = errno;
        close(op[0]);
        close(op[1]);
        linux_picker_unusable(backend, "could not be piped", strerror(e));
        return -1;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(op[0]);
        close(op[1]);
        close(ep[0]);
        close(ep[1]);
        linux_picker_unusable(backend, "could not be prepared", NULL);
        return -1;
    }
    posix_spawn_file_actions_adddup2(&actions, op[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, ep[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, op[0]);
    posix_spawn_file_actions_addclose(&actions, op[1]);
    posix_spawn_file_actions_addclose(&actions, ep[0]);
    posix_spawn_file_actions_addclose(&actions, ep[1]);

    char** spawn_env = linux_build_spawn_env();
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv,
                                spawn_env ? spawn_env : environ);
    posix_spawn_file_actions_destroy(&actions);
    close(op[1]);
    close(ep[1]);

    if (rc != 0) {
        close(op[0]);
        close(ep[0]);
        linux_free_spawn_env(spawn_env);
        linux_picker_unusable(backend, "could not be started", strerror(rc));
        return -1;
    }

    char sout[4096];
    char serr[512];
    size_t sout_n = 0;
    size_t serr_n = 0;
    sout[0] = '\0';
    serr[0] = '\0';
    linux_drain_pipes(op[0], ep[0], sout, sizeof(sout), &sout_n, serr,
                      sizeof(serr), &serr_n);
    close(op[0]);
    close(ep[0]);
    linux_free_spawn_env(spawn_env);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            linux_picker_unusable(backend, "could not be waited on",
                                  strerror(errno));
            return -1;
        }
    }

    linux_trim_trailing_ws(serr);
    const char* detail = serr[0] ? serr : NULL;

    if (!WIFEXITED(status)) {
        char why[64];
        snprintf(why, sizeof(why), "was killed by signal %d",
                 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        linux_picker_unusable(backend, why, detail);
        return -1;
    }

    const int code = WEXITSTATUS(status);
    if (code == 0) {
        char* nl = strpbrk(sout, "\r\n");
        if (nl) *nl = '\0';
        if (!sout[0]) {
            linux_picker_unusable(backend, "exited 0 without printing a path",
                                  detail);
            return -1;
        }
        if (out && out_cap) snprintf(out, out_cap, "%s", sout);
        return 1;
    }
    if (code == 1) return 0; /* documented cancel for zenity and kdialog */

    {
        char why[64];
        snprintf(why, sizeof(why), "exited %d", code);
        linux_picker_unusable(backend, why, detail);
    }
    return -1;
}

static void linux_append_patterns(char* dst, size_t dst_cap,
                                  const char* const* patterns, int num_patterns) {
    dst[0] = '\0';
    if (!patterns || num_patterns <= 0) {
        snprintf(dst, dst_cap, "*");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < num_patterns; i++) {
        if (!patterns[i] || !patterns[i][0]) continue;
        const int n = snprintf(dst + used, dst_cap - used, "%s%s",
                               used ? " " : "", patterns[i]);
        if (n < 0 || (size_t)n >= dst_cap - used) break;
        used += (size_t)n;
    }
    if (!dst[0]) snprintf(dst, dst_cap, "*");
}

static int linux_pick_open_zenity(const char* title, const char* const* patterns,
                                  int num_patterns, const char* desc,
                                  char* out, size_t out_cap) {
    char title_arg[640];
    char filter_arg[768];
    char pats[256];
    linux_append_patterns(pats, sizeof(pats), patterns, num_patterns);
    snprintf(title_arg, sizeof(title_arg), "--title=%s",
             title && title[0] ? title : "Select file");
    if (desc && desc[0])
        snprintf(filter_arg, sizeof(filter_arg), "--file-filter=%s | %s", desc, pats);
    else
        snprintf(filter_arg, sizeof(filter_arg), "--file-filter=%s", pats);

    /* When the caller supplies patterns (e.g. PSX *.cue only), do not offer
     * "All files" — that undoes the filter in the native dialog. */
    if (patterns && num_patterns > 0) {
        char* argv[] = {
            "zenity",
            "--file-selection",
            title_arg,
            filter_arg,
            NULL
        };
        return linux_spawn_capture(argv, out, out_cap);
    }
    char* argv[] = {
        "zenity",
        "--file-selection",
        title_arg,
        filter_arg,
        "--file-filter=All files | *",
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

static int linux_pick_open_kdialog(const char* title, const char* const* patterns,
                                   int num_patterns, const char* desc,
                                   char* out, size_t out_cap) {
    char filter[768];
    char pats[256];
    char title_buf[512];
    linux_append_patterns(pats, sizeof(pats), patterns, num_patterns);
    if (desc && desc[0])
        snprintf(filter, sizeof(filter), "%s | %s", pats, desc);
    else
        snprintf(filter, sizeof(filter), "%s", pats);
    snprintf(title_buf, sizeof(title_buf), "%s",
             title && title[0] ? title : "Select file");

    char* argv[] = {
        "kdialog",
        "--getopenfilename",
        ".",
        filter,
        "--title",
        title_buf,
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

static int linux_pick_save_zenity(const char* title, const char* const* patterns,
                                  int num_patterns, const char* desc,
                                  char* out, size_t out_cap) {
    char title_arg[640];
    char filter_arg[768];
    char pats[256];
    linux_append_patterns(pats, sizeof(pats), patterns, num_patterns);
    snprintf(title_arg, sizeof(title_arg), "--title=%s",
             title && title[0] ? title : "Save file");
    if (desc && desc[0])
        snprintf(filter_arg, sizeof(filter_arg), "--file-filter=%s | %s", desc, pats);
    else
        snprintf(filter_arg, sizeof(filter_arg), "--file-filter=%s", pats);

    if (patterns && num_patterns > 0) {
        char* argv[] = {
            "zenity",
            "--file-selection",
            "--save",
            "--confirm-overwrite",
            title_arg,
            filter_arg,
            NULL
        };
        return linux_spawn_capture(argv, out, out_cap);
    }
    char* argv[] = {
        "zenity",
        "--file-selection",
        "--save",
        "--confirm-overwrite",
        title_arg,
        filter_arg,
        "--file-filter=All files | *",
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

static int linux_pick_save_kdialog(const char* title, const char* const* patterns,
                                   int num_patterns, const char* desc,
                                   char* out, size_t out_cap) {
    char filter[768];
    char pats[256];
    char title_buf[512];
    linux_append_patterns(pats, sizeof(pats), patterns, num_patterns);
    if (desc && desc[0])
        snprintf(filter, sizeof(filter), "%s | %s", pats, desc);
    else
        snprintf(filter, sizeof(filter), "%s", pats);
    snprintf(title_buf, sizeof(title_buf), "%s",
             title && title[0] ? title : "Save file");

    char* argv[] = {
        "kdialog",
        "--getsavefilename",
        ".",
        filter,
        "--title",
        title_buf,
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

static int linux_pick_folder_zenity(const char* title, char* out, size_t out_cap) {
    char title_arg[640];
    snprintf(title_arg, sizeof(title_arg), "--title=%s",
             title && title[0] ? title : "Select folder");
    char* argv[] = {
        "zenity",
        "--file-selection",
        "--directory",
        title_arg,
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

static int linux_pick_folder_kdialog(const char* title, char* out, size_t out_cap) {
    char title_buf[512];
    snprintf(title_buf, sizeof(title_buf), "%s",
             title && title[0] ? title : "Select folder");
    char* argv[] = {
        "kdialog",
        "--getexistingdirectory",
        ".",
        "--title",
        title_buf,
        NULL
    };
    return linux_spawn_capture(argv, out, out_cap);
}

/* Returns 1=ok, 0=cancel (dialog ran), -1=no usable backend (try tinyfd). */
static int linux_pick_open(const char* title, const char* const* patterns,
                           int num_patterns, const char* desc,
                           char* out, size_t out_cap) {
    const int want_kd = linux_desktop_prefers_kdialog();
    const int have_kd = linux_have_cmd("kdialog");
    const int have_zn = linux_have_cmd("zenity");
    int r;
    if (want_kd && have_kd) {
        r = linux_pick_open_kdialog(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    if (have_zn) {
        r = linux_pick_open_zenity(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    if (!want_kd && have_kd) {
        r = linux_pick_open_kdialog(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    return -1;
}

static int linux_pick_save(const char* title, const char* const* patterns,
                           int num_patterns, const char* desc,
                           char* out, size_t out_cap) {
    const int want_kd = linux_desktop_prefers_kdialog();
    const int have_kd = linux_have_cmd("kdialog");
    const int have_zn = linux_have_cmd("zenity");
    int r;
    if (want_kd && have_kd) {
        r = linux_pick_save_kdialog(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    if (have_zn) {
        r = linux_pick_save_zenity(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    if (!want_kd && have_kd) {
        r = linux_pick_save_kdialog(title, patterns, num_patterns, desc, out, out_cap);
        if (r >= 0) return r;
    }
    return -1;
}

static int linux_pick_folder(const char* title, char* out, size_t out_cap) {
    const int want_kd = linux_desktop_prefers_kdialog();
    const int have_kd = linux_have_cmd("kdialog");
    const int have_zn = linux_have_cmd("zenity");
    int r;
    if (want_kd && have_kd) {
        r = linux_pick_folder_kdialog(title, out, out_cap);
        if (r >= 0) return r;
    }
    if (have_zn) {
        r = linux_pick_folder_zenity(title, out, out_cap);
        if (r >= 0) return r;
    }
    if (!want_kd && have_kd) {
        r = linux_pick_folder_kdialog(title, out, out_cap);
        if (r >= 0) return r;
    }
    return -1;
}
#endif /* __linux__ */

bool launcher_native_file_picker_available(void) {
#if defined(__linux__)
    return linux_have_cmd("zenity") || linux_have_cmd("kdialog");
#else
    return true;
#endif
}

bool launcher_pick_rom(char* out_path, size_t out_cap) {
    if (!out_path || out_cap == 0) return false;
    out_path[0] = '\0';

#if defined(__linux__)
    {
        const int r = linux_pick_open("Select game file", NULL, 0, NULL,
                                      out_path, out_cap);
        return r == 1;
    }
#else
    // No filter: this fallback runs only when the active console's profile
    // supplied no rom_filter, and it cannot know what that console accepts.
    // Offering "All files" is honest; naming one system's extensions here is
    // how a PSX build ended up asking players for a SNES cartridge.
    const char* sel = tinyfd_openFileDialog(
        "Select game file",
        "",       // default path/file
        0, NULL,  // no patterns -> all files
        NULL,     // no filter description
        0);       // single select
    if (!sel || !sel[0]) return false;

    snprintf(out_path, out_cap, "%s", sel);
    return true;
#endif
}

bool launcher_pick_folder(const char* title, char* out_path, size_t out_cap) {
    if (!out_path || out_cap == 0) return false;
    out_path[0] = '\0';

#if defined(__linux__)
    {
        const int r = linux_pick_folder(title, out_path, out_cap);
        if (r >= 0) return r == 1; /* ok or cancel — never fall through to tinyfd */
        return false;
    }
#endif

    const char* sel = tinyfd_selectFolderDialog(title ? title : "Select folder", "");
    if (!sel || !sel[0]) return false;
    snprintf(out_path, out_cap, "%s", sel);
    return true;
}

int launcher_try_pick_file(const char* title, const char* const* patterns,
                           int num_patterns, const char* desc,
                           char* out_path, size_t out_cap) {
    if (!out_path || out_cap == 0) return -1;
    out_path[0] = '\0';

#if defined(__linux__)
    {
        const int r = linux_pick_open(title, patterns, num_patterns, desc,
                                      out_path, out_cap);
        if (r != 1) return r;
        if (launcher_reject_if_pattern_mismatch(title, patterns, num_patterns,
                                                out_path))
            return 0;
        return 1;
    }
#else
    const char* sel = tinyfd_openFileDialog(
        title ? title : "Select file",
        "",
        num_patterns > 0 ? num_patterns : 0,
        num_patterns > 0 ? patterns : NULL,
        desc,
        0);
    if (!sel || !sel[0]) return 0;
    snprintf(out_path, out_cap, "%s", sel);
    if (launcher_reject_if_pattern_mismatch(title, patterns, num_patterns,
                                            out_path))
        return 0;
    return 1;
#endif
}

bool launcher_pick_file(const char* title, const char* const* patterns, int num_patterns,
                        const char* desc, char* out_path, size_t out_cap) {
    const int r = launcher_try_pick_file(title, patterns, num_patterns, desc,
                                         out_path, out_cap);
    return r == 1;
}

bool launcher_file_picker_selftest(void) {
    const char* req = getenv("RECOMP_UI_PICKER_SELFTEST");
    if (!req || !req[0] || strcmp(req, "0") == 0 || strcmp(req, "false") == 0)
        return false;

    const int available = launcher_native_file_picker_available() ? 1 : 0;
    printf("[picker-selftest] native_available=%d\n", available);

    char path[4096];
    path[0] = '\0';
    int r = -1;
    if (available)
        r = launcher_try_pick_file("Select game file", NULL, 0, NULL, path,
                                   sizeof(path));

    printf("[picker-selftest] result=%d path=[%s]\n", r, path);
    printf("[picker-selftest] builtin_fallback=%s\n", r == -1 ? "yes" : "no");
    fflush(stdout);
    return true;
}

bool launcher_pick_save_file(const char* title, const char* const* patterns, int num_patterns,
                             const char* desc, char* out_path, size_t out_cap) {
    if (!out_path || out_cap == 0) return false;
    out_path[0] = '\0';

#if defined(__linux__)
    {
        const int r = linux_pick_save(title, patterns, num_patterns, desc,
                                      out_path, out_cap);
        if (r >= 0) return r == 1; /* ok or cancel — never fall through to tinyfd */
        return false;
    }
#endif

    const char* sel = tinyfd_saveFileDialog(
        title ? title : "Save file",
        "",
        num_patterns > 0 ? num_patterns : 0,
        num_patterns > 0 ? patterns : NULL,
        desc);
    if (!sel || !sel[0]) return false;
    snprintf(out_path, out_cap, "%s", sel);
    return true;
}
