// launcher_debug.c — LNG_SCRIPT interpreter + framebuffer capture.

#include "launcher_debug.h"
#include "launcher_debug_tcp.h"
#include "launcher_sdlcompat.h"   // SDL2/SDL3 event-symbol shim + GL header

// A host that already compiles the stb_image_write implementation (e.g.
// gb-recompiled's gb_printer.c) defines RECOMP_UI_HOST_STB_WRITE so this TU
// pulls in the DECLARATIONS only and links against the host's single copy —
// otherwise two implementations collide at link time. Standalone recomp-ui
// (no host stb) leaves it undefined and provides the implementation here.
#ifndef RECOMP_UI_HOST_STB_WRITE
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "third_party/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LNG_MAX_CMDS 64

static char  g_script[2048];
static char* g_cmds[LNG_MAX_CMDS];
static int   g_cmd_count = 0;
static int   g_cmd_index = 0;
static int   g_wait_frames = 0;
static bool  g_active = false;
static bool g_tcp_failed, g_reply_pending;
static unsigned long g_frame;
static SDL_Event g_events[8];
static int g_event_count;
static float g_mouse_x, g_mouse_y;
static int g_click_phase;
static bool g_mouse_valid;

bool launcher_debug_hidden(void) {
    const char* hidden = SDL_getenv("LNG_TEST_HIDDEN");
    return SDL_getenv("LNG_TCP_PORT") != NULL || (hidden && !strcmp(hidden, "1"));
}
static void debug_event(SDL_Event* e) {
    if (!launcher_debug_hidden()) SDL_PushEvent(e);
    else if (g_event_count < (int)(sizeof(g_events) / sizeof(g_events[0])))
        g_events[g_event_count++] = *e;
}
bool launcher_debug_next_event(SDL_Event* e) {
    if (!g_event_count) return false;
    *e = g_events[0];
    --g_event_count;
    memmove(g_events, g_events + 1, (size_t)g_event_count * sizeof(*e));
    return true;
}
bool launcher_debug_mouse_frame(float* x, float* y, bool* down) {
    if (!launcher_debug_hidden() || !g_mouse_valid) return false;
    *x = g_mouse_x; *y = g_mouse_y; *down = g_click_phase == 1;
    if (g_click_phase) g_click_phase = g_click_phase == 1 ? 2 : 0;
    return true;
}
void launcher_debug_shutdown(void) {
    launcher_debug_tcp_close();
    g_active = false;
}

bool launcher_debug_active(void) { return g_active; }

void launcher_debug_init(void) {
    launcher_debug_shutdown();
    g_cmd_count = g_cmd_index = g_wait_frames = g_event_count = g_click_phase = 0;
    g_frame = 0;
    g_mouse_valid = g_reply_pending = g_tcp_failed = false;
    const char* port = SDL_getenv("LNG_TCP_PORT");
    if (port) {
        g_active = true;
        g_tcp_failed = !launcher_debug_tcp_open(port);
    }
    const char* s = SDL_getenv("LNG_SCRIPT");
    if (!s || !s[0]) return;

    snprintf(g_script, sizeof(g_script), "%s", s);
    g_cmd_count = 0;
    char* tok = strtok(g_script, ";");
    while (tok && g_cmd_count < LNG_MAX_CMDS) {
        while (*tok == ' ') ++tok;          // trim leading spaces
        if (*tok) g_cmds[g_cmd_count++] = tok;
        tok = strtok(NULL, ";");
    }
    g_active = g_active || g_cmd_count > 0;
    if (g_active) fprintf(stderr, "[dbg] script: %d commands\n", g_cmd_count);
}

bool launcher_capture_png(const char* path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
    if (!px) return false;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // RGBA/UNSIGNED_BYTE is portable to GLES default framebuffers; RGB reads
    // can fail with GL_INVALID_OPERATION on ANGLE. Keep the saved PNG RGB.
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        px[i * 3] = px[i * 4];
        px[i * 3 + 1] = px[i * 4 + 1];
        px[i * 3 + 2] = px[i * 4 + 2];
    }

    // GL origin is bottom-left; PNG wants top-down. Flip rows in place.
    const size_t stride = (size_t)w * 3;
    unsigned char* row = (unsigned char*)malloc(stride);
    if (row) {
        for (int y = 0; y < h / 2; ++y) {
            unsigned char* a = px + (size_t)y * stride;
            unsigned char* b = px + (size_t)(h - 1 - y) * stride;
            memcpy(row, a, stride); memcpy(a, b, stride); memcpy(b, row, stride);
        }
        free(row);
    }

    int ok = stbi_write_png(path, w, h, 3, px, (int)stride);
    free(px);
    if (ok) fprintf(stderr, "[dbg] shot -> %s (%dx%d)\n", path, w, h);
    else    fprintf(stderr, "[dbg] shot FAILED -> %s\n", path);
    return ok != 0;
}

