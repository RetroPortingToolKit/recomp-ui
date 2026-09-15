// launcher_platform_sdl2.c — SDL2 implementation of the shared platform layer.
//
// Compatibility implementation for SDL2 hosts. It implements the same
// launcher_platform.h contract as launcher_platform_sdl3.c.
//
// DPI on SDL2: SDL_WINDOW_ALLOW_HIGHDPI makes the window size logical (points)
// while SDL_GL_GetDrawableSize reports physical pixels; their ratio is the
// content scale. That covers Windows per-monitor, macOS Retina and X11.
// It does NOT cover Wayland fractional scaling — SDL2 only supports integer
// buffer scale, so at 125%/150% the compositor downscales and text softens.
// SDL3 is the preferred path for fractional Wayland scaling.

#include <stdlib.h>
#include "launcher_platform.h"
#include "launcher_boot_timing.h"
#include "launcher_gl.h"

#include <stdio.h>

static bool s_quit_sdl = true;

void launcher_platform_set_quit_sdl(bool quit_sdl) {
    s_quit_sdl = quit_sdl;
}

static bool forced_usable_bounds(SDL_Rect* out) {
    const char* value = getenv("LNG_FORCE_USABLE_BOUNDS");
    if (!value || !value[0] || !out) return false;
    int w = 0, h = 0;
    if (sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        return false;
    out->x = 0;
    out->y = 0;
    out->w = w;
    out->h = h;
    return true;
}

static void fit_initial_window_to_display(int* width, int* height) {
    if (!width || !height || *width <= 0 || *height <= 0) return;

    SDL_Rect usable = {0};
    bool have_bounds = forced_usable_bounds(&usable);
    if (!have_bounds)
        have_bounds = SDL_GetDisplayUsableBounds(0, &usable) == 0;
    if (!have_bounds || usable.w <= 0 || usable.h <= 0) return;

    // Leave room for desktop panels/title bars. Normal desktop sizes keep the
    // default 1100x880 window; only cramped displays get a smaller launch size.
    const int margin = 64;
    int max_w = usable.w - margin;
    int max_h = usable.h - margin;
    if (max_w <= 0 || max_h <= 0) return;
    if (*width <= max_w && *height <= max_h) return;

    float scale_w = (float)max_w / (float)*width;
    float scale_h = (float)max_h / (float)*height;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale <= 0.0f || scale >= 1.0f) return;

    const int min_w = 820;
    const int min_h = 600;
    int fitted_w = (int)((float)*width * scale);
    int fitted_h = (int)((float)*height * scale);
    if (fitted_w < min_w && max_w >= min_w) fitted_w = min_w;
    if (fitted_h < min_h && max_h >= min_h) fitted_h = min_h;
    if (fitted_w > max_w) fitted_w = max_w;
    if (fitted_h > max_h) fitted_h = max_h;
    if (fitted_w > 0) *width = fitted_w;
    if (fitted_h > 0) *height = fitted_h;
}

// Usable (work-area) bounds of the display the window is actually on, honouring
// the LNG_FORCE_USABLE_BOUNDS override the initial fit already understands.
static bool window_usable_bounds(SDL_Window* window, SDL_Rect* out) {
    int display;
    if (!out) return false;
    if (forced_usable_bounds(out)) return true;
    display = window ? SDL_GetWindowDisplayIndex(window) : 0;
    if (display < 0 || SDL_GetDisplayUsableBounds(display, out) != 0) return false;
    return out->w > 0 && out->h > 0;
}

// Centre the whole FRAME in the work area, never letting the caption cross the
// top edge. Called before a maximize too, so Restore lands somewhere sane.
static void place_window(SDL_Window* window, const SDL_Rect* usable, int w, int h,
                         int frame_t, int frame_l, int frame_b, int frame_r) {
    int x, y;
    SDL_SetWindowSize(window, w, h);
    x = usable->x + frame_l + (usable->w - (w + frame_l + frame_r)) / 2;
    y = usable->y + frame_t + (usable->h - (h + frame_t + frame_b)) / 2;
    if (x < usable->x + frame_l) x = usable->x + frame_l;
    if (y < usable->y + frame_t) y = usable->y + frame_t;
    SDL_SetWindowPosition(window, x, y);
}

