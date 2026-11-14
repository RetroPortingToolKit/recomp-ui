// psx_menu_provider_test: recomp-ui's psxrecomp menu provider against a fake
// PsxMenuHost (psx_menu_host.h from the psxrecomp runtime include path).
// Exercises: default-selection decline, open/pause, netplay pause refusal,
// no texture/FMV/linear filtering rows (psxrecomp's Image Filtering mod owns
// them), restart-only relaunch on close, after-match edits never relaunch,
// disc insert through the host's tray, netplay disc refusal, exits.
// With the runtime's session rows: controller 1 port, window size, volume,
// reinsert, live look rows, a restart-only look row, and Restart game.
#include "psx_menu_host.h"
#include "recomp_launcher.h"
#include "recomp_runtime_ui.h"
#include "recomp_ui_psx_menu.h"
#include "../src/consoles/psx/psx_menu_provider_internal.h"
#include "common/recomp_runtime_ui_internal.h"

#include <cstdio>
#include <cstring>

// Presentation stubs (the real ones need SDL + a GL context).
namespace rui_psx {
int sdl_event(const void *) { return 0; }
void draw(int, int) {}
void release_gl() {}
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static PsxHostVideoSettings g_now{}, g_applied{};
static int g_applies = 0, g_paused = 0, g_pause_calls = 0, g_netplay = 0, g_exit = -1;
static int g_discs = 3, g_mounted = 1, g_inserted = 0;
static unsigned g_bits_next = 0;

static int h_set_paused(int p) { ++g_pause_calls; if (p && g_netplay) return 0; g_paused = p; return 1; }
static int h_netplay(void) { return g_netplay; }
static void h_video_get(PsxHostVideoSettings *o) { *o = g_now; }
static unsigned h_video_apply(const PsxHostVideoSettings *n) {
    g_applied = *n; ++g_applies;
    unsigned bits = psx_host_video_restart_bits(&g_now, n);
    if (g_netplay && n->render_thread != g_now.render_thread) bits |= PSX_MENU_RESTART_AFTER_MATCH;
    return bits | g_bits_next;
}
static int h_mod_set(const char *, const char *, const char *, const char *) { return 1; }
static void h_reload(void) {}
static void h_exit(int how) { g_exit = how; }
static int h_disc_count(void) { return g_discs; }
static int h_disc_mounted(void) { return g_mounted; }
static const char *h_disc_path(int) { return "disc.cue"; }
static int h_disc_insert(int n) { if (g_netplay) return 3; g_inserted = n; return 0; }
static void h_game_info(RecompLauncherCGameInfo *gi) {
    static const char* const names[] = {"Software", "OpenGL", "Vulkan"};
    gi->renderer_labels = names;
    gi->num_renderers = 3;
    gi->has_vsync_vrr = 1;
}
#if defined(PSX_MENU_HOST_HAS_SESSION)
static int g_scale = 2, g_volume = 80, g_port = 0, g_reinserts = 0, g_fullscreen_host = 0, g_port_ok = 1;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
static int g_look[32];
static int look_row(int s) { return s >= PSX_MENU_SESSION_RESTART_GAME && s <= PSX_MENU_SESSION_DITHERING; }
static int look_at_restart(int s) {
    return s == PSX_MENU_SESSION_PGXP || s == PSX_MENU_SESSION_CRT_FILTER;
}
#endif
static int h_sess_enabled(int s) {
    if (s == PSX_MENU_SESSION_WINDOW_SCALE) return !g_fullscreen_host;
    if (s == PSX_MENU_SESSION_CONTROLLER_PORT) return g_port_ok;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    if (s == PSX_MENU_SESSION_RESTART_GAME) return !g_netplay;
#endif
    return 1;
}
static int h_sess_get(int s, int *o) {
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    if (look_row(s)) { *o = g_look[s]; return 1; }
#endif
    if (s == PSX_MENU_SESSION_WINDOW_SCALE) *o = g_scale;
    else if (s == PSX_MENU_SESSION_VOLUME) *o = g_volume;
    else if (s == PSX_MENU_SESSION_CONTROLLER_PORT) *o = g_port;
    else return 0;
    return 1;
}
static int h_sess_set(int s, int v) {
    if (!h_sess_enabled(s)) return 0;
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    if (look_row(s)) { g_look[s] = v; return look_at_restart(s) ? PSX_MENU_SESSION_AT_RESTART : 1; }
#endif
    if (s == PSX_MENU_SESSION_WINDOW_SCALE) g_scale = v;
    else if (s == PSX_MENU_SESSION_VOLUME) g_volume = v;
    else if (s == PSX_MENU_SESSION_CONTROLLER_PORT) g_port = v;
    else if (s == PSX_MENU_SESSION_REINSERT_DISC) ++g_reinserts;
    else return 0;
    return 1;
}
#endif


static PsxMenuHost make_host(int default_selection) {
    PsxMenuHost h{};
    h.abi_version = PSX_MENU_ABI_VERSION;
    h.set_paused = h_set_paused; h.netplay_active = h_netplay;
    h.video_get = h_video_get; h.video_apply = h_video_apply;
    h.mod_set = h_mod_set; h.reload_bindings = h_reload; h.request_exit = h_exit;
    h.disc_count = h_disc_count; h.disc_mounted = h_disc_mounted;
    h.disc_path = h_disc_path; h.disc_insert = h_disc_insert;
    h.launcher_game_info = reinterpret_cast<void (*)(struct RecompLauncherCGameInfo *)>(h_game_info);
#if defined(PSX_MENU_HOST_HAS_DEFAULT_SELECTION)
    h.default_selection = default_selection;
#else
    (void)default_selection;
#endif
    return h;
}

static void in(int i) { recomp_ui_psx_menu_input(i); }
enum { T = RECOMP_RUNTIME_UI_INPUT_TOGGLE, B = RECOMP_RUNTIME_UI_INPUT_BACK, U = RECOMP_RUNTIME_UI_INPUT_UP,
       D = RECOMP_RUNTIME_UI_INPUT_DOWN, L = RECOMP_RUNTIME_UI_INPUT_LEFT, R = RECOMP_RUNTIME_UI_INPUT_RIGHT,
       A = RECOMP_RUNTIME_UI_INPUT_ACCEPT };
// Navigate with the menu's own inputs to a row by section name and item key.
static bool go(const char *section, const char *key) {
    RecompRuntimeUi *ui = rui_psx::model();
    if (ui->in_section) in(B);
    for (size_t i = 0; i < ui->section_count && std::strcmp(ui->sections[ui->section_index], section); ++i) in(D);
    if (std::strcmp(ui->sections[ui->section_index], section)) return false;
    in(A);
    for (size_t i = 0; i < 16; ++i) {
        const RecompRuntimeUiItem *it = recomp_runtime_ui_page_entry(ui, ui->row_index, nullptr, 0);
        if (it && !std::strcmp(it->key, key)) return true;
        in(D);
    }
    return false;
}

int main() {
    const PsxMenuProvider *p = recomp_ui_psx_menu_provider();
    CHECK(p && p->abi_version == PSX_MENU_ABI_VERSION && !std::strcmp(p->name, "recomp-ui"));

#if defined(PSX_MENU_HOST_HAS_DEFAULT_SELECTION)
    // Chosen by default for a single-disc title: keep its presentation.
    g_discs = 1;
    PsxMenuHost dh = make_host(1);
    CHECK(p->attach(p->ctx, nullptr, nullptr, &dh) == 0);
#endif
    // Chosen by default for a multi-disc title, or by the title: attach.
    g_discs = 3;
    PsxMenuHost host = make_host(0);
    CHECK(p->attach(p->ctx, nullptr, nullptr, &host) == 1);
    CHECK(!p->is_open(p->ctx));

    // Open: the menu reads the host's values and pauses through it.
    g_now.renderer = 1;
    g_now.vsync = 1;
    g_now.render_thread = 1;
    in(T);
    CHECK(p->is_open(p->ctx) && g_paused == 1);

    // The game's look is not a menu row: texture, FMV and linear filtering
    // are psxrecomp's Image Filtering mod.
    CHECK(!go("Graphics", "graphics.texture_filter"));
    CHECK(!go("Graphics", RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER));
    CHECK(!go("Graphics", RECOMP_RUNTIME_UI_KEY_FMV_FILTER));

    // Close: pause dropped, nothing edited, no relaunch.
    if (rui_psx::model()->in_section) in(B);
    in(B);
    CHECK(!p->is_open(p->ctx) && g_paused == 0 && g_exit == -1 && g_applies == 0);

    // Restart-only edit: render thread off -> quick relaunch on close.
    in(T);
    CHECK(go("Graphics", "graphics.render_thread"));
    in(L);
    CHECK(g_applied.render_thread == 0);
    in(T);
    CHECK(g_exit == PSX_MENU_EXIT_RELAUNCH);
    g_exit = -1;

    // Netplay: pause refused (menu stays open, game runs); an edit that waits
    // for the match end never relaunches; disc changes refused.
    g_netplay = 1;
    in(T);
    CHECK(p->is_open(p->ctx) && g_paused == 0);
    CHECK(go("Graphics", "graphics.render_thread"));
    in(L);
    in(T);
    CHECK(g_exit == -1);
    g_netplay = 0;

    // Every offered API is selectable, including Vulkan; it takes a restart.
    in(T);
    CHECK(go("Graphics", "graphics.renderer"));
    in(R);
    CHECK(g_applied.renderer == 2);
    in(T);
    CHECK(g_exit == PSX_MENU_EXIT_RELAUNCH);
    g_exit = -1;

    // VRR is its own value and applies without restarting the game.
    in(T);
    CHECK(go("Graphics", "graphics.vsync"));
    in(R); in(R);  // On -> Adaptive -> VRR
    CHECK(g_applied.vsync == 2);
    in(T);
    CHECK(g_exit == -1);

    // The presentation thread is independently configurable under OpenGL.
    in(T);
    CHECK(go("Graphics", "graphics.present_thread"));
    in(R);
    CHECK(g_applied.present_thread == 1);
    in(T);
    CHECK(g_exit == PSX_MENU_EXIT_RELAUNCH);
    g_exit = -1;

    // Disc: select disc 2 and insert through the host's tray.
    g_mounted = 1;
    in(T);
    CHECK(go("Disc", "media.disc"));
    in(R);
    CHECK(go("Disc", "media.insert"));
    in(A);
    CHECK(g_inserted == 2 && !p->is_open(p->ctx));

    // Exits (both confirm with a second press).
    in(T);
    CHECK(go("System", RECOMP_RUNTIME_UI_KEY_RETURN_TO_LAUNCHER));
    in(A); in(A);
    CHECK(g_exit == PSX_MENU_EXIT_TO_FRONTEND && !p->is_open(p->ctx));
    in(T);
    CHECK(go("System", RECOMP_RUNTIME_UI_KEY_QUIT));
    in(A); in(A);
    CHECK(g_exit == PSX_MENU_EXIT_QUIT);

#if defined(PSX_MENU_HOST_HAS_SESSION)
    // Without the runtime's session rows the menu has none of them.
    in(T);
    CHECK(!go("Input", "input.controller_port"));
    CHECK(!go("Display", RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE));
    CHECK(!go("Disc", "media.reinsert"));
    if (rui_psx::model()->in_section) in(B);
    in(B);
    p->detach(p->ctx);

    // With them: controller 1 port, window size, volume and reinsert.
    PsxMenuHost sh = make_host(0);
    sh.session_get = h_sess_get; sh.session_set = h_sess_set; sh.session_enabled = h_sess_enabled;
    CHECK(p->attach(p->ctx, nullptr, nullptr, &sh) == 1);
    in(T);
    CHECK(go("Input", "input.controller_port"));
    in(R);
    CHECK(g_port == 1);
    in(L);
    CHECK(g_port == 0);
    g_port_ok = 0;  // netplay or a multitap: the row refuses
    in(R);
    CHECK(g_port == 0);
    g_port_ok = 1;
    CHECK(go("Display", RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE));
    in(R);
    CHECK(g_scale == 3);
    g_fullscreen_host = 1;  // fullscreen owns the window size
    in(R);
    CHECK(g_scale == 3);
    g_fullscreen_host = 0;
    CHECK(go("Audio", RECOMP_RUNTIME_UI_KEY_VOLUME));
    in(L);
    CHECK(g_volume < 80);
    CHECK(go("Disc", "media.reinsert"));
    in(A);
    CHECK(g_reinserts == 1 && !p->is_open(p->ctx));
#if defined(PSX_MENU_HOST_HAS_LOOK_ROWS)
    // Live look rows apply without a relaunch.
    g_exit = -1;
    in(T);
    CHECK(go("Graphics", "graphics.internal_resolution"));
    in(R);
    CHECK(g_look[PSX_MENU_SESSION_INTERNAL_RESOLUTION] == 1);
    CHECK(go("Display", "display.dithering"));
    in(R);
    CHECK(g_look[PSX_MENU_SESSION_DITHERING] == 1);
    CHECK(go("Display", "display.texture_filtering"));
    in(R); in(R);
    CHECK(g_look[PSX_MENU_SESSION_TEXTURE_FILTERING] == 2);
    if (rui_psx::model()->in_section) in(B);
    in(B);
    CHECK(!p->is_open(p->ctx) && g_exit == -1);
    // A restart-only row (PGXP) relaunches when the menu closes.
    in(T);
    CHECK(go("Graphics", "graphics.pgxp"));
    in(R);
    CHECK(g_look[PSX_MENU_SESSION_PGXP] == 1);
    in(T);
    CHECK(!p->is_open(p->ctx) && g_exit == PSX_MENU_EXIT_RELAUNCH);
    g_exit = -1;
    // Restart game asks twice, and is refused online. The second press tells
    // the host the relaunch is a cold boot before it asks for the relaunch.
    g_look[PSX_MENU_SESSION_RESTART_GAME] = 0;
    in(T);
    CHECK(go("System", "system.restart_game"));
    in(A);
    CHECK(p->is_open(p->ctx) && g_exit == -1 && g_look[PSX_MENU_SESSION_RESTART_GAME] == 0);
    in(A);
    CHECK(!p->is_open(p->ctx) && g_exit == PSX_MENU_EXIT_RELAUNCH);
    CHECK(g_look[PSX_MENU_SESSION_RESTART_GAME] == 1);
    g_exit = -1;
    g_look[PSX_MENU_SESSION_RESTART_GAME] = 0;
    g_netplay = 1;
    in(T);
    CHECK(go("System", "system.restart_game"));
    in(A); in(A);
    CHECK(g_exit == -1);
    in(T);
    g_netplay = 0;
    CHECK(!p->is_open(p->ctx));
#endif
#endif

    // Detach while open drops the pause.
    in(T);
    CHECK(g_paused == 1);
    p->detach(p->ctx);
    CHECK(g_paused == 0 && !p->is_open(p->ctx));

    if (!g_fail) std::printf("psx_menu_provider_test: ok\n");
    return g_fail ? 1 : 0;
}
