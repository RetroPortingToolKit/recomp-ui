#ifndef RECOMP_LAUNCHER_WINDOW_SIZE_H
#define RECOMP_LAUNCHER_WINDOW_SIZE_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This sidecar belongs to the launcher. Host game-window settings stay intact.
 * Logical dimensions go through the platform's existing DPI/work-area fit. */
static int launcher_window_size_path(const char* config, char* out, size_t cap) {
    if (!config || !config[0] || !out || !cap) return 0;
    const char* slash = strrchr(config, '/');
    const char* backslash = strrchr(config, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const size_t prefix = slash ? (size_t)(slash - config + 1) : 0;
    const char leaf[] = "launcher-window.ini";
    if (prefix + sizeof(leaf) > cap) return 0;
    memcpy(out, config, prefix);
    memcpy(out + prefix, leaf, sizeof(leaf));
    return 1;
}

static int launcher_window_dimension(const char* text) {
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || value < 200 || value > 16384) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    return *end ? 0 : (int)value;
}

static void launcher_window_size_load(const char* path, int* width, int* height) {
    FILE* file = path && path[0] ? fopen(path, "rb") : NULL;
    if (!file) return;
    char line[256];
    int w = 0, h = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "logical_width=", 14)) w = launcher_window_dimension(line + 14);
        if (!strncmp(line, "logical_height=", 15)) h = launcher_window_dimension(line + 15);
    }
    fclose(file);
    if (w && h) { *width = w; *height = h; }
}

static void launcher_window_size_save(const char* path, int width, int height) {
    if (!path || !path[0] || width < 200 || height < 200 || width > 16384 || height > 16384) return;
    FILE* file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "# Launcher dimensions in logical UI units.\nlogical_width=%d\nlogical_height=%d\n", width, height);
    fclose(file);
}

#endif
