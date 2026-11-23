#pragma once

#include <string.h>

/* Keep these pseudo-scancodes compatible with the PlayStation binding store.
 * SDL button order is Left, Middle, Right, X1, X2. */
enum { RUI_MOUSE_BIND_BASE = 512, RUI_MOUSE_BIND_COUNT = 5 };

static inline const char *rui_mouse_bind_name(int code) {
    static const char *const names[] = {"Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5"};
    if (code <= RUI_MOUSE_BIND_BASE || code > RUI_MOUSE_BIND_BASE + RUI_MOUSE_BIND_COUNT)
        return NULL;
    return names[code - RUI_MOUSE_BIND_BASE - 1];
}

static inline int rui_mouse_bind_parse(const char *lower_name) {
    if (strncmp(lower_name, "mouse", 5) || lower_name[5] < '1' ||
        lower_name[5] >= '1' + RUI_MOUSE_BIND_COUNT || lower_name[6])
        return 0;
    return RUI_MOUSE_BIND_BASE + lower_name[5] - '0';
}