// Size and place the window for the resolved scale, keeping every part the user
// has to grab — title bar, borders — inside the desktop's work area.
//
// The size the host asks for is LOGICAL, so where the point/pixel split is
// synthesized an 1100x880 launcher needs 1650x1320 pixels on a 150% desktop and
// 3300x2640 on a 300% one — usually more than the monitor has. Clamping those
// numbers to the work area and centring the result is not enough, and is how a
// window ends up unusable: SDL sizes and positions the CLIENT area, so the
// caption lives above y and has to be paid for here, and SDL_WINDOWPOS_CENTERED
// centres on the DISPLAY bounds rather than the work area — a window as tall as
// the work area is then pushed up by half the taskbar's height and its title bar
// leaves the top of the screen, with no grabbable edges either because it
// already spans the work area.
static void fit_window_to_display(LauncherPlatform* p, int logical_w, int logical_h) {
    SDL_Rect usable = {0};
    int frame_t = 0, frame_l = 0, frame_b = 0, frame_r = 0;
    int want_w, want_h, max_w, max_h, margin;
    float scale;

    if (!p || !p->window || logical_w <= 0 || logical_h <= 0) return;
    scale = p->input_scale > 0.0f ? p->input_scale : 1.0f;
    want_w = (int)((float)logical_w * scale);
    want_h = (int)((float)logical_h * scale);

    if (!window_usable_bounds(p->window, &usable)) {
        // No work area to reason about: honour the scale and let the window
        // manager place it, which is all this could do before any of the above.
        if (scale != 1.0f) {
            SDL_SetWindowSize(p->window, want_w, want_h);
            SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            launcher_platform_refresh_metrics(p);
        }
        return;
    }

    // Frame extents come from the live window, so they already carry the
    // display's own scale. Platforms without server-side decorations report
    // nothing and leave these at 0, where the margin alone is the slack.
    SDL_GetWindowBordersSize(p->window, &frame_t, &frame_l, &frame_b, &frame_r);
    margin = (int)(16.0f * scale);

    max_w = usable.w - frame_l - frame_r - margin * 2;
    max_h = usable.h - frame_t - frame_b - margin * 2;
    if (max_w <= 0 || max_h <= 0) {   // a work area smaller than its own chrome
        place_window(p->window, &usable, want_w, want_h,
                     frame_t, frame_l, frame_b, frame_r);
    } else if (want_w > max_w || want_h > max_h) {
        // Too big to place by hand. Maximizing is the honest version of what a
        // clamp was reaching for: the OS owns the geometry, the title bar stays
        // reachable, snapping works, and Restore gives back the size set here.
        place_window(p->window, &usable,
                     want_w < max_w ? want_w : max_w,
                     want_h < max_h ? want_h : max_h,
                     frame_t, frame_l, frame_b, frame_r);
        SDL_MaximizeWindow(p->window);
    } else {
        place_window(p->window, &usable, want_w, want_h,
                     frame_t, frame_l, frame_b, frame_r);
    }
    launcher_platform_refresh_metrics(p);
}

bool launcher_platform_open(LauncherPlatform* p, const char* title,
                            int logical_w, int logical_h) {
    if (!p) return false;
    SDL_zerop(p);
    launcher_boot_timing_mark("rui:platform_open:begin");

    SDL_SetMainReady();   // we built with SDL_MAIN_HANDLED (real main() is entry)
#if defined(_WIN32)
    // Match the Windows compatibility override users discovered manually:
    // let the launcher own DPI scaling instead of being bitmap-scaled by the OS.
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "0");
#endif
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5", "1",
                            SDL_HINT_DEFAULT);
    // Bluetooth DualSense controllers start in basic-report mode. Requesting
    // enhanced reports exposes their motion sensors through SDL's standard
    // game-controller sensor API; USB controllers already use this mode.
    SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1",
                            SDL_HINT_DEFAULT);
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
    // Launcher navigation is positional: the lower face button accepts and the
    // right face button cancels, matching ImGui's gamepad navigation model.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0",
                            SDL_HINT_DEFAULT);
