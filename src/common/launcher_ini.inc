/* Included by launcher_binds.c so consumers with explicit source manifests
 * get persistence fixes through a pin bump, without adding a translation unit.
 * The settings regression includes this same implementation without SDL binds. */
#include "launcher_ini.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif
#ifdef _MSC_VER
#define strncasecmp _strnicmp
#define strdup _strdup
#endif

static int ini_ieq(const char* a, size_t alen, const char* b) {
    size_t bl = strlen(b);
    if (alen != bl) return 0;
    for (size_t i = 0; i < alen; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static char* ini_read_whole(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (buf) { *out_len = (long)fread(buf, 1, (size_t)n, f); buf[*out_len] = 0; }
    fclose(f);
    return buf;
}

// Does `line` (leading ws / optional '#') assign `key`? Mirrors config.c.
// allow_commented extends the match to "# Key = value" placeholder lines —
// callers must prefer an ACTIVE line when one exists, because a prose comment
// that merely BEGINS with "Key = ..." ("# Vsync = 0 paces the loop...") is
// indistinguishable from a placeholder, and replacing it both destroys the
// comment and leaves the real assignment below as a duplicate.
static int line_is_key_ex(const char* line, const char* key, int allow_commented) {
    const char* i = line;
    while (*i == ' ' || *i == '\t') ++i;
    if (*i == '#') {
        if (!allow_commented) return 0;
        ++i; while (*i == ' ' || *i == '\t') ++i;
    }
    size_t kl = strlen(key);
    if (strncasecmp(i, key, kl) != 0) return 0;
    i += kl;
    while (*i == ' ' || *i == '\t') ++i;
    return *i == '=';
}

// Surgically set "Key = value" inside [section] of `path`, preserving every
// other line (comments, blank lines, and unrelated sections — e.g. the NES
// keybinds.ini's [zapper]/[gamepad1]/[gamepad2] sections survive untouched).
// Creates the file and/or section when absent. Exported (launcher_binds.h)
// for console units whose native bind files carry sections the launcher
// doesn't own (consoles/nes/nes_binds.c).
void launcher_ini_kv_write(const char* path, const char* section,
                           const char* key, const char* value) {
    long len = 0; char* text = ini_read_whole(path, &len);
    /* split into a growable line array, PRESERVING blank lines (strtok would
     * collapse them, losing the user's config formatting). */
    int cap = 64, n = 0;
    char** lines = (char**)malloc(sizeof(char*) * cap);
    if (text) {
        char* start = text;
        for (long i = 0; i <= len; ++i) {
            if (i == len || text[i] == '\n') {
                char* end = text + i;
                if (end > start && end[-1] == '\r') end[-1] = 0;
                else if (i < len) text[i] = 0;
                else text[i] = 0;
                if (i == len && start == text + len) break;  // no trailing empty
                if (n == cap) { cap *= 2; lines = (char**)realloc(lines, sizeof(char*) * cap); }
                lines[n++] = strdup(start);
                start = text + i + 1;
            }
        }
    }
    char assign[768];
    snprintf(assign, sizeof(assign), "%s = %s", key, value ? value : "");

    /* locate [section] body [start,end) */
    int ks = -1, ke = -1;
    for (int i = 0; i < n; ++i) {
        const char* p = lines[i]; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '[') continue;
        const char* close = strchr(p, ']');
        size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
        if (ks >= 0) { ke = i; break; }
        if (ini_ieq(p + 1, sl, section)) ks = i + 1;
    }
    if (ks >= 0 && ke < 0) ke = n;

    if (ks < 0) {
        char header[96];
        snprintf(header, sizeof(header), "[%s]", section);
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        if (n && lines[n-1][0]) lines[n++] = strdup("");
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(header);
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(assign);
    } else {
        int hit = -1;
        for (int i = ks; i < ke; ++i)
            if (line_is_key_ex(lines[i], key, 0)) { hit = i; break; }
        if (hit < 0)   /* no active line: a "# Key = value" placeholder will do */
            for (int i = ks; i < ke; ++i)
                if (line_is_key_ex(lines[i], key, 1)) { hit = i; break; }
        if (hit >= 0) { free(lines[hit]); lines[hit] = strdup(assign); }
        else {
            int at = ke;
            while (at > ks) { const char* p = lines[at-1]; while (*p==' '||*p=='\t') ++p; if (*p) break; --at; }
            if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
            for (int i = n; i > at; --i) lines[i] = lines[i-1];
            lines[at] = strdup(assign); ++n;
        }
    }

    FILE* f = fopen(path, "wb");
    if (f) { for (int i = 0; i < n; ++i) { fputs(lines[i], f); fputc('\n', f); } fclose(f); }
    for (int i = 0; i < n; ++i) free(lines[i]);
    free(lines); free(text);
}