// Synthesize a click at logical window coords: warp the cursor (so backends
// that sample SDL_GetMouseState see it) and push button events (so backends
// that consume the event queue see it). Covers both ImGui and Clay.
static void synth_click(LauncherPlatform* p, float x, float y) {
    if (launcher_debug_hidden()) {
        g_mouse_x = x; g_mouse_y = y; g_mouse_valid = true; g_click_phase = 1;
        return;
    }
#if defined(LNG_SDL3)
    SDL_WarpMouseInWindow(p->window, x, y);
#else
    // SDL2 stores mouse coordinates as integers. Make its existing truncation
    // behavior explicit while preserving SDL3's subpixel coordinates.
    const int event_x = (int)x;
    const int event_y = (int)y;
    SDL_WarpMouseInWindow(p->window, event_x, event_y);
#endif

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(p->window);
#if defined(LNG_SDL3)
    e.motion.x = x; e.motion.y = y;
#else
    e.motion.x = event_x; e.motion.y = event_y;
#endif
    debug_event(&e);

    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = SDL_GetWindowID(p->window);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.clicks = 1;
#if defined(LNG_SDL3)
    e.button.x = x; e.button.y = y;
    e.button.down = true;
#else
    e.button.x = event_x; e.button.y = event_y;
    e.button.state = SDL_PRESSED;
#endif
    debug_event(&e);

    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
#if defined(LNG_SDL3)
    e.button.down = false;
#else
    e.button.state = SDL_RELEASED;
#endif
    debug_event(&e);
}

static void synth_key(LauncherPlatform* p, SDL_Keycode key) {
    SDL_Scancode sc = SDL_GetScancodeFromKey(key
#if defined(LNG_SDL3)
        , NULL
#endif
        );
    SDL_Event e;
    SDL_zero(e);
    e.key.windowID = SDL_GetWindowID(p->window);
    e.type = SDL_EVENT_KEY_DOWN;
#if defined(LNG_SDL3)
    e.key.key = key;
    e.key.scancode = sc;
    e.key.down = true;
#else
    e.key.keysym.sym = key;
    e.key.keysym.scancode = sc;
    e.key.state = SDL_PRESSED;
#endif
    debug_event(&e);
    e.type = SDL_EVENT_KEY_UP;
#if defined(LNG_SDL3)
    e.key.down = false;
#else
    e.key.state = SDL_RELEASED;
#endif
    debug_event(&e);
}

