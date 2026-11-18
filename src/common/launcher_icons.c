/*
 * launcher_icons.c - "Add to Steam" and "create a desktop shortcut".
 *
 * Windows only; every other platform answers "not available here" so the
 * launcher can offer the buttons unconditionally and report the truth.
 *
 * Desktop shortcut: a real .lnk (IShellLinkW) next to the other desktop icons,
 * starting the program with the selected image as its argument.
 *
 * Steam: a non-Steam shortcut is added by appending an entry to the user's
 * `userdata/<account>/config/shortcuts.vdf` (Steam's binary VDF). Steam reads
 * that file at start and rewrites it on exit, so this refuses while steam.exe
 * is running, keeps a backup, and skips a duplicate. Steam must be restarted to
 * show the new entry — the caller says so in its status line.
 */
#include "launcher_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if !defined(_WIN32)

int launcher_desktop_shortcut(const char *exe_path, const char *args,
                              const char *name, char *err, size_t err_cap) {
    (void)exe_path; (void)args; (void)name;
    if (err && err_cap) snprintf(err, err_cap, "Desktop shortcuts are handled on Windows only.");
    return 0;
}

int launcher_steam_shortcut(const char *exe_path, const char *args,
                            const char *name, const char *icon_path,
                            char *err, size_t err_cap) {
    (void)exe_path; (void)args; (void)name; (void)icon_path;
    if (err && err_cap) snprintf(err, err_cap, "Steam shortcuts are handled on Windows only.");
    return 0;
}

#else /* _WIN32 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <tlhelp32.h>

/* ---- small helpers ------------------------------------------------------- */

static void set_err(char *err, size_t cap, const char *text) {
    if (err && cap) snprintf(err, cap, "%s", text ? text : "");
}

static int wide_from_utf8(const char *in, wchar_t *out, size_t out_chars) {
    if (!in) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)out_chars);
    if (n <= 0)
        n = MultiByteToWideChar(CP_ACP, 0, in, -1, out, (int)out_chars);
    return n > 0;
}

static int file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int steam_is_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    int running = 0;
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, L"steam.exe") == 0) { running = 1; break; }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return running;
}

/* The program's own path, so the shortcut points at the running executable. */
static int own_exe_path(char *out, size_t cap) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return n > 0 && n < cap;
}

/* ---- desktop shortcut ---------------------------------------------------- */

int launcher_desktop_shortcut(const char *exe_path, const char *args,
                              const char *name, char *err, size_t err_cap) {
    char exe[1024];
    if (!exe_path || !exe_path[0]) {
        if (!own_exe_path(exe, sizeof exe)) { set_err(err, err_cap, "Cannot read the program path."); return 0; }
        exe_path = exe;
    }
    const char *label = (name && name[0]) ? name : "Legend Of Zelda NES";

    /* Desktop folder; RECOMP_DESKTOP_DIR overrides it (tests, portable runs). */
    wchar_t desktop[MAX_PATH];
    desktop[0] = 0;
    const char *override = getenv("RECOMP_DESKTOP_DIR");
    if (override && override[0]) {
        if (!wide_from_utf8(override, desktop, ARRAYSIZE(desktop))) {
            set_err(err, err_cap, "Invalid desktop folder path.");
            return 0;
        }
    } else if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop)) || !desktop[0]) {
        set_err(err, err_cap, "Cannot find the Desktop folder.");
        return 0;
    }

    wchar_t target[MAX_PATH], workdir[MAX_PATH], arguments[1024], title[MAX_PATH], link_path[MAX_PATH];
    if (!wide_from_utf8(exe_path, target, ARRAYSIZE(target))) { set_err(err, err_cap, "Invalid program path."); return 0; }
    if (!wide_from_utf8(exe_path, workdir, ARRAYSIZE(workdir))) { set_err(err, err_cap, "Invalid program path."); return 0; }
    if (args && args[0]) { if (!wide_from_utf8(args, arguments, ARRAYSIZE(arguments))) { set_err(err, err_cap, "Invalid arguments."); return 0; } }
    else arguments[0] = 0;
    if (!wide_from_utf8(label, title, ARRAYSIZE(title))) { set_err(err, err_cap, "Invalid shortcut name."); return 0; }
    /* Trim the working directory to the program's folder. */
    {
        wchar_t *slash = wcsrchr(workdir, L'\\');
        if (slash) slash[1] = 0;
    }
    _snwprintf(link_path, ARRAYSIZE(link_path), L"%s\\%s.lnk", desktop, title);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const int uninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;

    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&link)) && link) {
        link->lpVtbl->SetPath(link, target);
        link->lpVtbl->SetArguments(link, arguments);
        link->lpVtbl->SetWorkingDirectory(link, workdir);
        link->lpVtbl->SetIconLocation(link, target, 0);
        link->lpVtbl->SetDescription(link, L"The Legend of Zelda (NES) recompilation");
        if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file)) && file) {
            ok = SUCCEEDED(file->lpVtbl->Save(file, link_path, TRUE));
            file->lpVtbl->Release(file);
        }
        link->lpVtbl->Release(link);
    }
    if (uninit) CoUninitialize();
    if (!ok) {
        set_err(err, err_cap, "Windows refused to create the shortcut file.");
        return 0;
    }
    return 1;
}

