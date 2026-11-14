// psx_menu_provider.cpp -- recomp-ui as a psxrecomp in-game menu provider
// (recomp_ui_psx_menu.h): the menu model and its wiring to PsxMenuHost.
// Every value shown comes from the host and every change goes back through
// it, so the runtime's policies (live apply, restart bits, pause refusal in
// netplay, tray, exits) are the same ones any other provider gets.
#if defined(__has_include)
#if __has_include("psx_menu_host.h")
#define RUI_PSX_HAS_MENU_HOST 1
#endif
#endif

#if defined(RUI_PSX_HAS_MENU_HOST)
#include "psx_menu_host.h"
#include "recomp_launcher.h"
#include "recomp_ui_psx_menu.h"
#include "psx_menu_provider_internal.h"
#include "common/recomp_runtime_ui_internal.h"

#include <cstdio>
#include <cstring>

namespace rui_psx {
namespace {

const PsxMenuHost *g_host = nullptr;
RecompRuntimeUi *g_ui = nullptr;
PsxHostVideoSettings g_video{};    // the menu's working copy, from video_get
unsigned g_restart = 0;            // restart-only edits made while open
constexpr unsigned kSessionRestart = 1u << 16;  // a session row saved for the restart
bool g_restart_armed = false;      // Restart game: the first press asks again
bool g_pause_in_menu = true;
bool g_paused = false;
int g_disc = 1;
char g_status[160];
char g_subtitle[96];

// The game's look comes from the runtime's look rows (psxrecomp's bundled
// mods and [video] settings) when it has them; FMV filtering stays on the
// Mods page.
const char* const kRenderers[] = {"Software", "OpenGL", "Vulkan"};
const int kRendererValues[] = {0, 1, 2};
const char* const kVsync[] = {"Off", "On", "Adaptive", "VRR"};
const int kVsyncValues[] = {0, 1, -1, 2};
int g_renderers = 2;
bool g_vsync_overridden = false;
enum { RENDERER, RENDER_THREAD, PRESENT_THREAD,
#ifdef PSX_HOST_VIDEO_HAS_VSYNC
       VSYNC,
#endif
       DISC, INSERT };
RecompRuntimeUiItem g_extra[] = {
    {"graphics.renderer", "Graphics", "Renderer", "Applies with a restart when the menu closes.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 1, 1, kRenderers, 2, kRendererValues},
    {"graphics.render_thread", "Graphics", "Render thread",
     "Use a second CPU thread for OpenGL; restarts when this menu closes.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {"graphics.present_thread", "Graphics", "Present thread",
     "Present OpenGL frames on another thread. Requires Render thread; restarts when this menu closes.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
#ifdef PSX_HOST_VIDEO_HAS_VSYNC
    {"graphics.vsync", "Graphics", "VSync", "Driver synchronization and variable-refresh presentation.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kVsync, 3, kVsyncValues},
#endif
    {"media.disc", "Disc", "Selected disc", "Choose a disc, then Insert disc and resume.",
     RECOMP_RUNTIME_UI_INT, 1, 1, 1, nullptr, 0, nullptr},
    {"media.insert", "Disc", "Insert disc and resume", "Change the disc while the game keeps running.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
};

// Session rows: only when the runtime offers them (PsxMenuHost session_*).
const char *const kPortChoices[] = {"Port 1", "Port 2"};
const char *const kIresChoices[] = {"1x (Native)", "2x", "3x", "4x", "5x", "6x", "8x"};
const char *const kAaChoices[] = {"Off", "FXAA", "FXAA (high quality)"};
const char *const kSmoothChoices[] = {"Game default", "Smooth", "Sharp"};
const char *const kTextureChoices[] = {"Game default", "Nearest", "Bilinear", "xBR"};
const char *const kDitherChoices[] = {"Off (true colour)", "Console", "Fine"};
enum { REINSERT, CONTROLLER_PORT, LOOK_FIRST };
const RecompRuntimeUiItem g_session_rows[] = {
    {"media.reinsert", "Disc", "Reinsert current disc", "Open and close the lid with the same disc.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
    {"input.controller_port", "Input", "Controller 1 port",
     "Move controller 1 to console port 2 or back, as if the plug were moved. Some games ask for it.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 1, 1, kPortChoices, 2, nullptr},
    // Look rows: only when the runtime has them (PSX_MENU_HOST_HAS_LOOK_ROWS).
    {"system.restart_game", "System", "Restart game",
     "Reset the console: the game starts again from its boot screen. Unsaved progress is lost.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
    {"graphics.internal_resolution", "Graphics", "Internal resolution",
     "Render the 3D picture at a higher scale. Higher is sharper and uses more GPU.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 6, 1, kIresChoices, 7, nullptr},
    {"graphics.pgxp", "Graphics", "PGXP precision",
     "Steadier 3D: polygons stop wobbling and textures stop warping. Applies with a quick restart.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {"graphics.antialiasing", "Graphics", "Antialiasing",
     "Smooth jagged edges of the finished picture.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kAaChoices, 3, nullptr},
    {"graphics.crt_filter", "Graphics", "CRT filter",
     "A period TV look (colour and scanlines). Applies with a quick restart.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {"display.scanlines", "Display", "Scanlines",
     "Dark gaps between display lines, like a CRT. Uses the CRT filter.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {"display.smoothing", "Display", "Smoothing",
     "How the picture is scaled to the window: Smooth blends pixels, Sharp keeps hard edges.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kSmoothChoices, 3, nullptr},
    {"display.texture_filtering", "Display", "Texture filtering",
     "Nearest is the console's look; Bilinear softens; xBR rounds pixel-art edges.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 3, 1, kTextureChoices, 4, nullptr},
    {"display.dithering", "Display", "Dithering",
     "The console's dither pattern. Off shows true colour; Fine dithers at the internal resolution.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kDitherChoices, 3, nullptr},
};
constexpr size_t kSessionRowCount = sizeof g_session_rows / sizeof g_session_rows[0];
RecompRuntimeUiItem g_items[sizeof g_extra / sizeof g_extra[0] + kSessionRowCount];

#if defined(PSX_MENU_HOST_HAS_SESSION)
bool has_session() { return g_host && g_host->session_get && g_host->session_set && g_host->session_enabled; }
int session_of(const RecompRuntimeUiItem *item) {
    if (!item || !item->key) return 0;
    if (!std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) return PSX_MENU_SESSION_WINDOW_SCALE;
    if (!std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VOLUME)) return PSX_MENU_SESSION_VOLUME;
    if (!std::strcmp(item->key, "input.controller_port")) return PSX_MENU_SESSION_CONTROLLER_PORT;
    if (!std::strcmp(item->key, "media.reinsert")) return PSX_MENU_SESSION_REINSERT_DISC;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    static const struct { const char *key; int setting; } kLook[] = {
        {"system.restart_game", PSX_MENU_SESSION_RESTART_GAME},
        {"graphics.internal_resolution", PSX_MENU_SESSION_INTERNAL_RESOLUTION},
        {"graphics.pgxp", PSX_MENU_SESSION_PGXP},
        {"graphics.antialiasing", PSX_MENU_SESSION_ANTIALIASING},
        {"graphics.crt_filter", PSX_MENU_SESSION_CRT_FILTER},
        {"display.scanlines", PSX_MENU_SESSION_SCANLINES},
        {"display.smoothing", PSX_MENU_SESSION_SMOOTHING},
        {"display.texture_filtering", PSX_MENU_SESSION_TEXTURE_FILTERING},
        {"display.dithering", PSX_MENU_SESSION_DITHERING},
    };
    for (const auto &l : kLook)
        if (!std::strcmp(item->key, l.key)) return l.setting;
#endif
    return 0;
}
#else
bool has_session() { return false; }
#endif

bool is(const RecompRuntimeUiItem *item, const char *key) { return item && item->key && !std::strcmp(item->key, key); }
int disc_count() { return g_host && g_host->disc_count ? g_host->disc_count() : 0; }
bool netplay() { return g_host && g_host->netplay_active && g_host->netplay_active(); }

void status(const char *text) {
    std::snprintf(g_status, sizeof g_status, "%s", text);
    recomp_runtime_ui_set_status(g_ui, g_status);
}

void apply_video() {
    if (!g_host || !g_host->video_apply) return;
    const unsigned bits = g_host->video_apply(&g_video);
    if (bits & PSX_MENU_RESTART_AFTER_MATCH) {
        status("Applies after the match");
        return;  // never relaunch during netplay
    }
    g_restart = (g_restart & kSessionRestart) |
                (bits & (PSX_VIDEO_RESTART_RENDERER | PSX_VIDEO_RESTART_RESOLUTION | PSX_VIDEO_RESTART_THREADS));
    if (g_restart) status("Applies with a quick restart when you close the menu");
}

int cb_get(void *, const RecompRuntimeUiItem *item, int *out) {
#if defined(PSX_MENU_HOST_HAS_SESSION)
    if (const int s = session_of(item)) return has_session() ? g_host->session_get(s, out) : 0;
#endif
    if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) *out = g_video.fullscreen;
    else if (is(item, "graphics.renderer")) *out = g_video.renderer;
    else if (is(item, "graphics.render_thread")) *out = g_video.render_thread;
    else if (is(item, "graphics.present_thread")) *out = g_video.present_thread;
#ifdef PSX_HOST_VIDEO_HAS_VSYNC
    else if (is(item, "graphics.vsync")) *out = g_video.vsync;
#endif
    else if (is(item, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) *out = g_pause_in_menu ? 1 : 0;
    else if (is(item, "media.disc")) *out = g_disc;
    else return 0;
    return 1;
}

int cb_set(void *, const RecompRuntimeUiItem *item, int v) {
#if defined(PSX_MENU_HOST_HAS_SESSION)
    if (const int s = session_of(item)) {
        const int rc = has_session() ? g_host->session_set(s, v) : 0;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
        if (rc == PSX_MENU_SESSION_AT_RESTART) {
            g_restart |= kSessionRestart;
            status("Applies with a quick restart when you close the menu");
        }
#endif
        if (!rc && netplay()) status("Unavailable online");
        return rc != 0;
    }
#endif
    if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) g_video.fullscreen = v;
    else if (is(item, "graphics.renderer")) {
        if (v < 0 || v >= g_renderers) return 0;
        g_video.renderer = v;
    }
    else if (is(item, "graphics.render_thread")) g_video.render_thread = v ? 1 : 0;
    else if (is(item, "graphics.present_thread")) g_video.present_thread = v ? 1 : 0;
#ifdef PSX_HOST_VIDEO_HAS_VSYNC
    else if (is(item, "graphics.vsync")) g_video.vsync = v;
#endif
    else if (is(item, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) {
        g_pause_in_menu = v != 0;
        const bool want = g_pause_in_menu && recomp_runtime_ui_is_open(g_ui);
        if (g_host && g_host->set_paused) {
            if (want && !g_paused) g_paused = g_host->set_paused(1) != 0;
            else if (!want && g_paused) { g_host->set_paused(0); g_paused = false; }
        }
        return 1;
    } else if (is(item, "media.disc")) {
        if (v < 1 || v > disc_count()) return 0;
        g_disc = v;
        return 1;
    } else {
        return 0;
    }
    apply_video();
    return 1;
}

void close_menu() { recomp_runtime_ui_close(g_ui); }

int cb_action(void *, const RecompRuntimeUiItem *item) {
    if (!g_host) return 0;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    if (is(item, "system.restart_game")) {
        if (!has_session() || !g_host->session_enabled(PSX_MENU_SESSION_RESTART_GAME)) {
            status("Restart is unavailable online.");
            return 0;
        }
        if (!g_restart_armed) {
            g_restart_armed = true;
            status("Press again to restart the game. Unsaved progress is lost.");
            return 0;
        }
        g_restart_armed = false;
        g_restart = 0;
        close_menu();
        // A cold boot: not the save state or replay the game was started from.
        (void)g_host->session_set(PSX_MENU_SESSION_RESTART_GAME, 1);
        g_host->request_exit(PSX_MENU_EXIT_RELAUNCH);
        return 0;
    }
#endif
    g_restart_armed = false;
    if (is(item, RECOMP_RUNTIME_UI_KEY_RESUME)) {
        close_menu();
    } else if (is(item, RECOMP_RUNTIME_UI_KEY_RETURN_TO_LAUNCHER)) {
        close_menu();
        g_host->request_exit(PSX_MENU_EXIT_TO_FRONTEND);
    } else if (is(item, RECOMP_RUNTIME_UI_KEY_QUIT)) {
        close_menu();
        g_host->request_exit(PSX_MENU_EXIT_QUIT);
#if defined(PSX_MENU_HOST_HAS_SESSION)
    } else if (is(item, "media.reinsert")) {
        if (!has_session() || !g_host->session_set(PSX_MENU_SESSION_REINSERT_DISC, 1)) {
            status("Disc changes are unavailable in this game mode.");
            return 0;
        }
        close_menu();  // the lid cycle runs in the game
#endif
    } else if (is(item, "media.insert")) {
        const int rc = g_host->disc_insert ? g_host->disc_insert(g_disc) : -1;
        if (rc != 0) {
            status(rc == 3 ? "Disc changes are unavailable in this game mode." : "That disc could not be inserted.");
            return 0;
        }
        close_menu();  // the lid cycle runs in the game
    }
    return 0;  // keep our own status instead of the generic "Done"
}

int cb_enabled(void *, const RecompRuntimeUiItem *item) {
#if defined(PSX_MENU_HOST_HAS_SESSION)
    if (const int s = session_of(item)) return has_session() && g_host->session_enabled(s);
#endif
    if (is(item, "graphics.render_thread")) return g_video.renderer == 1;
    if (is(item, "graphics.present_thread")) return g_video.renderer == 1 && g_video.render_thread;
    if (is(item, "graphics.vsync")) return !g_vsync_overridden;
    if (item && item->key && !std::strncmp(item->key, "media.", 6)) return disc_count() > 1 && !netplay();
    return 1;
}

void cb_visibility(void *, int open) {
    if (!g_host) return;
    if (open) {
        // Every opening starts at the first section, so the menu reads the
        // same way each time (and key-driven checks are deterministic).
        recomp_runtime_ui_enter_section(g_ui, 0);
        recomp_runtime_ui_leave_section(g_ui);
        if (g_host->video_get) g_host->video_get(&g_video);
        g_restart = 0;
        g_restart_armed = false;
        const int mounted = g_host->disc_mounted ? g_host->disc_mounted() : 0;
        g_disc = mounted > 0 ? mounted : 1;
        if (disc_count() > 1) std::snprintf(g_subtitle, sizeof g_subtitle, "Mounted: Disc %d", mounted);
        else g_subtitle[0] = 0;
        g_paused = false;
        if (g_pause_in_menu && g_host->set_paused) {
            g_paused = g_host->set_paused(1) != 0;
            if (!g_paused) status("Online: the game keeps running");
        }
    } else {
        if (g_paused && g_host->set_paused) g_host->set_paused(0);
        g_paused = false;
        if (g_restart) {
            g_restart = 0;
            g_host->request_exit(PSX_MENU_EXIT_RELAUNCH);
        }
    }
}

int p_attach(void *, void *, void *, const PsxMenuHost *host) {
    std::lock_guard<std::mutex> lk(lock());
    if (!host || host->abi_version != PSX_MENU_ABI_VERSION) return 0;
    // Chosen by default (the title chose no provider): keep the title's
    // presentation -- recomp-ui's game menu is for multi-disc titles.
#if defined(PSX_MENU_HOST_HAS_DEFAULT_SELECTION)
    if (host->default_selection && (!host->disc_count || host->disc_count() < 2)) return 0;
#endif
    g_host = host;
    if (g_ui) recomp_runtime_ui_destroy(g_ui);
    const int discs = disc_count();
    g_extra[DISC].maximum = discs > 1 ? discs : 1;
    RecompLauncherCGameInfo gi{};
    if (host->launcher_game_info) host->launcher_game_info(&gi);
    g_renderers = gi.num_renderers > 0 && gi.num_renderers <= 3 ? gi.num_renderers : 2;
    g_extra[RENDERER].choice_count = g_renderers;
    g_extra[RENDERER].maximum = g_renderers - 1;
    g_extra[RENDERER].choices = gi.renderer_labels ? gi.renderer_labels : kRenderers;
#ifdef PSX_HOST_VIDEO_HAS_VSYNC
    g_extra[VSYNC].choice_count = gi.has_vsync_vrr ? 4 : 3;
    g_extra[VSYNC].maximum = gi.has_vsync_vrr ? 3 : 2;
#endif
    g_vsync_overridden = false;
    if (gi.mods && gi.mods->feature_count && gi.mods->feature_get) {
        const int count = gi.mods->feature_count(gi.mods->ctx);
        for (int i = 0; i < count; ++i) {
            RecompLauncherCModFeature feature{};
            if (gi.mods->feature_get(gi.mods->ctx, i, &feature) &&
                feature.enabled && feature.forces_vsync_off) g_vsync_overridden = true;
        }
    }
    RecompRuntimeUiStandardConfig cfg{};
    cfg.menu.title = "Game menu";
    cfg.menu.subtitle = g_subtitle;
    cfg.menu.theme = "psx";
    cfg.menu.accept_label = "Enter / Cross";
    cfg.menu.back_label = "Esc / Circle";
    cfg.menu.callbacks.get_value = cb_get;
    cfg.menu.callbacks.set_value = cb_set;
    cfg.menu.callbacks.run_action = cb_action;
    cfg.menu.callbacks.is_enabled = cb_enabled;
    cfg.menu.callbacks.visibility_changed = cb_visibility;
    cfg.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN | RECOMP_RUNTIME_UI_STANDARD_PAUSE_IN_MENU |
                   RECOMP_RUNTIME_UI_STANDARD_RESUME | RECOMP_RUNTIME_UI_STANDARD_RETURN_TO_LAUNCHER |
                   RECOMP_RUNTIME_UI_STANDARD_QUIT;
    // Window size, volume, the controller 1 port, a same-disc reinsert and
    // the look rows come from the runtime's session rows when it has them.
    const bool session = has_session();
    if (session)
        cfg.features |= RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE | RECOMP_RUNTIME_UI_STANDARD_VOLUME;
    cfg.window_scale_max = 4;
    size_t n = 0;
    const size_t base = discs > 1 ? INSERT + 1 : DISC;
    for (size_t i = 0; i < base; ++i) g_items[n++] = g_extra[i];
    if (session) {
        if (discs > 0) g_items[n++] = g_session_rows[REINSERT];
        g_items[n++] = g_session_rows[CONTROLLER_PORT];
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
        for (size_t i = LOOK_FIRST; i < kSessionRowCount; ++i) g_items[n++] = g_session_rows[i];
#endif
    }
    cfg.extra_items = g_items;
    cfg.extra_item_count = n;
    g_ui = recomp_runtime_ui_create_standard(&cfg);
    if (!g_ui) { g_host = nullptr; return 0; }
    return 1;
}

int p_event(void *, const void *ev) { return sdl_event(ev); }
int p_is_open(void *) {
    std::lock_guard<std::mutex> lk(lock());
    return g_ui && recomp_runtime_ui_is_open(g_ui);
}
void p_set_open(void *, int open) {
    std::lock_guard<std::mutex> lk(lock());
    if (!g_ui) return;
    if (open) recomp_runtime_ui_open(g_ui);
    else recomp_runtime_ui_close(g_ui);
}
void p_draw(void *, int w, int h) { draw(w, h); }
void p_detach(void *) {
    release_gl();
    std::lock_guard<std::mutex> lk(lock());
    if (g_ui) {
        recomp_runtime_ui_close(g_ui);   // drops a held pause
        recomp_runtime_ui_destroy(g_ui);
    }
    g_ui = nullptr;
    g_host = nullptr;
}

const PsxMenuProvider g_provider = {
    PSX_MENU_ABI_VERSION, "recomp-ui", nullptr,
    p_attach, p_event, p_is_open, p_set_open, p_draw, p_detach,
};

}  // namespace

std::mutex &lock() {
    static std::mutex m;
    return m;
}
RecompRuntimeUi *model() { return g_ui; }
void input(int in) {
    if (!g_ui) return;
    if (in == RECOMP_RUNTIME_UI_INPUT_TOGGLE) {
        if (recomp_runtime_ui_is_open(g_ui)) recomp_runtime_ui_close(g_ui);
        else recomp_runtime_ui_open(g_ui);
        return;
    }
    recomp_runtime_ui_handle_input(g_ui, (RecompRuntimeUiInput)in, 1, 0);
}

}  // namespace rui_psx

extern "C" const PsxMenuProvider *recomp_ui_psx_menu_provider(void) { return &rui_psx::g_provider; }
extern "C" void recomp_ui_psx_menu_input(int in) {
    std::lock_guard<std::mutex> lk(rui_psx::lock());
    rui_psx::input(in);
}

#endif  // RUI_PSX_HAS_MENU_HOST