void launcher_debug_step(LauncherPlatform* p, LauncherModel* m) {
    if (!g_active) return;
    ++g_frame;
    if (g_tcp_failed) { m->action = LNG_ACTION_QUIT; return; }

    // Never clobber an action the UI already set this frame (e.g. PLAY -> LAUNCH);
    // otherwise a script that clicks PLAY and then ends would overwrite LAUNCH
    // with the script-exhausted QUIT below.
    if (m->action != LNG_ACTION_NONE) return;

    if (g_wait_frames > 0) { --g_wait_frames; return; }
    if (g_reply_pending) {
        char reply[128];
        snprintf(reply, sizeof(reply), "{\"ok\":true,\"frame\":%lu}", g_frame);
        launcher_debug_tcp_reply(reply);
        g_reply_pending = false;
        return;
    }

    char tcp_command[2048];
    bool tcp = false, ok = true;
    const char* c;
    if (g_cmd_index < g_cmd_count) c = g_cmds[g_cmd_index++];
    else if (launcher_debug_tcp_active()) {
        int result = launcher_debug_tcp_command(tcp_command, sizeof(tcp_command));
        if (!result) return;
        if (result < 0) {
            launcher_debug_tcp_reply("{\"ok\":false,\"error\":\"invalid_command\"}");
            return;
        }
        c = tcp_command; tcp = true;
    } else { m->action = LNG_ACTION_QUIT; return; }

    if (!strcmp(c, "state")) {
        char reply[2048];
        int at = snprintf(reply, sizeof(reply),
            "{\"ok\":true,\"frame\":%lu,\"hidden\":%s,\"window_hidden\":%s,\"view\":\"%s\",\"view_id\":%d,"
            "\"players\":%d,\"netplay\":%s,\"width\":%d,\"height\":%d,\"inputs\":[",
            g_frame, launcher_debug_hidden() ? "true" : "false",
            (SDL_GetWindowFlags(p->window) & SDL_WINDOW_HIDDEN) ? "true" : "false",
            launcher_view_name(m->view), (int)m->view, launcher_model_visible_player_count(m),
            m->netplay_supported ? "true" : "false", p->logical_w, p->logical_h);
        for (int i = 0; i < launcher_model_visible_player_count(m); ++i)
            at += snprintf(reply + at, sizeof(reply) - (size_t)at,
                "%s{\"source\":%d,\"instance\":%u}", i ? "," : "",
                m->s.player_src[i], m->s.player_gamepad_instance[i]);
        snprintf(reply + at, sizeof(reply) - (size_t)at, "]}");
        launcher_debug_tcp_reply(reply);
        return;
    }

    if (strncmp(c, "view:", 5) == 0) {
        const char* v = c + 5;
        if      (strcmp(v, "dashboard")  == 0) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
        else if (strcmp(v, "settings")   == 0) launcher_model_set_view(m, LNG_VIEW_SETTINGS);
        else if (strcmp(v, "controller") == 0) launcher_model_set_view(m, LNG_VIEW_CONTROLLER);
        else if (strcmp(v, "assist_tools") == 0) launcher_model_set_view(m, LNG_VIEW_ASSIST_TOOLS);
        else if (strcmp(v, "credits") == 0) launcher_model_set_view(m, LNG_VIEW_CREDITS);
        else if (strcmp(v, "mods") == 0) launcher_model_set_view(m, LNG_VIEW_MODS);
        /* The LAN-vs-online fork. Reachable only by clicking NETPLAY on the
         * dashboard, which made it the one netplay page a screenshot script
         * could not open. */
        else if (strcmp(v, "netplay_mode") == 0)
            launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
        else if (strcmp(v, "netplay") == 0) {
            m->netplay_list_fresh = false;
            launcher_model_set_view(m, LNG_VIEW_NETPLAY);
        }
        else if (strcmp(v, "lobby") == 0) launcher_model_set_view(m, LNG_VIEW_LOBBY);
        else ok = false;
    } else if (strncmp(c, "player:", 7) == 0) {
        // Select which player the Controller view configures. Clamp to the
        // launcher's real player range (N64 profiles run up to 4) instead of
        // the old 0/1-only test hook, so scripted screenshots can reach P3/P4.
        int pl = atoi(c + 7);
        if (pl < 0) pl = 0;
        if (pl > LNG_MAX_PLAYERS - 1) pl = LNG_MAX_PLAYERS - 1;
        m->cfg_player = pl;
    } else if (strncmp(c, "capbtn:", 7) == 0) {
        launcher_model_begin_capture(m, atoi(c + 7));   // rebind a player button (generic spec index)
    } else if (strncmp(c, "caphk:", 6) == 0) {
        launcher_model_begin_hk_capture(m, (LngHotkey)atoi(c + 6)); // rebind a hotkey
    } else if (strncmp(c, "size:", 5) == 0) {
        int w = 0, h = 0;
        if (sscanf(c + 5, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            SDL_SetWindowSize(p->window, w, h);
            launcher_platform_refresh_metrics(p);
        }
    } else if (strncmp(c, "click:", 6) == 0) {
        float x = 0, y = 0;
        if (sscanf(c + 6, "%f,%f", &x, &y) == 2) synth_click(p, x, y);
        else ok = false;
    } else if (strncmp(c, "key:", 4) == 0) {
        if (strcmp(c + 4, "escape") == 0) synth_key(p, SDLK_ESCAPE);
        else {
            SDL_Keycode k = SDL_GetKeyFromName(c + 4);
            if (k != SDLK_UNKNOWN) synth_key(p, k);
            else { fprintf(stderr, "[dbg] unknown key: %s\n", c + 4); ok = false; }
        }
    } else if (strncmp(c, "text:", 5) == 0) {
        /* Type UTF-8 into the focused widget, as an OS text-input event. */
        static char s_text[256];
        SDL_Event e;
        snprintf(s_text, sizeof(s_text), "%s", c + 5);
        SDL_zero(e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(p->window);
#if defined(LNG_SDL3)
        e.text.text = s_text;
        e.text.windowID = SDL_GetWindowID(p->window);
#else
        snprintf(e.text.text, sizeof(e.text.text), "%s", s_text);
#endif
        debug_event(&e);
    } else if (strncmp(c, "wait:", 5) == 0) {
        g_wait_frames = atoi(c + 5);
        if (g_wait_frames < 0 || g_wait_frames > 100000) { g_wait_frames = 0; ok = false; }
    } else if (strncmp(c, "shot:", 5) == 0) {
        ok = launcher_capture_png(c + 5, p->pixel_w, p->pixel_h);
    } else if (strcmp(c, "quit") == 0) {
        m->action = LNG_ACTION_QUIT;
    } else {
        fprintf(stderr, "[dbg] unknown command: %s\n", c);
        ok = false;
    }
    if (tcp) {
        if (!ok) launcher_debug_tcp_reply("{\"ok\":false,\"error\":\"command_failed\"}");
        else if (m->action == LNG_ACTION_QUIT)
            launcher_debug_tcp_reply("{\"ok\":true}");
        else g_reply_pending = true; // acknowledge after the next rendered frame / wait
    }
}