/* ---- Steam ----------------------------------------------------------------- */

/* Binary-VDF writer helpers: Steam's format is a stream of tagged records. */
typedef struct {
    unsigned char *data;
    size_t len, cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    unsigned char *p = (unsigned char *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int buf_bytes(Buf *b, const void *p, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

static int buf_tag(Buf *b, unsigned char tag) { return buf_bytes(b, &tag, 1); }

static int buf_str(Buf *b, const char *s) {
    size_t n = strlen(s);
    return buf_bytes(b, s, n) && buf_tag(b, 0);
}

static int buf_i32(Buf *b, int32_t v) { return buf_bytes(b, &v, sizeof v); }

/* One shortcuts.vdf entry. Steam fills defaults for the fields not written. */
static int append_shortcut(Buf *b, int index, const char *app_name,
                           const char *exe, const char *start_dir,
                           const char *args, const char *icon, uint32_t appid) {
    char key[24];
    snprintf(key, sizeof key, "%d", index);
    if (!buf_tag(b, 0x00) || !buf_str(b, key)) return 0;
    if (!buf_tag(b, 0x02) || !buf_str(b, "appid") || !buf_i32(b, (int32_t)appid)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "AppName") || !buf_str(b, app_name)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "Exe") || !buf_str(b, exe)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "StartDir") || !buf_str(b, start_dir)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "icon") || !buf_str(b, icon)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "LaunchOptions") || !buf_str(b, args)) return 0;
    if (!buf_tag(b, 0x01) || !buf_str(b, "ShortcutPath") || !buf_str(b, "")) return 0;
    return buf_tag(b, 0x08);   /* end of this entry's map */
}

/* Highest numeric entry key in an existing file, or -1 when it has none. */
static int highest_index(const unsigned char *d, size_t n) {
    int best = -1;
    for (size_t i = 0; i + 2 < n; ++i) {
        if (d[i] != 0x00) continue;
        size_t j = i + 1;
        if (j >= n || d[j] < '0' || d[j] > '9') continue;
        long v = 0;
        while (j < n && d[j] >= '0' && d[j] <= '9') { v = v * 10 + (d[j] - '0'); ++j; }
        if (j < n && d[j] == 0x00 && v >= 0 && v < 100000 && (int)v > best) best = (int)v;
    }
    return best;
}

static int entry_exists(const unsigned char *d, size_t n, const char *app_name,
                        const char *exe, const char *args) {
    /* Cheap substring test: one entry carries AppName, Exe and LaunchOptions
     * verbatim, so an entry for this image means all three appear in it. */
    size_t an = strlen(app_name), en = strlen(exe), gn = strlen(args);
    for (size_t i = 0; i + an < n; ++i) {
        if (memcmp(d + i, app_name, an) != 0) continue;
        for (size_t k = 0; k + en < n; ++k) {
            if (memcmp(d + k, exe, en) != 0) continue;
            for (size_t g = 0; g + gn < n; ++g)
                if (memcmp(d + g, args, gn) == 0) return 1;
        }
    }
    return 0;
}