#endif
#ifdef LNG_GLES2
    // The host links ANGLE's libGLESv2/libEGL; SDL must create the context
    // through that same ES library (via EGL), or the directly-linked ANGLE
    // entry points run with no current context and crash on the first GL call.
    // Must be set BEFORE SDL_Init. Mirrors gb-recompiled's own platform init.
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_SENSOR) != 0) {   // SDL2: 0 == success
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    {
        SDL_version version;
        SDL_GetVersion(&version);
        fprintf(stderr, "[launcher] SDL runtime %u.%u.%u (%s)\n",
                (unsigned)version.major, (unsigned)version.minor,
                (unsigned)version.patch, SDL_GetRevision());
    }

    // GL dialect. Default: desktop GL 3.3 core (matches the vendored ImGui
    // opengl3 backend + "#version 330"). A host that reuses its OWN ImGui copy
    // compiled for GLES 2 (e.g. gb-recompiled, which renders through ANGLE)
    // defines LNG_GLES2 so the launcher's context MATCHES that backend — a
    // core-profile context with a GLES2 backend has no VAO and crashes.
#ifdef LNG_GLES2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);   // ES 2.0, matching the host backend
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // Test hook: LNG_FORCE_SCALE=<1..4> simulates a HiDPI display on ANY platform
    // (incl. Windows, which has no native point/pixel split) — the window is
    // created that many times larger in pixels while the UI keeps laying out in
    // the original logical size, so the DPI-independent layout + framebuffer
    // scaling can be validated end to end. See launcher_platform_refresh_metrics.
    int win_w = logical_w, win_h = logical_h;
    fit_initial_window_to_display(&win_w, &win_h);
    {
        const char* fs = getenv("LNG_FORCE_SCALE");
        if (fs && fs[0]) {
            float v = (float)atof(fs);
            if (v > 1.0f && v <= 4.0f) { win_w = (int)(logical_w * v); win_h = (int)(logical_h * v); }
        }
    }
    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__ANDROID__)
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
#endif
    p->window = SDL_CreateWindow(title ? title : "Launcher",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h,
                                 window_flags);
    if (!p->window) {
        fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

    p->gl = SDL_GL_CreateContext(p->window);
    if (!p->gl) {
        fprintf(stderr, "[launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(p->window);
        p->window = NULL;
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(p->window, p->gl);
    SDL_GL_SetSwapInterval(1);

    SDL_RaiseWindow(p->window);   // foreground + keyboard focus (gamepad/kbd nav)

    launcher_platform_refresh_metrics(p);

    // The window was created `logical_w` PIXELS wide. Where the point/pixel
    // split is synthesized (Windows, X11) that is only logical_w/scale logical
    // units — a 1100-unit layout arriving in 733 of them drops straight to the
    // narrow one-column breakpoint on a 150% desktop — so this asks for the
    // pixels the requested logical size really needs, and keeps the result
    // reachable on displays that cannot show them all.
    fit_window_to_display(p, logical_w, logical_h);
    launcher_boot_timing_mark("rui:platform_open:window+gl_ready");
    return true;
}

void launcher_platform_set_icon(LauncherPlatform* p, const char* image_path) {
    if (!p || !p->window || !image_path || !image_path[0]) return;
    int w = 0, h = 0;
    unsigned char* pixels = launcher_image_load_rgba(image_path, &w, &h);
    if (!pixels) return;
    /* SDL copies the pixels into its own icon storage, so the decoded buffer
     * is ours to free as soon as SetWindowIcon returns. The channel masks are
     * byte-order dependent: stb hands back R,G,B,A in memory order. */
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        pixels, w, h, 32, w * 4,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff
#else
        0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000
#endif
    );
    if (surf) {
        SDL_SetWindowIcon(p->window, surf);
        SDL_FreeSurface(surf);
    }
    launcher_image_free(pixels);
}

// Test hook: LNG_FORCE_SCALE=<1..4> pins the display scale (see
// launcher_platform_open, which also enlarges the window to match), so the
// HiDPI path can be exercised end to end on a 100% display. 0 when unset.
static float forced_display_scale(void) {
    const char* fs = getenv("LNG_FORCE_SCALE");
    float v;
    if (!fs || !fs[0]) return 0.0f;
    v = (float)atof(fs);
    return (v > 1.0f && v <= 4.0f) ? v : 0.0f;
}

void launcher_platform_refresh_metrics(LauncherPlatform* p) {
    int win_w = 0, win_h = 0;
    float s = 1.0f, forced;
    if (!p || !p->window) return;

    SDL_GetWindowSize(p->window, &win_w, &win_h);
    SDL_GL_GetDrawableSize(p->window, &p->pixel_w, &p->pixel_h);

    // SDL2 has no SDL_GetWindowDisplayScale. Derive the content scale from the
    // drawable/window ratio, which is what ALLOW_HIGHDPI exposes.
    if (win_w > 0 && p->pixel_w > 0)
        s = (float)p->pixel_w / (float)win_w;

    // On Windows the drawable and window sizes are both in pixels (no
    // point/pixel split), so the ratio is always 1.0 and we must ask the OS for
    // the real DPI instead. 96 dpi == 100% scaling.
    if (s <= 1.001f) {
        int disp = SDL_GetWindowDisplayIndex(p->window);
        float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
        if (disp >= 0 && SDL_GetDisplayDPI(disp, &ddpi, &hdpi, &vdpi) == 0 && hdpi > 0.0f) {
            float dpi_scale = hdpi / 96.0f;
            if (dpi_scale > s) s = dpi_scale;
        }
    }
    if (s <= 0.0f) s = 1.0f;
    forced = forced_display_scale();
    if (forced > 0.0f) s = forced;

    p->logical_w = win_w;
    p->logical_h = win_h;
    p->display_scale = s;
    p->input_scale = 1.0f;

    // No point/pixel split to read (Windows, X11, and the forced test hook,
    // which enlarged the window in pixels): synthesize one so the layout gets
    // pixel/scale logical units and ImGui renders it at pixel density — the
    // Retina/Deck model, on any OS. Where the ratio ALREADY carries the
    // density (macOS retina), the window size is the logical size: leave it.
    if (s > 1.0f && win_w > 0 && (p->pixel_w == win_w || forced > 0.0f)) {
        p->logical_w = (int)(p->pixel_w / s);
        p->logical_h = (int)(p->pixel_h / s);
    }
    if (p->logical_w > 0)
        p->input_scale = (float)win_w / (float)p->logical_w;
}

void launcher_platform_present(LauncherPlatform* p) {
    if (p && p->window) SDL_GL_SwapWindow(p->window);
}

void launcher_platform_close(LauncherPlatform* p) {
    if (!p) return;
    if (p->gl)     { SDL_GL_DeleteContext(p->gl); p->gl = NULL; }
    if (p->window) { SDL_DestroyWindow(p->window); p->window = NULL; }
    SDL_GL_ResetAttributes();   // leave a clean slate for the game's SDL usage
    /* Soft-return rematch hosts that skip SDL_Quit (s_quit_sdl=false) keep the
     * subsystem alive across launcher↔game transitions. When we do quit, hosts
     * MUST re-SDL_Init (video+audio+gamecontroller) before recreating the game
     * window — see docs/HOST_NETPLAY.md. */
    if (s_quit_sdl)
        SDL_Quit();
}
