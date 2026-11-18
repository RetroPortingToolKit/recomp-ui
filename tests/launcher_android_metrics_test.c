/* Exercise the production SDL2 metrics with deterministic surface/DPI inputs;
 * no window or Android/JNI runtime is needed for this layout policy. */
#define DECLSPEC
#include "launcher_platform.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define __ANDROID__ 1
#include "../src/common/launcher_platform_sdl2.c"

static int width, height, pixels_w, pixels_h;
static float density;
void SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
    (void)window; *w = width; *h = height;
}
void SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h) {
    (void)window; *w = pixels_w; *h = pixels_h;
}
int SDL_GetWindowDisplayIndex(SDL_Window* window) { (void)window; return 0; }
int SDL_GetDisplayDPI(int display, float* d, float* h, float* v) {
    (void)display; *d = *h = *v = density; return density > 0.0f ? 0 : -1;
}
void launcher_boot_timing_mark(const char* phase) { (void)phase; }
unsigned char* launcher_image_load_rgba(const char* p, int* w, int* h) {
    (void)p; (void)w; (void)h; return NULL;
}
void launcher_image_free(unsigned char* p) { (void)p; }

static LauncherPlatform metrics(int w, int h, float dpi) {
    LauncherPlatform p = {0}; p.window = (SDL_Window*)&p;
    width = pixels_w = w; height = pixels_h = h; density = dpi;
    launcher_platform_refresh_metrics(&p);
    assert(p.pixel_w == w && p.pixel_h == h);
    assert(fabsf(p.input_scale * p.logical_w - width) < 0.01f);
    return p;
}
static void force_scale(const char* value) {
#ifdef _WIN32
    _putenv_s("LNG_FORCE_SCALE", value);
#else
    setenv("LNG_FORCE_SCALE", value, 1);
#endif
}
int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    force_scale("");
    const int surfaces[][2] = {{2400,1080}, {1920,1080}, {1280,720}};
    for (unsigned i = 0; i < sizeof surfaces / sizeof *surfaces; ++i) {
        LauncherPlatform p = metrics(surfaces[i][0], surfaces[i][1], 420.0f);
        assert(p.logical_w >= 820 && p.logical_h >= 600);
        /* The physical centre must hit the same logical centre. */
        assert(fabsf((width / 2.0f) / p.input_scale - p.logical_w / 2.0f) < 0.01f);
    }
    LauncherPlatform p = metrics(800, 480, 420.0f);
    assert(p.display_scale == 1.0f && p.logical_w == 800 && p.logical_h == 480);
    p = metrics(1920, 1080, 0.0f);
    assert(p.display_scale == 1.0f);
    /* Existing point/pixel splits and the explicit test override stay exact. */
    width = 1200; height = 800; pixels_w = 2400; pixels_h = 1600;
    launcher_platform_refresh_metrics(&p);
    assert(p.display_scale == 2.0f && p.logical_w == 1200 && p.input_scale == 1.0f);
    force_scale("2.5");
    p = metrics(2400, 1080, 420.0f);
    assert(p.display_scale == 2.5f && p.logical_w == 960 && p.logical_h == 432);
    force_scale("");
    puts("Android launcher metrics and input mapping passed");
    return 0;
}