static int read_reg_string(HKEY root, const char *sub, const char *value, char *out, size_t cap) {
    HKEY key;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    DWORD type = 0, size = (DWORD)cap;
    LONG r = RegQueryValueExA(key, value, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    out[cap - 1] = 0;
    return 1;
}

static DWORD read_reg_dword(HKEY root, const char *sub, const char *value, DWORD fallback) {
    HKEY key;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
    DWORD type = 0, data = 0, size = sizeof data;
    LONG r = RegQueryValueExA(key, value, NULL, &type, (LPBYTE)&data, &size);
    RegCloseKey(key);
    return (r == ERROR_SUCCESS && type == REG_DWORD) ? data : fallback;
}

int launcher_steam_shortcut(const char *exe_path, const char *args,
                            const char *name, const char *icon_path,
                            char *err, size_t err_cap) {
    char exe[1024];
    if (!exe_path || !exe_path[0]) {
        if (!own_exe_path(exe, sizeof exe)) { set_err(err, err_cap, "Cannot read the program path."); return 0; }
        exe_path = exe;
    }
    const char *label = (name && name[0]) ? name : "Legend Of Zelda NES";
    const char *icon = (icon_path && icon_path[0]) ? icon_path : exe_path;

    if (steam_is_running()) {
        set_err(err, err_cap, "Close Steam first: it rewrites its shortcut list when it exits.");
        return 0;
    }

    /* Steam root: RECOMP_STEAM_ROOT (tests/portable), else the registry. */
    char steam[1024];
    steam[0] = 0;
    const char *override = getenv("RECOMP_STEAM_ROOT");
    if (override && override[0]) {
        snprintf(steam, sizeof steam, "%s", override);
    } else if (!read_reg_string(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", steam, sizeof steam) ||
               !steam[0]) {
        set_err(err, err_cap, "Steam is not installed for this user.");
        return 0;
    }

    /* The account whose shortcuts Steam will read: the active user id when the
     * registry names one, otherwise the only (or newest) userdata folder. */
    char userdata[1200];
    snprintf(userdata, sizeof userdata, "%s\\userdata", steam);
    DWORD active = read_reg_dword(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser", 0);

    char account[64] = "";
    if (active) snprintf(account, sizeof account, "%lu", (unsigned long)active);
    char config_path[1400];
    snprintf(config_path, sizeof config_path, "%s\\%s\\config\\shortcuts.vdf", userdata, account);
    if (!account[0] || !file_exists(config_path)) {
        /* Fall back to the newest numeric userdata folder. */
        char pattern[1300];
        snprintf(pattern, sizeof pattern, "%s\\*", userdata);
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA(pattern, &fd);
        ULONGLONG newest = 0;
        account[0] = 0;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                int numeric = fd.cFileName[0] != 0;
                for (const char *p = fd.cFileName; *p; ++p) if (*p < '0' || *p > '9') { numeric = 0; break; }
                if (!numeric) continue;
                ULONGLONG stamp = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (stamp >= newest) {
                    newest = stamp;
                    snprintf(account, sizeof account, "%s", fd.cFileName);
                }
            } while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        if (!account[0]) {
            set_err(err, err_cap, "No Steam account folder found; start Steam once, then retry.");
            return 0;
        }
        snprintf(config_path, sizeof config_path, "%s\\%s\\config\\shortcuts.vdf", userdata, account);
    }

    /* Read the current list (missing file = empty library). */
    Buf existing = {0};
    FILE *f = fopen(config_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && buf_reserve(&existing, (size_t)n)) {
            existing.len = fread(existing.data, 1, (size_t)n, f);
        }
        fclose(f);
    }
    int empty = existing.len < 12 || !memchr(existing.data, 0, existing.len);

    /* Quoted forms Steam stores. */
    char exe_q[1200], dir_q[1200];
    snprintf(exe_q, sizeof exe_q, "\"%s\"", exe_path);
    snprintf(dir_q, sizeof dir_q, "\"%.*s\"", (int)(strrchr(exe_path, '\\') ? (size_t)(strrchr(exe_path, '\\') - exe_path) : 0), exe_path);

    if (!empty && entry_exists(existing.data, existing.len, label, exe_q, args ? args : "")) {
        free(existing.data);
        return 1;   /* already in the library */
    }

    Buf out = {0};
    const int index = empty ? 0 : highest_index(existing.data, existing.len) + 1;
    /* Deterministic appid, the convention non-Steam shortcuts use. */
    uint32_t appid = 0xA5A50000u ^ (uint32_t)(strlen(exe) * 2654435761u) ^ (uint32_t)index;
    appid |= 0x80000000u;

    /* Layout: "shortcuts" map header, every existing entry, the new entry, then
     * the two terminators (the shortcuts map's and the root map's). Each entry
     * carries its own 0x08, so exactly two trailing bytes are replaced. */
    size_t keep = 0;
    if (empty) {
        if (!buf_tag(&out, 0x00) || !buf_str(&out, "shortcuts")) { set_err(err, err_cap, "Out of memory."); goto fail; }
    } else {
        if (existing.len < 3 || existing.data[existing.len - 1] != 0x08 || existing.data[existing.len - 2] != 0x08) {
            set_err(err, err_cap, "The Steam shortcut file has an unexpected layout; nothing was changed.");
            goto fail;
        }
        keep = existing.len - 2;
        if (!buf_bytes(&out, existing.data, keep)) { set_err(err, err_cap, "Out of memory."); goto fail; }
    }
    if (!append_shortcut(&out, index, label, exe_q, dir_q, args ? args : "", icon, appid) ||
        !buf_tag(&out, 0x08) || !buf_tag(&out, 0x08)) {
        set_err(err, err_cap, "Out of memory.");
        goto fail;
    }

    /* Backup + write, both renamed into place so a failure cannot truncate. */
    {
        char backup[1500], temp[1500];
        snprintf(backup, sizeof backup, "%s.bak", config_path);
        snprintf(temp, sizeof temp, "%s.new", config_path);
        if (!empty) CopyFileA(config_path, backup, FALSE);
        FILE *w = fopen(temp, "wb");
        if (!w) { set_err(err, err_cap, "Cannot write the Steam shortcut file."); goto fail; }
        size_t wrote = fwrite(out.data, 1, out.len, w);
        fclose(w);
        if (wrote != out.len || !MoveFileExA(temp, config_path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(temp);
            set_err(err, err_cap, "Cannot update the Steam shortcut file.");
            goto fail;
        }
    }
    free(out.data);
    free(existing.data);
    return 1;

fail:
    free(out.data);
    free(existing.data);
    return 0;
}

#endif /* _WIN32 */
