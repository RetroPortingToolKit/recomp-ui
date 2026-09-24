// launcher_imgui.cpp — Dear ImGui (MIT) backend for the next-gen launcher.
//
// Draws the shared LauncherModel with Dear ImGui + SDL3 + OpenGL3, at parity
// with the shipping legacy MMX launcher (box art, controller art, and all
// panels). Icons are drawn as vector primitives rather than font glyphs so they
// stay crisp at any DPI and don't depend on the text font's glyph coverage.
// Demonstrates the two hard requirements:
//   (1) DPI: fonts re-rasterize at (logical size * display_scale); style
//       re-scales on display-scale change -> crisp at 125/150/175% + monitors.
//   (2) Live resize: immediate mode redraws every frame; a logical-width
//       breakpoint switches the dashboard between two columns and one column.

#include "launcher_backend.h"
#include "launcher_boot_timing.h"
#include "launcher_gl.h"
#include "launcher_input.h"
#include "launcher_files.h"
#include "launcher_debug.h"
#include "launcher_binds.h"
#include "launcher_udp_port.h"
#include "launcher_panels.h"
#include "launcher_system.h"
#include "launcher_i18n.h"
#include "recomp_moderation.h"   // local ignore/block list (online only)
#include "consoles/n64/n64_binds.h"   // RUI_N64_FIELD_* for the pad-capture path

#include "launcher_sdlcompat.h"   // pulls the right SDL header + event shim

#include "imgui.h"
#include "launcher_nav.h"
#if defined(LNG_SDL3)
  #include "imgui_impl_sdl3.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL3_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL3_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL3_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL3_Shutdown
#else
  #include "imgui_impl_sdl2.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL2_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL2_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL2_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL2_Shutdown
#endif
#include "imgui_impl_opengl3.h"

#include <atomic>
#if defined(_MSC_VER)
  #include <intrin.h>
#endif

// ---- Dear ImGui version compatibility ----------------------------------------
// recomp-ui's vendored ImGui is 1.91.x, but a host can reuse its OWN single
// ImGui copy via recomp_ui.cmake HOST_IMGUI (e.g. gb-recompiled vendors 1.90.4;
// an rt64 host links 1.90.x). Map the few 1.91-renamed identifiers this file
// uses onto their 1.90 spellings so the same source compiles against either.
// (ConfigNavCursorVisibleAlways has no 1.90 equivalent — it is #if-guarded at
// its use site.)
#if !defined(IMGUI_VERSION_NUM) || IMGUI_VERSION_NUM < 19100
  #ifndef ImGuiChildFlags_Borders
  #define ImGuiChildFlags_Borders     ImGuiChildFlags_Border
  #endif
  #ifndef ImGuiCol_NavCursor
  #define ImGuiCol_NavCursor          ImGuiCol_NavHighlight
  #endif
  #ifndef ImGuiButtonFlags_EnableNav
  // 1.90's InvisibleButton participates in nav by default; the opt-in flag is 0.
  #define ImGuiButtonFlags_EnableNav  0
  #endif
#endif

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

extern "C" const char* launcher_backend_name(void) { return "Dear ImGui"; }

static const char* ui_text(const char* en) {
    return launcher_i18n_text(en);
}

struct ShaderPresetEntry {
    std::string label;
    std::string path;
};

static std::vector<ShaderPresetEntry> g_shader_presets;

static bool shader_path_has_supported_ext(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".glsl" || ext == ".glslp";
}

static std::string shader_label_from_relative_path(std::filesystem::path rel) {
    rel.replace_extension();
    std::string label = rel.generic_string();
    for (char& c : label)
        if (c == '/' || c == '_' || c == '-') c = ' ';
    bool cap = true;
    for (char& c : label) {
        if (std::isspace((unsigned char)c)) {
            cap = true;
        } else if (cap) {
            c = (char)std::toupper((unsigned char)c);
            cap = false;
        }
    }
    return label;
}

static bool shader_relative_path_is_private(const std::filesystem::path& rel) {
    for (const std::filesystem::path& part : rel) {
        std::string s = part.string();
        if (!s.empty() && (s[0] == '_' || s[0] == '.'))
            return true;
    }
    return false;
}

static void refresh_shader_presets() {
    g_shader_presets.clear();
    const std::filesystem::path root = std::filesystem::path("assets") / "shaders";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !shader_path_has_supported_ext(it->path()))
            continue;
        std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (shader_relative_path_is_private(rel))
            continue;
        g_shader_presets.push_back({
            shader_label_from_relative_path(rel),
            it->path().generic_string()
        });
    }
    std::sort(g_shader_presets.begin(), g_shader_presets.end(),
              [](const ShaderPresetEntry& a, const ShaderPresetEntry& b) {
                  return a.label < b.label;
              });
}

// `volatile` on purpose. Under a host build with -Os -ffunction-sections
// -fdata-sections + -Wl,--gc-sections (gb-recompiled's generated projects),
// GCC 15.2 miscompiled the plain global: a store to g_th did not stick (read
// back NULL at the same address unless another statement intervened). Marking
// it volatile forces every read/write to hit memory and sidesteps the bug.
// The theme pointer is set once per launcher run and read every frame, so the
// volatile access cost is irrelevant.
const LauncherTheme* volatile g_th = nullptr;

namespace {

// ImGui coordinates are DPI-independent: the platform layer reports the window
// in LOGICAL units and the GL backend applies DisplayFramebufferScale when it
// submits vertices to the (Retina/HiDPI) drawable. Where the OS has no
// point/pixel split of its own (Windows, X11) that split is synthesized —
// see apply_logical_display below — so this holds on every platform.
// Scaling widget geometry here too would DOUBLE every size on HiDPI (and make
// labels collide with their controls), so keep all layout tokens logical.
// Font SIZES are logical too; only their raster density follows the display
// (see apply_scale).
// (Ported from launcher_ng's "Fix launcher DPI layout and text alignment".)
float  px(float logical) { return logical; }
ImVec4 col(const LngColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

/* ---- HiDPI: window coordinates <-> logical units ---------------------------
 *
 * The platform layer hands us a logical size and a pixel size (see
 * launcher_platform_refresh_metrics). Where SDL reports no point/pixel split
 * of its own — Windows, X11 — it synthesizes one from the display scale, and
 * p->input_scale is then the number of SDL window coordinates per logical
 * unit. Everything below is a no-op at input_scale 1.0, which is every
 * platform that carries its density in the pixel size (macOS retina, Wayland).
 */

/* SDL2 spells mouse coordinates Sint32 and SDL3 spells them float; one
 * template covers both without an #if at each field. */
template <typename T> inline void div_coord(T& v, float s) {
    v = (T)((float)v / s);
}

/* Mouse input arrives in window coordinates. Move it into the logical units
 * the UI is laid out in, before ImGui or the bind-capture path sees it. */
void scale_mouse_event(SDL_Event& e, float coords) {
    if (coords == 1.0f) return;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        div_coord(e.motion.x, coords);
        div_coord(e.motion.y, coords);
        div_coord(e.motion.xrel, coords);
        div_coord(e.motion.yrel, coords);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        div_coord(e.button.x, coords);
        div_coord(e.button.y, coords);
        break;
#if defined(LNG_SDL3)
    case SDL_EVENT_MOUSE_WHEEL:
        div_coord(e.wheel.mouse_x, coords);
        div_coord(e.wheel.mouse_y, coords);
        break;
#endif
    default:
        break;
    }
}

/* True where the SDL backend re-asserts the cursor from the global mouse
 * state every frame (the same driver whitelist it uses). Elsewhere — Wayland —
 * there is no global cursor and motion events are the only source, so asking
 * for one would pin the pointer to the window origin. */
bool sdl_has_global_mouse(void) {
    const char* drv = SDL_GetCurrentVideoDriver();
    static const char* kWhitelist[] = {"windows", "cocoa", "x11", "DIVE", "VMAN"};
    if (!drv) return false;
    for (const char* w : kWhitelist)
        if (std::strncmp(drv, w, std::strlen(w)) == 0) return true;
    return false;
}

/* Install the logical coordinate space for the frame about to be built.
 *
 * Runs after the SDL backend's NewFrame — which fills io.DisplaySize with the
 * WINDOW size and queues an unscaled cursor position — and before
 * ImGui::NewFrame(), so the values here are the ones that take effect. The GL
 * backend multiplies DisplaySize by FramebufferScale for its viewport and
 * scissor rects, which is what turns a logical-sized layout into a
 * pixel-resolution frame. */
void apply_logical_display(const LauncherPlatform* p) {
    if (!p || !p->window || p->input_scale == 1.0f) return;
    if (p->logical_w <= 0 || p->logical_h <= 0) return;

    ImGuiIO& io = ImGui::GetIO();
    if (SDL_GetWindowFlags(p->window) & SDL_WINDOW_MINIMIZED) {
        io.DisplaySize = ImVec2(0.0f, 0.0f);   // as the backend would have it
        return;
    }
    io.DisplaySize = ImVec2((float)p->logical_w, (float)p->logical_h);
    io.DisplayFramebufferScale = ImVec2(p->display_scale, p->display_scale);

    if (sdl_has_global_mouse() && SDL_GetKeyboardFocus() == p->window) {
        int wx = 0, wy = 0;
        SDL_GetWindowPosition(p->window, &wx, &wy);
#if defined(LNG_SDL3)
        float gx = 0.0f, gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
#else
        int gxi = 0, gyi = 0;
        SDL_GetGlobalMouseState(&gxi, &gyi);
        const float gx = (float)gxi, gy = (float)gyi;
#endif
        io.AddMousePosEvent((gx - (float)wx) / p->input_scale,
                            (gy - (float)wy) / p->input_scale);
    }
}

/* Auto Map All run state.
 *
 * File scope, not a function static, because the gamepad-navigation suppressor
 * in the event loop has to see it. A run spends one frame between steps with
 * m->capturing false -- the capture has committed and the next has not begun --
 * and the nav flag is recomputed BEFORE the panel code that starts the next
 * step. For that frame the pad drove the menu instead of the mapping, so a
 * twelve-button run scattered focus twelve times. */
static int s_automap_i = -1;      /* next cell to capture, -1 = idle */
static int s_automap_player = -1; /* a run belongs to one player */
static inline bool automap_in_progress(void) { return s_automap_i >= 0; }


/* A download glyph -- arrow into a tray -- drawn rather than glyphed because
 * the launcher ships no icon font (see the font loader above: body + optional
 * JP face, nothing pictographic). Sized from the row height so it lines up
 * with the text beside it at any font size.
 *
 * Returns true on click. `enabled` false draws it dimmed and inert, which is
 * how a row that is already installed, or already transferring, is shown. */
bool download_icon_button(const char* id, float side, bool enabled,
                          const LauncherTheme& th, const char* tooltip)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = false;

    if (enabled) {
        clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
    } else {
        ImGui::Dummy(ImVec2(side, side));
    }
    const bool hovered = ImGui::IsItemHovered();
    const bool active_hover = enabled && hovered;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImVec2(p0.x + side * 0.5f, p0.y + side * 0.5f);
    ImVec4 tint = enabled ? col(active_hover ? th.accent : th.text)
                          : col(th.text_muted);
    if (!enabled) tint.w *= 0.45f;
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(tint);
    const float t = (side * 0.09f) > 1.0f ? (side * 0.09f) : 1.0f; /* stroke */
    const float a = side * 0.30f;                /* arrow half-height */
    const float w = side * 0.24f;                /* arrowhead half-width */

    if (active_hover) {
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + side, p0.y + side),
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(tint.x, tint.y, tint.z, 0.14f)),
                          side * 0.2f);
    }
    /* shaft */
    dl->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a * 0.35f), ink, t);
    /* head */
    dl->AddTriangleFilled(ImVec2(c.x - w, c.y + a * 0.05f),
                          ImVec2(c.x + w, c.y + a * 0.05f),
                          ImVec2(c.x, c.y + a * 0.62f), ink);
    /* tray */
    dl->AddLine(ImVec2(c.x - w * 1.35f, c.y + a * 0.80f),
                ImVec2(c.x + w * 1.35f, c.y + a * 0.80f), ink, t);
    dl->AddLine(ImVec2(c.x - w * 1.35f, c.y + a * 0.80f),
                ImVec2(c.x - w * 1.35f, c.y + a * 0.42f), ink, t);
    dl->AddLine(ImVec2(c.x + w * 1.35f, c.y + a * 0.80f),
                ImVec2(c.x + w * 1.35f, c.y + a * 0.42f), ink, t);

    if (hovered && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}
// g_th moved to external linkage above the anonymous namespace (see note).

LauncherTexture g_boxart, g_pad, g_pad_analog, g_pad_digital, g_brand, g_memcard;
// Optional platform wordmark (SystemProfile.wordmark_image) — rendered in the
// header instead of the platform text when the asset is present. Absent => text.
LauncherTexture g_wordmark;
// N64 Transfer Pak cartridge art, indexed by host cart_kind: [0] empty/unknown
// (gray GB shell), [1] red, [2] blue, [3] yellow, [4] green. Loaded only for a
// tpak game; real GB cart PNGs from the legacy launchers (assets/consoles/n64).
LauncherTexture g_cart[5];
// Disc-verdict icons (verify.mode==1 systems, e.g. PSX) — keyed by
// VerifyResult.verdict (0 none,1 ok,2 warn,3 bad); see draw_verdict_block().
LauncherTexture g_verdict_ok, g_verdict_warn, g_verdict_bad, g_verdict_none;
ImTextureID tid(const LauncherTexture& t) { return (ImTextureID)(intptr_t)t.id; }

LauncherPad g_pads[LNG_MAX_PADS];   // live gamepad list (repolled every frame)
int         g_pad_count = 0;

// Gamepad navigation is off until a real pad event arrives. A control already
// asserted when the launcher opens -- a stuck axis, a wedged virtual device --
// generates no event, so it can never drive the menu or press the focused
// PLAY button. See the arming note in the event loop.
static bool s_pad_nav_armed = false;

char        g_pick_buf[512] = {};    // ROM picker result

/* ---------------------------------------------------------------------------
 * File pickers: ONE entry point, and a fallback nothing can bypass.
 *
 * ui_pick() is the only way this backend opens a picker. It tries the host's
 * native dialog first and, when that reports itself unusable (-1), browses
 * in-app instead. Callers never learn which happened.
 *
 * The completion CLOSURE is what makes that universal. The old model routed a
 * fixed enum -- BuiltinPickerKind {Rom, Bios, SetupToolchainZip, DiscSlot} --
 * into the browser so it could apply the result itself. A picker therefore got
 * a fallback only by adding an enum case, so 14 of 17 sites just called the
 * blocking launcher_pick_* API instead and silently did nothing on a host with
 * no usable zenity/kdialog. The enum had already buckled once: a disc-slot
 * index does not fit in it and rode a separate global. With on_pick carrying
 * the follow-up, a new picker anywhere gets the fallback for free, and the
 * browser never learns what the caller wanted.
 *
 * The blocking launcher_pick_* wrappers are deliberately not called from this
 * file; tests/check_no_raw_pickers.py fails the build if they come back.
 * ------------------------------------------------------------------------ */

enum class PickMode { OpenFile, SaveFile, Folder };

struct PickRequest {
    PickMode mode = PickMode::OpenFile;
    std::string title;
    std::vector<std::string> patterns;
    std::string description;
    /* File or folder the browser should open on. Empty = work it out. */
    std::string start_path;
    /* Render inline inside the setup wizard rather than as its own modal. */
    bool from_setup = false;
    /* The chosen path, or nullptr if the user cancelled. Called in the same
     * frame as the click on the native path, and whenever the user confirms on
     * the built-in path -- callers must not assume either. */
    std::function<void(const char*)> on_pick;
};

struct BuiltinPickerState {
    bool active = false;
    bool from_setup = false;
    bool focus_path = false;
    PickMode mode = PickMode::OpenFile;
    char title[96] = "Select file";
    char directory[1024] = {};
    char selected[1024] = {};
    char error[256] = {};
    /* Save mode only: set once the player has confirmed an overwrite. */
    bool overwrite_armed = false;
    std::vector<std::string> patterns;
    std::string description;
    std::function<void(const char*)> on_pick;
};
BuiltinPickerState g_picker;

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool builtin_picker_matches(const std::filesystem::path& path) {
    if (g_picker.patterns.empty()) return true;
    const std::string filename = lower_ascii(path.filename().string());
    for (const std::string& raw : g_picker.patterns) {
        const std::string pattern = lower_ascii(raw);
        if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
        if (pattern.size() > 1 && pattern[0] == '*' &&
            filename.size() >= pattern.size() - 1 &&
            filename.compare(filename.size() - (pattern.size() - 1),
                             pattern.size() - 1, pattern.substr(1)) == 0)
            return true;
        if (filename == pattern) return true;
    }
    return false;
}

/* Where the browser opens. The caller's hint wins; the rest is the generic
 * "somewhere the player recognises" chain. */
static std::filesystem::path builtin_picker_initial_path(
        LauncherModel* m, const std::string& hint) {
    std::error_code ec;
    if (!hint.empty()) {
        std::filesystem::path hinted(hint);
        if (std::filesystem::is_regular_file(hinted, ec) ||
            std::filesystem::is_directory(hinted, ec))
            return hinted;
    }
    if (m && m->rom_full[0]) {
        ec.clear();
        std::filesystem::path current(m->rom_full);
        if (std::filesystem::is_regular_file(current, ec)) return current;
    }
    if (const char* image = std::getenv("RECOMP_APPIMAGE_PATH")) {
        std::filesystem::path appimage(image);
        ec.clear();
        if (std::filesystem::exists(appimage.parent_path(), ec))
            return appimage.parent_path();
    }
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home);
    ec.clear();
    return std::filesystem::current_path(ec);
}

/* Prefer native (zenity/kdialog on Linux, tinyfiledialogs elsewhere). Fall
 * back to the in-app browser when native is unavailable or returns -1. */
static bool prefer_builtin_file_picker(void) {
#if defined(__linux__)
    if (const char* env = std::getenv("RECOMP_UI_BUILTIN_FILE_PICKER")) {
        return env[0] != '\0' && std::strcmp(env, "0") != 0 &&
               std::strcmp(env, "false") != 0 &&
               std::strcmp(env, "FALSE") != 0;
    }
#endif
    return false;
}

/* Set once a native backend has reported itself unusable (-1). The ROM row
 * then keeps an explicit "Use the built-in browser" control visible, so the
 * player is never left with a button whose only native backend is broken. */
static bool g_native_picker_failed = false;

/* True when the ROM row should show that explicit control: either the host has
 * no zenity/kdialog at all, or the one it has has already failed on us. */
static bool builtin_file_picker_should_be_offered(void) {
    if (prefer_builtin_file_picker()) return false;
    return g_native_picker_failed || !launcher_native_file_picker_available();
}

static void open_builtin_picker(LauncherModel* m, PickRequest&& req) {
    g_picker = BuiltinPickerState{};
    g_picker.active = true;
    g_picker.mode = req.mode;
    g_picker.from_setup = req.from_setup;
    g_picker.patterns = std::move(req.patterns);
    g_picker.description = std::move(req.description);
    g_picker.on_pick = std::move(req.on_pick);

    const char* fallback_title = req.mode == PickMode::Folder     ? "Select folder"
                                 : req.mode == PickMode::SaveFile ? "Save file"
                                                                  : "Select file";
    std::snprintf(g_picker.title, sizeof(g_picker.title), "%s",
                  !req.title.empty() ? req.title.c_str() : fallback_title);

    std::filesystem::path initial = builtin_picker_initial_path(m, req.start_path);
    std::error_code ec;
    if (std::filesystem::is_regular_file(initial, ec)) {
        if (req.mode != PickMode::Folder)
            std::snprintf(g_picker.selected, sizeof(g_picker.selected), "%s",
                          initial.string().c_str());
        initial = initial.parent_path();
    }
    if (initial.empty() || !std::filesystem::is_directory(initial, ec))
        initial = std::filesystem::current_path(ec);
    std::snprintf(g_picker.directory, sizeof(g_picker.directory), "%s",
                  initial.string().c_str());
    g_picker.focus_path = true;
}

/* Close the browser and hand the answer back. State is cleared BEFORE the
 * callback runs, so an on_pick that opens another picker is not immediately
 * stomped by this one's teardown. */
static void builtin_picker_finish(const char* path) {
    std::function<void(const char*)> cb = std::move(g_picker.on_pick);
    g_picker = BuiltinPickerState{};
    if (cb) cb(path);
}

/* THE picker entry point. Native first, built-in browser when native cannot
 * run. Every Browse button in this file goes through here. */
static void ui_pick(LauncherModel* m, PickRequest req) {
    if (!prefer_builtin_file_picker() &&
        launcher_native_file_picker_available()) {
        std::vector<const char*> pats;
        pats.reserve(req.patterns.size());
        for (const std::string& pat : req.patterns) pats.push_back(pat.c_str());
        const char* const* pp = pats.empty() ? nullptr : pats.data();
        const int npat = (int)pats.size();
        const char* desc =
            req.description.empty() ? nullptr : req.description.c_str();
        const char* title = req.title.empty() ? nullptr : req.title.c_str();

        int r = -1;
        switch (req.mode) {
        case PickMode::OpenFile:
            r = launcher_try_pick_file(title, pp, npat, desc, g_pick_buf,
                                       sizeof(g_pick_buf));
            break;
        case PickMode::SaveFile:
            r = launcher_try_pick_save_file(title, pp, npat, desc, g_pick_buf,
                                            sizeof(g_pick_buf));
            break;
        case PickMode::Folder:
            r = launcher_try_pick_folder(title, g_pick_buf, sizeof(g_pick_buf));
            break;
        }
        if (r == 1) {
            if (req.on_pick) req.on_pick(g_pick_buf);
            return;
        }
        if (r == 0) {
            if (req.on_pick) req.on_pick(nullptr);
            return;
        }
        /* r == -1: the backend is unusable (it could not start, died, or
         * exited with something other than 0/1 -- launcher_files.c has already
         * logged which and why). Remember that, and browse in-app. */
        g_native_picker_failed = true;
    }
    open_builtin_picker(m, std::move(req));
}

/* Shorthands for the three shapes, so a call site stays one statement. */
static void ui_pick_file(LauncherModel* m, const char* title,
                         std::vector<std::string> patterns,
                         const char* description,
                         std::function<void(const char*)> on_pick) {
    PickRequest req;
    req.mode = PickMode::OpenFile;
    if (title) req.title = title;
    req.patterns = std::move(patterns);
    if (description) req.description = description;
    req.on_pick = std::move(on_pick);
    ui_pick(m, std::move(req));
}

static void ui_pick_save_file(LauncherModel* m, const char* title,
                              std::vector<std::string> patterns,
                              const char* description,
                              std::function<void(const char*)> on_pick) {
    PickRequest req;
    req.mode = PickMode::SaveFile;
    if (title) req.title = title;
    req.patterns = std::move(patterns);
    if (description) req.description = description;
    req.on_pick = std::move(on_pick);
    ui_pick(m, std::move(req));
}

static void ui_pick_folder(LauncherModel* m, const char* title,
                           std::function<void(const char*)> on_pick) {
    PickRequest req;
    req.mode = PickMode::Folder;
    if (title) req.title = title;
    req.on_pick = std::move(on_pick);
    ui_pick(m, std::move(req));
}

/* The ROM row remembers the disc it already has, then any hint the host left. */
static std::string rom_picker_start_path(LauncherModel* m) {
    std::error_code ec;
    if (m && m->rom_full[0] &&
        std::filesystem::is_regular_file(std::filesystem::path(m->rom_full), ec))
        return m->rom_full;
    if (const char* hint = std::getenv("RECOMP_DISC_HINT")) {
        ec.clear();
        if (std::filesystem::is_regular_file(std::filesystem::path(hint), ec))
            return hint;
    }
    return std::string();
}

static PickRequest rom_pick_request(LauncherModel* m, const char* title,
                                    const char* const* patterns,
                                    int pattern_count, const char* description,
                                    bool from_setup) {
    PickRequest req;
    req.title = title && title[0] ? title : "Select game file";
    for (int i = 0; patterns && i < pattern_count; ++i)
        if (patterns[i]) req.patterns.emplace_back(patterns[i]);
    if (description) req.description = description;
    req.start_path = rom_picker_start_path(m);
    req.from_setup = from_setup;
    return req;
}

static void request_rom_picker(LauncherModel* m, const char* title,
                               const char* const* patterns, int pattern_count,
                               const char* description, bool from_setup) {
    PickRequest req = rom_pick_request(m, title, patterns, pattern_count,
                                       description, from_setup);
    req.on_pick = [m](const char* path) {
        if (path) launcher_model_set_rom(m, path);
    };
    ui_pick(m, std::move(req));
}

/* Browse for ONE disc of a set. Binding is by slot, so filling in disc 3 does
 * not change which disc is mounted (launcher_model_set_disc_path). The slot
 * rides the closure; it used to need a global of its own. */
static void request_disc_slot_picker(LauncherModel* m, int slot,
                                     const char* title,
                                     const char* const* patterns,
                                     int pattern_count, const char* description,
                                     bool from_setup) {
    PickRequest req = rom_pick_request(m, title, patterns, pattern_count,
                                       description, from_setup);
    req.on_pick = [m, slot](const char* path) {
        if (path) launcher_model_set_disc_path(m, slot, path);
    };
    ui_pick(m, std::move(req));
}

static void request_bios_picker(LauncherModel* m, const char* title,
                                bool from_setup) {
    PickRequest req;
    req.title = title && title[0] ? title : "Select BIOS file";
    req.patterns = {"*.bin", "*.rom"};
    req.description = "BIOS image (.bin .rom)";
    if (m && m->s.bios_path[0]) req.start_path = m->s.bios_path;
    req.from_setup = from_setup;
    req.on_pick = [m](const char* path) {
        if (path) launcher_model_request_bios_path(m, path);
    };
    ui_pick(m, std::move(req));
}

static void draw_builtin_rom_picker_contents(LauncherModel* m,
                                             const LauncherTheme& th,
                                             bool standalone_popup) {
    namespace fs = std::filesystem;
    ImGui::TextColored(col(th.accent), "%s", g_picker.title);
    ImGui::TextColored(col(th.text_muted),
                       "Built-in browser (no desktop file-picker service required)");
    if (!g_picker.description.empty())
        ImGui::TextColored(col(th.text_muted), "Showing: %s",
                           g_picker.description.c_str());
    ImGui::Dummy(ImVec2(0, px(6)));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ui_text("Folder"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(500));
    bool enter_dir = ImGui::InputText("##builtin_picker_directory",
                                      g_picker.directory,
                                      sizeof(g_picker.directory),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter_dir) {
        std::error_code ec;
        if (!fs::is_directory(fs::path(g_picker.directory), ec))
            std::snprintf(g_picker.error, sizeof(g_picker.error),
                          "That folder does not exist or cannot be opened.");
        else
            g_picker.error[0] = '\0';
    }

    if (ImGui::Button("Up", ImVec2(px(82), px(30)))) {
        fs::path parent = fs::path(g_picker.directory).parent_path();
        if (!parent.empty()) {
            std::snprintf(g_picker.directory, sizeof(g_picker.directory),
                          "%s", parent.string().c_str());
            g_picker.selected[0] = '\0';
            g_picker.error[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Home", ImVec2(px(82), px(30)))) {
        if (const char* home = std::getenv("HOME")) {
            std::snprintf(g_picker.directory, sizeof(g_picker.directory),
                          "%s", home);
            g_picker.selected[0] = '\0';
            g_picker.error[0] = '\0';
        }
    }

    struct PickerEntry {
        fs::path path;
        bool directory;
    };
    std::vector<PickerEntry> entries;
    std::error_code ec;
    const fs::path directory(g_picker.directory);
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    if (!ec) {
        for (const fs::directory_entry& entry : it) {
            std::error_code type_ec;
            const bool is_dir = entry.is_directory(type_ec);
            if (type_ec) continue;
            /* Folder mode lists folders only: a file is never the answer. */
            if (g_picker.mode == PickMode::Folder) {
                if (is_dir) entries.push_back({entry.path(), true});
            } else if (is_dir || builtin_picker_matches(entry.path())) {
                entries.push_back({entry.path(), is_dir});
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const PickerEntry& a, const PickerEntry& b) {
                      if (a.directory != b.directory) return a.directory > b.directory;
                      return lower_ascii(a.path.filename().string()) <
                             lower_ascii(b.path.filename().string());
                  });
    } else if (!g_picker.error[0]) {
        std::snprintf(g_picker.error, sizeof(g_picker.error),
                      "Unable to open this folder.");
    }

    ImGui::BeginChild("##builtin_picker_entries", ImVec2(0, px(300)),
                      ImGuiChildFlags_Borders);
    for (const PickerEntry& entry : entries) {
        const std::string name = entry.path.filename().string();
        std::string label = entry.directory ? "[Folder] " + name : name;
        label += "##" + entry.path.string();
        const bool selected =
            !entry.directory && entry.path.string() == g_picker.selected;
        if (ImGui::Selectable(label.c_str(), selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (entry.directory) {
                std::snprintf(g_picker.directory,
                              sizeof(g_picker.directory), "%s",
                              entry.path.string().c_str());
                g_picker.selected[0] = '\0';
                g_picker.error[0] = '\0';
            } else {
                std::snprintf(g_picker.selected,
                              sizeof(g_picker.selected), "%s",
                              entry.path.string().c_str());
                g_picker.error[0] = '\0';
            }
        }
    }
    ImGui::EndChild();

    /* Folder mode has nothing to type a filename into: the folder field above
     * is the answer. */
    if (g_picker.mode != PickMode::Folder) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(g_picker.mode == PickMode::SaveFile ? "Save as"
                                                                  : "File");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (g_picker.focus_path) {
            ImGui::SetKeyboardFocusHere();
            g_picker.focus_path = false;
        }
        if (ImGui::InputText("##builtin_picker_selected", g_picker.selected,
                             sizeof(g_picker.selected)))
            g_picker.overwrite_armed = false; /* retyping re-arms the guard */
    }
    if (g_picker.error[0])
        ImGui::TextColored(col(th.warn), "%s", g_picker.error);

    ImGui::Dummy(ImVec2(0, px(6)));

    const char* confirm_label = g_picker.mode == PickMode::Folder ? "Use this folder"
                                : g_picker.mode == PickMode::SaveFile
                                      ? (g_picker.overwrite_armed ? "Overwrite" : "Save here")
                                      : "Use selected file";

    if (ImGui::Button(confirm_label, ImVec2(px(180), px(34)))) {
        std::error_code file_ec;
        if (g_picker.mode == PickMode::Folder) {
            fs::path chosen(g_picker.directory);
            if (fs::is_directory(chosen, file_ec)) {
                const std::string out = chosen.string();
                if (standalone_popup) ImGui::CloseCurrentPopup();
                builtin_picker_finish(out.c_str());
                return;
            }
            std::snprintf(g_picker.error, sizeof(g_picker.error), "%s",
                          "Open an existing folder first.");
        } else if (g_picker.mode == PickMode::SaveFile) {
            /* A save destination need NOT exist yet -- that is the whole point
             * of the mode. What must exist is the folder it lands in. */
            fs::path target(g_picker.selected);
            if (target.empty() || !target.has_filename()) {
                std::snprintf(g_picker.error, sizeof(g_picker.error), "%s",
                              "Type a file name to save as.");
            } else {
                if (target.is_relative())
                    target = fs::path(g_picker.directory) / target;
                fs::path parent = target.parent_path();
                if (parent.empty()) parent = fs::path(g_picker.directory);
                if (!fs::is_directory(parent, file_ec)) {
                    std::snprintf(g_picker.error, sizeof(g_picker.error), "%s",
                                  "That folder does not exist.");
                } else if (fs::exists(target, file_ec) &&
                           !g_picker.overwrite_armed) {
                    g_picker.overwrite_armed = true;
                    std::snprintf(g_picker.error, sizeof(g_picker.error), "%s",
                                  "That file already exists. Press Overwrite "
                                  "to replace it.");
                } else {
                    const std::string out = target.string();
                    if (standalone_popup) ImGui::CloseCurrentPopup();
                    builtin_picker_finish(out.c_str());
                    return;
                }
            }
        } else {
            fs::path selected(g_picker.selected);
            if (fs::is_regular_file(selected, file_ec) &&
                builtin_picker_matches(selected)) {
                const std::string out = selected.string();
                if (standalone_popup) ImGui::CloseCurrentPopup();
                builtin_picker_finish(out.c_str());
                return;
            }
            if (g_picker.description.empty())
                std::snprintf(g_picker.error, sizeof(g_picker.error), "%s",
                              "Select an existing file of the right type.");
            else
                std::snprintf(g_picker.error, sizeof(g_picker.error),
                              "Select an existing file matching: %s",
                              g_picker.description.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_text("Cancel"), ImVec2(px(110), px(34)))) {
        if (standalone_popup) ImGui::CloseCurrentPopup();
        builtin_picker_finish(nullptr);
        return;
    }
}

static void draw_standalone_builtin_rom_picker(LauncherModel* m,
                                                const LauncherTheme& th) {
    if (!g_picker.active || g_picker.from_setup) return;
    /* One popup id for every picker: the title is data now, not a fixed set of
     * cases, so a new picker needs no new id here. */
    ImGui::OpenPopup("##builtin_picker_modal");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(720), 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##builtin_picker_modal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        draw_builtin_rom_picker_contents(m, th, true);
        ImGui::EndPopup();
    }
}

// Context flag the dashboard composer sets just before invoking the "game"
// panel's registered draw() — the LauncherPanelDrawFn signature (Model*,
// const Theme*) has no room for the layout-context fill_h flag that
// draw_game_panel needs (fill the column height in the wide 2-column
// dashboard vs hug its content in the narrow stacked layout). Same pattern as
// the other per-frame context globals below (g_th, g_pads).
bool g_game_fill_h = false;
// SAVE (memory-card) fill-height: multitap (3+) only — cards stretch in a
// reserved band under a scrolling controller stack. 2P hugs content; edge
// inset comes from Child WindowPadding (same top/bottom as other panels).
bool g_save_fill_h = false;

// ---- panel registry lookup helper ------------------------------------------
// Resolve `id` against a SystemProfile's NULL-terminated composition array:
// the panel must both be LISTED (this system composes it at all) and
// AVAILABLE (this game instance offers it) to be drawn. Returns nullptr
// otherwise — the caller simply skips that slot.
const LauncherPanel* find_composed(const char* const* ids, const char* id, LauncherModel* m) {
    if (!ids || !id) return nullptr;
    for (int i = 0; ids[i]; ++i) {
        if (strcmp(ids[i], id) != 0) continue;
        const LauncherPanel* p = launcher_panel_find(id);
        return (p && launcher_panel_available(p, m)) ? p : nullptr;
    }
    return nullptr;
}

#include "emoji/recomp_emoji.h"
#include <unordered_map>
#include <vector>

/* ---- Color emoji in the atlas ---------------------------------------------
 * stb_truetype only rasterizes outlines, so the emoji merged from OpenMoji
 * are black shapes. For text the launcher OWNS (chat lines), each emoji
 * sequence is rendered through recomp_emoji (DirectWrite / FreeType) to an
 * RGBA sprite, given a private-use codepoint, and blitted into the atlas as
 * a custom glyph; the string is then drawn with that codepoint substituted,
 * so TextUnformatted / CalcTextSize / wrapping all work unchanged.
 *
 * The atlas is static in this ImGui, so a never-seen emoji marks it dirty
 * and the next frame rebuilds fonts once (apply_scale). One hitch per new
 * emoji, then never again. Where no provider exists (no FreeType, a console
 * port) registration fails, nothing is substituted, and the outline glyphs
 * draw exactly as before — that is the fallback. */
struct EmojiSprite {
    std::string seq;
    ImWchar     cp;
    int         w, h;
    std::vector<unsigned char> rgba;
    bool        ok;
    int         rect_id;
};
static std::vector<EmojiSprite> g_emoji_sprites;
static std::unordered_map<std::string, int> g_emoji_lookup;
static bool    g_emoji_atlas_dirty = false;
static int     g_emoji_px = 0;
static ImFont* g_emoji_font = nullptr;
static size_t  g_emoji_sprite_bytes = 0;
#ifdef IMGUI_USE_WCHAR32
static const ImWchar kEmojiPuaBase = 0xF0000;   /* Plane 15 private use */
static const int     kEmojiPuaSlots = 0xFFFD;
#else
static const ImWchar kEmojiPuaBase = 0xE000;    /* BMP private use */
static const int     kEmojiPuaSlots = 0x1900;
#endif
/* The cache is fed by REMOTE text -- a chat line is whatever a peer sent --
 * so its size is an attacker's choice unless it is bounded here. Registering
 * a sequence costs a sprite that is never evicted (the codepoints must stay
 * put; see emoji_atlas_reserve) AND marks the atlas dirty, which rebuilds
 * every font and re-uploads the texture on the next frame. ZWJ and skin-tone
 * combinations are effectively unbounded, so a peer sending novel sequences
 * would otherwise buy one full atlas rebuild per line, forever, and grow the
 * cache to the PUA ceiling -- 65533 sprites, hundreds of MB.
 *
 * Two budgets, whichever binds first. Past them registration fails, which is
 * the SAME path as a machine with no color provider: nothing is substituted
 * and the outline glyphs draw. No new failure mode, just the existing one.
 * Both are far above any honest session (a chatty player sees emoji in the
 * low hundreds), so a real user never reaches them. */
static const size_t kEmojiMaxSprites = 1024;
static const size_t kEmojiMaxSpriteBytes = 8u * 1024u * 1024u;

static bool emoji_cache_has_room(void) {
    const size_t ceiling = (size_t)kEmojiPuaSlots < kEmojiMaxSprites
                               ? (size_t)kEmojiPuaSlots : kEmojiMaxSprites;
    return g_emoji_sprites.size() < ceiling &&
           g_emoji_sprite_bytes < kEmojiMaxSpriteBytes;
}

/* Codepoint for an emoji sequence, rendering and registering it on first
 * sight. 0 when it cannot be rendered (caller keeps the original bytes). */
static ImWchar emoji_register(const char* seq, size_t len) {
    if (g_emoji_px <= 0) return 0;
    std::string key(seq, len);
    auto it = g_emoji_lookup.find(key);
    if (it != g_emoji_lookup.end()) {
        const EmojiSprite& s = g_emoji_sprites[(size_t)it->second];
        return s.ok ? s.cp : 0;
    }
    /* Full: draw the original bytes rather than remember one more sequence.
     * Nothing is recorded, so the line costs no memory and no rebuild -- a
     * flood of novel sequences is absorbed at a flat cost from here on. */
    if (!emoji_cache_has_room()) return 0;
    EmojiSprite s;
    s.seq = key;
    s.cp = (ImWchar)(kEmojiPuaBase + (ImWchar)g_emoji_sprites.size());
    s.w = s.h = 0;
    s.ok = false;
    s.rect_id = -1;
    RecompEmojiBitmap bm;
    if (recomp_emoji_render(seq, len, g_emoji_px, &bm)) {
        s.w = bm.w;
        s.h = bm.h;
        s.rgba.assign(bm.rgba, bm.rgba + (size_t)bm.w * (size_t)bm.h * 4);
        s.ok = true;
        recomp_emoji_free(&bm);
        g_emoji_sprite_bytes += s.rgba.size();
        g_emoji_atlas_dirty = true;
    }
    g_emoji_lookup[key] = (int)g_emoji_sprites.size();
    g_emoji_sprites.push_back(std::move(s));
    return g_emoji_sprites.back().ok ? g_emoji_sprites.back().cp : 0;
}

static size_t utf8_encode(ImWchar cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* `in` with every renderable emoji sequence replaced by its atlas
 * codepoint. Sequences that could not be rendered are copied through. */
static void emoji_display(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;
    const size_t len = std::strlen(in);
    size_t pos = 0, o = 0;
    while (pos < len && o + 1 < cap) {
        size_t start = 0, seq_len = 0;
        if (!recomp_emoji_scan(in, len, pos, &start, &seq_len)) start = len;
        /* Plain run up to the next sequence. */
        size_t run = start - pos;
        if (run > cap - 1 - o) run = cap - 1 - o;
        std::memcpy(out + o, in + pos, run);
        o += run;
        pos = start;
        if (pos >= len) break;
        const ImWchar cp = emoji_register(in + start, seq_len);
        if (cp) {
            char enc[4];
            const size_t n = utf8_encode(cp, enc);
            if (o + n >= cap) break;
            std::memcpy(out + o, enc, n);
            o += n;
        } else {
            size_t n = seq_len;
            if (n > cap - 1 - o) n = cap - 1 - o;
            std::memcpy(out + o, in + start, n);
            o += n;
        }
        pos = start + seq_len;
    }
    out[o] = '\0';
}

static size_t emoji_utf8_decode(const char* s, size_t len, unsigned int* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (len == 0) { *cp = 0; return 1; }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && len >= 2) { *cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); return 2; }
    if ((p[0] & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

/* The inverse of emoji_display: atlas codepoints back to the sequences they
 * stand for. What goes on the wire is always this form. */
static void emoji_restore(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;
    const size_t len = std::strlen(in);
    size_t pos = 0, o = 0;
    while (pos < len && o + 1 < cap) {
        unsigned int cp = 0;
        const size_t adv = emoji_utf8_decode(in + pos, len - pos, &cp);
        const char* src = in + pos;
        size_t src_len = adv;
        if (cp >= (unsigned int)kEmojiPuaBase &&
            cp < (unsigned int)kEmojiPuaBase + g_emoji_sprites.size()) {
            const EmojiSprite& sp = g_emoji_sprites[cp - (unsigned int)kEmojiPuaBase];
            src = sp.seq.data();
            src_len = sp.seq.size();
        }
        if (src_len > cap - 1 - o) break;
        std::memcpy(out + o, src, src_len);
        o += src_len;
        pos += adv;
    }
    out[o] = '\0';
}

/* InputText edit callback: keep the buffer in display form while typing, so
 * the box shows the same color glyphs as the log. The whole text is restored
 * and re-substituted on every edit rather than patched, so an emoji typed in
 * pieces (a thumb, then a skin tone from the OS picker) still joins into one
 * sequence. The caret is re-placed by substituting the prefix before it. */
static int emoji_input_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
    char raw[1024];
    char disp[1024];
    emoji_restore(data->Buf, raw, sizeof(raw));
    emoji_display(raw, disp, sizeof(disp));
    if (std::strcmp(disp, data->Buf) == 0) return 0;
    if ((int)std::strlen(disp) >= data->BufSize) return 0;
    int cursor = data->CursorPos;
    if (cursor < 0) cursor = 0;
    if (cursor > data->BufTextLen) cursor = data->BufTextLen;
    char pre[1024];
    char pre_raw[1024];
    char pre_disp[1024];
    const size_t pre_n = (size_t)cursor < sizeof(pre) - 1 ? (size_t)cursor : sizeof(pre) - 1;
    std::memcpy(pre, data->Buf, pre_n);
    pre[pre_n] = '\0';
    emoji_restore(pre, pre_raw, sizeof(pre_raw));
    emoji_display(pre_raw, pre_disp, sizeof(pre_disp));
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, disp);
    data->CursorPos = (int)std::strlen(pre_disp);
    data->SelectionStart = data->SelectionEnd = data->CursorPos;
    return 0;
}

/* Country flag for an ISO 3166-1 alpha-2 code, drawn inline before a name.
 * The flag is the pair of regional-indicator symbols, which the color emoji
 * provider renders as the real flag. Without a color provider the outline
 * font has no flags, so the code is shown in a muted "[JP]" instead of two
 * meaningless letter boxes. Draws nothing for an empty / malformed code. */
const RecompLauncherCNetplayCallbacks* np_cb(LauncherModel* m);   /* fwd */

/*
 * Push the block list to the server when it changes, and again on a fresh
 * connection.
 *
 * Sent rather than merely applied here because only one of a block's three
 * effects can be done client-side. Hiding somebody's chat is ours. Not being
 * PAIRED with them, and their not seeing or joining our room, are the
 * server's -- and the second of those is the direction a client cannot do at
 * all, because it cannot know it was blocked.
 *
 * Resent on reconnect: the server holds the list for the life of a connection
 * and never persists it, so a dropped socket would otherwise silently leave
 * the player unprotected on the next one.
 */
static void np_push_blocks(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np || !np->set_blocks) return;
    char list[256 * 41];
    recomp_moderation_blocked_list(list, sizeof(list));

    static char s_last[sizeof(list)];
    static bool s_was_connected;
    const bool connected = np->connected && np->connected(np->ctx);
    if (!connected) { s_was_connected = false; return; }
    /* A reconnect counts as a change even when the list did not move. */
    if (s_was_connected && std::strcmp(s_last, list) == 0) return;
    s_was_connected = true;
    std::snprintf(s_last, sizeof(s_last), "%s", list);
    np->set_blocks(np->ctx, list);
}

/*
 * Right-click a player: ignore them, or block them.
 *
 * ONLINE ONLY, and that gate is the point rather than a simplification. A LAN
 * / Direct IP room has no lobby server, so nobody published an account id and
 * there is nothing durable to key a list on -- a menu there could only offer
 * to remember a name, which is precisely the thing that blocks the wrong
 * person later. It is also a room of people who already know each other.
 *
 * A guest has no account either. They get the menu disabled with the reason
 * shown, rather than silently doing nothing: "I clicked Block and it did not
 * block" is a worse outcome than being told why.
 *
 * `account` is the key and `name` is only what to put in the menu header and
 * store as a label.
 */
static void np_player_menu(LauncherModel* m, const LauncherTheme& th,
                           const char* account, const char* name,
                           const char* mid = nullptr) {
    if (m->netplay_mode != 2) return;   /* LAN/Direct IP has no accounts */
    if (!ImGui::BeginPopupContextItem("##player_menu")) return;

    char disp[96];
    emoji_display((name && name[0]) ? name : "Player", disp, sizeof(disp));
    ImGui::TextColored(col(th.text_muted), "%s", disp);
    ImGui::Separator();

    const bool has_key = account && account[0];
    if (!has_key) {
        ImGui::TextColored(col(th.text_muted),
                           "Signed-out players cannot be ignored:");
        ImGui::TextColored(col(th.text_muted),
                           "there is no account to remember.");
        ImGui::EndPopup();
        return;
    }

    const RecompModLevel lvl = recomp_moderation_level(account);
    bool ignored = lvl != RECOMP_MOD_NONE;
    bool blocked = lvl == RECOMP_MOD_BLOCKED;

    if (ImGui::MenuItem("Ignore Player", nullptr, ignored, !blocked)) {
        recomp_moderation_set(account, name,
                              ignored ? RECOMP_MOD_NONE : RECOMP_MOD_IGNORED);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(blocked ? "Already blocked, which includes this"
                                  : "Hide their chat. They can still be in "
                                    "your lobby.");
    if (ImGui::MenuItem("Block Player", nullptr, blocked)) {
        recomp_moderation_set(account, name,
                              blocked ? RECOMP_MOD_NONE : RECOMP_MOD_BLOCKED);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide their chat, hide them from the lists, and ask "
                          "the matchmaker not to pair you.");

    /* Chat only, and only with an id.
     *
     * Ignoring and blocking are decisions about a PERSON and belong on any
     * row that names one. A report is about a LINE -- the server records the
     * words it relayed, not a person -- so it is offered where a line exists
     * to point at, and nowhere else. Offering it on a seat row would leave
     * the player choosing which of that person's messages they meant from a
     * menu that never showed them one.
     *
     * A line with no id predates the server's message ids and cannot be
     * reported at all: there is no referent both sides agree on. */
    const auto* np_rep = np_cb(m);
    if (mid && mid[0] && np_rep && np_rep->chat_report) {
        ImGui::Separator();
        if (ImGui::MenuItem("Report Message...")) {
            std::snprintf(m->netplay_report_mid, sizeof(m->netplay_report_mid),
                          "%s", mid);
            std::snprintf(m->netplay_report_who, sizeof(m->netplay_report_who),
                          "%s", (name && name[0]) ? name : "Player");
            m->netplay_report_note[0] = '\0';
            m->netplay_report_reason = 0;
            m->netplay_report_status[0] = '\0';
            m->netplay_report_modal_open = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Send this line to the server's moderation "
                              "queue. The server records what IT relayed, "
                              "not text from here.");
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Moderation list..."))
        m->netplay_moderation_modal_open = true;
    ImGui::EndPopup();
}

static void np_draw_country_flag(const LauncherTheme& th, const char* cc) {
    if (!cc || !cc[0] || !cc[1]) return;
    const char a = (char)std::toupper((unsigned char)cc[0]);
    const char b = (char)std::toupper((unsigned char)cc[1]);
    if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z') return;
    /* The sheet is the normal path; the provider only when the sheet lacks
     * the code AND is not DirectWrite, which would draw two boxed letters. */
    const bool provider_flags =
        recomp_emoji_backend_available() &&
        std::strcmp(recomp_emoji_backend_name(), "directwrite") != 0;
    if (recomp_emoji_flags_has(a, b) || provider_flags) {
        char seq[9];
        char disp[32];
        size_t n = utf8_encode((ImWchar)(0x1F1E6 + (a - 'A')), seq);
        n += utf8_encode((ImWchar)(0x1F1E6 + (b - 'A')), seq + n);
        seq[n] = '\0';
        emoji_display(seq, disp, sizeof(disp));
        ImGui::TextUnformatted(disp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%c%c", a, b);
    } else {
        ImGui::TextColored(col(th.text_muted), "[%c%c]", a, b);
    }
    ImGui::SameLine(0, px(6));
}

/* apply_scale hooks: reserve atlas rects before Build, blit after. */
static void emoji_atlas_reserve(ImFontAtlas* atlas, ImFont* font, float body) {
    const int px = (int)(body + 0.5f);
    g_emoji_font = font;
    if (px != g_emoji_px) {
        /* Rendered at another size: re-render in place. The codepoints must
         * stay put — an input box may be holding them mid-edit. */
        g_emoji_px = px;
        /* A bigger px is a bigger sprite, so the byte budget is re-measured
         * from scratch here: a cache that fit at 16px may not at 64. Sprites
         * past the budget stay ok=false and draw as outline glyphs, exactly
         * as they would on a machine with no color provider. */
        g_emoji_sprite_bytes = 0;
        for (EmojiSprite& s : g_emoji_sprites) {
            RecompEmojiBitmap bm;
            s.ok = false;
            s.rgba.clear();
            s.rgba.shrink_to_fit();
            if (g_emoji_sprite_bytes >= kEmojiMaxSpriteBytes) continue;
            if (recomp_emoji_render(s.seq.data(), s.seq.size(), px, &bm)) {
                s.w = bm.w;
                s.h = bm.h;
                s.rgba.assign(bm.rgba, bm.rgba + (size_t)bm.w * (size_t)bm.h * 4);
                s.ok = true;
                recomp_emoji_free(&bm);
                g_emoji_sprite_bytes += s.rgba.size();
            }
        }
    }
    for (EmojiSprite& s : g_emoji_sprites) {
        s.rect_id = -1;
        if (!s.ok || !font) continue;
        s.rect_id = atlas->AddCustomRectFontGlyph(
            font, s.cp, s.w, s.h, (float)s.w + 1.0f,
            ImVec2(0.0f, (body - (float)s.h) * 0.5f));
    }
}

static void emoji_atlas_blit(ImFontAtlas* atlas) {
    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    bool any = false;
    for (const EmojiSprite& s : g_emoji_sprites)
        if (s.ok && s.rect_id >= 0) { any = true; break; }
    if (!any) return;
    atlas->GetTexDataAsRGBA32(&pixels, &tw, &th);
    if (!pixels) return;
    for (const EmojiSprite& s : g_emoji_sprites) {
        if (!s.ok || s.rect_id < 0) continue;
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(s.rect_id);
        if (!r || !r->IsPacked()) continue;
        for (int y = 0; y < s.h && y < (int)r->Height; ++y)
            std::memcpy(pixels + ((size_t)(r->Y + y) * (size_t)tw + r->X) * 4,
                        s.rgba.data() + (size_t)y * (size_t)s.w * 4,
                        (size_t)(s.w < (int)r->Width ? s.w : (int)r->Width) * 4);
    }
}
/* Rasterize glyphs at the display's pixel density while keeping their LOGICAL
 * point size: the layout stays DPI-independent and text comes out genuinely
 * sharper instead of a magnified 100% atlas. RasterizerDensity landed in ImGui
 * 1.90.6; a host reusing an older copy (see the version shims at the top)
 * still scales, just from a magnified atlas. */
static void set_raster_density(ImFontConfig& cfg, float density) {
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19060
    cfg.RasterizerDensity = (density > 0.0f) ? density : 1.0f;
#else
    (void)cfg; (void)density;
#endif
}

// Merge an optional TTF over the active font when the file exists.
static void merge_font_if_present(const char* path, float size, float density,
                                  const ImWchar* ranges) {
    if (!path || !path[0] || !ranges) return;
    if (FILE* f = fopen(path, "rb")) {
        fclose(f);
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        set_raster_density(cfg, density);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg, ranges);
    }
}

// ---- DPI: rebuild fonts + re-derive style from an unscaled baseline ----------
void apply_scale(const LauncherTheme& th, float scale, const char* font_path,
                 const char* jp_font_path, const char* symbols_font_path,
                 const char* emoji_font_path) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    // Glyph SIZES stay logical (the layout is DPI-independent); only the
    // raster density follows the display, so text is sharp rather than a
    // magnified 100% atlas. Style is not re-scaled either — the frame is,
    // via io.DisplayFramebufferScale (see apply_logical_display).
    const float density = (scale > 0.0f) ? scale : 1.0f;
    set_raster_density(cfg, density);
    const float body = th.font_body;
    // Cover Basic Latin + Latin-1 AND General Punctuation so em/en dashes and
    // curly quotes used in the game notes render as glyphs, not "?" tofu.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
        0x2010, 0x2027,   // dashes, curly quotes, ellipsis (General Punctuation)
        0,
    };
    bool loaded = false;
    ImFont* base_font = nullptr;
    if (font_path && font_path[0]) {
        base_font = io.Fonts->AddFontFromFileTTF(font_path, body, &cfg, kRanges);
        loaded = base_font != nullptr;
    }
    if (!loaded) { cfg.SizePixels = body; base_font = io.Fonts->AddFontDefault(&cfg); }
    // Merge a Japanese subset atlas over the Latin base when the game ships one
    // (PMS-J's kana cart names / trainer strings). MergeMode folds the JP glyphs
    // into the same font so mixed Latin+kana strings render in one pass; absent
    // file => Latin-only, unchanged for every other console.
    if (jp_font_path && jp_font_path[0]) {
        if (FILE* jf = fopen(jp_font_path, "rb")) {
            fclose(jf);
            ImFontConfig jcfg; jcfg.OversampleH = 2; jcfg.OversampleV = 2;
            set_raster_density(jcfg, density);
            jcfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(jp_font_path, body, &jcfg,
                                         io.Fonts->GetGlyphRangesJapanese());
        }
    }
    // Symbol / emoji fallbacks (kick 🥾, lock 🔒, etc.). Outline fonts only —
    // CBDT color emoji (Noto Color Emoji) is not supported by stb_truetype.
    static const ImWchar kSymbolRanges[] = {
        0x2000, 0x206F,   // General Punctuation
        0x2190, 0x21FF,   // Arrows
        0x2300, 0x23FF,   // Misc Technical
        0x2460, 0x24FF,   // Enclosed Alphanumerics
        0x25A0, 0x25FF,   // Geometric Shapes
        0x2600, 0x26FF,   // Misc Symbols
        0x2700, 0x27BF,   // Dingbats
        0x2B00, 0x2BFF,   // Misc Symbols and Arrows
        0,
    };
#ifdef IMGUI_USE_WCHAR32
    static const ImWchar kEmojiRanges[] = {
        0x1F300, 0x1F5FF, // Misc Symbols and Pictographs (incl. 🔒)
        0x1F600, 0x1F64F, // Emoticons
        0x1F680, 0x1F6FF, // Transport and Map
        0x1F900, 0x1F9FF, // Supplemental Symbols and Pictographs (incl. 🥾)
        0,
    };
    merge_font_if_present(symbols_font_path, body, density, kSymbolRanges);
    merge_font_if_present(emoji_font_path, body, density, kEmojiRanges);
#else
    merge_font_if_present(symbols_font_path, body, density, kSymbolRanges);
    (void)emoji_font_path;
#endif
    /* Color emoji sprites ride in the same atlas as custom glyphs. They stay
     * at LOGICAL resolution: a custom atlas rect draws at its texel size, so
     * rendering them denser would make them bigger, not sharper. Correct size,
     * a little soft on a HiDPI display — the one thing density cannot fix. */
    emoji_atlas_reserve(io.Fonts, base_font, body);
    io.Fonts->Build();
    emoji_atlas_blit(io.Fonts);
    g_emoji_atlas_dirty = false;
    {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            std::fprintf(stderr, "[rui] color emoji backend: %s; flags: %s\n",
                         recomp_emoji_backend_name(),
                         recomp_emoji_flags_available() ? "bundled sheet"
                                                        : "provider");
        }
    }
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();

    ImGuiStyle style; ImGui::StyleColorsDark(&style);
    style.WindowRounding = th.radius_lg; style.ChildRounding = th.radius_lg;
    style.FrameRounding  = th.radius_sm; style.GrabRounding  = th.radius_sm;
    style.WindowPadding  = ImVec2(th.spacing_lg, th.spacing_lg);
    style.FramePadding   = ImVec2(th.spacing_md, th.spacing_sm);
    style.ItemSpacing    = ImVec2(th.spacing_md, th.spacing_sm);
#if defined(__ANDROID__)
    style.TouchExtraPadding = ImVec2(5.0f, 5.0f);
    style.ScrollbarSize = 24.0f;
#endif
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;   // controls get a visible outline
    style.Colors[ImGuiCol_WindowBg]        = col(th.background);
    style.Colors[ImGuiCol_ChildBg]         = col(th.panel);
    style.Colors[ImGuiCol_PopupBg]         = col(th.panel);
    style.Colors[ImGuiCol_Border]          = col(th.border);
    style.Colors[ImGuiCol_FrameBg]         = col(th.control);
    style.Colors[ImGuiCol_FrameBgHovered]  = col(th.control_hovered);
    style.Colors[ImGuiCol_FrameBgActive]   = col(th.control_hovered);
    style.Colors[ImGuiCol_Button]          = col(th.control);
    style.Colors[ImGuiCol_ButtonHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_ButtonActive]    = col(th.accent);
    style.Colors[ImGuiCol_Header]          = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderActive]    = col(th.accent);
    style.Colors[ImGuiCol_CheckMark]       = col(th.accent);
    style.Colors[ImGuiCol_Text]            = col(th.text);
    style.Colors[ImGuiCol_TextDisabled]    = col(th.text_muted);
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19100
    style.Colors[ImGuiCol_TextLink]        = col(th.accent2);
#endif
    style.Colors[ImGuiCol_Separator]       = col(th.border);
    style.Colors[ImGuiCol_ScrollbarBg]     = col(th.panel);
    style.Colors[ImGuiCol_ScrollbarGrab]   = col(th.border);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = col(th.control_hovered);
    // Gamepad/keyboard focus ring: bright cyan so a Deck user always sees where
    // they are. NavCursor is the 1.91.4+ name; older ImGui (e.g. an rt64 host on
    // 1.90.x) calls the same slot NavHighlight.
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19140
    style.Colors[ImGuiCol_NavCursor]       = col(th.focus_ring);
#else
    style.Colors[ImGuiCol_NavHighlight]    = col(th.focus_ring);
#endif
    ImGui::GetStyle() = style;
}

// ---- CRT / neon atmosphere (drawn with ImDrawList) ---------------------------
ImU32 imcol(const LngColor& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a * a));
}

// Vertical center-bright gradient (CRT ground) + faint scanlines. Drawn on the
// background/foreground draw lists so it sits behind/over the whole UI.
void draw_crt_background(ImVec2 origin, ImVec2 size) {
    const LauncherTheme& th = *g_th;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImU32 ink = imcol(th.background), lift = imcol(th.background2);
    float midY = origin.y + size.y * 0.42f;
    // top: ink -> lift, bottom: lift -> ink  (soft horizontal glow band)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, midY),
                                ink, ink, lift, lift);
    bg->AddRectFilledMultiColor(ImVec2(origin.x, midY), ImVec2(origin.x + size.x, origin.y + size.y),
                                lift, lift, ink, ink);
    // a soft violet bloom behind the header (arcade marquee glow)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + px(90)),
                                imcol(th.accent, 0.10f), imcol(th.accent, 0.10f),
                                imcol(th.accent, 0.0f),  imcol(th.accent, 0.0f));
    // scanlines over everything, very subtle — only for CRT-style themes (the PSX
    // theme sets scanlines = 0 for a flat, disc-era look).
    if (th.scanlines) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float step = px(3.0f); if (step < 2.0f) step = 2.0f;
        ImU32 sl = imcol(th.scanline);
        for (float y = origin.y; y < origin.y + size.y; y += step)
            fg->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), sl, 1.0f);
    }
}

// Neon glow: concentric rounded rects fading outward behind [min,max].
void glow_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& c, float intensity, int layers = 5) {
    for (int i = layers; i >= 1; --i) {
        float grow = px(2.0f) * i;
        float a = intensity * (0.10f) * (float)(layers - i + 1) / layers;
        dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow),
                          ImVec2(mx.x + grow, mx.y + grow),
                          imcol(c, a), rounding + grow);
    }
}

// Filled rounded rect with a vertical gradient (top -> bottom).
void grad_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& top, const LngColor& bot) {
    dl->AddRectFilled(mn, mx, imcol(bot), rounding);   // base (rounded)
    // overlay a gradient clipped to the rounded rect via a slightly-inset fill
    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilledMultiColor(mn, mx, imcol(top), imcol(top), imcol(bot), imcol(bot));
    dl->PopClipRect();
}

// ---- primitive icons (crisp at any DPI, no font dependency) -------------------
void draw_check(const LngColor& c) {   // green check, advances cursor like text
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), y = p.y + s * 0.5f;
    ImU32 u = ImGui::GetColorU32(col(c));
    dl->AddLine(ImVec2(p.x + s*0.15f, y), ImVec2(p.x + s*0.40f, y + s*0.28f), u, px(2.0f));
    dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.28f), ImVec2(p.x + s*0.85f, y - s*0.28f), u, px(2.0f));
    ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine(0, px(6));
}
void draw_dot(bool on, const LngColor& good, const LngColor& off) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), r = px(5.0f);
    ImVec2 c(p.x + r, p.y + s * 0.5f);
    if (on) dl->AddCircleFilled(c, r, ImGui::GetColorU32(col(good)));
    else    dl->AddCircle(c, r, ImGui::GetColorU32(col(off)), 0, px(1.5f));
    ImGui::Dummy(ImVec2(r * 2, s)); ImGui::SameLine(0, px(8));
}
// The primary neon CTA (PLAY): glow + violet gradient + play triangle. Fully
// custom-drawn over an InvisibleButton so it looks nothing like a stock button.
// `arrow` draws the ▶. PLAY wants it; a primary action that is not "start the
// game" does not -- an Automatch button that already carries ⚡ would show two
// glyphs arguing about what it does.
bool neon_cta(const char* id, const char* label, ImVec2 size, bool enabled = true,
              bool arrow = true) {
    const LauncherTheme& th = *g_th;
    ImVec2 p = ImGui::GetCursorScreenPos();
    // EnableNav is REQUIRED: ImGui::InvisibleButton() adds ImGuiItemFlags_NoNav by
    // default, which silently excludes the CTA from gamepad/keyboard nav — that was
    // why PLAY could never be focused at runtime (only via boot SetItemDefaultFocus)
    // while normal widgets (Skip, Settings) always could.
    if (!enabled) ImGui::BeginDisabled();
    bool clk = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    bool hov = enabled && ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();
    bool foc = ImGui::IsItemFocused();   // gamepad/keyboard nav focus
    ImVec2 mn = p, mx = ImVec2(p.x + size.x, p.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = px(th.radius_sm);

    glow_rect(dl, mn, mx, r, th.accent, hov ? 1.6f : 1.0f, 6);
    LngColor top = hov ? th.accent : th.accent;
    LngColor bot = act ? th.accent_dim : th.accent_dim;
    grad_rect(dl, mn, mx, r, top, bot);
    dl->AddRect(mn, mx, imcol(th.accent, hov ? 0.9f : 0.5f), r, 0, px(1.0f));  // crisp edge
    // InvisibleButton draws no nav highlight itself — paint the cyan focus ring
    // when nav-focused so the CTA reads as selectable via controller/keyboard.
    if (foc) {
        ImVec2 om = ImVec2(mn.x - px(2), mn.y - px(2)), ox = ImVec2(mx.x + px(2), mx.y + px(2));
        dl->AddRect(om, ox, imcol(th.focus_ring), r + px(2), 0, px(th.focus_ring_width));
    }

    // centered "▶ label"
    float th_h = ImGui::GetTextLineHeight();
    float tw = ImGui::CalcTextSize(label).x;
    float tri = arrow ? px(11.0f) : 0.0f, gap = arrow ? px(10.0f) : 0.0f;
    float total = tri + gap + tw;
    float cx = p.x + (size.x - total) * 0.5f, cy = p.y + size.y * 0.5f;
    ImU32 fg = imcol(th.accent_text);
    if (arrow)
        dl->AddTriangleFilled(ImVec2(cx, cy - tri*0.55f), ImVec2(cx, cy + tri*0.55f),
                              ImVec2(cx + tri, cy), fg);
    dl->AddText(ImVec2(cx + tri + gap, cy - th_h*0.5f), fg, label);
    if (!enabled) ImGui::EndDisabled();
    return clk && enabled;
}

// Uppercase section eyebrow with letter-spacing + a short accent tick, e.g.
//   ▎ CONTROLLERS   — encodes "this is a section header", arcade panel style.
void eyebrow_tracked(const char* s) {
    const LauncherTheme& th = *g_th;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    // accent tick (secondary accent — section headings read in the theme's
    // heading color, distinct from the primary CTA on dual-accent themes)
    dl->AddRectFilled(ImVec2(p.x, p.y + h*0.12f), ImVec2(p.x + px(3.0f), p.y + h*0.9f),
                      imcol(th.accent2), px(1.5f));
    // letter-spaced text
    float x = p.x + px(10.0f);
    ImU32 c = imcol(th.accent2);
    char buf[2] = {0,0};
    for (const char* q = s; *q; ++q) {
        buf[0] = *q;
        dl->AddText(ImVec2(x, p.y), c, buf);
        x += ImGui::CalcTextSize(buf).x + px(2.2f);
    }
    ImGui::Dummy(ImVec2(x - p.x, h));
    ImGui::Spacing();
}

// Draw a texture fit inside a logical box, preserving aspect.
void image_fit(const LauncherTexture& t, float box_w, float box_h) {
    if (!t.id || t.w <= 0 || t.h <= 0) { ImGui::Dummy(ImVec2(px(box_w), px(box_h))); return; }
    float bw = px(box_w), bh = px(box_h);
    float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
    ImGui::Image(tid(t), ImVec2(t.w * s, t.h * s));
}

// Like image_fit, but horizontally centers the FITTED image within avail_w.
// image_fit alone centers on the box width, so a near-square art (the N64 pad)
// fit into a landscape box draws narrow and sits left-of-center — this offsets
// by the real fitted width instead.
void image_fit_centered(const LauncherTexture& t, float box_w, float box_h, float avail_w) {
    float fitted_w = px(box_w);
    if (t.id && t.w > 0 && t.h > 0) {
        float bw = px(box_w), bh = px(box_h);
        float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
        fitted_w = t.w * s;
    }
    float off = (avail_w - fitted_w) * 0.5f;
    if (off > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
    image_fit(t, box_w, box_h);
}

void eyebrow(const char* s) { eyebrow_tracked(ui_text(s)); }
// A card: filled + bordered. Hugs its content by default; `fill_h` stretches it
// to the remaining height (used by the dashboard columns so the layout doesn't
// leave a big empty gap under short cards).
bool begin_panel(const char* id, float logical_w = 0.0f, bool fill_h = false,
                 bool no_scroll = false) {
    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (!fill_h) flags |= ImGuiChildFlags_AutoResizeY;
    // A fill-height card (e.g. GAME) must SCROLL when the window is too short —
    // otherwise its folded-in content (SAVES) clips out of reach. Only the
    // fixed-size settings cards, which are sized to fit, suppress the scrollbar
    // (no_scroll) to avoid a stray bar. Content-hugging cards (AutoResizeY) never
    // overflow themselves, so scrollable-by-default is a no-op for them.
    ImGuiWindowFlags wflags = no_scroll ? (ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse)
                                        : 0;
    return ImGui::BeginChild(id, ImVec2(px(logical_w), 0.0f), flags, wflags);
}
void end_panel() { ImGui::EndChild(); }

// A layout container: no fill, no border. Without this a nested child inherits
// ChildBg and paints a large panel-coloured rectangle behind the real cards,
// which reads as "dead space".
bool begin_container(const char* id, ImVec2 size, ImGuiChildFlags flags = ImGuiChildFlags_None,
                     ImGuiWindowFlags wflags = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    return ImGui::BeginChild(id, size, flags, wflags);
}
void end_container() { ImGui::EndChild(); ImGui::PopStyleColor(); }

void state_mark(bool ok, const LauncherTheme& th);   // fwd
void draw_save_row(LauncherModel* m, const LauncherTheme& th);   // fwd (Save module row-drawer)

// One metadata row inside a 3-column table: label | value | optional check.
// `show_mark` puts a mint check / amber cross in its own column instead of a
// text badge, so it can never crowd the panel edge.
void kv_row(const char* k, const char* v, const LauncherTheme& th,
            bool show_mark, bool ok) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(ui_text(k));
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v);
    ImGui::TableNextColumn();
    if (show_mark) state_mark(ok, th);
}

// Key/value row, drawn full width: muted label column, value, and an optional
// right-aligned badge. No wrapping — the row owns the whole panel width, so
// long values (CRC/SHA) have room instead of being clipped or char-wrapped.
void kv(const char* k, const char* v, const LauncherTheme& th,
        const char* badge = nullptr, bool good = true) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(ui_text(k)); ImGui::PopStyleColor();
    ImGui::SameLine(x0 + px(84.0f));
    ImGui::TextUnformatted(v);
    if (badge) {
        char b[24]; snprintf(b, sizeof(b), "[%s]", badge);
        const float bw = ImGui::CalcTextSize(b).x;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        ImGui::PushStyleColor(ImGuiCol_Text, col(good ? th.good : th.warn));
        ImGui::TextUnformatted(b); ImGui::PopStyleColor();
    }
}
// `total_w` > 0 stretches the value field so the whole widget measures exactly
// that — which is what lets a right-anchored stepper line its LEFT edge up
// with the dropdowns above it instead of floating a few pixels inside them.
// 0 keeps the natural width every other caller has always had.
void stepper(const char* id, int value, const char* suffix, int* out_delta,
             float total_w = 0.0f) {
    ImGui::PushID(id);
    const float bh = px(30), bw = px(32), gap = px(6);
    float fw = px(58);
    if (total_w > 0.0f) {
        const float want = total_w - bw * 2.0f - gap * 2.0f;
        if (want > fw) fw = want;
    }
    if (ImGui::Button("-", ImVec2(bw, bh))) *out_delta = -5;
    ImGui::SameLine(0, gap);
    // value centered in a fixed-width field so "+" never shifts with the digits
    char buf[32]; snprintf(buf, sizeof(buf), "%d%s", value, suffix);
    float cx = ImGui::GetCursorPosX();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImGui::SetCursorPosX(cx + (fw - ts.x) * 0.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(cx + fw + gap);
    if (ImGui::Button("+", ImVec2(bw, bh))) *out_delta = +5;
    ImGui::PopID();
}

// "Label ......... [control]" row: label baseline-aligned to the control.
// col_w > 0 reserves a FIXED label column so the control starts at the same x
// on every row — the caller passes the widest label's width (+gap) to line all
// the controls up into a clean grid. col_w == 0 keeps the legacy flow layout
// (control hugs the label with a fixed gap).
void row_label(const char* text, const LauncherTheme& th, float col_w = 0.0f) {
    float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text_muted), "%s", ui_text(text));
    if (col_w > 0.0f) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(x0 + col_w);          // fixed label column → controls align
    } else {
        ImGui::SameLine(0.0f, px(th.spacing_md));  // flow from label width (no fixed-x overlap)
    }
}

/*
 * "Label ....................... [control]" — the label at the card's left
 * edge, the control pushed out to its RIGHT edge.
 *
 * The settings cards used to align on the LEFT of the control column
 * (row_label's col_w), which meant the caller had to measure every label in
 * the card first and pass the widest, and a card's controls then floated in
 * the middle with dead space to their right. Anchoring to the right edge
 * needs no measuring pass at all -- it reads off the card's own width -- and
 * a card whose rows all use the same control width comes out flush on BOTH
 * sides, which is the grid the col_w pass was trying to build.
 *
 * `ctrl_w` is the width the caller is about to draw: px(SETTINGS_CTRL_W) for
 * the shared button/dropdown width, ImGui::GetFrameHeight() for a checkbox
 * (they are square), or a bespoke width for a row that needs one. A label too
 * long to leave room keeps a minimum gap and lets its control run wide rather
 * than colliding with the text.
 */
void row_label_right(const char* text, const LauncherTheme& th, float ctrl_w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text_muted), "%s", ui_text(text));
    ImGui::SameLine(0.0f, 0.0f);
    float shift = ImGui::GetContentRegionAvail().x - ctrl_w;
    const float min_gap = px(th.spacing_md);
    if (shift < min_gap) shift = min_gap;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + shift);
}

/* One width for every button and dropdown in the Display and Audio cards, so
 * right-anchoring also lines their left edges up. Wide enough for the longest
 * value any of them shows ("Borderless", "Adaptive", "2 frames", "32000 Hz").
 * Rows whose value cannot fit -- a screen model, a file path -- pass their own
 * width to row_label_right instead. */
#define SETTINGS_CTRL_W 150.0f

/*
 * False while the window is too narrow to run DISPLAY and AUDIO side by side,
 * so the settings cards stack in one column. draw_settings sets it each frame;
 * the card drawers read it, because a STACKED card must hug its content — the
 * legacy fixed band exists only to make two side-by-side cards the same
 * height, and pinning it in one column leaves a tall empty region under a
 * short card.
 *
 * A file-scope flag rather than a parameter: the panel registry's draw
 * signature is the generic (model, theme) shared by every view, and threading
 * one layout bit through all of it to reach two call sites would be worse.
 */
bool g_settings_two_col = true;

/* One entry of a settings dropdown: the stored value and what it reads as. */
struct SettingsChoice { int value; const char* label; };

/*
 * A labelled dropdown row: right-anchored combo over a fixed list of choices.
 * Returns the value the player picked this frame, or `current` when they
 * picked nothing, so the caller commits through its own model setter.
 *
 * Cycle buttons were fine when a setting had two or three states, but the
 * player cannot see what the other states ARE without pressing through them,
 * and a wrap-around list has no "back". A dropdown shows the whole set and
 * puts every entry one click away -- which is what Renderer and VSync already
 * did, so this is the control the card was already half using.
 */
int settings_combo_row(const char* label, const LauncherTheme& th,
                       const char* id, const SettingsChoice* choices,
                       int count, int current) {
    const char* current_label = "";
    for (int i = 0; i < count; ++i)
        if (choices[i].value == current) { current_label = choices[i].label; break; }

    row_label_right(label, th, px(SETTINGS_CTRL_W));
    ImGui::SetNextItemWidth(px(SETTINGS_CTRL_W));
    int picked = current;
    if (ImGui::BeginCombo(id, ui_text(current_label))) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(ui_text(choices[i].label),
                                  choices[i].value == current))
                picked = choices[i].value;
        ImGui::EndCombo();
    }
    return picked;
}

/* Window scale reads as "1x".."6x". Spelled out rather than formatted per
 * frame so the list is plain data, like every other choice list here. */
static const SettingsChoice kScaleChoices[LNG_WINDOW_SCALE_MAX] = {
    {1, "1x"}, {2, "2x"}, {3, "3x"}, {4, "4x"}, {5, "5x"}, {6, "6x"}
};
static const SettingsChoice kFullscreenChoices[] = {
    {0, "Off"}, {1, "Borderless"}, {2, "Exclusive"}
};
static const SettingsChoice kRunAheadChoices[RECOMP_LAUNCHER_RUN_AHEAD_MAX + 1] = {
    {0, "Off"}, {1, "1 frame"}, {2, "2 frames"}, {3, "3 frames"}, {4, "4 frames"}
};

/* The three rows those lists drive, so the legacy and deep Display surfaces
 * cannot drift apart in what they offer. */
void row_window_scale(LauncherModel* m, const LauncherTheme& th) {
    launcher_model_set_scale(
        m, settings_combo_row("Window scale", th, "##window_scale",
                              kScaleChoices, LNG_WINDOW_SCALE_MAX,
                              m->s.window_scale < 1 ? 1 : m->s.window_scale));
}

void row_fullscreen(LauncherModel* m, const LauncherTheme& th) {
    launcher_model_set_fullscreen(
        m, settings_combo_row("Fullscreen", th, "##fullscreen",
                              kFullscreenChoices, 3, m->s.fullscreen));
}

void row_run_ahead(LauncherModel* m, const LauncherTheme& th) {
    launcher_model_set_run_ahead(
        m, settings_combo_row("Run-ahead", th, "##run_ahead", kRunAheadChoices,
                              RECOMP_LAUNCHER_RUN_AHEAD_MAX + 1,
                              m->s.run_ahead));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(
            "Emulate ahead and show a frame from the future, so\n"
            "the game's own input lag is hidden. Each frame of depth\n"
            "costs one extra emulated frame -- start at 1.\n\n"
            "Local only: the runtime turns it off during netplay,\n"
            "where a peer's input cannot be predicted.");
}

// ---- views -----------------------------------------------------------------
// Box art, centered, framed. No neon glow — the art is photographic content and
// a violet halo around it reads as a bug, not a design. Glow is reserved for
// the PLAY CTA, where it means "this is the action".
void hero_boxart_centered(const LauncherTexture& t, float box_h, float avail_w) {
    const LauncherTheme& th = *g_th;
    float bh = px(box_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (t.id && t.w > 0 && t.h > 0) {
        float s = bh / (float)t.h;
        float iw = t.w * s, ih = bh;
        if (iw > avail_w) { s = avail_w / (float)t.w; iw = avail_w; ih = t.h * s; }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddImageRounded(tid(t), mn, mx, ImVec2(0,0), ImVec2(1,1),
                            imcol(lng_rgba(1,1,1,1)), px(4.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(4.0f), 0, px(1.0f));
        ImGui::Dummy(ImVec2(iw, ih));
    } else {
        // No box art was supplied for this game — draw a tasteful SNES-cartridge
        // placeholder so the GAME card never shows dead space. Game-agnostic: any
        // title that declares no boxart.tga gets this instead of an empty slot.
        float iw = bh * 0.72f;               // match a box-art portrait aspect
        if (iw > avail_w) iw = avail_w;
        float ih = bh;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddRectFilled(mn, mx, imcol(th.panel_hovered), px(6.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(6.0f), 0, px(1.0f));

        // cartridge body, centered in the slot
        float cw = iw * 0.52f, ch = cw * 1.04f;
        float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
        ImVec2 bmn = ImVec2(cx - cw * 0.5f, cy - ch * 0.5f);
        ImVec2 bmx = ImVec2(cx + cw * 0.5f, cy + ch * 0.5f);
        dl->AddRectFilled(bmn, bmx, imcol(th.accent_dim), cw * 0.10f);
        // top ridges
        for (int i = 0; i < 3; i++) {
            float rx = bmn.x + cw * (0.20f + i * 0.24f);
            dl->AddRectFilled(ImVec2(rx, bmn.y - ch * 0.05f),
                              ImVec2(rx + cw * 0.12f, bmn.y + ch * 0.10f),
                              imcol(th.accent), cw * 0.03f);
        }
        // recessed label window
        dl->AddRectFilled(ImVec2(bmn.x + cw * 0.16f, bmn.y + ch * 0.30f),
                          ImVec2(bmx.x - cw * 0.16f, bmx.y - ch * 0.16f),
                          imcol(th.panel), cw * 0.04f);
        ImGui::Dummy(ImVec2(iw, ih));
    }
}

// A verified/failed state marker: mint check or amber cross. Replaces the
// [MATCH] badge that crowded the panel edge.
void state_mark(bool ok, const LauncherTheme& th) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight();
    ImU32 c = imcol(ok ? th.good : th.warn);
    float y = p.y + s * 0.5f;
    if (ok) {
        dl->AddLine(ImVec2(p.x + s*0.16f, y), ImVec2(p.x + s*0.40f, y + s*0.26f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.26f), ImVec2(p.x + s*0.84f, y - s*0.26f), c, px(2.0f));
    } else {
        dl->AddLine(ImVec2(p.x + s*0.22f, y - s*0.24f), ImVec2(p.x + s*0.78f, y + s*0.24f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.78f, y - s*0.24f), ImVec2(p.x + s*0.22f, y + s*0.24f), c, px(2.0f));
    }
    ImGui::Dummy(ImVec2(s, s));
}

// Pick the verdict icon for VerifyResult.verdict (0 none,1 ok,2 warn,3 bad).
const LauncherTexture& verdict_texture(int verdict) {
    switch (verdict) {
        case 1:  return g_verdict_ok;
        case 2:  return g_verdict_warn;
        case 3:  return g_verdict_bad;
        default: return g_verdict_none;
    }
}

// Disc Selection dropdown for a multi-image title (GameInfo.discs). Draws
// nothing at all for a single-image game, so every existing launcher keeps
// its current layout. Selecting a row remounts that disc: the model re-runs
// the disc verdict against it and records the choice in Settings.disc_index,
// which the host persists like any other setting.
void draw_disc_selector(LauncherModel* m, const LauncherTheme& th, float availw) {
    const int count = launcher_model_disc_count(m);
    if (count <= 1) return;
    const int sel = launcher_model_disc_selected(m);

    ImGui::TextColored(col(th.text_muted), "%s", ui_text("Disc Selection"));
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetNextItemWidth(availw);
    if (ImGui::BeginCombo("##disc_selection",
                          sel >= 0 ? launcher_model_disc_label(m, sel) : "")) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(launcher_model_disc_label(m, i), i == sel))
                launcher_model_select_disc(m, i);
            if (i == sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, px(10)));
}

// Disc-verdict block (verify.mode==1 systems, e.g. PSX): a verdict icon +
// headline, followed by a Serial/Region/ISO-header checklist. Replaces the
// CRC/SHA "verified" line that mode==0 (cart/ROM-hash) systems draw instead
// (see draw_game_panel) — same slot in the card, different module. Reads
// m->verify, populated by launcher_model_set_rom()/run_verify() in
// launcher_model.c (real probe when the SystemProfile has one, a synthesized
// placeholder verdict otherwise).
void draw_verdict_block(LauncherModel* m, const LauncherTheme& th, float availw) {
    const VerifyResult& v = m->verify;
    // Keep the Serial/Region/ISO checklist mounted even before a disc is
    // picked (setup wizard) so AutoResize modals don't jump when verify runs.
    const bool pending = !m->rom_present;
    const char* headline =
        pending          ? "No disc selected" :
        v.verdict == 1   ? "Disc verified" :
        v.verdict == 2   ? "Disc verified (warnings)" :
        v.verdict == 3   ? "Disc verification failed" :
                           "Disc not recognized";
    const char* headline_text = ui_text(headline);
    // th has no dedicated "bad"/error slot (only good/warn) — reuse warn for
    // the warn AND none cases (both are cautionary, matching the ROM-hash
    // line's existing amber-for-"not recognized" convention) and fall back to
    // a plain red only for the explicit "bad" verdict. Pending = muted.
    LngColor headline_color = pending ? th.text_muted
                              : (v.verdict == 1) ? th.good
                              : (v.verdict == 3) ? lng_rgba(0.945f, 0.322f, 0.322f, 1.0f)
                              : th.warn;

    const LauncherTexture& icon = verdict_texture(pending ? 0 : v.verdict);
    float ih = ImGui::GetTextLineHeight() * 1.35f;
    float iw = (icon.id && icon.h > 0) ? ih * ((float)icon.w / (float)icon.h) : ih;
    float w = iw + px(6) + ImGui::CalcTextSize(headline_text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availw - w) * 0.5f);
    if (icon.id) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(tid(icon), p, ImVec2(p.x + iw, p.y + ih));
        ImGui::Dummy(ImVec2(iw, ih));
    } else if (!pending) {
        state_mark(v.verdict == 1, th);   // icon failed to load: vector fallback
    } else {
        ImGui::Dummy(ImVec2(iw, ih));
    }
    ImGui::SameLine(0, px(6));
    ImGui::TextColored(col(headline_color), "%s", headline_text);
    ImGui::Dummy(ImVec2(0, px(8)));

    // Disc Selection: which image of a multi-disc set Play boots. It sits
    // ABOVE the identity checklist on purpose — Serial / Region / ISO header
    // describe the SELECTED disc (each disc of a set carries its own serial),
    // so the control that decides which disc that is has to read first.
    // Single-disc titles never compose this.
    draw_disc_selector(m, th, availw);

    // Checklist: Serial / Region / ISO header. Before a disc is chosen, show
    // em-dashes with no pass/fail marks so the layout still reserves the rows.
    if (ImGui::BeginTable("verdict_checklist", 3, ImGuiTableFlags_SizingStretchProp)) {
        const float label_width = std::max(px(96), ImGui::CalcTextSize(ui_text("ISO header")).x + px(18));
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, px(28));
        const char* dash = "\xE2\x80\x94";
        kv_row("Serial",     pending ? dash : (v.serial[0] ? v.serial : dash),
               th, !pending, v.serial[0] != '\0');
        kv_row("Region",     pending ? dash : (v.region[0] ? v.region : dash),
               th, !pending, v.region[0] != '\0');
        kv_row("ISO header", pending ? dash : (v.iso_ok ? "OK" : "Mismatch"),
               th, !pending, v.iso_ok);
        const char* sbi_text = v.sbi_status == RECOMP_SBI_OK ? "OK" :
                              v.sbi_status == RECOMP_SBI_MISSING ? "Missing" : "N/A";
        kv_row("SBI File", pending ? dash : sbi_text,
               th, !pending && v.sbi_status != RECOMP_SBI_NA,
               v.sbi_status == RECOMP_SBI_OK);
        // TOC fingerprinting is a netplay capability, not part of ordinary
        // offline disc identification. Never expose it for offline titles,
        // even if a host accidentally leaves stale netplay fields populated.
        if (m->netplay_supported && !pending &&
            (v.track_count > 0 || v.netplay_detail[0])) {
            char tracks_buf[32];
            if (v.track_count > 0)
                std::snprintf(tracks_buf, sizeof(tracks_buf), "%d", v.track_count);
            else
                std::snprintf(tracks_buf, sizeof(tracks_buf), "%s", dash);
            kv_row("Tracks", tracks_buf, th, true, v.netplay_ok != 0);
        }
        ImGui::EndTable();
    }
    if (m->netplay_supported && !pending &&
        v.netplay_detail[0] && !v.netplay_ok) {
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availw);
        ImGui::TextColored(col(th.warn), "%s", v.netplay_detail);
        ImGui::PopTextWrapPos();
    }
}

void draw_game_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("game", 0, fill_h)) { end_panel(); return; }
    // No "GAME" eyebrow: the box art itself tells the user this is the game.
    const float availw = ImGui::GetContentRegionAvail().x;

    // Verify module: verify.mode==1 systems (PSX) render a disc-verdict block
    // (icon + Serial/Region/ISO checklist) here instead of the CRC/SHA line;
    // mode==0 systems (SNES/cart) keep the CRC/SHA line exactly as before.
    const bool disc_verdict = m->profile && m->profile->verify.mode == 1;

    // Box art on top (centered), everything else BELOW it. Height is derived
    // from the space actually left after the metadata + button, so the art is
    // as large as it can be WITHOUT pushing the last row out of the card.
    {
        // Reserve space for everything under the art: verified line + 2 meta rows
        // + Change ROM, plus the SAVES block when this game has battery SRAM.
        float reserve = px(198.0f);
        if (disc_verdict) reserve += px(120.0f);          // taller: icon+headline + tracks row
        // Disc Selection label + combo + spacing, for a multi-image title.
        if (launcher_model_disc_count(m) > 1) reserve += px(62.0f);
        if (m->saves_supported) reserve += px(96.0f);    // compact SAVES row below Change ROM
        if (m->password_save_path) reserve += px(96.0f); // password-save row (same footprint)
        if (m->msu1_patch_available) reserve += px(198.0f);  // MSU-1 patch-available sub-block
                                                              // (title + up-to-3-line wrapped note + 2 stacked buttons)
        float art_h = ImGui::GetContentRegionAvail().y - reserve;
        if (art_h > px(368.0f)) art_h = px(368.0f);   // allow a larger hero box art (~15% bigger than before)
        if (art_h < px(248.0f)) art_h = px(248.0f);   // keep it big enough to balance the side column
        hero_boxart_centered(g_boxart, art_h, availw);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Region + verification state, centered under the art.
    const char* noun = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
    if (disc_verdict) {
        draw_verdict_block(m, th, availw);
    } else {
        const bool verified = launcher_model_rom_verified(m);
        char line[64];
        if (!m->rom_present)   snprintf(line, sizeof(line), "No %s loaded", noun);
        else if (verified)     snprintf(line, sizeof(line), "%s verified", noun);
        else                   snprintf(line, sizeof(line), "%s not recognized", noun);
        float w = ImGui::GetTextLineHeight() + px(6) + ImGui::CalcTextSize(line).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availw - w) * 0.5f);
        state_mark(verified, th);
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(verified ? col(th.good) : col(th.warn), "%s", line);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Metadata a PLAYER cares about — just Region + File. The "is my ROM good?"
    // question is answered by the ROM-verified line above; raw size and CRC/SHA
    // digests are developer noise, so they're not shown.
    if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(76));
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        // The disc-verdict block already reports Region in its checklist, so
        // don't repeat it here. Otherwise show Region only when the host gave
        // one — an empty value would render a bare "Region" label with nothing
        // beside it, which reads as a bug.
        if (!disc_verdict && m->region[0])
            kv_row("Region", m->region, th, false, false);
        kv_row("File",   m->rom_file, th, false, false);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    // "Browse For Disc 2", not "Change Disc": on a multi-disc set the button
    // does not swap which disc the game is on — the Disc Selection dropdown
    // above does that — it re-points the SELECTED disc at a file on this
    // machine. Naming the disc number is what keeps those two apart. A
    // single-image title has nothing to number, so it reads "Browse For Disc"
    // ("Browse For ROM" on cartridge consoles, from the profile's rom_noun).
    const int disc_no = launcher_model_disc_count(m) > 1
                            ? launcher_model_disc_number(
                                  m, launcher_model_disc_selected(m))
                            : 0;
    char change_label[48];
    if (disc_no > 0)
        snprintf(change_label, sizeof(change_label), "%s %s %d",
                 ui_text("Browse For"), ui_text(noun), disc_no);
    else
        snprintf(change_label, sizeof(change_label), "%s %s",
                 ui_text("Browse For"), ui_text(noun));
    if (m->import_sbi_cb)
        std::strncat(change_label, " / SBI", sizeof(change_label) - std::strlen(change_label) - 1);
    if (ImGui::Button(change_label, ImVec2(availw, px(34)))) {
        // Native file dialog filter comes from the active console's
        // SystemProfile.rom_filter — never a hardcoded per-system set. Every
        // shipped profile supplies one; the fallback is console-NEUTRAL (all
        // files, titled with this console's own rom_noun) so a profile that
        // forgets rom_filter degrades to "any file" rather than prompting for
        // some other machine's media.
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        char title[64];
        if (disc_no > 0)
            snprintf(title, sizeof(title), "Select %s %d", noun, disc_no);
        else
            snprintf(title, sizeof(title), "Select %s%s", noun, m->import_sbi_cb ? " / SBI" : "");
        if (prof && prof->rom_filter.patterns && prof->rom_filter.pattern_count > 0)
            request_rom_picker(m, title, prof->rom_filter.patterns,
                               prof->rom_filter.pattern_count,
                               prof->rom_filter.desc, false);
        else
            request_rom_picker(m, title, NULL, 0, NULL, false);
    }

    // No desktop file-picker service, or the one this host has just failed:
    // say so and give the in-app browser its own control, rather than leaving
    // the player with a Browse button whose backend cannot run.
    if (builtin_file_picker_should_be_offered()) {
        ImGui::TextColored(col(th.text_muted), "%s",
                           g_native_picker_failed
                               ? "The desktop file picker could not run."
                               : "No desktop file picker (zenity/kdialog) found.");
        char builtin_label[64];
        snprintf(builtin_label, sizeof(builtin_label), "%s %s",
                 ui_text("Browse For"), ui_text(noun));
        std::strncat(builtin_label, " (built-in)",
                     sizeof(builtin_label) - std::strlen(builtin_label) - 1);
        if (ImGui::Button(builtin_label, ImVec2(availw, px(28)))) {
            const SystemProfile* prof = (const SystemProfile*)m->profile;
            char title[64];
            snprintf(title, sizeof(title), "Select %s", noun);
            PickRequest req =
                (prof && prof->rom_filter.patterns &&
                 prof->rom_filter.pattern_count > 0)
                    ? rom_pick_request(m, title, prof->rom_filter.patterns,
                                       prof->rom_filter.pattern_count,
                                       prof->rom_filter.desc, false)
                    : rom_pick_request(m, title, NULL, 0, NULL, false);
            req.on_pick = [m](const char* path) {
                if (path) launcher_model_set_rom(m, path);
            };
            open_builtin_picker(m, std::move(req));
        }
    }

    if (m->setup_error[0]) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(col(th.warn), "%s", m->setup_error);
        ImGui::PopTextWrapPos();
    }

    // MSU-1 patch-available sub-block: this game ships an IPS patch that
    // converts the verified vanilla ROM into its MSU-1 streamed-audio variant.
    // Ported from the legacy launcher's dashboard "MSU-1 patch available" card
    // (snesrecomp/runner/src/launcher/launcher_gui.cpp: msu1_patch_available +
    // do_patch()/patch_rom/skip_patch). "warn" amber styling — this is a
    // choice the player should notice, not a routine control.
    if (m->msu1_patch_available) {
        ImGui::Dummy(ImVec2(0, px(10)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col(th.panel_hovered));
        ImGui::PushStyleColor(ImGuiCol_Border, col(th.warn));
        if (ImGui::BeginChild("msu1_patch_block", ImVec2(availw, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::TextColored(col(th.warn), "MSU-1 patch available");
            const float inner_w = ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner_w);
            ImGui::TextColored(col(th.text_muted), "%s",
                (m->msu1_note && m->msu1_note[0])
                    ? m->msu1_note
                    : "An MSU-1 patch exists for this game. Patch a copy beside "
                      "your ROM (the original is never modified)?");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, px(8)));
            // Stacked full-width buttons, not side-by-side: "Skip (Play Unpatched)"
            // is long enough that splitting the row in half clips its label at
            // common card widths (verified via the LNG_DEMO_MSU harness).
            if (ImGui::Button(ui_text("Patch ROM"), ImVec2(inner_w, px(32))))
                launcher_model_apply_msu1_patch(m);
            ImGui::Dummy(ImVec2(0, px(th.spacing_xs)));
            if (ImGui::Button(ui_text("Skip (Play Unpatched)"), ImVec2(inner_w, px(32))))
                launcher_model_skip_msu1_patch(m);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    // SAVES lives in the GAME card as a compact row (no separate card / eyebrow).
    // Present only for games with battery SRAM — data-driven, never by name.
    // Content lives in draw_save_row() (the Save module's shared row-drawer,
    // also used standalone by panel_save's own card — see below); folding it
    // in here, uncarded, is what preserves today's exact GAME-card layout.
    if (m->saves_supported || m->password_save_path || m->password_sram_path) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PushStyleColor(ImGuiCol_Separator, col(th.border));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, px(6)));
        draw_save_row(m, th);
    }
    end_panel();
}

// Save module (docs/ARCHITECTURE.md): one reusable picker row — label + path +
// Import/Clear. SAVE_SRAM and (until the block-grid UI lands) SAVE_MEMCARD
// both render this same compact row: kind-switched data, one widget.
void draw_save_row(LauncherModel* m, const LauncherTheme& th) {
    // Password/mantra save variant (e.g. Faxanadu): the row shows the current
    // password text instead of a binary save file. Editable behind an Edit ->
    // type -> Save confirm step, mirroring the legacy NES launcher's flow.
    if (m->password_save_path || m->password_sram_path) {
        static bool s_pw_editing = false;
        static char s_pw_buf[128];
        const char* label = (m->password_sram_label && m->password_sram_label[0])
                              ? m->password_sram_label
                              : ((m->password_save_label && m->password_save_label[0])
                                     ? m->password_save_label : "Password");
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        // Value sits right after the label's actual end with comfortable
        // padding — a RELATIVE gap, not an absolute column, so a wider label
        // ("Password") never clips under the value regardless of row indent.
        ImGui::SameLine(0.0f, px(th.spacing_lg));
        const float bw = px(84);
        if (!s_pw_editing) {
            ImGui::AlignTextToFramePadding();
            if (m->password_text[0]) ImGui::TextUnformatted(m->password_text);
            else ImGui::TextColored(col(th.text_muted), "(Not set)");
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
            if (ImGui::Button(ui_text("Edit"), ImVec2(bw, px(30)))) {
                snprintf(s_pw_buf, sizeof(s_pw_buf), "%s", m->password_text);
                s_pw_editing = true;
            }
        } else {
            float avail = ImGui::GetContentRegionAvail().x - bw * 2 - px(th.spacing_sm) * 2;
            if (avail < px(80)) avail = px(80);
            ImGui::SetNextItemWidth(avail);
            ImGui::InputText("##pwedit", s_pw_buf, sizeof(s_pw_buf));
            ImGui::SameLine(0, px(th.spacing_sm));
            if (ImGui::Button(ui_text("Save"), ImVec2(bw, px(30)))) {
                launcher_model_password_commit(m, s_pw_buf);
                s_pw_editing = false;
            }
            ImGui::SameLine(0, px(th.spacing_sm));
            if (ImGui::Button(ui_text("Cancel"), ImVec2(bw, px(30))))
                s_pw_editing = false;
        }
        if (!m->saves_supported) return;   // password-only game: no SRAM row below
        ImGui::Dummy(ImVec2(0, px(4)));
    }
    const char* sp = m->sram_path ? m->sram_path : "";
    const char* base = sp;
    for (const char* q = sp; *q; ++q) if (*q == '/' || *q == '\\') base = q + 1;

    // Reflect the ACTUAL save file state, not just the configured path. Showing
    // the filename unconditionally made a present and an absent save look
    // identical — so Clear (which correctly no-ops when there's nothing to
    // delete) read as "broken". Stat the file each frame: show its size when it
    // exists, "no save yet" when it doesn't, and disable Clear when empty so the
    // button's effect is always visible.
    long sz = -1;
    if (sp[0]) { FILE* f = fopen(sp, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); } }
    const bool has_save = sz >= 0;

    const float bw = px(84);
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ui_text("Save"));
    ImGui::PopStyleColor();
    ImGui::SameLine(px(76));
    ImGui::AlignTextToFramePadding();
    if (has_save) {
        // Just the file name (no size annotation) — right-elided with "…" so a
        // long name never runs under the Import/Clear buttons to its right.
        const float buttons_w = bw * 2 + px(th.spacing_sm);
        const float avail = ImGui::GetContentRegionAvail().x - buttons_w - px(th.spacing_md);
        char shown[128];
        snprintf(shown, sizeof shown, "%s", base);
        if (avail > 0 && ImGui::CalcTextSize(shown).x > avail) {
            size_t n = strlen(base);
            while (n > 0) {
                char tmp[132];
                snprintf(tmp, sizeof tmp, "%.*s\xE2\x80\xA6", (int)n, base);  // "<head>…"
                if (ImGui::CalcTextSize(tmp).x <= avail) { snprintf(shown, sizeof shown, "%s", tmp); break; }
                --n;
            }
            if (n == 0) snprintf(shown, sizeof shown, "\xE2\x80\xA6");
        }
        ImGui::TextUnformatted(shown);
    } else {
        ImGui::TextColored(col(th.text_muted), "no save yet");
    }
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw*2 - px(th.spacing_sm));
    static const char* kSramPatterns[] = { "*.srm", "*.sav" };
    if (ImGui::Button("Import", ImVec2(bw, px(30)))) {
        ui_pick_file(m, "Import SRAM save", {"*.srm", "*.sav"},
                     "Battery save (.srm .sav)", [m](const char* path) {
                         if (path)
                             launcher_model_import_sram(m, path);   // backs up existing to .bak, then copies in
                     });
    }
    ImGui::SameLine(0, px(th.spacing_sm));
    ImGui::BeginDisabled(!has_save);              // nothing to clear when no save exists
    if (ImGui::Button(ui_text("Clear"), ImVec2(bw, px(30))))
        launcher_model_clear_sram(m);             // backs up to .bak, then deletes
    ImGui::EndDisabled();
}

// ---- panel adapters: LauncherPanelDrawFn = void(LauncherModel*, const LauncherTheme*) ----
void panel_game_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_game_panel(m, *th, g_game_fill_h);
}

// ---- Save module, SAVE_MEMCARD half (PSX) -----------------------------------
// SAVE_SRAM keeps the compact row above, folded into the GAME card (SNES,
// unchanged). SAVE_MEMCARD (PSX) is a standalone WIDE dashboard panel (see
// kPanelsDashboardPsx in launcher_system.h): one sub-section per card slot —
// icon + path picker (Browse/New) + a real 15-block usage grid, matching PS1
// memory-card conventions (each card holds 15 save blocks).

// One compact memory-card slot: icon + name, inline block count, and Browse/
// New actions. `probe` (SystemProfile.save.probe) is the host hook that
// would refresh m->memcard_blocks_used[slot] from the real card image; it is
// NULL in every profile today (unimplemented proto hook), so this falls back to
// a representative placeholder count rather than an all-empty slot.
// Draws the CONTENT of one small memory-card card (the caller supplies the card
// chrome + width). Vertical stack so nothing crowds at a controller-narrow
// width: icon + label, block count, then a Browse/New button pair.
void draw_memcard_slot(LauncherModel* m, const LauncherTheme& th, int slot) {
    ImGui::PushID(slot);

    const bool enabled = m->s.memcard_enabled[slot] != 0;

    const float slotw   = ImGui::GetContentRegionAvail().x;
    const float start_x = ImGui::GetCursorPosX();
    const float top_y   = ImGui::GetCursorPosY();

    // Block usage source is decided by the model (real inspect result first;
    // see launcher_model_memcard_blocks_used for the fallback order).
    const uint16_t used = launcher_model_memcard_blocks_used(m, slot);
    int used_count = 0;
    for (int i = 0; i < 15; ++i) if (used & (1u << i)) ++used_count;

    // --- Header: memory-card image on the LEFT; the Enabled toggle (top-right)
    // and the card name + block count (under the toggle) on the RIGHT. The
    // block grid + Browse/New sit BELOW the image, full width. ---
    // Image is fit by height (memcard.tga is 148x164 portrait). In the
    // fill-height (wide PSX) layout it grows to fill the card, reserving room
    // below for the grid + buttons and capping its width to ~half the card so
    // the right column keeps room for the toggle and name.
    float img_h;
    if (g_save_fill_h) {
        const float avail_h = ImGui::GetContentRegionAvail().y;
        // Grid + gap + Browse/New + bottom pad — keep buttons clear of the frame.
        img_h = avail_h - px(110.0f);
        const float img_h_by_w = (0.50f * slotw) * (164.0f / 148.0f);  // cap width ~half
        if (img_h > img_h_by_w) img_h = img_h_by_w;
        if (img_h < px(92.0f))  img_h = px(92.0f);
        if (img_h > px(200.0f)) img_h = px(200.0f);
    } else {
        img_h = px(108.0f);
    }
    float iw = img_h * (148.0f / 164.0f), ih = img_h;
    if (g_memcard.w > 0 && g_memcard.h > 0) {
        const float s = img_h / (float)g_memcard.h;
        iw = g_memcard.w * s;
        ImGui::Image(tid(g_memcard), ImVec2(iw, ih));
    } else {
        ImGui::Dummy(ImVec2(iw, ih));
    }

    const float rc_x    = start_x + iw + px(14.0f);   // right column x
    const float frame_h = ImGui::GetFrameHeight();
    const float line_h  = ImGui::GetTextLineHeight();

    // Enabled toggle at the top of the right column — left-aligned at rc_x so
    // it lines up vertically with the card name and block count below it.
    {
        ImGui::SetCursorPos(ImVec2(rc_x, top_y));
        bool enabled_box = enabled;
        if (ImGui::Checkbox(ui_text("Enabled"), &enabled_box))
            launcher_model_toggle_memcard(m, slot);
    }

    // Card name, on its own line under the toggle.
    const char* mp = m->s.memcard_path[slot];
    const char* base = mp;
    for (const char* q = mp; *q; ++q) if (*q == '/' || *q == '\\') base = q + 1;
    char label[40];
    if (base[0]) snprintf(label, sizeof(label), "%s", base);
    else         snprintf(label, sizeof(label), "%s %d", ui_text("Memory Card"), slot + 1);
    ImGui::SetCursorPos(ImVec2(rc_x, top_y + frame_h + px(10.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, col(enabled ? th.accent : th.text_muted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    // Block count under the name.
    char cap[16]; snprintf(cap, sizeof(cap), "%d / 15", used_count);
    ImGui::SetCursorPos(ImVec2(rc_x, top_y + frame_h + px(10.0f) + line_h + px(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(cap);
    ImGui::PopStyleColor();

    // Resume the card body BELOW the image, full width.
    ImGui::SetCursorPos(ImVec2(start_x, top_y + ih + px(14.0f)));

    // Dim the rest of the slot body when disabled (visual only; the Browse/New
    // controls stay clickable so the slot can be re-configured while off).
    const float body_alpha = enabled ? 1.0f : 0.4f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * body_alpha);

    // 15-block usage grid, full width under the image.
    {
        const int   kB   = 15;
        const float bgap = px(4.0f);
        const float availw = ImGui::GetContentRegionAvail().x;
        float cell = (availw - bgap * (kB - 1)) / (float)kB;
        if (cell > px(18.0f)) cell = px(18.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        for (int i = 0; i < kB; ++i) {
            const bool onb = (used & (1u << i)) != 0;
            const ImVec2 mn(p0.x + i * (cell + bgap), p0.y);
            const ImVec2 mx(mn.x + cell, mn.y + cell);
            dl->AddRectFilled(mn, mx, imcol(onb ? th.accent : th.control), px(3.0f));
            dl->AddRect(mn, mx, imcol(th.border), px(3.0f), 0, px(1.0f));
        }
        ImGui::Dummy(ImVec2(cell * kB + bgap * (kB - 1), cell));
    }

    // Gap above Browse/New only — bottom inset is Child WindowPadding (same as
    // the top). An extra bottom Dummy doubled the pad and looked top-heavy.
    static const char* kCardPatterns[] = { "*.mcd", "*.mcr", "*.mc" };
    const float cw = ImGui::GetContentRegionAvail().x;
    const float bw = (cw - px(th.spacing_sm)) * 0.5f;
    const float btn_h = px(32.0f);
    const float btn_gap = px(16.0f);
    if (g_save_fill_h) {
        const float slack = ImGui::GetContentRegionAvail().y - btn_h;
        ImGui::Dummy(ImVec2(0, slack > btn_gap ? slack : btn_gap));
    } else {
        ImGui::Dummy(ImVec2(0, btn_gap));
    }
    if (ImGui::Button(ui_text("Browse"), ImVec2(bw, btn_h))) {
        ui_pick_file(m, "Select memory card image", {"*.mcd", "*.mcr", "*.mc"},
                     "PS1 memory card (.mcd .mcr .mc)",
                     [m, slot](const char* path) {
                         if (path) launcher_model_set_memcard_path(m, slot, path);
                     });
    }
    ImGui::SameLine(0, px(th.spacing_sm));
    if (ImGui::Button(ui_text("New"), ImVec2(bw, btn_h))) {
        // "New" picks a DESTINATION (the file need not exist yet — a save
        // dialog, not the open dialog Browse uses), then writes a real,
        // freshly formatted blank 128KB card there and adopts it.
        ui_pick_save_file(m, "Create new memory card",
                          {"*.mcd", "*.mcr", "*.mc"}, "PS1 memory card (.mcd)",
                          [m, slot](const char* path) {
                              if (path) launcher_model_new_memcard(m, slot, path);
                          });
    }

    ImGui::PopStyleVar();  // body_alpha
    ImGui::PopID();
}

// Available whenever this system's SaveSpec says there's something to show:
// SAVE_MEMCARD (PSX) always offers the panel — the memory-card slots exist
// independent of whether the GAME itself also has legacy SRAM — while
// SAVE_SRAM keeps the original per-GAME gate (sram_path != NULL) and
// SAVE_NONE stays hidden.
int avail_save(const LauncherModel* m) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const SaveKind kind = prof ? prof->save.kind : SAVE_NONE;
    if (kind == SAVE_MEMCARD) return 1;
    if (kind == SAVE_SRAM)    return m->saves_supported;
    return 0;
}

// Fixed logical width for dashboard player / memcard cards. Extra horizontal
// space adds more columns instead of stretching each card.
static float dash_card_width(float availw, float gap, int count) {
    const float pref = px(300.0f);
    if (count < 1) count = 1;
    int cols = (int)((availw + gap) / (pref + gap));
    if (cols < 1) cols = 1;
    if (cols > count) cols = count;
    float cardw = pref;
    if (cols == 1 && availw < pref) cardw = availw;  // narrow: shrink to fit
    return cardw;
}

static int dash_card_columns(float availw, float gap, float cardw, int count) {
    if (count < 1) return 1;
    int cols = (int)((availw + gap) / (cardw + gap));
    if (cols < 1) cols = 1;
    if (cols > count) cols = count;
    return cols;
}

void panel_save_draw(LauncherModel* m, const LauncherTheme* th) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const SaveKind kind = prof ? prof->save.kind : SAVE_NONE;
    if (kind == SAVE_MEMCARD) {
        // No outer "MEMORY CARDS" card/eyebrow: the small per-slot cards ARE the
        // UI. Fixed width (same as player cards); wider windows add columns
        // instead of stretching the pair across the column.
        const int slots = (prof->save.slots > 0 && prof->save.slots <= 2) ? prof->save.slots : 2;
        const float gap = px(th->spacing_sm);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float cw = dash_card_width(avail, gap, slots);
        const int cols = dash_card_columns(avail, gap, cw, slots);
        // Multitap fill-height: slot cards take the reserved band under the
        // controller stack. 2P / narrow leave g_save_fill_h false and hug.
        const float fill_h = ImGui::GetContentRegionAvail().y;
        const bool do_fill = g_save_fill_h && fill_h > px(180.0f);
        g_save_fill_h = do_fill;   // draw_memcard_slot reads this for the bottom slack
        for (int slot = 0; slot < slots; ++slot) {
            if (slot % cols) ImGui::SameLine(0, gap);
            else if (slot) ImGui::Dummy(ImVec2(0, gap));
            char cid[16]; snprintf(cid, sizeof(cid), "mcc%d", slot);
            char pid[16]; snprintf(pid, sizeof(pid), "mcp%d", slot);
            begin_container(cid, ImVec2(cw, do_fill ? fill_h : 0.0f),
                            do_fill ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
                if (begin_panel(pid, cw, do_fill, do_fill))
                    draw_memcard_slot(m, *th, slot);
                end_panel();
            end_container();
        }
        g_save_fill_h = false;
        return;
    }
    if (!begin_panel("save", 0)) { end_panel(); return; }
    eyebrow("SAVES");
    draw_save_row(m, *th);
    end_panel();
}

// ---- N64 Transfer Pak: one card per controller port -----------------------
// Composes only for games whose GameInfo passes tpak_slots > 0 (the Stadium
// titles) — the availability gate below keeps it off every other console/game.
static const char* elide_left(const char* s, float max_w, char* out, size_t cap);  // fwd (defined below)

int avail_tpak(const LauncherModel* m) { return m->tpak_slots > 0; }

// Real GB-cartridge art for a Transfer Pak slot, picked by the host-reported
// cart kind (1 red / 2 blue / 3 yellow / 4 green); the gray empty shell for an
// empty slot or a recognized-but-uncolored cart. Falls back to a blank box if
// the art didn't load. `box` is the logical fit size.
void draw_tpak_cart(int cart_kind, bool present, float box) {
    const int idx = (present && cart_kind >= 1 && cart_kind <= 4) ? cart_kind : 0;
    image_fit(g_cart[idx], box, box);   // image_fit Dummies when the texture is absent
}

static const char* rui_basename(const char* path) {
    const char* base = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    return base;
}

// Transfer Pak config modal state. A tile click stages a request (open_req);
// the modal itself is drawn once per frame at root scope (draw_tpak_modal) so
// OpenPopup and BeginPopupModal share the same ID stack from wherever a tile
// was clicked (dashboard row OR the Controller page).
static int g_tpak_open_req  = -1;
static int g_tpak_modal_slot = -1;

// One compact port tile: the cartridge (gray shell when empty, colored R/B/Y
// once a cart is set), the cart name, and a Configure button — clicking either
// the cart or Configure opens the config modal. This is ALL that shows inline
// now; picking cart + save happens in the modal, so the dashboard row stays a
// short strip of carts instead of tall cards that push the layout into a
// scroll.
void draw_tpak_tile(LauncherModel* m, const LauncherTheme& th, int slot) {
    char eb[24]; snprintf(eb, sizeof(eb), "TRANSFER PAK %d", slot + 1);
    eyebrow(eb);

    const bool has_cart  = m->s.tpak_rom_path[slot][0] != '\0';
    const bool inspected = m->tpak_inspected[slot];
    const RecompLauncherCTpak* info = &m->tpak_info[slot];
    const float inner = ImGui::GetContentRegionAvail().x;

    // Centered cartridge art, itself a click target that opens the modal.
    ImGui::PushID(slot);
    const float art = px(66);
    ImVec2 art_cursor = ImGui::GetCursorScreenPos();
    image_fit_centered(g_cart[(has_cart && inspected && info->cart_kind >= 1 &&
                               info->cart_kind <= 4) ? info->cart_kind : 0], 60, 66, inner);
    // invisible hit-box over the art
    ImGui::SetCursorScreenPos(art_cursor);
    if (ImGui::InvisibleButton("cart_hit", ImVec2(inner, art))) g_tpak_open_req = slot;

    // name line (centered), muted "Empty" when nothing inserted
    const char* label = !has_cart ? "Empty"
                        : (inspected && info->cart_label[0])
                            ? info->cart_label : rui_basename(m->s.tpak_rom_path[slot]);
    float lw = ImGui::CalcTextSize(label).x;
    if (lw < inner) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - lw) * 0.5f);
    ImGui::TextColored(has_cart ? col(th.text) : col(th.text_muted), "%s", label);
    ImGui::Dummy(ImVec2(0, px(4)));

    if (ImGui::Button(has_cart ? ui_text("Configure") : ui_text("Insert..."),
                      ImVec2(ImGui::GetContentRegionAvail().x, px(28))))
        g_tpak_open_req = slot;
    ImGui::PopID();
}

// The per-port config surface, drawn as a modal. Everything cartridge-related
// lives here: a large live cart preview, the cartridge/trainer facts, and the
// Change / Remove / save-file actions. Reached from any tile (dashboard or
// Controller page). Draw ONCE per frame at root scope.
void draw_tpak_modal(LauncherModel* m, const LauncherTheme& th) {
    if (g_tpak_open_req >= 0) {
        g_tpak_modal_slot = g_tpak_open_req;
        g_tpak_open_req = -1;
        ImGui::OpenPopup("Transfer Pak");
    }
    ImGui::SetNextWindowSize(ImVec2(px(440), 0), ImGuiCond_Appearing);
    // No OS-style title bar (it renders in ImGui's un-themed blue) — the card's
    // own "TRANSFER PAK · PORT n" eyebrow is the heading.
    if (!ImGui::BeginPopupModal("Transfer Pak", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoTitleBar))
        return;
    const int slot = g_tpak_modal_slot;
    if (slot < 0 || slot >= m->tpak_slots) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    const bool has_cart  = m->s.tpak_rom_path[slot][0] != '\0';
    const bool inspected = m->tpak_inspected[slot];
    const RecompLauncherCTpak* info = &m->tpak_info[slot];

    ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
    ImGui::Text("TRANSFER PAK  \xC2\xB7  PORT %d", slot + 1);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, px(6)));

    // Large live cart preview, centered — turns from the gray shell into the
    // colored cart the moment a recognized ROM is picked.
    const float avail = ImGui::GetContentRegionAvail().x;
    image_fit_centered(g_cart[(has_cart && inspected && info->cart_kind >= 1 &&
                               info->cart_kind <= 4) ? info->cart_kind : 0], 132, 138, avail);
    ImGui::Dummy(ImVec2(0, px(6)));

    // name + trainer, centered
    auto centered = [&](ImU32 c, const char* s) {
        float w = ImGui::CalcTextSize(s).x;
        if (w < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, c); ImGui::TextUnformatted(s); ImGui::PopStyleColor();
    };
    if (!has_cart) {
        centered(ImGui::GetColorU32(col(th.text_muted)), "No cartridge inserted");
    } else {
        const char* label = (inspected && info->cart_label[0])
                              ? info->cart_label : rui_basename(m->s.tpak_rom_path[slot]);
        centered(ImGui::GetColorU32(col(th.text)), label);
        char line[80];
        if (inspected && info->trainer_name[0]) {
            if (info->trainer_id[0])
                snprintf(line, sizeof(line), "Trainer %s  \xC2\xB7  ID %s",
                         info->trainer_name, info->trainer_id);
            else snprintf(line, sizeof(line), "Trainer %s", info->trainer_name);
        } else snprintf(line, sizeof(line), "No save data");
        centered(ImGui::GetColorU32(col(th.text_muted)), line);
    }

    ImGui::Dummy(ImVec2(0, px(10)));
    // Change / Remove cartridge
    const float full = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(has_cart ? "Change cartridge..." : "Insert cartridge...",
                      ImVec2(full, px(32)))) {
        ui_pick_file(m, "Select Game Boy cartridge", {"*.gb", "*.gbc"},
                     "Game Boy cartridge (.gb .gbc)",
                     [m, slot](const char* path) {
                         if (path) launcher_model_set_tpak_rom(m, slot, path);
                     });
    }
    if (has_cart) {
        if (ImGui::Button("Remove cartridge", ImVec2(full, px(28))))
            launcher_model_clear_tpak(m, slot);

        // Battery-save row: label + value + Browse / Reset.
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Battery save");
        const char* sv = m->s.tpak_save_path[slot][0]
                           ? rui_basename(m->s.tpak_save_path[slot])
                           : "Default (runtime chooses)";
        char elided[128];
        elide_left(sv, ImGui::GetContentRegionAvail().x, elided, sizeof(elided));
        ImGui::TextUnformatted(elided);
        const float bw = (ImGui::GetContentRegionAvail().x - px(th.spacing_sm)) * 0.5f;
        if (ImGui::Button(ui_text("Browse save..."), ImVec2(bw, px(26)))) {
            ui_pick_file(m, "Select battery save", {"*.sav", "*.srm"},
                         "Battery save (.sav)", [m, slot](const char* path) {
                             if (path) launcher_model_set_tpak_save(m, slot, path);
                         });
        }
        ImGui::SameLine(0, px(th.spacing_sm));
        if (ImGui::Button("Use default", ImVec2(bw, px(26))))
            launcher_model_set_tpak_save(m, slot, "");
    }

    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ImGui::Button("Done", ImVec2(ImGui::GetContentRegionAvail().x, px(30))))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void panel_tpak_draw(LauncherModel* m, const LauncherTheme* th) {
    // Compact per-port tiles in one full-width row (click a cart to configure).
    const int slots = m->tpak_slots;
    const float gap = px(th->spacing_sm);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cw = (avail - gap * (slots - 1)) / (float)slots;
    static const char* kCid[RECOMP_LAUNCHER_MAX_TPAKS] = { "tpc0", "tpc1", "tpc2", "tpc3" };
    static const char* kPid[RECOMP_LAUNCHER_MAX_TPAKS] = { "tpp0", "tpp1", "tpp2", "tpp3" };
    for (int slot = 0; slot < slots; ++slot) {
        if (slot) ImGui::SameLine(0, gap);
        begin_container(kCid[slot], ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY);
            if (begin_panel(kPid[slot], cw, false))
                draw_tpak_tile(m, *th, slot);
            end_panel();
        end_container();
    }
}

// PSX-style pad-mode selector: Analog / D-Pad segmented row. Caller only draws
// this when pad_mode_supported && pad_mode_selectable (a locked mode draws
// nothing — there's nothing to pick). Hybrid is deliberately absent: it is a
// mod-only mode a trusted game plugin requests at runtime, never a player
// choice.
void pad_mode_selector(LauncherModel* m, const LauncherTheme& th, int p, float w) {
    struct Seg { int mode; const char* label; };
    Seg segs[8];
    int n = 0;
    // A console with a custom pad-mode list (ControllerSpec.modes, e.g. Genesis
    // 3-Button/6-Button) drives the segments from that list; otherwise the
    // legacy PSX-shaped Analog/D-Pad set.
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->controller.modes && prof->controller.mode_count > 0) {
        int mc = prof->controller.mode_count;
        if (mc > (int)(sizeof(segs) / sizeof(segs[0]))) mc = (int)(sizeof(segs) / sizeof(segs[0]));
        for (int i = 0; i < mc; ++i)
            segs[n++] = { prof->controller.modes[i].mode, prof->controller.modes[i].label };
    } else {
        segs[n++] = { 1, "Analog" };
        segs[n++] = { 2, "D-Pad" };
    }

    // Keyboard has no analog sticks — Analog is unavailable (PSX modes).
    const bool kb_digital_only =
        m->s.player_src[p] == 1 &&
        !(prof && prof->controller.modes && prof->controller.mode_count > 0);

    const float gap = px(4.0f);
    const float seg_w = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, gap);
        bool sel = m->s.pad_mode[p] == segs[i].mode;
        // Analog needs sticks; grey out on keyboard.
        const bool stick_mode = (segs[i].mode == 1);
        const bool disabled = kb_digital_only && stick_mode;
        ImGui::PushID(i);
        if (disabled) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, col(th.control));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(th.control));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(th.control));
            ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
            ImGui::Button(segs[i].label, ImVec2(seg_w, px(28)));
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? col(th.accent) : col(th.control));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? col(th.accent) : col(th.control_hovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(th.accent));
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? col(th.accent_text) : col(th.text));
            if (ImGui::Button(segs[i].label, ImVec2(seg_w, px(28))))
                launcher_model_set_pad_mode(m, p, segs[i].mode);
            ImGui::PopStyleColor(4);
        }
        ImGui::PopID();
    }
}

// Each player is its OWN self-contained card ("PLAYER 1" as its eyebrow), not a
// floating column inside one big CONTROLLERS box. A 1-player game shows a
// single card (no wasted width); a 2-player game shows two identical cards side
// by side. Same module, composed per the game's declared player count.
// Input-source Selectables shared by the player card (##src) and the Controller
// config (##csrc) combos. When the game sets has_mouse_controls, player 0's
// keyboard entry splits into "Keyboard + Mouse" (mouse-aim on) and "Keyboard"
// (off); otherwise it is the single legacy "Keyboard" entry, byte-for-byte
// identical to before for every non-mouse game.
void draw_source_selectables(LauncherModel* m, int p) {
    const SystemProfile* src_prof = (const SystemProfile*)m->profile;
    const bool psx = src_prof && src_prof->id && !strcmp(src_prof->id, "psx");
    const bool snes_prof = src_prof && src_prof->id &&
                           !strcmp(src_prof->id, "snes");
    const bool n64_prof = src_prof && src_prof->id &&
                          !strcmp(src_prof->id, "n64");
    if (ImGui::Selectable(ui_text("None"), m->s.player_src[p] == 0)) {
        launcher_model_set_source(m, p, 0, 0, nullptr, nullptr);
        if (psx) launcher_binds_refresh(m);
    }
    if (m->has_mouse_controls && p == 0) {
        const bool kbm = m->s.player_src[p] == 1 && m->s.mouse_enabled;
        const bool kb  = m->s.player_src[p] == 1 && !m->s.mouse_enabled;
        if (ImGui::Selectable(ui_text("Keyboard + Mouse"), kbm))
            launcher_model_set_mouse_source(m, 1);
        if (ImGui::Selectable(ui_text("Keyboard"), kb))
            launcher_model_set_mouse_source(m, 0);
    } else {
        if (ImGui::Selectable(ui_text("Keyboard"), m->s.player_src[p] == 1)) {
            launcher_model_set_source(m, p, 1, 0, nullptr, nullptr);
            if (psx) launcher_binds_refresh(m);
        }
    }

    // Unified pad list (no duplicates): saved mappings + live devices.
    // Pads already selected on another player are disabled (keyboard is not).
    struct PadOpt {
        char guid[40];
        char name[64];
        uint32_t id;
        bool live;
    };
    constexpr int kMaxOpts = LNG_MAX_PADS + 16 + LNG_MAX_PLAYERS;
    PadOpt opts[kMaxOpts]{};
    int nopt = 0;
    auto already = [&](const char* guid) -> bool {
        if (!guid || !guid[0]) return true;
        for (int i = 0; i < nopt; ++i)
            if (std::strcmp(opts[i].guid, guid) == 0) return true;
        return false;
    };
    auto push_opt = [&](const char* guid, const char* name, uint32_t id,
                        bool live) {
        if (!guid || !guid[0] || already(guid) || nopt >= kMaxOpts) return;
        std::snprintf(opts[nopt].guid, sizeof(opts[nopt].guid), "%s", guid);
        const char* nm = (name && name[0] && std::strcmp(name, "Gamepad") != 0)
                             ? name : "Controller";
        std::snprintf(opts[nopt].name, sizeof(opts[nopt].name), "%s", nm);
        opts[nopt].id = id;
        opts[nopt].live = live;
        ++nopt;
    };

    if (psx) {
        const int known = launcher_binds_psx_known_count();
        for (int i = 0; i < known; ++i) {
            char guid[40] = {}, name[64] = {};
            if (!launcher_binds_psx_known_at(i, guid, (int)sizeof(guid),
                                             name, (int)sizeof(name)))
                continue;
            uint32_t id = 0;
            bool live = false;
            const bool custom = launcher_binds_psx_name_is_custom(guid) != 0;
            for (int j = 0; j < g_pad_count; ++j) {
                if (g_pads[j].guid[0] &&
                    std::strcmp(g_pads[j].guid, guid) == 0) {
                    live = true;
                    id = g_pads[j].id;
                    // Custom rename wins; otherwise prefer the live driver name.
                    if (!custom && g_pads[j].name[0] &&
                        std::strcmp(g_pads[j].name, "Gamepad") != 0)
                        std::snprintf(name, sizeof(name), "%s", g_pads[j].name);
                    break;
                }
            }
            push_opt(guid, name, id, live);
        }
        // Settings-restored GUID that isn't in the registry yet.
        if (m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0]) {
            bool live = false;
            uint32_t id = m->player_pad_id[p];
            for (int j = 0; j < g_pad_count; ++j) {
                if (g_pads[j].guid[0] &&
                    std::strcmp(g_pads[j].guid,
                                m->s.player_gamepad_guid[p]) == 0) {
                    live = true;
                    id = g_pads[j].id;
                    break;
                }
            }
            push_opt(m->s.player_gamepad_guid[p], m->player_pad_name[p], id,
                     live);
        }
    }

    for (int i = 0; i < g_pad_count; ++i)
        push_opt(g_pads[i].guid, g_pads[i].name, g_pads[i].id, true);

    const int claim_n = launcher_model_visible_player_count(m);
    for (int i = 0; i < nopt; ++i) {
        bool claimed = false;
        for (int o = 0; o < claim_n; ++o) {
            if (o == p) continue;
            if (m->s.player_src[o] == 2 && m->s.player_gamepad_guid[o][0] &&
                std::strcmp(m->s.player_gamepad_guid[o], opts[i].guid) == 0) {
                claimed = true;
                break;
            }
        }
        char label[96];
        if (opts[i].live)
            std::snprintf(label, sizeof(label), "%s", opts[i].name);
        else
            std::snprintf(label, sizeof(label), "%s %s",
                          opts[i].name, ui_text("(disconnected)"));
        const bool sel = m->s.player_src[p] == 2 &&
                         m->s.player_gamepad_guid[p][0] &&
                         std::strcmp(m->s.player_gamepad_guid[p],
                                     opts[i].guid) == 0;
        if (claimed) ImGui::BeginDisabled();
        if (ImGui::Selectable(label, sel) && !claimed) {
            launcher_model_set_source(m, p, 2, opts[i].id, opts[i].name,
                                     opts[i].guid);
            if (psx) {
                launcher_binds_apply_psx_pad_profile(m, p);
                launcher_binds_refresh(m);
            } else if (n64_prof) {
                /* Per-GUID store: the labels ARE this controller's mapping, so
                 * selecting a different pad shows a different page. Nothing to
                 * copy into a live file the way SNES has to. */
                launcher_binds_refresh(m);
            } else if (snes_prof) {
                /* Selecting a controller restores the profile saved for it, so
                 * swapping pads swaps layouts instead of leaving whichever one
                 * was configured last in [GamepadMap]. Does nothing when that
                 * GUID has no saved profile. */
                launcher_binds_apply_snes_pad_profile(m, p + 1);
                launcher_binds_refresh(m);
            }
        }
        if (claimed) ImGui::EndDisabled();
    }

    if (nopt == 0) {
        ImGui::BeginDisabled();
        ImGui::Selectable(ui_text("(no gamepad connected)"));
        ImGui::EndDisabled();
    }
}

/* Best-effort local lobby seat (0-based) for the NETPLAY pad card label.
 * Prefers fill_launch.local_slot, else display_name match, else host→0.
 * Label only — runtime netplay always samples dashboard card 0. */
static int np_guess_local_seat(const LauncherModel* m) {
    if (!m || !m->netplay_supported) return 0;
    const auto* np = m->netplay;
    if (!np || !np->ctx) return 0;
    if (np->fill_launch) {
        RecompLauncherCNetplayLaunch launch{};
        if (np->fill_launch(np->ctx, &launch) && launch.enabled &&
            launch.local_slot >= 0)
            return launch.local_slot;
    }
    const char* me = m->s.netplay_player_name;
    if ((!me || !me[0]) && np->player_name)
        me = np->player_name(np->ctx);
    if (np->member_count && np->member_get && me && me[0]) {
        const int n = np->member_count(np->ctx);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayMember mem{};
            if (!np->member_get(np->ctx, i, &mem)) continue;
            if (mem.display_name[0] && std::strcmp(mem.display_name, me) == 0)
                return mem.slot >= 0 ? mem.slot : i;
        }
    }
    if (np->is_host && np->is_host(np->ctx)) return 0;
    return 0;
}

void draw_player_panel(LauncherModel* m, const LauncherTheme& th, int p, float w) {
    char id[24];  snprintf(id, sizeof(id), "player%d", p);
    char eb[40];
    if (p == 0 && m->netplay_supported) {
        const int seat = np_guess_local_seat(m);
        snprintf(eb, sizeof(eb), "%s %d / %s", ui_text("PLAYER"), seat + 1,
                 ui_text("NETPLAY"));
    } else {
        snprintf(eb, sizeof(eb), "%s %d", ui_text("PLAYER"), p + 1);
    }

    if (!begin_panel(id, w, false)) { end_panel(); return; }
    ImGui::PushID(p);
    eyebrow(eb);

    const float inner = ImGui::GetContentRegionAvail().x;
    const float cw    = inner;   // controls span the card => flush by construction

    // pad art centered in the card: PSX-style games swap analog/digital art
    // with the mode; consoles without a mode-swap art PAIR (SNES, and Genesis —
    // which has pad modes but a SINGLE pad image) always show the generic
    // g_pad. The swap only happens when the profile actually ships the pair.
    {
        const SystemProfile* aprof = (const SystemProfile*)m->profile;
        const bool has_swap_art = aprof && aprof->controller.image_analog != nullptr;
        // The art follows the SELECTED mode. The Analog / D-Pad pair right
        // below this image is a two-state control with no other feedback, so
        // the pad picture is the answer to "which did I just pick?" — leaving
        // it on the DualShock while D-Pad is lit reads as a broken control.
        //
        // This deliberately replaces the earlier rule (art keyed only to a
        // game LOCKED to D-Pad, on the reasoning that mode picks a PROTOCOL
        // and the player is still physically holding a DualShock). Owner
        // decision: the selector's feedback value wins.
        //
        // s.pad_mode already carries the locked value for a non-selectable
        // title (launcher_model_init), so a locked D-Pad game keeps exactly
        // the art it showed before. Mode 2 is D-Pad; 1 Analog, 0 Hybrid — both
        // of those are stick-bearing pads and keep the analog image. A console
        // with its own mode vocabulary (Genesis 3/6-Button) never reaches here:
        // it ships a single pad image, so has_swap_art is false.
        //
        // Keyboard is digital at runtime whatever the stored mode says (the
        // Analog segment is greyed out for it), so it shows the digital pad
        // rather than promising sticks the player does not have.
        const bool kb_digital =
            m->s.player_src[p] == 1 &&
            !(aprof && aprof->controller.modes && aprof->controller.mode_count > 0);
        const bool digital =
            has_swap_art && (kb_digital || m->s.pad_mode[p] == 2);
        const LauncherTexture& art = has_swap_art
            ? (digital ? g_pad_digital : g_pad_analog) : g_pad;
        // Center on the FITTED width so a near-square pad (N64) or a portrait
        // handheld (GB/GBC) sits centered, not left-shifted by the landscape
        // box's spare width.
        image_fit_centered(art, 120, 78, inner);
    }
    ImGui::Dummy(ImVec2(0, px(6)));

    // Pad-mode selector: only when the game supports pad modes AND the mode
    // is user-selectable (not locked to a single mode).
    if (m->pad_mode_supported && m->pad_mode_selectable) {
        pad_mode_selector(m, th, p, cw);
        ImGui::Dummy(ImVec2(0, px(6)));
    }

    ImGui::SetNextItemWidth(cw);
    if (ImGui::BeginCombo("##src", ui_text(launcher_model_player_src_label(m, p)))) {
        draw_source_selectables(m, p);
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    // Configure + connection status share ONE half/half row (Configure left,
    // status right) so the card stays short — that keeps the memory cards below
    // it from being pushed off the bottom. (Analog-stick deadzone lives on the
    // Configure page's per-player Deadzone stepper, not on this card.)
    {
        const float gap  = px(th.spacing_sm);
        const float half = (cw - gap) * 0.5f;
        const float btnh = px(32);
        if (ImGui::Button(ui_text("Configure"), ImVec2(half, btnh))) launcher_model_open_config(m, p);
        ImGui::SameLine(0, gap);
        const bool on = m->s.player_src[p] != 0;
        const char* st = ui_text(on ? "connected" : "not assigned");
        const float sw = px(10) + px(8) + ImGui::CalcTextSize(st).x;
        // center the dot+label within the right half, vertically on the button
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (half - sw) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnh - ImGui::GetTextLineHeight()) * 0.5f);
        draw_dot(on, th.good, th.text_muted);
        ImGui::TextColored(on ? col(th.good) : col(th.text_muted), "%s", st);
    }
    ImGui::PopID();
    end_panel();
}

// Lays out player cards: stretch to fill the row until there is room for
// another card at the standard width, then bump the column count (memcards
// stay fixed-width via dash_card_width).
void draw_controllers_row(LauncherModel* m, const LauncherTheme& th) {
    if (m->lock_device) return;   // fixed pad: hide the player controller cards entirely
    int n = launcher_model_visible_player_count(m);
    if (n < 1) n = 1;
    if (n > LNG_MAX_PLAYERS) n = LNG_MAX_PLAYERS;
    const float gap = px(th.spacing_md);
    const float availw = ImGui::GetContentRegionAvail().x;
    const float pref = px(300.0f);
    int cols = (int)((availw + gap) / (pref + gap));
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;
    float cardw = (availw - gap * (float)(cols - 1)) / (float)cols;
    if (cardw < 1.0f) cardw = availw;
    for (int p = 0; p < n; ++p) {
        if (p % cols) ImGui::SameLine(0, gap);
        else if (p) ImGui::Dummy(ImVec2(0, gap));   // new row of cards
        char cid[16];
        std::snprintf(cid, sizeof(cid), "pc%d", p);
        begin_container(cid, ImVec2(cardw, 0), ImGuiChildFlags_AutoResizeY);
        draw_player_panel(m, th, p, cardw);
        end_container();
    }
}

void panel_controller_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_controllers_row(m, *th);
}

// The dashboard COMPOSES whichever panels this game's SystemProfile lists in
// panels_dashboard — it does not hardcode a fixed set. GAME is always
// present; the side column stacks CONTROLLERS plus any optional modules
// (SAVES only when the game has SRAM, folded into the GAME card — see
// draw_save_row). A different system's profile simply contributes a
// different panel list.
void draw_dashboard(LauncherModel* m, const LauncherTheme& th, int logical_w) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const LauncherPanel* game_p = find_composed(prof->panels_dashboard, "game", m);
    const LauncherPanel* ctrl_p = find_composed(prof->panels_dashboard, "controller", m);
    const LauncherPanel* save_p = find_composed(prof->panels_dashboard, "save", m);
    const LauncherPanel* tpak_p = find_composed(prof->panels_dashboard, "tpak", m);
    // Online identity: stacks directly under the controller card in the
    // right column (double opt-in — see avail_identity).
    const LauncherPanel* ident_p =
        find_composed(prof->panels_dashboard, "identity", m);

    if (logical_w >= 820) {
        const float gap = px(th.spacing_md);
        // When a WIDE panel (save/tpak) follows this row, the columns must hug
        // their own content (AutoResizeY) instead of stretching to fill the
        // whole scrollable "body" — otherwise there's never any room left
        // below them and the WIDE panel silently draws off the bottom edge.
        // SNES's composition never lists "save" here (save_p == nullptr), so
        // it keeps the original fill-to-height columns byte-identical.
        const bool has_save = (save_p != nullptr);
        const bool has_tpak = (tpak_p != nullptr);
        if (has_save) {
            // Capture body height before the row so multitap can grow the right
            // column to the footer.
            const float row_h = ImGui::GetContentRegionAvail().y;
            // 2P: AutoResizeY the right column so hug-height memcards (with
            // even Browse/New pad) are never clipped by a boxart-height cap.
            // Multitap (3+): fill to footer and scroll controllers when they
            // would crush the save band.
            const bool many_players = launcher_model_visible_player_count(m) > 2;
            if (game_p) {
                g_game_fill_h = false;
                begin_container("dash_l", ImVec2(px(400), 0), ImGuiChildFlags_AutoResizeY);
                game_p->draw(m, &th);
                end_container();
            }
            if (game_p && ctrl_p) ImGui::SameLine(0, gap);
            if (ctrl_p) {
                if (many_players) {
                    const float right_h = row_h > px(80.0f) ? row_h : px(80.0f);
                    begin_container("dash_r", ImVec2(0, right_h),
                                    ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
                    // Tall enough for a clean fill-height memcard pair (icon,
                    // grid, Browse/New with padding) while controllers scroll
                    // in the space above.
                    const float save_reserve = save_p ? px(300.0f) : 0.0f;
                    float ctrl_h = ImGui::GetContentRegionAvail().y
                                   - save_reserve - (save_p ? gap : 0.0f);
                    if (ctrl_h < px(100.0f)) ctrl_h = px(100.0f);
                    begin_container("dash_ctrl", ImVec2(0, ctrl_h));
                        ctrl_p->draw(m, &th);
                    end_container();
                    if (ident_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        ident_p->draw(m, &th);
                    }
                    if (save_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        g_save_fill_h = true;
                        save_p->draw(m, &th);
                        g_save_fill_h = false;
                    }
                    end_container();
                } else {
                    begin_container("dash_r", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
                    ctrl_p->draw(m, &th);
                    if (ident_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        ident_p->draw(m, &th);
                    }
                    if (save_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        save_p->draw(m, &th);
                    }
                    end_container();
                }
            }
        } else if (has_tpak) {
            // N64: no SRAM save panel, but a FULL-WIDTH Transfer Pak row follows
            // below. Both columns must hug their own content (AutoResizeY) so
            // there's room left underneath for that row — otherwise the tpak row
            // draws off the bottom edge.
            if (game_p) {
                g_game_fill_h = false;
                begin_container("dash_l", ImVec2(px(400), 0), ImGuiChildFlags_AutoResizeY);
                game_p->draw(m, &th);
                end_container();
            }
            if (game_p && ctrl_p) ImGui::SameLine(0, gap);
            if (ctrl_p) {
                begin_container("dash_r", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
                    ctrl_p->draw(m, &th);
                    if (ident_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        ident_p->draw(m, &th);
                    }
                end_container();
            }
        } else {
            // No WIDE save panel (SNES): original fill-to-height columns,
            // byte-identical.
            if (game_p) {
                g_game_fill_h = true;
                begin_container("dash_l", ImVec2(px(400), 0), ImGuiChildFlags_None);
                game_p->draw(m, &th);
                end_container();
            }
            if (game_p && ctrl_p) ImGui::SameLine(0, gap);
            if (ctrl_p) {
                begin_container("dash_r", ImVec2(0, 0), ImGuiChildFlags_None);
                    ctrl_p->draw(m, &th);
                    if (ident_p) {
                        ImGui::Dummy(ImVec2(0, gap));
                        ident_p->draw(m, &th);
                    }
                end_container();
            }
        }
        // Transfer Pak: a genuinely FULL-WIDTH row under both columns — four
        // per-port cards need the whole window, not the side column.
        if (tpak_p) {
            ImGui::Dummy(ImVec2(0, gap));
            tpak_p->draw(m, &th);
        }
    } else {
        if (game_p) { g_game_fill_h = false; game_p->draw(m, &th); }
        if (game_p && ctrl_p) ImGui::Spacing();
        if (ctrl_p) ctrl_p->draw(m, &th);
        // Narrow single-column layout: identity and memcards stack under
        // the controller.
        if (ident_p) { ImGui::Spacing(); ident_p->draw(m, &th); }
        if (save_p) { ImGui::Spacing(); save_p->draw(m, &th); }
        if (tpak_p) { ImGui::Spacing(); tpak_p->draw(m, &th); }
    }
}

// Trim a path from the LEFT to fit max_w, prefixing "…" so the meaningful tail
// stays visible (e.g. "…\build\bin-x64-Release\msu"). Same idea as the old hash
// ellipsis.
static const char* elide_left(const char* s, float max_w, char* out, size_t cap) {
    if (ImGui::CalcTextSize(s).x <= max_w) { snprintf(out, cap, "%s", s); return out; }
    size_t n = strlen(s);
    for (size_t start = 1; start < n; ++start) {
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "\xE2\x80\xA6%s", s + start);   // "…" + tail
        if (ImGui::CalcTextSize(tmp).x <= max_w) { snprintf(out, cap, "%s", tmp); return out; }
    }
    snprintf(out, cap, "\xE2\x80\xA6");
    return out;
}

static bool has_display_aspect_row(const LauncherModel* m) {
    return m && ((m->aspect_labels && m->num_aspect_labels > 0) ||
                 m->aspect_mask != 0);
}

static void draw_aspect_row(LauncherModel* m, const LauncherTheme& th) {
    if (!has_display_aspect_row(m)) return;
    /* px(180) rather than the shared column, for the Screen layout reason:
     * both the label and the value come from the HOST, not from a vocabulary
     * this file owns, and a button does not elide.
     *
     * The EXPERIMENTAL tag sits AFTER the button on the same line, so it has
     * to be inside the width reserved here -- right-anchoring aligns the right
     * edge of what it is told about, and a tag left out of the sum hangs off
     * the card. */
    float ctrl_w = px(180);
    if (m->aspect_experimental)
        ctrl_w += px(8) + ImGui::CalcTextSize("EXPERIMENTAL").x;
    row_label_right(m->aspect_setting_label && m->aspect_setting_label[0]
                        ? m->aspect_setting_label
                        : "Aspect ratio",
                    th, ctrl_w);
    ImGui::PushID("aspect_ratio");
    if (ImGui::Button(launcher_model_aspect_label(m), ImVec2(px(180), px(30))))
        launcher_model_cycle_aspect(m);
    if (m->aspect_experimental) {
        ImGui::SameLine(0, px(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(th.warn), "EXPERIMENTAL");
    }
    if (m->aspect_setting_help && m->aspect_setting_help[0] &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", m->aspect_setting_help);
    ImGui::PopID();
}

// True when this game exposes ANY of the deeper PSX-style DISPLAY controls.
// SNES (and any console leaving every has_* flag 0) takes the legacy-only
// branch below and gets the fixed-band DISPLAY card. Fullscreen is NOT part
// of this predicate: it is a universal row drawn on BOTH branches (the ABI's
// has_fullscreen_toggle no longer gates anything — see recomp_launcher.h).
bool any_deep_display(const LauncherModel* m) {
    return m->has_window_size || launcher_model_renderer_offered(m) ||
           m->has_supersampling ||
           m->has_antialiasing || m->has_texture_filter || m->has_screen_kind ||
           m->has_fmv_filter ||
           m->has_frame_interp || m->has_skip_fmv ||
           m->has_geometry_precision ||
           m->has_rewind_depth;
           /* has_turbo_loads is intentionally absent: it draws no row (below).
            * has_vsync no longer forces the deep surface: the legacy branch
            * draws its own VSync row (as an On/Off checkbox), so a console
            * adding only vsync keeps its fixed-band card. Every console that
            * wants the tri-state cycle (PSX) is already deep via has_renderer
            * and friends. */
}

// Whether the DISPLAY card should grow to fit its content (AutoResizeY) rather
// than sit at the legacy fixed band height. True for the deep PSX surface, and
// also for a console that renders the extra widescreen-cells row (Genesis) once
// widescreen is on — that row is a 4th line the 3-row fixed height would clip.
// Gated exactly like the row itself so the SNES legacy surface stays pinned to
// the fixed height (byte-identical to before this console existed).
bool video_card_grows(const LauncherModel* m) {
    if (any_deep_display(m)) return true;
    if (has_display_aspect_row(m)) return true;
    if (m->has_shader) return true;
    if (m->has_sharp_filter || m->has_affine_filter) return true;
    if (m->has_frame_blend || m->has_vsync) return true;
    if (m->has_run_ahead) return true;
    if (m->num_display_layouts > 0) return true;
    // NES legacy-surface additions (Integer scaling row, HD texture pack block)
    // add extra rows the fixed no_scroll band wasn't sized for.
    if (m->has_integer_scale || m->hdpack_supported) return true;
    return false;
}

void draw_shader_row(LauncherModel* m, const LauncherTheme& th, float col_w = 0.0f) {
    if (!m || !m->has_shader) return;
    row_label("Shader", th, col_w);

    const float browse_w = px(78);
    const float folder_w = px(72);
    const float clear_w = px(64);
    const float gap = px(th.spacing_sm);
    float combo_w = ImGui::GetContentRegionAvail().x - browse_w - folder_w - clear_w - gap * 3.0f;
    if (combo_w < px(120)) combo_w = px(120);

    refresh_shader_presets();
    std::string current_label = m->s.shader_path[0] ? ui_text("Custom") : ui_text("None");
    for (const ShaderPresetEntry& preset : g_shader_presets) {
        if (preset.path == m->s.shader_path) {
            current_label = preset.label;
            break;
        }
    }

    ImGui::SetNextItemWidth(combo_w);
    if (ImGui::BeginCombo("##shader_preset", current_label.c_str())) {
        refresh_shader_presets();
        if (ImGui::Selectable(ui_text("None"), !m->s.shader_path[0]))
            launcher_model_clear_shader_path(m);
        for (const ShaderPresetEntry& preset : g_shader_presets) {
            bool selected = preset.path == m->s.shader_path;
            if (ImGui::Selectable(preset.label.c_str(), selected))
                launcher_model_set_shader_path(m, preset.path.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", preset.path.c_str());
        }
        if (m->s.shader_path[0] && current_label == ui_text("Custom"))
            ImGui::Selectable(ui_text("Custom"), true, ImGuiSelectableFlags_Disabled);
        ImGui::EndCombo();
    }
    if (m->s.shader_path[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", m->s.shader_path);

    ImGui::SameLine(0, gap);
    static const char* kShaderPatterns[] = { "*.glsl", "*.glslp" };
    if (ImGui::Button(ui_text("Browse"), ImVec2(browse_w, px(30)))) {
        ui_pick_file(m, "Select GLSL shader", {"*.glsl", "*.glslp"},
                     "GLSL shader (.glsl .glslp)", [m](const char* path) {
                         if (path) launcher_model_set_shader_path(m, path);
                     });
    }
    ImGui::SameLine(0, gap);
    if (ImGui::Button(ui_text("Folder"), ImVec2(folder_w, px(30)))) {
        std::filesystem::path shader_dir = std::filesystem::path("assets") / "shaders";
        std::error_code ec;
        std::filesystem::create_directories(shader_dir, ec);
        if (ImGui::GetPlatformIO().Platform_OpenInShellFn)
            ImGui::GetPlatformIO().Platform_OpenInShellFn(ImGui::GetCurrentContext(),
                                                          shader_dir.string().c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Open assets/shaders to add presets.");
    ImGui::SameLine(0, gap);
    ImGui::BeginDisabled(!m->s.shader_path[0]);
    if (ImGui::Button(ui_text("Clear"), ImVec2(clear_w, px(30))))
        launcher_model_clear_shader_path(m);
    ImGui::EndDisabled();
}

void draw_display_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("DISPLAY");

    if (!any_deep_display(m)) {
        // ---- legacy minimal surface (SNES/NES etc.) ---------------------------
        // Labels at the left edge, controls at the right (row_label_right), so
        // no measuring pass over the labels is needed and the card reads as a
        // grid flush on both sides.
        const float cb = ImGui::GetFrameHeight();   // a checkbox is square
        row_window_scale(m, th);
        // Universal fullscreen row (every console; Off/Borderless/Exclusive,
        // the legacy launcher's vocabulary). Sits right under Window scale,
        // matching the old Display panel order.
        row_fullscreen(m, th);
        if (m->num_display_layouts > 0) {
            row_label_right("Screen layout", th, px(180));
            ImGui::PushID("screen_layout");
            if (ImGui::Button(ui_text(launcher_model_display_layout_label(m)),
                              ImVec2(px(180), px(30))))
                launcher_model_cycle_display_layout(m);
            ImGui::PopID();
        }
        draw_aspect_row(m, th);
        if (m->has_integer_scale) {   // NES module: snap the image to integer multiples
            row_label_right("Integer scaling", th, cb);
            bool is = m->s.integer_scale != 0;
            if (ImGui::Checkbox("##intscale", &is)) launcher_model_toggle_integer_scale(m);
        }
        if (m->has_sharp_filter) {
            row_label_right("Scaling filter", th, px(180));
            if (ImGui::Button(ui_text(launcher_model_scaling_filter_label(m)),
                              ImVec2(px(180), px(30))))
                launcher_model_cycle_scaling_filter(m);
        } else {
            row_label_right("Linear filtering", th, cb);
            bool filter = m->s.linear_filter != 0;
            if (ImGui::Checkbox("##filter", &filter))
                launcher_model_toggle_filter(m);
        }
        if (m->has_affine_filter) {
            row_label_right("Affine background smoothing", th, cb);
            bool affine = m->s.affine_filter != 0;
            if (ImGui::Checkbox("##affine_filter", &affine))
                launcher_model_toggle_affine_filter(m);
        }
        if (m->has_frame_blend) {
            row_label_right("Frame blending", th, cb);
            bool fb = m->s.frame_blend != 0;
            if (ImGui::Checkbox("##frame_blend", &fb))
                launcher_model_toggle_frame_blend(m);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip(
                    "Average each frame with the previous one.\n"
                    "Steadies alternate-frame flicker transparency\n"
                    "(thrusters, explosions) as a CRT would; costs a\n"
                    "little motion ghosting.");
        }
        if (m->has_run_ahead) row_run_ahead(m, th);
        // On/Off checkbox rather than the deep surface's tri-state dropdown:
        // legacy-surface hosts map this onto a boolean renderer flag, so
        // offering "Adaptive" here would promise what they cannot deliver.
        if (m->has_vsync) {
            row_label_right("VSync", th, cb);
            bool vs = m->s.vsync != RECOMP_LAUNCHER_VSYNC_OFF;
            if (ImGui::Checkbox("##vsync", &vs))
                launcher_model_toggle_vsync(m);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip(
                    "On: the swap waits for the panel — no tearing.\n"
                    "Off: swap immediately — lowest display latency, may "
                    "tear.\n\n"
                    "The runtime still paces frames to the console's own "
                    "rate either way, so Off does not run the game fast.");
        }
        // The shader row is a compound control (combo + Browse/Folder/Clear)
        // that already fills to the card's right edge, so it flows from its
        // label rather than reserving a fixed control width.
        draw_shader_row(m, th);
        // HD texture packs (NES module, Mesen hires.txt format): one line —
        //   [x] HD texture pack   …folder tail   [Browse]
        // Mirrors the MSU-1 row in Audio (same enable + folder pattern).
        if (m->hdpack_supported) {
            bool on = m->s.hdpack_enabled != 0;
            if (ImGui::Checkbox(ui_text("HD texture pack"), &on))
                launcher_model_toggle_hdpack(m);
            const float bw = px(78);
            ImGui::SameLine(0, px(14));
            float avail = ImGui::GetContentRegionAvail().x - bw - px(th.spacing_sm);
            if (avail < px(50)) avail = px(50);
            const char* dir = m->s.hdpack_dir[0] ? m->s.hdpack_dir : "(not set)";
            char elided[192]; elide_left(dir, avail, elided, sizeof(elided));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(th.text_muted), "%s", elided);
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
            if (ImGui::Button(ui_text("Browse"), ImVec2(bw, px(30)))) {
                ui_pick_folder(m, "Select HD pack folder (contains hires.txt)",
                               [m](const char* path) {
                                   if (path) launcher_model_set_hdpack_dir(m, path);
                               });
            }
        }
        return;
    }

    // ---- deeper PSX-style surface, capability-gated per control -----------
    // Order matches the original PSX launcher: Window size, Renderer,
    // Supersampling, Aspect ratio, Texture filtering, Antialiasing, Screen
    // model, Frame interpolation (+Presentation target), Skip FMVs, Turbo
    // loads, Fullscreen.
    // Labels left, controls right (row_label_right), same as the legacy card.
    const float cb = ImGui::GetFrameHeight();   // a checkbox is square
    if (m->has_window_size) {
        row_label_right("Window size", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_window_size_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_window_size(m);
    } else {
        row_window_scale(m, th);
    }

    // NES module rows can appear on this branch too (has_renderer puts NES
    // on the deep surface): integer scaling right under the window row,
    // mirroring the legacy branch's ordering.
    if (m->has_integer_scale) {
        row_label_right("Integer scaling", th, cb);
        bool is = m->s.integer_scale != 0;
        if (ImGui::Checkbox("##intscale", &is)) launcher_model_toggle_integer_scale(m);
    }

    /* renderer_offered, not has_renderer: a host that declares its renderers
     * may declare NONE, and an empty dropdown is worse than no row. */
    if (launcher_model_renderer_offered(m)) {
        /* A list, not a cycle button. The vocabulary is up to five entries on
         * hosts that supply their own (Auto / D3D11 / D3D9 / OpenGL /
         * Software), and reaching the last one by clicking through the other
         * four is not a choice a player should have to count out. */
        row_label_right("Renderer", th, px(SETTINGS_CTRL_W));
        ImGui::SetNextItemWidth(px(SETTINGS_CTRL_W));
        if (ImGui::BeginCombo("##renderer",
                              ui_text(launcher_model_renderer_label(m)))) {
            const int n = launcher_model_renderer_count(m);
            for (int i = 0; i < n; ++i) {
                const char* lbl = launcher_model_renderer_label_at(m, i);
                if (!lbl || !lbl[0]) continue;
                if (ImGui::Selectable(ui_text(lbl), m->s.renderer == i))
                    launcher_model_set_renderer(m, i);
            }
            ImGui::EndCombo();
        }
        /* The control column is a FIXED width shared by every row in the card,
         * and an honest renderer label does not fit in it -- "OpenGL
         * (experimental ...)" closes to "OpenGL (exper". The popup sizes
         * itself to the longest entry and reads fine, but the CLOSED combo is
         * what a player looks at most of the time, so the full label is a
         * hover away rather than lost. */
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", ui_text(launcher_model_renderer_label(m)));
        /* The host's own words about what this choice costs and when it takes
         * effect. recomp-ui does not know either -- it cannot: "OpenGL" means
         * a different thing on every console here, and a backend's maturity
         * changes without this file changing. So the sentence is the host's,
         * drawn verbatim. */
        const char* note = launcher_model_renderer_note(m);
        if (note && *note)
            ImGui::TextColored(col(th.text_muted), "%s", ui_text(note));
    }

    if (m->has_supersampling) {
        row_label_right("Supersampling", th, px(SETTINGS_CTRL_W));
        ImGui::PushID("supersampling");
        if (ImGui::Button(ui_text(launcher_model_supersampling_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_supersampling(m);
        ImGui::PopID();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Internal-resolution SSAA.\n"
                "Offline: full software/GL supersampling.\n"
                "Netplay (OpenGL): present quality via GPU FBO;\n"
                "CPU VRAM authority stays at 1x for snaps/digests.");
        }
    }

    // Universal fullscreen row (every console — no longer gated on the
    // vestigial has_fullscreen_toggle). A tri-state dropdown, so Exclusive is
    // both reachable and visible without pressing through the other two.
    row_fullscreen(m, th);
    if (m->num_display_layouts > 0) {
        /* Wider than the shared column: these labels come from the HOST, not
         * from a vocabulary this file owns, and a button (unlike a combo) does
         * not elide -- so it keeps the room the widest stock layout name
         * needs. Right-anchored like every other row regardless. */
        row_label_right("Screen layout", th, px(180));
        ImGui::PushID("screen_layout");
        if (ImGui::Button(ui_text(launcher_model_display_layout_label(m)),
                          ImVec2(px(180), px(30))))
            launcher_model_cycle_display_layout(m);
        ImGui::PopID();
    }
    draw_aspect_row(m, th);

    if (m->has_sharp_filter) {
        row_label_right("Scaling filter", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_scaling_filter_label(m)),
                          ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_scaling_filter(m);
    } else if (m->has_texture_filter) {
        row_label_right("Texture filtering", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_texture_filter_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_toggle_texture_filter(m);
    } else {
        row_label_right("Linear filtering", th, cb);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
    }

    if (m->has_frame_blend) {
        row_label_right("Frame blending", th, cb);
        bool fb = m->s.frame_blend != 0;
        if (ImGui::Checkbox("##frame_blend", &fb))
            launcher_model_toggle_frame_blend(m);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(
                "Average each frame with the previous one.\n"
                "Steadies alternate-frame flicker transparency\n"
                "(thrusters, explosions) as a CRT would; costs a\n"
                "little motion ghosting.");
    }

    if (m->has_antialiasing) {
        row_label_right("Antialiasing", th, px(SETTINGS_CTRL_W));
        ImGui::PushID("antialiasing");
        if (ImGui::Button(ui_text(launcher_model_aa_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_aa(m);
        ImGui::PopID();
    }

    /* FMV reconstruction. Distinct from Texture filtering above: that samples
     * the rasterizer's textures, this scales a decoded video frame up to the
     * window, and the good answer differs between the two. Antialiasing off
     * means nearest everywhere, so the row has nothing to say then. */
    if (m->has_fmv_filter) {
        row_label_right("FMV filtering", th, px(SETTINGS_CTRL_W));
        ImGui::PushID("fmv_filter");
        const bool aa_off = m->has_antialiasing && m->s.antialiasing == 0;
        if (aa_off) ImGui::BeginDisabled();
        if (ImGui::Button(ui_text(aa_off ? "Nearest"
                                         : launcher_model_fmv_filter_label(m)),
                          ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_fmv_filter(m);
        if (aa_off) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Antialiasing is off, so video is presented "
                                  "with hard pixels.");
        }
        ImGui::PopID();
    }

    if (m->has_affine_filter) {
        row_label_right("Affine background smoothing", th, cb);
        bool affine = m->s.affine_filter != 0;
        if (ImGui::Checkbox("##affine_filter", &affine))
            launcher_model_toggle_affine_filter(m);
    }

    draw_shader_row(m, th);

    if (m->has_geometry_precision) {
        // Geometry correction deliberately draws NO row. It moves vertices, and
        // the runtime can only recover the sub-pixel original for ~5% of them
        // (93% of lookups are ambiguous), so a corrected triangle meets an
        // uncorrected neighbour and their shared edge splits open. Measured on
        // Ape Escape; see psxrecomp ENHANCEMENTS.md G1.8/G1.9. The setting stays
        // in the ABI and remains readable from game.toml/settings.toml so the
        // work is still testable, exactly as has_turbo_loads/Settings.turbo_loads
        // stayed after their row was dropped — it simply has no control.
        //
        // Perspective textures are unaffected by that problem: they only change
        // UV interpolation inside a polygon whose provenance is already proven,
        // so no vertex moves and adjacent polygons cannot disagree about an edge.
        row_label_right("Perspective textures", th, cb);
        bool persp = m->s.perspective_texturing != 0;
        if (ImGui::Checkbox("##persptex", &persp))
            launcher_model_toggle_perspective_texturing(m);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Interpolates textures with perspective, which "
                              "stops large floors and walls from warping as the "
                              "camera moves.\n\nApplied only to polygons the "
                              "runtime can prove came from the 3D pipeline, so "
                              "2D art and menus are left alone.");
    }

    if (m->has_screen_kind) {
        row_label_right("Screen model", th, px(220));
        // Wide enough for the longest label ("Super Game Boy (No Border)");
        // shorter models (e.g. "DMG") center within the same fixed box.
        if (ImGui::Button(ui_text(launcher_model_screen_kind_label(m)), ImVec2(px(220), px(30))))
            launcher_model_cycle_screen_kind(m);
    }

    // Frame interpolation is only meaningful under OpenGL (Software has no
    // interpolation pass); Presentation target only matters once frame
    // interpolation is actually on.
    if (m->has_frame_interp && m->s.renderer) {
        row_label_right("Frame interpolation", th, cb);
        bool fi = m->s.frame_interp != 0;
        if (ImGui::Checkbox("##fi", &fi)) launcher_model_toggle_frame_interp(m);
        if (m->s.frame_interp) {
            row_label_right("Presentation target", th, px(SETTINGS_CTRL_W));
            if (ImGui::Button(ui_text(launcher_model_interp_fps_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
                launcher_model_cycle_interp_fps(m);
        }
    }

    // VSync sits with frame interpolation because both decide how a finished
    // frame reaches the panel, not how it is drawn.
    if (m->has_vsync) {
        row_label_right("VSync", th, px(SETTINGS_CTRL_W));
        ImGui::SetNextItemWidth(px(SETTINGS_CTRL_W));
        if (ImGui::BeginCombo("##vsync_mode",
                              ui_text(launcher_model_vsync_label(m)))) {
            static const struct { int v; const char* label; } kVsync[] = {
                { RECOMP_LAUNCHER_VSYNC_OFF,      "Off" },
                { RECOMP_LAUNCHER_VSYNC_ON,       "On" },
                { RECOMP_LAUNCHER_VSYNC_ADAPTIVE, "Adaptive" },
            };
            for (const auto& o : kVsync)
                if (ImGui::Selectable(ui_text(o.label), m->s.vsync == o.v))
                    launcher_model_set_vsync(m, o.v);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "On: the swap waits for the panel — no tearing.\n"
                "Off: swap immediately — lowest display latency, may tear.\n"
                "Adaptive: vsync while the game keeps up, immediate when it "
                "drops below the refresh rate.\n\n"
                "The runtime still paces frames to the console's own rate "
                "either way, so Off does not run the game fast.");
        }
    }

    if (m->has_skip_fmv) {
        row_label_right("Skip FMVs", th, cb);
        bool sk = m->s.auto_skip_fmv != 0;
        if (ImGui::Checkbox("##skipfmv", &sk)) launcher_model_toggle_skip_fmv(m);
    }

    if (m->has_rewind_depth) {
        row_label_right("Rewind", th, cb);
        bool rewind_on = m->s.rewind_enabled != 0;
        if (ImGui::Checkbox("##rewind_enabled", &rewind_on))
            launcher_model_toggle_rewind_enabled(m);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Rewind the last few seconds of play (F8, or Select+R3).\n"
                "Off by default: it keeps whole-machine snapshots in memory,\n"
                "which costs hundreds of MB and a capture every few frames.\n"
                "Turning it off frees that immediately.");
        }
        /* The two tuning rows only mean anything once it is on. */
        ImGui::BeginDisabled(!rewind_on);
        row_label_right("Rewind buffer", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_rewind_depth_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_rewind_depth(m);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "How many local rewind snapshots to keep (50 / 100 / 150 / 200).\n"
                "Each one is a few MB of machine state.\n"
                "Takes effect on the next launch.");
        }
        row_label_right("Rewind interval", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_rewind_interval_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_rewind_interval(m);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Frames between rewind snapshots (1 / 4 / 8 / 12 / 15).\n"
                "FMV still densifies toward 4 when this is sparser.\n"
                "Takes effect on the next launch.");
        }
        ImGui::EndDisabled();
    }

    /* Run-ahead sits with Rewind rather than with the filters: both spend
     * machine snapshots to move the player in time, and neither changes how
     * a frame is drawn. */
    if (m->has_run_ahead) row_run_ahead(m, th);

    /* Turbo loads is deliberately NOT a Display row on any console. Load
     * acceleration is owned by the framework's Mods catalog (Fast Loading /
     * CD Speed), which exposes the multiplier, the instant scheduler and the
     * distinction between host pacing and drive speed. A single opaque
     * checkbox here duplicated that at lower fidelity and let the two
     * surfaces disagree. The has_turbo_loads capability and
     * Settings.turbo_loads remain in the ABI for hosts that still persist the
     * value; they simply no longer draw a control. */

    // HD texture packs — toggle plus the ACTIVE pack's name, read-only.
    //
    // Unlike the legacy NES row above, there is no folder picker here: which
    // pack is active is a per-title decision owned by the host's pack manager
    // (Retro Launcher's Texture Packs modal), which can list installed packs and show
    // per-pack coverage. A second, blinder picker in this panel could only
    // fight it — so this panel does the one thing it is better placed to do,
    // which is flip the pack off and on without leaving the game.
    if (m->hdpack_supported) {
        bool on = m->s.hdpack_enabled != 0;
        if (ImGui::Checkbox(ui_text("HD textures"), &on))
            launcher_model_toggle_hdpack(m);
        ImGui::SameLine(0, px(14));

        // Show the pack's folder NAME, not its path: the path is a launcher
        // data-dir location the player never typed and cannot act on.
        const char* dir = m->s.hdpack_dir;
        const char* name = dir;
        for (const char* c = dir; *c; ++c)
            if ((*c == '/' || *c == '\\') && c[1]) name = c + 1;
        const float avail = ImGui::GetContentRegionAvail().x;
        char elided[192];
        elide_left(name[0] ? name : "(none selected)", avail, elided, sizeof(elided));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(th.text_muted), "%s", elided);
        if (ImGui::IsItemHovered() && dir[0]) ImGui::SetTooltip("%s", dir);
    }
}

// Video/Display module (docs/ARCHITECTURE.md): base window-scale/fullscreen
// row set, specialized per system — SNES adds linear-filter + widescreen,
// PSX adds the full deep surface (window size/renderer/supersampling/aspect/
// texture-filter/AA/screen-model/frame-interp/skip-fmv/turbo/fullscreen).
// draw_display_controls() gates each row on the model's has_* caps (sourced
// from the ABI, unchanged); this adapter supplies the card chrome, choosing
// AutoResizeY (deep surface, more rows than the fixed band fits) vs a fixed
// row_h band with no_scroll (legacy minimal surface) — exactly the sizing
// draw_settings used to pick inline, now co-located with its own content.
void panel_video_draw(LauncherModel* m, const LauncherTheme* th) {
    // video_card_grows() folds in NES's legacy-surface additions (Integer
    // scaling row, HD texture pack block) alongside the deep/widescreen surfaces.
    if (video_card_grows(m) || !g_settings_two_col) {
        if (begin_panel("disp", 0, false)) draw_display_controls(m, *th);
        end_panel();
    } else {
        if (begin_panel("disp", 0, true, /*no_scroll*/true)) draw_display_controls(m, *th);
        end_panel();
    }
}

void draw_audio_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("AUDIO");
    // Labels at the card's left edge, controls at its right (row_label_right),
    // matching DISPLAY. Every control is SETTINGS_CTRL_W wide -- including the
    // volume stepper, which stretches its value field to reach it -- so the
    // column is flush on both sides.
    const float cb = ImGui::GetFrameHeight();   // a checkbox is square
    // Sample rate: hidden for consoles whose runtime has no audio-frequency
    // setting (SystemProfile.hide_audio_freq — NES has Volume only).
    const SystemProfile* audio_prof = (const SystemProfile*)m->profile;
    if (!audio_prof || !audio_prof->hide_audio_freq) {
        row_label_right("Sample rate", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_freq_label(m)),
                          ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_freq(m);
    }
    row_label_right("Volume", th, px(SETTINGS_CTRL_W));
    int dv = 0; stepper("vol", m->s.volume, "%", &dv, px(SETTINGS_CTRL_W));
    if (dv) launcher_model_volume_delta(m, dv);

    // Output-device pick (host-enumerated names; N64/RT64 hosts) — "(system
    // default)" first, committing "" so an unplugged device degrades sanely.
    // A device name is far longer than a setting value, so this row keeps the
    // full-width combo on its own rather than squeezing into the column.
    if (m->num_audio_devices > 0 && m->audio_device_labels) {
        row_label("Output device", th);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##audiodev", ui_text(launcher_model_audio_device_label(m)))) {
            if (ImGui::Selectable(ui_text("(system default)"), m->s.audio_device[0] == '\0'))
                launcher_model_set_audio_device(m, NULL);
            for (int i = 0; i < m->num_audio_devices; ++i) {
                const char* name = m->audio_device_labels[i];
                if (!name || !name[0]) continue;
                bool sel = strcmp(m->s.audio_device, name) == 0;
                if (ImGui::Selectable(name, sel))
                    launcher_model_set_audio_device(m, name);
            }
            ImGui::EndCombo();
        }
    }

    if (m->has_spu_hq) {
        row_label_right("High-quality SPU", th, cb);
        bool hq = m->s.spu_hq != 0;
        if (ImGui::Checkbox("##spuhq", &hq)) launcher_model_toggle_spu_hq(m);
    }

    // NOTE: analog deadzone is NOT here — it belongs to the input device, so it
    // lives on each controller card (draw_player_panel), gated on
    // has_deadzone_pct. Kept out of Audio deliberately.

    // MSU-1: no header/subsection — just one line under the rows above:
    //   [x] Enable MSU-1 music (?)   …folder tail     [Browse]
    if (m->msu1_supported) {
        bool on = m->s.msu1_enabled != 0;
        if (ImGui::Checkbox(ui_text("Enable MSU-1 music"), &on))
            launcher_model_toggle_msu1(m);
        if (m->msu1_note && m->msu1_note[0]) {
            ImGui::SameLine(0, px(6));
            ImGui::TextColored(col(th.accent), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(m->msu1_note);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        if (m->num_msu1_packs > 0) {
            const char* selected = "Custom...";
            for (int i = 0; i < m->num_msu1_packs; ++i)
                if (!std::strcmp(m->s.msu1_pack, m->msu1_packs[i].id)) selected = m->msu1_packs[i].name;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##msu_soundtrack", selected)) {
                for (int i = 0; i < m->num_msu1_packs; ++i) {
                    const auto& pack = m->msu1_packs[i];
                    if (ImGui::Selectable(pack.name, !std::strcmp(m->s.msu1_pack, pack.id)))
                        launcher_model_set_msu1_pack(m, pack.id);
                }
                if (ImGui::Selectable("Custom...", !m->s.msu1_pack[0])) {
                    launcher_model_set_msu1_pack(m, "");
                    ui_pick_file(m, "Select MSU-1 music file", {"*.msu"}, "MSU-1 music", [m](const char* path) {
                        if (path) launcher_model_set_msu1_dir(m, path);
                    });
                }
                ImGui::EndCombo();
            }
        }
        if (!m->num_msu1_packs || !m->s.msu1_pack[0]) {
            const float bw = px(78);
            if (!m->num_msu1_packs) ImGui::SameLine(0, px(14));
            float avail = ImGui::GetContentRegionAvail().x - bw - px(th.spacing_sm);
            if (avail < px(50)) avail = px(50);
            const char* dir = m->s.msu1_dir[0] ? m->s.msu1_dir : "(not set)";
            char elided[192]; elide_left(dir, avail, elided, sizeof(elided));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(th.text_muted), "%s", elided);
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
            if (ImGui::Button(ui_text("Browse"), ImVec2(bw, px(30)))) {  // px(30) matches the other settings buttons + the row's frame height (px(28) sat the label high)
                auto selected_path = [m](const char* path) {
                    if (path) launcher_model_set_msu1_dir(m, path);
                };
                if (m->num_msu1_packs > 0)
                    ui_pick_file(m, "Select MSU-1 music file", {"*.msu"}, "MSU-1 music", selected_path);
                else
                    ui_pick_folder(m, "Select MSU-1 music folder", selected_path);
            }
        }
    }

    // Localization: only games that declare a language list get this
    // mini-section (mirrors the real PSX launcher's Language cycle).
    if (m->num_languages > 0) {
        ImGui::Dummy(ImVec2(0, px(6)));
        eyebrow("LOCALIZATION");
        row_label_right("Language", th, px(SETTINGS_CTRL_W));
        if (ImGui::Button(ui_text(launcher_model_language_label(m)), ImVec2(px(SETTINGS_CTRL_W), px(30))))
            launcher_model_cycle_language(m);
    }
}

// Audio module adapter — same AutoResizeY-vs-fixed-row_h sizing pattern as
// panel_video_draw, decided from the same "deep" predicate draw_settings used
// to compute inline.
void panel_audio_draw(LauncherModel* m, const LauncherTheme* th) {
    const bool deep_audio = m->num_msu1_packs > 0 || m->has_spu_hq || m->num_languages > 0 || m->num_audio_devices > 0;   /* deadzone moved to controller card */
    if (deep_audio || !g_settings_two_col) {
        if (begin_panel("audio", 0, false)) draw_audio_controls(m, *th);
        end_panel();
    } else {
        if (begin_panel("audio", 0, true, /*no_scroll*/true)) draw_audio_controls(m, *th);
        end_panel();
    }
}

// INPUT module: multitap / pad-bus options — half-width card stacked under
// AUDIO and above SYSTEM in the right column (see draw_settings). Composed
// for PSX; shown when the title can use multitap seats or the analog hack.
int avail_input(const LauncherModel* m) {
    return launcher_model_multitap_available(m) ||
           launcher_model_multitap_analog_available(m) ||
           launcher_model_virtual_stylus_available(m);
}
void draw_input_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("INPUT");
    bool previous_row = false;
    if (launcher_model_multitap_available(m)) {
        bool on = launcher_model_multitap_enabled(m) != 0;
        if (ImGui::Checkbox("Multitap", &on) &&
            (on ? 1 : 0) != launcher_model_multitap_enabled(m))
            launcher_model_toggle_multitap(m);
        previous_row = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(px(320));
            ImGui::TextUnformatted(
                "Enable SCPH-1070 multitap for 3+ player seats. "
                "Off limits Play Local to two native controller ports. "
                "Netplay lobbies with 3 or more players always use multitap.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (launcher_model_multitap_analog_available(m)) {
        if (previous_row)
            ImGui::Dummy(ImVec2(0, px(th.spacing_sm)));
        bool on = launcher_model_multitap_analog_enabled(m) != 0;
        if (ImGui::Checkbox("Multitap analog (hack)", &on) &&
            (on ? 1 : 0) != launcher_model_multitap_analog_enabled(m))
            launcher_model_toggle_multitap_analog(m);
        previous_row = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(px(320));
            ImGui::TextUnformatted(
                "Allow DualShock sticks on multitap tap seats (not faithful — "
                "many titles expect digital taps). Saved to game.toml / "
                "settings. Netplay hosts can enforce this for the lobby.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (launcher_model_virtual_stylus_available(m)) {
        if (previous_row)
            ImGui::Dummy(ImVec2(0, px(th.spacing_sm)));
        bool on = launcher_model_virtual_stylus_enabled(m) != 0;
        if (ImGui::Checkbox("Virtual Stylus", &on) &&
            (on ? 1 : 0) != launcher_model_virtual_stylus_enabled(m))
            launcher_model_toggle_virtual_stylus(m);
    }
}
void panel_input_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("input", 0)) draw_input_controls(m, *th);
    end_panel();
}

// SYSTEM module: BIOS path picker — a half-width card stacked under AUDIO in
// the right column (see draw_settings), composed only for systems whose
// profile lists "system" (PSX, GBA) AND only shown for a game instance that
// needs one (has_bios) — composition + availability, both layers, matching
// the architecture.
int avail_system(const LauncherModel* m) { return m->has_bios; }
void draw_system_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("SYSTEM");
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const bool is_gba = prof && prof->id && std::strcmp(prof->id, "gba") == 0;
    const bool is_psx = prof && prof->id && std::strcmp(prof->id, "psx") == 0;
    // Empty means "use the BIOS this build ships with" — not "unset". Runtimes
    // that bundle a redistributable BIOS (PSX/OpenBIOS, GBA) boot straight from
    // it, so the row states that outcome instead of the old "(default)", which
    // read as a missing setting the player still had to deal with.
    const bool has_pick = m->s.bios_path[0] != 0;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(14), px(8)));
    const float btn_h = px(34);
    const float browse_w = px(96);

    // Line 1: BIOS label + path + Browse
    row_label("BIOS", th);
    float avail = ImGui::GetContentRegionAvail().x - browse_w - px(th.spacing_sm);
    if (avail < px(50)) avail = px(50);
    const char* bp = has_pick ? m->s.bios_path
                              : (is_gba ? ui_text("Retail GBA BIOS required")
                                        : "OpenBIOS");
    char elided[192]; elide_left(bp, avail, elided, sizeof(elided));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(has_pick ? th.text : th.text_muted), "%s", elided);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                    browse_w);
    if (ImGui::Button(ui_text("Browse"), ImVec2(browse_w, btn_h))) {
        request_bios_picker(m,
                            is_gba ? "Select Game Boy Advance BIOS (gba_bios.bin)"
                                   : "Select BIOS file",
                            false);
    }

    // Line 2: secondary action under the path (PSX OpenBIOS / GBA Clear).
    if (is_psx || has_pick) {
        const float indent = ImGui::CalcTextSize("BIOS").x + px(th.spacing_md);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        if (is_psx) {
            if (!has_pick) ImGui::BeginDisabled();
            if (ImGui::Button(ui_text("Use OpenBIOS"), ImVec2(0, btn_h)))
                launcher_model_request_bios_path(m, "");
            if (!has_pick) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(has_pick
                                      ? "Switch to bundled OpenBIOS (no rebuild)."
                                      : "OpenBIOS is already selected.");
        } else {
            if (ImGui::Button(ui_text("Clear"), ImVec2(0, btn_h)))
                launcher_model_request_bios_path(m, "");
            if (ImGui::IsItemHovered()) {
                if (is_gba)
                    ImGui::SetTooltip("Remove this selection. A retail GBA BIOS "
                                      "is required before the game can launch.");
                else
                    ImGui::SetTooltip("Stop using this BIOS and go back to the one "
                                      "included with this build.");
            }
        }
    }

    /* MotK codegen host: local PGO train under SYSTEM (no separate VIDEO card). */
    if (is_psx && m->pgo_optimize_with_progress_cb) {
        ImGui::Dummy(ImVec2(0, px(14)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                               ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(col(th.text_muted),
                           "Train this PC on the intro FMV, then rebuild with "
                           "profile-guided optimization. Safe for rollback "
                           "netplay — each peer may optimize independently.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, px(8)));
        const bool can_pgo = m->rom_present && m->rom_full[0] &&
                             strcmp(m->rom_size, "--") != 0 &&
                             !m->setup_preparing;
        if (!can_pgo) ImGui::BeginDisabled();
        if (ImGui::Button(ui_text("Optimize FMV Playback"), ImVec2(0, btn_h)))
            launcher_model_request_pgo_optimize(m);
        if (!can_pgo) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(m->setup_preparing
                                      ? "A build job is already running."
                                      : "Select a disc image first.");
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Instrument → play intro (~3 min) → rebuild with PGO. "
                "Uses existing generated C (no setup wizard).");
        }
    }
    ImGui::PopStyleVar();
}
void panel_system_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("system", 0)) draw_system_controls(m, *th);
    end_panel();
}

// SOLAR SENSOR module: a few GBA cartridges carry a photodiode the game reads
// as gameplay input (Boktai's Gun del Sol charges from real sunlight), so the
// player has to be able to say WHERE that brightness is measured. Composed only
// for a system whose profile lists "solar" AND a game that has the hardware.
//
// The postal code is edited behind an Edit/Save step, the same shape as the
// password-save row, so a half-typed code never reaches the host mid-keystroke
// and the row still shows the working value while editing is abandoned.
int avail_solar(const LauncherModel* m) { return m->has_solar_sensor; }

void draw_solar_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("SOLAR SENSOR");

    // Light source first: it decides whether the rest of the card is live.
    row_label("Light source", th);
    {
        const bool manual = m->s.solar_source != 0;
        const float bw = px(96);
        ImGui::SameLine(ImGui::GetCursorPosX() +
                        ImGui::GetContentRegionAvail().x - bw * 2 -
                        px(th.spacing_sm));
        if (ImGui::Button(manual ? "Live" : "Live ✓", ImVec2(bw, px(28))))
            launcher_model_set_solar_source(m, 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Read the current sunlight where you are.");
        ImGui::SameLine(0.0f, px(th.spacing_sm));
        if (ImGui::Button(manual ? "Fixed ✓" : "Fixed", ImVec2(bw, px(28))))
            launcher_model_set_solar_source(m, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hold a chosen level. Makes no network request.");
    }

    if (m->s.solar_source != 0) {
        // Fixed level: the location rows below would be inert, so offer the
        // level instead of greying out three rows the player cannot use.
        ImGui::Dummy(ImVec2(0, px(4)));
        row_label("Level", th);
        int step = m->s.solar_manual_step;
        const float sw = px(200);
        ImGui::SameLine(ImGui::GetCursorPosX() +
                        ImGui::GetContentRegionAvail().x - sw);
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderInt("##solarlevel", &step, 0, 8, "%d / 8"))
            launcher_model_set_solar_manual_step(m, step);
        return;
    }

    ImGui::Dummy(ImVec2(0, px(4)));
    row_label("Postal code", th);
    {
        static bool s_zip_editing = false;
        static char s_zip_buf[16];
        const float bw = px(78);
        if (!s_zip_editing) {
            ImGui::AlignTextToFramePadding();
            const bool set = m->s.solar_zip[0] != 0;
            ImGui::TextColored(col(set ? th.text : th.text_muted), "%s",
                               set ? m->s.solar_zip : "(not set — sensor stays dark)");
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            ImGui::GetContentRegionAvail().x - bw);
            if (ImGui::Button(ui_text("Edit"), ImVec2(bw, px(28)))) {
                snprintf(s_zip_buf, sizeof(s_zip_buf), "%s", m->s.solar_zip);
                s_zip_editing = true;
            }
        } else {
            float avail = ImGui::GetContentRegionAvail().x - bw * 2 -
                          px(th.spacing_sm) * 2;
            if (avail < px(80)) avail = px(80);
            ImGui::SetNextItemWidth(avail);
            const bool submitted =
                ImGui::InputText("##solarzip", s_zip_buf, sizeof(s_zip_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0, px(th.spacing_sm));
            if (submitted || ImGui::Button(ui_text("Save"), ImVec2(bw, px(28)))) {
                launcher_model_set_solar_zip(m, s_zip_buf);
                s_zip_editing = false;
            }
            ImGui::SameLine(0, px(th.spacing_sm));
            if (ImGui::Button(ui_text("Cancel"), ImVec2(bw, px(28))))
                s_zip_editing = false;
        }
    }

    ImGui::Dummy(ImVec2(0, px(4)));
    row_label("Country", th);
    {
        static bool s_cc_editing = false;
        static char s_cc_buf[8];
        const float bw = px(78);
        if (!s_cc_editing) {
            ImGui::AlignTextToFramePadding();
            const bool set = m->s.solar_country[0] != 0;
            ImGui::TextColored(col(set ? th.text : th.text_muted), "%s",
                               set ? m->s.solar_country : "us");
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            ImGui::GetContentRegionAvail().x - bw);
            if (ImGui::Button(ui_text("Edit##cc"), ImVec2(bw, px(28)))) {
                snprintf(s_cc_buf, sizeof(s_cc_buf), "%s", m->s.solar_country);
                s_cc_editing = true;
            }
        } else {
            float avail = ImGui::GetContentRegionAvail().x - bw * 2 -
                          px(th.spacing_sm) * 2;
            if (avail < px(60)) avail = px(60);
            ImGui::SetNextItemWidth(avail);
            const bool submitted =
                ImGui::InputText("##solarcc", s_cc_buf, sizeof(s_cc_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0, px(th.spacing_sm));
            if (submitted || ImGui::Button(ui_text("Save##cc"), ImVec2(bw, px(28)))) {
                launcher_model_set_solar_country(m, s_cc_buf);
                s_cc_editing = false;
            }
            ImGui::SameLine(0, px(th.spacing_sm));
            if (ImGui::Button(ui_text("Cancel##cc"), ImVec2(bw, px(28))))
                s_cc_editing = false;
        }
    }

    ImGui::Dummy(ImVec2(0, px(4)));
    row_label("Full sun", th);
    {
        // Clear-sky midday is ~900 W/m^2 at mid latitudes but far less in
        // winter or at high latitude, where leaving this at 900 would mean the
        // gauge could never fill on a genuinely sunny day.
        int wm2 = m->s.solar_full_sun > 0 ? m->s.solar_full_sun : 900;
        const float sw = px(200);
        ImGui::SameLine(ImGui::GetCursorPosX() +
                        ImGui::GetContentRegionAvail().x - sw);
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderInt("##solarfullsun", &wm2, 300, 1200, "%d W/m²"))
            launcher_model_set_solar_full_sun(m, wm2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Irradiance that reads as a full gauge. Lower it "
                              "in winter or at high latitude.");
    }
}

void panel_solar_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("solar", 0)) draw_solar_controls(m, *th);
    end_panel();
}

// Hotkeys module: the universal emulator-hotkeys catalog, opt-in per system
// via SystemProfile.hotkeys_mask. SNES opts into LNG_HOTKEYS_ALL — the full
// catalog, grid byte-identical to the original hardcoded panel. PSX opts into
// a narrower everyday-transport subset (launcher_system.h), so its grid packs
// only the set bits — no holes, columns re-wrap around the smaller count.
void draw_hotkeys_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("HOTKEYS");
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    uint32_t mask = prof ? prof->hotkeys_mask : LNG_HOTKEYS_ALL;
    // Solar controls are a per-cartridge capability, not part of the GBA-wide
    // catalog. They remain absent for every existing game and have no default
    // binding when the capability is enabled.
    if (m->has_solar_sensor) mask |= LNG_HOTKEYS_SOLAR;
    // Same responsive grid treatment as the bindings list.
    const float cell_w = px(280.0f);
    int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
    cols = cols < 1 ? 1 : (cols > 3 ? 3 : cols);
    // Uniform label column: measure the widest bound hotkey name up front so
    // every bind button in the grid starts at the same x within its cell —
    // aligned left edges (and, since the buttons are fixed-width, aligned right
    // edges too), instead of hugging each variable-width label.
    float label_w = 0.0f;
    for (int h = 0; h < LNG_HK_COUNT; ++h) {
        if (!(mask & (1u << h))) continue;
        float w = ImGui::CalcTextSize(ui_text(launcher_hotkey_name((LngHotkey)h))).x;
        if (w > label_w) label_w = w;
    }
    label_w += px(16.0f);   // gap between label and its bind button
    if (ImGui::BeginTable("hk", cols)) {
        for (int h = 0; h < LNG_HK_COUNT; ++h) {
            if (!(mask & (1u << h))) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(h);
            const char* hkname = ui_text(launcher_hotkey_name((LngHotkey)h));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(th.text_muted), "%s", hkname);
            // Pad with RELATIVE spacing (not absolute x) so the bind button
            // starts at a uniform offset within every table cell — absolute
            // SameLine() fights ImGui's per-cell cursor tracking and spills
            // buttons into the wrong column.
            ImGui::SameLine(0.0f, label_w - ImGui::CalcTextSize(hkname).x);
            const bool cap = m->hk_capturing && m->capture_hk == (LngHotkey)h;
            const char* lbl = cap ? ui_text("[ press... ]")
                            : m->hotkeys[h][0] ? m->hotkeys[h] : ui_text("(unbound)");
            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
            if (ImGui::Button(lbl, ImVec2(px(130), 0)))
                launcher_model_begin_hk_capture(m, (LngHotkey)h);
            if (cap) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void panel_hotkeys_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("hotkeys", 0)) draw_hotkeys_controls(m, *th);
    end_panel();
}

// The settings VIEW composes whichever panels this game's SystemProfile
// lists in panels_settings, in order: DISPLAY (MAIN) + AUDIO (SIDE) share the
// top band; further SIDE cards (INPUT, SYSTEM, SOLAR, …) then stack under
// AUDIO in the same right column in composition order; WIDE panels (HOTKEYS)
// still stack full-width below that — driven by the composition array + the
// registry's available() gate.
void draw_settings(LauncherModel* m, const LauncherTheme& th) {
    // Row 1: DISPLAY | AUDIO share the top band. For the legacy minimal
    // surface (no deep caps set — e.g. SNES) both cards are pinned to the
    // SAME fixed height, exactly as before, so that screenshot is unchanged.
    // A PSX-style game with the deeper capability set has far more rows than
    // that fixed height fits — rather than clip (or reintroduce a stray
    // scrollbar via no_scroll on an overflowing fixed-height card), those
    // cards switch to AutoResizeY so they simply grow to fit their content.
    // (Same "deep" predicates panel_video_draw/panel_audio_draw use for their
    // OWN inner card — computed twice, independently, so outer/inner sizing
    // never has to be threaded through the generic draw(model,theme) signature.)
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const float gap  = px(th.spacing_md);
    const float row_h = px(240.0f);   // legacy fixed band height (4 rows: the
                                      // universal Fullscreen row joined scale/
                                      // filter/widescreen on the legacy surface)

    /*
     * Two columns, until half the window is too narrow to hold a settings row.
     *
     * A row is a label plus a control anchored to the card's right edge, so
     * the card has a real floor: below it the label and the control collide
     * (row_label_right's minimum gap wins and the control overhangs), which
     * is worse than reading the same rows one column at a time. Past that
     * floor AUDIO drops to a full-width card UNDER DISPLAY rather than being
     * squeezed beside it, and every SIDE card follows it down the same
     * single column.
     *
     * The floor is measured, not guessed: the longest label the shared cards
     * draw, plus the control column, plus the card's own padding.
     */
    const float full_w = ImGui::GetContentRegionAvail().x;
    const float col_floor = ImGui::CalcTextSize("Affine background smoothing").x +
                            px(SETTINGS_CTRL_W) + px(th.spacing_md) * 3.0f;
    const bool two_col = (full_w - gap) * 0.5f >= col_floor;
    const float half = two_col ? (full_w - gap) * 0.5f : full_w;
    g_settings_two_col = two_col;   // read by panel_video_draw/panel_audio_draw

    const bool deep_display = video_card_grows(m);   // superset of any_deep_display: folds in NES + widescreen (N64 covered too)
    const bool deep_audio   = m->num_msu1_packs > 0 || m->has_spu_hq || m->num_languages > 0 || m->num_audio_devices > 0;   /* deadzone moved to controller card */

    const LauncherPanel* video_p   = find_composed(prof->panels_settings, "video", m);
    const LauncherPanel* audio_p   = find_composed(prof->panels_settings, "audio", m);
    const LauncherPanel* hotkeys_p = find_composed(prof->panels_settings, "hotkeys", m);
    const bool psx_settings = prof && prof->id && std::strcmp(prof->id, "psx") == 0;

    // Left content edge in SCREEN space — where DISPLAY starts and where any
    // full-width content (HOTKEYS) below both columns must resume.
    const float content_left_x = ImGui::GetCursorScreenPos().x;

    /* The legacy fixed band exists ONLY so the two side-by-side cards share a
     * height. Stacked, there is nothing to line up with, and pinning the band
     * would leave a tall empty region under a two-row AUDIO card — so in one
     * column both cards hug their content instead. */
    const bool pin_band = two_col;
    float left_bottom = 0.0f;   // DISPLAY's bottom edge (screen space)
    if (video_p) {
        if (deep_display || !pin_band) begin_container("set_l", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        else               begin_container("set_l", ImVec2(half, row_h));
        video_p->draw(m, &th);
        end_container();
        left_bottom = ImGui::GetItemRectMax().y;
    }
    if (video_p && audio_p) {
        if (two_col) ImGui::SameLine(0, gap);
        // One column: put AUDIO under DISPLAY with the same gap the columns
        // would have had, instead of leaving it to item spacing.
        else ImGui::SetCursorScreenPos(ImVec2(content_left_x, left_bottom + gap));
    }
    // Capture the column's SCREEN x BEFORE opening AUDIO's child, so additional
    // SIDE cards (INPUT/SYSTEM/…) can reopen at the same x once AUDIO's child
    // ends (a finished child returns the cursor to the LEFT edge of the row on
    // the next line, not to its own column). In one-column mode this is simply
    // the content's left edge, so the stacking code below needs no second case.
    const float right_x = ImGui::GetCursorScreenPos().x;
    float audio_bottom = 0.0f;   // AUDIO's bottom edge (screen space)
    if (audio_p) {
        if (deep_audio || !pin_band) begin_container("set_r", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        else             begin_container("set_r", ImVec2(half, row_h));
        audio_p->draw(m, &th);
        end_container();
        audio_bottom = ImGui::GetItemRectMax().y;
    }

    // Remaining SIDE-slot cards (INPUT, SYSTEM, SOLAR, …) stack under AUDIO
    // in panels_settings order. Falls back to full width only if a profile
    // composes them without "audio".
    float stack_bottom = audio_bottom;
    bool stacked_side = false;
    bool stacked_hotkeys = false;
    if (prof->panels_settings) {
        for (int i = 0; prof->panels_settings[i]; ++i) {
            const char* id = prof->panels_settings[i];
            if (!id || !id[0]) continue;
            if (strcmp(id, "video") == 0 || strcmp(id, "audio") == 0 ||
                (!psx_settings && strcmp(id, "hotkeys") == 0))
                continue;
            const LauncherPanel* side_p =
                find_composed(prof->panels_settings, id, m);
            const bool force_side_hotkeys =
                psx_settings && strcmp(id, "hotkeys") == 0;
            if (!side_p ||
                (side_p->slot != LNG_SLOT_SIDE && !force_side_hotkeys))
                continue;
            if (audio_p) {
                ImGui::SetCursorScreenPos(
                    ImVec2(right_x, stack_bottom + gap));
                char cname[48];
                snprintf(cname, sizeof(cname), "set_r_%s", id);
                begin_container(cname, ImVec2(half, 0),
                                ImGuiChildFlags_AutoResizeY);
                side_p->draw(m, &th);
                end_container();
                stack_bottom = ImGui::GetItemRectMax().y;
                stacked_side = true;
                if (force_side_hotkeys)
                    stacked_hotkeys = true;
            } else {
                side_p->draw(m, &th);
                stack_bottom = ImGui::GetItemRectMax().y;
                stacked_side = true;
                if (force_side_hotkeys)
                    stacked_hotkeys = true;
            }
        }
    }
    if (stacked_side && audio_p) {
        // Manual SetCursorScreenPos pulled SIDE cards out of row flow — resume
        // below the taller column so HOTKEYS never overlaps DISPLAY.
        const float below_y =
            (left_bottom > stack_bottom) ? left_bottom : stack_bottom;
        ImGui::SetCursorScreenPos(ImVec2(content_left_x, below_y + gap));
    }
    if (hotkeys_p && !stacked_hotkeys) hotkeys_p->draw(m, &th);
}

static bool enabled_camera_controls(const LauncherModel* m) {
    const auto* mods = m ? m->mods : nullptr;
    if (!mods || !mods->feature_count || !mods->feature_get) return false;
    const int count = mods->feature_count(mods->ctx);
    for (int index = 0; index < count; ++index) {
        RecompLauncherCModFeature feature{};
        if (mods->feature_get(mods->ctx, index, &feature) &&
            feature.enabled && feature.camera_controls)
            return true;
    }
    return false;
}

const char* settings_key_label(int scancode) {
    if (scancode <= 0) return "(unbound)";
    const char* name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return (name && name[0]) ? name : "(unbound)";
}

static const char* settings_pad_button_label(int code) {
    const char* name = SDL_GetGamepadStringForButton((LNG_GamepadButton)code);
    if (!name || !name[0]) return "button";
    if (!std::strcmp(name, "back")) return "select";
    if (!std::strcmp(name, "leftstick")) return "l3";
    if (!std::strcmp(name, "rightstick")) return "r3";
    if (!std::strcmp(name, "leftshoulder")) return "l1";
    if (!std::strcmp(name, "rightshoulder")) return "r1";
    return name;
}

static bool settings_pad_button_is_select(int code) {
    return std::strcmp(settings_pad_button_label(code), "select") == 0;
}

static int button_mask_count(uint32_t mask) {
    int n = 0;
    while (mask) {
        n += (int)(mask & 1u);
        mask >>= 1;
    }
    return n;
}

void settings_pad_label(int binding, char* out, size_t capacity) {
    if (!out || !capacity) return;
    const char* name = nullptr;
    char text[96] = {};
    if (RECOMP_LAUNCHER_PAD_IS_BUTTON(binding)) {
        int code = RECOMP_LAUNCHER_PAD_BUTTON_CODE(binding);
        snprintf(text, sizeof text, "%s", settings_pad_button_label(code));
    } else if (RECOMP_LAUNCHER_PAD_IS_AXIS(binding)) {
        int code = RECOMP_LAUNCHER_PAD_AXIS_CODE(binding);
        name = SDL_GetGamepadStringForAxis((LNG_GamepadAxis)code);
        snprintf(text, sizeof text, "%s%c",
                 (name && name[0]) ? name : "axis",
                 RECOMP_LAUNCHER_PAD_AXIS_POSITIVE(binding) ? '+' : '-');
    } else if (RECOMP_LAUNCHER_PAD_IS_BUTTON_COMBO(binding)) {
        uint32_t mask = (uint32_t)RECOMP_LAUNCHER_PAD_BUTTON_COMBO_MASK(binding);
        for (int code = 0; code < 32; ++code) {
            if ((mask & ((uint32_t)1u << code)) == 0)
                continue;
            if (text[0])
                strncat(text, " + ", sizeof(text) - strlen(text) - 1);
            strncat(text, settings_pad_button_label(code),
                    sizeof(text) - strlen(text) - 1);
        }
        if (!text[0])
            snprintf(text, sizeof text, "(unbound)");
    } else {
        snprintf(text, sizeof text, "(unbound)");
    }
    snprintf(out, capacity, "%s", text);
}

void draw_assist_binding_editor(LauncherModel* m, const LauncherTheme& th,
                                const char* table_id, int action_limit,
                                bool show_reset) {
    /*
     * A host that only wants a few NAMED extra actions should not have to
     * take over the per-player button chips to get them.  `settings_bindings`
     * does both: it also swaps those chips onto the host-owned
     * player_key_bind/player_pad_bind arrays, which is a much larger promise
     * than "give me one more row".  Naming actions is enough on its own.
     *
     * Nothing else changes for existing games: a host that names no actions
     * has assist_binding_count == 0 and still renders nothing here.
     */
    if (m->assist_binding_count <= 0 || !m->assist_binding_labels)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
    ImGui::TextUnformatted(m->has_assist_tools ? "ASSIST CONTROLS" : "HOST SHORTCUTS");
    ImGui::PopStyleColor();
    ImGui::TextColored(
        col(th.text_muted),
        m->has_assist_tools
            ? "Global controls; they only operate while Assist Tools is enabled."
            : "Press a controller button or chord.");
    if (ImGui::BeginTable(table_id, 3, ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,
                                px(150));
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed,
                                px(180));
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed,
                                px(180));
        ImGui::TableHeadersRow();
        int count = m->assist_binding_count;
        if (action_limit > 0 && count > action_limit) count = action_limit;
        for (int action = 0; action < count; ++action) {
            ImGui::PushID(action);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(m->assist_binding_labels[action]);
            ImGui::TableSetColumnIndex(1);
            bool capture_key = m->capturing && m->capture_assist &&
                               !m->capture_pad && m->capture_btn == action;
            /* The two cells of a row are both Buttons whose ID is their
             * label, and a row with nothing bound in either column showed
             * "(unbound)" twice -- the same ID, and Dear ImGui's conflict
             * warning over the Fast-forward toggle row. Scope each column. */
            ImGui::PushID("key");
            if (ImGui::Button(
                    capture_key ? "[ press a key... ]" :
                        settings_key_label(m->s.assist_key_bind[action]),
                    ImVec2(px(170), 0)))
                launcher_model_begin_assist_capture(m, action, false);
            ImGui::PopID();
            ImGui::TableSetColumnIndex(2);
            bool capture_pad = m->capturing && m->capture_assist &&
                               m->capture_pad && m->capture_btn == action;
            char pad[48];
            settings_pad_label(m->s.assist_pad_bind[action], pad, sizeof pad);
            ImGui::PushID("pad");
            if (ImGui::Button(
                    capture_pad ? "[ press a button... ]" : pad,
                    ImVec2(px(170), 0)))
                launcher_model_begin_assist_capture(m, action, true);
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (show_reset && ImGui::Button(m->has_assist_tools
                                        ? "Reset Assist Controls"
                                        : "Reset Host Shortcuts"))
        launcher_model_reset_assist_bindings(m);
    if (m->capturing && m->capture_assist)
        ImGui::TextColored(col(th.warn), "Listening... (Esc cancels, Backspace unbinds)");
}

void draw_controller_assist_shortcuts(LauncherModel* m,
                                      const LauncherTheme& th) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
    ImGui::TextUnformatted(m->has_assist_tools ? "ASSIST SHORTCUTS" : "HOST SHORTCUTS");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(col(th.text_muted),
                       m->has_assist_tools
                           ? "(global; requires Assist Tools)"
                           : "(keyboard and controller)");
    if (ImGui::BeginTable("controller_assist_binds", 3,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch,
                                1.1f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableHeadersRow();
        for (int action = 0; action < m->assist_binding_count; ++action) {
            ImGui::PushID(action);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(m->assist_binding_labels[action]);
            ImGui::TableSetColumnIndex(1);
            bool capture_key = m->capturing && m->capture_assist &&
                               !m->capture_pad && m->capture_btn == action;
            /* Per-column IDs: see draw_assist_binding_editor. */
            ImGui::PushID("key");
            if (ImGui::Button(
                    capture_key ? "[ key... ]" :
                        settings_key_label(m->s.assist_key_bind[action]),
                    ImVec2(-FLT_MIN, 0)))
                launcher_model_begin_assist_capture(m, action, false);
            ImGui::PopID();
            ImGui::TableSetColumnIndex(2);
            bool capture_pad = m->capturing && m->capture_assist &&
                               m->capture_pad && m->capture_btn == action;
            char pad[48];
            settings_pad_label(m->s.assist_pad_bind[action], pad, sizeof pad);
            ImGui::PushID("pad");
            if (ImGui::Button(capture_pad ? "[ button... ]" : pad,
                              ImVec2(-FLT_MIN, 0)))
                launcher_model_begin_assist_capture(m, action, true);
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (m->capturing && m->capture_assist)
        ImGui::TextColored(col(th.warn), "Listening... (Esc cancels, Backspace unbinds)");
}

void draw_assist_tools(LauncherModel* m, const LauncherTheme& th) {
    if (!begin_panel("assist_tools_page", 0, false)) {
        end_panel();
        return;
    }
    eyebrow("ASSIST TOOLS / CHEATS");
    bool enabled = m->s.assist_tools != 0;
    if (ImGui::Checkbox(ui_text("Enable Assist Tools / Cheats"), &enabled))
        m->s.assist_tools = enabled ? 1 : 0;
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::PushTextWrapPos();
    ImGui::TextWrapped("%s",
        (m->assist_tools_note && m->assist_tools_note[0])
            ? m->assist_tools_note
            : "This optional host mode enables game-specific convenience "
              "features. The game window should disclose when it is active.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    // Fast-forward speed. Only hosts that publish a range get the slider; a
    // host that leaves the bounds unset keeps its own fixed rate and the
    // control stays absent rather than dead.
    if (m->assist_fast_forward_max > m->assist_fast_forward_min) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
    ImGui::TextUnformatted(ui_text("FAST-FORWARD SPEED"));
        ImGui::PopStyleColor();
        ImGui::TextColored(col(th.text_muted),
                           "Target emulation speed while Fast-forward is "
                           "held or latched.");
        int speed = m->s.assist_fast_forward_multiplier;
        ImGui::SetNextItemWidth(px(360));
        if (ImGui::SliderInt("##assist_fast_forward_speed", &speed,
                             m->assist_fast_forward_min,
                             m->assist_fast_forward_max, "%dx"))
            m->s.assist_fast_forward_multiplier = speed;
    }
    if (m->settings_bindings) {
        ImGui::Dummy(ImVec2(0, px(8)));
        draw_assist_binding_editor(m, th, "assist_binds", 0, true);
    }
    end_panel();
}

void draw_credits(LauncherModel* m, const LauncherTheme& th) {
    if (!begin_panel("credits_page", 0, false)) {
        end_panel();
        return;
    }
    eyebrow("CREDITS");
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text));
    ImGui::PushTextWrapPos();
    ImGui::TextWrapped("%s",
        (m->credits_text && m->credits_text[0])
            ? m->credits_text
            : "Credits have not been supplied for this title.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    end_panel();
}

/* Draw a bind row's LABEL and leave the cursor at `label_col_w` from the start
 * of this table cell, ready for the bind chip.
 *
 * Why not ImGui::SameLine(label_col_w): that offset is measured from the
 * window's content start, not the cell's, so inside a multi-column bind grid
 * the chip landed short of the reserved label column and the longest labels
 * ("D-Pad Right", "L-Stick Down") were drawn underneath it. Spacing off the
 * label just drawn is measured from the right place by construction. */
static void bind_row_label(const char* label, const ImVec4& colour,
                           float label_col_w, float min_gap) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colour, "%s", label);
    const float pad = label_col_w - ImGui::CalcTextSize(label).x;
    ImGui::SameLine(0.0f, pad > min_gap ? pad : min_gap);
}

// CONTROLLER-view rebind page: input source + deadzone, and the keyboard
// bindings grid — reached from the dashboard CONTROLLER panel's Configure
// button. The bindings grid walks the ACTIVE SystemProfile's
// ControllerSpec.buttons[]/button_count (launcher_system.h) so each system
// renders its own real vocabulary (SNES: A/B/X/Y/L/R/...; PSX: Triangle/
// Circle/Cross/Square/L1/L2/R1/R2/L3/R3/...) instead of a hardcoded SNES set.
void draw_controller_config_view(LauncherModel* m, const LauncherTheme& th) {
    const int p = m->cfg_player;
    const SystemProfile* cfg_prof = (const SystemProfile*)m->profile;
    const bool cfg_psx = cfg_prof && cfg_prof->id && !strcmp(cfg_prof->id, "psx");
    const bool cfg_snes = cfg_prof && cfg_prof->id && !strcmp(cfg_prof->id, "snes");
    const bool cfg_n64 = cfg_prof && cfg_prof->id && !strcmp(cfg_prof->id, "n64");
    if (cfg_psx && m->s.player_src[p] == 2 &&
        m->s.player_gamepad_guid[p][0] && !m->player_pad_name[p][0])
        launcher_binds_hydrate_psx_pad_names(m);

    if (begin_panel("cfg_src", 0)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
        ImGui::Text("%s %d", ui_text("CONTROLLER - PLAYER"), p + 1); ImGui::PopStyleColor(); ImGui::Spacing();
        row_label("Input source", th);
        ImGui::SetNextItemWidth(px(200));
        if (ImGui::BeginCombo("##csrc", ui_text(launcher_model_player_src_label(m, p)))) {
            draw_source_selectables(m, p);
            ImGui::EndCombo();
        }
        /* SNES: Save / Rename / Delete for the selected controller.
         *
         * Same three actions PSX offers, writing into this console's own
         * config.ini as [Controller.<guid>]. Disabled until a pad with a GUID
         * is the player's source -- there is nothing to key a profile on
         * before that, and a profile saved against "no device" could never be
         * found again. */
        if (cfg_snes) {
            static bool s_snes_rename_open = false;
            static char s_snes_rename_buf[64] = {};
            const bool can_pad = m->s.player_src[p] == 2 &&
                                 m->s.player_gamepad_guid[p][0];

            ImGui::SameLine();
            if (!can_pad) ImGui::BeginDisabled();
            if (ImGui::Button("Save Profile"))
                launcher_binds_save_snes_gamepad(m, p + 1);
            if (ImGui::IsItemHovered() && can_pad)
                ImGui::SetTooltip("Store this controller's mapping, name and "
                                  "deadzone so it comes back next time it is "
                                  "selected");
            ImGui::SameLine();
            if (ImGui::Button("Rename")) {
                std::snprintf(s_snes_rename_buf, sizeof(s_snes_rename_buf),
                              "%s", m->player_pad_name[p]);
                s_snes_rename_open = true;
            }
            {
                const float del_w = px(130.0f);
                const float right = ImGui::GetWindowContentRegionMax().x;
                if (right - del_w > ImGui::GetCursorPosX())
                    ImGui::SameLine(right - del_w);
                else
                    ImGui::SameLine();
                if (ImGui::Button("Delete Profile", ImVec2(del_w, 0)))
                    launcher_binds_delete_snes_gamepad(m, p + 1);
                if (ImGui::IsItemHovered() && can_pad)
                    ImGui::SetTooltip("Forget this controller's saved profile. "
                                      "The mapping in use is not changed.");
            }
            if (!can_pad) ImGui::EndDisabled();

            if (s_snes_rename_open) ImGui::OpenPopup("Rename Controller");
            ImVec2 c2 = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(c2, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("Rename Controller", &s_snes_rename_open,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Display name for this controller:");
                ImGui::SetNextItemWidth(px(320));
                const bool enter = ImGui::InputText(
                    "##snes_rename_pad", s_snes_rename_buf,
                    sizeof(s_snes_rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Spacing();
                if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
                    s_snes_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                const bool ok = s_snes_rename_buf[0] != '\0';
                ImGui::BeginDisabled(!ok);
                if ((ImGui::Button("OK", ImVec2(px(120), 0)) || enter) && ok) {
                    launcher_binds_rename_snes_gamepad(m, p + 1,
                                                       s_snes_rename_buf);
                    s_snes_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        /* N64: the same three actions, on the per-GUID input.ini store that
         * n64lle's host reads. Every capture already writes the mapping;
         * Save commits the name and deadzone beside it, Rename pins a name
         * the driver's reconnect must not overwrite, Delete forgets the
         * section and releases the slots that pointed at it. */
        if (cfg_n64) {
            static bool s_n64_rename_open = false;
            static char s_n64_rename_buf[64] = {};
            static double s_n64_saved_until = 0.0;
            const bool can_pad = m->s.player_src[p] == 2 &&
                                 m->s.player_gamepad_guid[p][0];

            ImGui::SameLine();
            if (!can_pad) ImGui::BeginDisabled();
            if (ImGui::Button(ui_text("Save Profile"))) {
                launcher_binds_save_n64_gamepad(m, p + 1);
                s_n64_saved_until = ImGui::GetTime() + 2.5;
            }
            if (ImGui::IsItemHovered() && can_pad)
                ImGui::SetTooltip("Store this controller's mapping, name and "
                                  "deadzone so it comes back next time it is "
                                  "selected");
            ImGui::SameLine();
            if (ImGui::Button(ui_text("Rename Gamepad"))) {
                std::snprintf(s_n64_rename_buf, sizeof(s_n64_rename_buf),
                              "%s", m->player_pad_name[p]);
                s_n64_rename_open = true;
            }
            {
                const float del_w = px(130.0f);
                const float right = ImGui::GetWindowContentRegionMax().x;
                if (right - del_w > ImGui::GetCursorPosX())
                    ImGui::SameLine(right - del_w);
                else
                    ImGui::SameLine();
                if (ImGui::Button(ui_text("Delete Gamepad"), ImVec2(del_w, 0)))
                    launcher_binds_delete_n64_gamepad(m, p + 1);
                if (ImGui::IsItemHovered() && can_pad)
                    ImGui::SetTooltip("Forget this controller's saved mapping, "
                                      "name and deadzone.");
            }
            if (!can_pad) ImGui::EndDisabled();
            if (ImGui::GetTime() < s_n64_saved_until)
                ImGui::TextColored(col(th.accent2), "Input Profile Saved!");

            if (s_n64_rename_open) ImGui::OpenPopup(ui_text("Rename Gamepad"));
            ImVec2 c3 = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(c3, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal(ui_text("Rename Gamepad"), &s_n64_rename_open,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(ui_text("Display name for this gamepad:"));
                ImGui::SetNextItemWidth(px(320));
                const bool enter = ImGui::InputText(
                    "##n64_rename_pad", s_n64_rename_buf,
                    sizeof(s_n64_rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Spacing();
                if (ImGui::Button(ui_text("Cancel"), ImVec2(px(120), 0))) {
                    s_n64_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                const bool ok = s_n64_rename_buf[0] != '\0';
                ImGui::BeginDisabled(!ok);
                if ((ImGui::Button("OK", ImVec2(px(120), 0)) || enter) && ok) {
                    launcher_binds_rename_n64_gamepad(m, p + 1, s_n64_rename_buf);
                    s_n64_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        if (cfg_psx) {
            static bool s_rename_open = false;
            static char s_rename_buf[64] = {};
            const bool can_pad = m->s.player_src[p] == 2 &&
                                 m->s.player_gamepad_guid[p][0];
            ImGui::SameLine();
            if (!can_pad) ImGui::BeginDisabled();
            if (ImGui::Button(ui_text("Rename Gamepad"))) {
                std::snprintf(s_rename_buf, sizeof(s_rename_buf), "%s",
                              m->player_pad_name[p]);
                s_rename_open = true;
            }
            if (!can_pad) ImGui::EndDisabled();

            // Delete sits on the right of the Input source row.
            {
                const float del_w = px(130.0f);
                const float right = ImGui::GetWindowContentRegionMax().x;
                ImGui::SameLine(right - del_w);
                if (!can_pad) ImGui::BeginDisabled();
                if (ImGui::Button(ui_text("Delete Gamepad"), ImVec2(del_w, 0)))
                    launcher_binds_delete_psx_gamepad(m, p + 1);
                if (!can_pad) ImGui::EndDisabled();
            }

            if (s_rename_open) ImGui::OpenPopup(ui_text("Rename Gamepad"));
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal(ui_text("Rename Gamepad"), &s_rename_open,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(ui_text("Display name for this gamepad:"));
                ImGui::SetNextItemWidth(px(320));
                bool enter = ImGui::InputText(
                    "##rename_pad", s_rename_buf, sizeof(s_rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Spacing();
                if (ImGui::Button(ui_text("Cancel"), ImVec2(px(120), 0))) {
                    s_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                const bool ok = s_rename_buf[0] != '\0';
                ImGui::BeginDisabled(!ok);
                if ((ImGui::Button("OK", ImVec2(px(120), 0)) || enter) && ok) {
                    launcher_binds_rename_psx_gamepad(m, p + 1, s_rename_buf);
                    s_rename_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }
        // Deadzone: a drag slider, not a stepper. The stepper moves in 5%
        // steps, which is the wrong granularity for a stick whose resting
        // noise a player is trying to just clear -- 8% and 12% are different
        // controllers. Whole percent, because the runner consumes a raw stick
        // radius derived from this and a fraction would not survive the trip.
        row_label("Deadzone", th);
        {
            int dz = m->s.deadzone[p];
            // 0-50% is the whole useful range -- half a stick's travel is
            // already absurd -- but a settings file carrying more than that
            // gets the full scale rather than a handle pinned at the end
            // showing a number the slider would silently rewrite.
            const int hi = dz > 50 ? 100 : 50;
            ImGui::SetNextItemWidth(px(240));
            if (ImGui::SliderInt("##dz", &dz, 0, hi, "%d%%",
                                 ImGuiSliderFlags_AlwaysClamp))
                launcher_model_set_deadzone(m, p, dz);
        }
    } end_panel();

    // Transfer Pak for THIS controller port (N64 tpak games), so it's reachable
    // from the Controller page without scrolling the dashboard. Same compact
    // tile + config modal; the port is the player being configured.
    if (m->tpak_slots > p) {
        if (begin_panel("cfg_tpak", 0)) {
            draw_tpak_tile(m, th, p);
        } end_panel();
    }

    // MOUSE card (opt-in, has_mouse_controls games only — Snap): shown whenever
    // this player's source is a keyboard family. Sensitivity + Invert X/Y +
    // three rebindable mouse buttons, each mapping to an N64 action. Placed
    // after the source/deadzone card and before the bindings card. Entirely
    // absent for every non-mouse game (has_mouse_controls == 0).
    if (m->has_mouse_controls && m->s.player_src[p] == 1) {
        if (begin_panel("cfg_mouse", 0)) {
            eyebrow("MOUSE");

            // Sensitivity: a float slider over the model's clamp range. The
            // model re-clamps on set, so the slider can never commit a value
            // outside [0.01, 0.50].
            row_label("Sensitivity", th);
            ImGui::SetNextItemWidth(px(200));
            float sens = m->s.mouse_sensitivity;
            if (ImGui::SliderFloat("##msens", &sens, 0.01f, 0.50f, "%.2f"))
                launcher_model_set_mouse_sensitivity(m, sens);

            // Invert toggles.
            bool ix = m->s.mouse_invert_x != 0;
            if (ImGui::Checkbox("Invert X", &ix)) launcher_model_toggle_mouse_invert_x(m);
            bool iy = m->s.mouse_invert_y != 0;
            if (ImGui::Checkbox("Invert Y", &iy)) launcher_model_toggle_mouse_invert_y(m);

            // Three rebindable mouse buttons -> an N64 action (or None). The
            // vocabulary is the active profile's ControllerSpec.buttons[].
            const SystemProfile* prof = (const SystemProfile*)m->profile;
            const ControllerSpec& spec = prof->controller;
            static const char* kMouseRows[3] = { "Left click", "Right click", "Middle click" };
            for (int i = 0; i < 3; ++i) {
                ImGui::PushID(i);
                row_label(kMouseRows[i], th);
                const int cur = m->s.mouse_bind[i];
                const char* cur_label = (cur >= 0 && cur < spec.button_count)
                                        ? spec.buttons[cur].label : "None";
                ImGui::SetNextItemWidth(px(200));
                if (ImGui::BeginCombo("##mbtn", cur_label)) {
                    if (ImGui::Selectable("None", cur < 0))
                        launcher_model_set_mouse_bind(m, i, -1);
                    for (int b = 0; b < spec.button_count; ++b) {
                        if (ImGui::Selectable(spec.buttons[b].label, cur == b))
                            launcher_model_set_mouse_bind(m, i, b);
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        } end_panel();
    }

    // MOTION card (opt-in): the host owns controller discovery and axis
    // mapping; recomp-ui owns only the portable sensitivity setting.
    if (m->has_gyro_controls && p == 0) {
        if (begin_panel("cfg_motion", 0)) {
            eyebrow("MOTION");
            row_label("Gyro sensitivity", th);
            ImGui::SetNextItemWidth(px(200));
            float sens = m->s.gyro_sensitivity;
            if (ImGui::SliderFloat("##gyrosens", &sens, 0.25f, 4.00f, "%.2fx"))
                launcher_model_set_gyro_sensitivity(m, sens);

            // Prefer the explicitly selected Player 1 pad; while the source
            // is still Keyboard, preview the first connected gyro-capable pad
            // so motion can be verified before changing the source dropdown.
            const LauncherPad* motion_pad = nullptr;
            if (m->s.player_src[p] == 2) {
                for (int i = 0; i < g_pad_count; ++i) {
                    const bool id_match =
                        m->player_pad_id[p] &&
                        g_pads[i].id == m->player_pad_id[p];
                    const bool guid_match =
                        m->s.player_gamepad_guid[p][0] &&
                        g_pads[i].guid[0] &&
                        std::strcmp(m->s.player_gamepad_guid[p],
                                    g_pads[i].guid) == 0;
                    if (id_match || guid_match) {
                        motion_pad = &g_pads[i];
                        break;
                    }
                }
            }

            const float rate = motion_pad && motion_pad->has_gyro
                                 ? motion_pad->gyro_z : 0.0f;
            // Match gbarecomp's PC mapping exactly: negate face-normal Z,
            // apply the gentler 128-units/rad/s DualSense base gain, then the
            // launcher multiplier, and clamp to the cartridge's +/-0x600.
            const float cartridge = std::clamp(
                -rate * 128.0f * m->s.gyro_sensitivity,
                -1536.0f, 1536.0f);
            const float level = cartridge / 1536.0f;

            ImGui::Spacing();
            ImGui::TextUnformatted(
                motion_pad
                    ? (motion_pad->has_gyro
                           ? motion_pad->name
                           : "Selected controller has no gyro sensor")
                    : "No gyro controller detected");

            // Centered live gauge. The moving needle and colored fill show the
            // exact signed value the host will send to the cartridge; reaching
            // either edge means the configured sensitivity is saturating.
            const float meter_w = std::min(px(360.0f),
                                           ImGui::GetContentRegionAvail().x);
            const ImVec2 meter_min = ImGui::GetCursorScreenPos();
            const ImVec2 meter_max(meter_min.x + meter_w,
                                   meter_min.y + px(26.0f));
            ImGui::InvisibleButton("##gyro_meter",
                                   ImVec2(meter_w, px(26.0f)));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(meter_min, meter_max, imcol(th.control),
                              px(th.radius_sm));
            dl->AddRect(meter_min, meter_max, imcol(th.border),
                        px(th.radius_sm));
            const float center = (meter_min.x + meter_max.x) * 0.5f;
            for (int tick = -2; tick <= 2; ++tick) {
                const float x = center + tick * meter_w * 0.20f;
                dl->AddLine(ImVec2(x, meter_min.y + px(7.0f)),
                            ImVec2(x, meter_max.y - px(7.0f)),
                            imcol(tick == 0 ? th.text_muted : th.border),
                            tick == 0 ? px(2.0f) : px(1.0f));
            }
            const float needle = center + level * meter_w * 0.5f;
            if (level != 0.0f) {
                dl->AddRectFilled(
                    ImVec2(std::min(center, needle), meter_min.y + px(9.0f)),
                    ImVec2(std::max(center, needle), meter_max.y - px(9.0f)),
                    imcol(th.accent), px(3.0f));
            }
            dl->AddLine(ImVec2(needle, meter_min.y + px(3.0f)),
                        ImVec2(needle, meter_max.y - px(3.0f)),
                        imcol(motion_pad && motion_pad->has_gyro
                                  ? th.good : th.warn),
                        px(3.0f));
            ImGui::Text("Output %+.0f / 1536   Sensor %+.2f rad/s",
                        cartridge, rate);

            ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
            ImGui::TextWrapped(
                "A compatible controller motion sensor is used automatically. "
                "Mouse drag remains available as a fallback.");
            ImGui::PopStyleColor();
        } end_panel();
    }

    /* Voxel/3D camera bindings are feature-driven, not a permanent NES
     * capability. The card appears only while at least one enabled feature
     * advertises camera_controls, and only on Player 1's page because the
     * presentation camera is global. */
    if (p == 0 && enabled_camera_controls(m)) {
        static const char* kActionLabels[LNG_CAMERA_BIND_COUNT] = {
            "Look up", "Look down", "Look left", "Look right",
            "Roll left", "Roll right", "Zoom in", "Zoom out",
            "Sprites smaller", "Sprites larger", "Reset view",
            "Toggle Voxel 3D",
        };
        static const char* kPadDefaults[LNG_CAMERA_BIND_COUNT] = {
            "Right stick up", "Right stick down",
            "Right stick left", "Right stick right",
            "-", "-", "-", "-", "-", "-", "-", "-",
        };
        launcher_binds_refresh_camera(m);
        if (begin_panel("cfg_camera", 0)) {
            eyebrow("3D CAMERA");
            ImGui::TextColored(
                col(th.text_muted),
                "Active while the enabled Voxel mod is running. Right-stick "
                "look is the gamepad default; keyboard actions use the numpad.");
            ImGui::Spacing();
            if (ImGui::BeginTable(
                    "camera_binds", 3,
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Action",
                                        ImGuiTableColumnFlags_WidthStretch,
                                        1.2f);
                ImGui::TableSetupColumn("Keyboard",
                                        ImGuiTableColumnFlags_WidthStretch,
                                        1.0f);
                ImGui::TableSetupColumn("Gamepad default",
                                        ImGuiTableColumnFlags_WidthStretch,
                                        1.0f);
                ImGui::TableHeadersRow();
                for (int action = 0; action < LNG_CAMERA_BIND_COUNT;
                     ++action) {
                    ImGui::PushID(action);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(kActionLabels[action]);
                    ImGui::TableSetColumnIndex(1);
                    const bool capturing =
                        m->camera_capturing &&
                        m->capture_camera == action;
                    if (capturing)
                        ImGui::PushStyleColor(
                            ImGuiCol_Button, col(th.accent));
                    if (ImGui::Button(
                            capturing ? "[ press a key... ]"
                                      : m->camera_binds[action],
                            ImVec2(-FLT_MIN, 0)))
                        launcher_model_begin_camera_capture(m, action);
                    if (capturing) ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(2);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(
                        col(th.text_muted), "%s",
                        kPadDefaults[action]);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("Reset Camera Bindings"))
                launcher_binds_reset_camera(m);
            if (m->camera_capturing)
                ImGui::TextColored(
                    col(th.warn), "Listening... (Esc cancels, Backspace unbinds)");
        } end_panel();
    }

    // Some systems ship no rebindable input layer (N64 Snap / PMS-J read no
    // input.cfg): GameInfo.hide_rebind drops the bindings card entirely and the
    // Controller view is source+deadzone only.
    if (m->hide_rebind) return;

    if (begin_panel("cfg_binds", 0)) {
        // Responsive grid: fit as many label+chip columns as the width allows
        // (1..4) instead of one tall column with dead space to the right.
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const ControllerSpec& spec = prof->controller;
        const bool is_psx = prof && prof->id && !strcmp(prof->id, "psx");

        // ---- PSX: bindings for whatever the player's input SOURCE is -------
        // Column-major layout (top→bottom then next column) matching
        // kPsxGamepadBindOrder, for the gamepad grid and the keyboard grid
        // alike so switching source does not reshuffle the vocabulary.
        //
        // The gamepad grid used to be the ONLY thing this branch drew, and it
        // renders only for a selected SDL GUID -- so picking Keyboard as the
        // input source left the panel saying "select a gamepad" and offered no
        // way to rebind the keyboard at all. Every other console reaches the
        // generic keyboard grid in the else-branch below; PSX diverts here and
        // so had lost it. Nothing else was missing: launcher_binds.c already
        // fills m->binds / m->binds_alt for PSX from psxrecomp's own
        // 24-scancode keybinds.ini, launcher_binds_set_button_slot() already
        // persists back to it, launcher_binds_reset_player() already resets the
        // keyboard map when no gamepad is selected, and the capture handler
        // already accepts keys and mouse buttons into either slot.
        if (is_psx) {
            const bool has_pad_src = m->s.player_src[p] == 2 &&
                                     m->s.player_gamepad_guid[p][0];

            ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
            if (has_pad_src) ImGui::Text("GAMEPAD BINDINGS - PLAYER %d", p + 1);
            else             ImGui::Text("KEYBOARD BINDINGS - PLAYER %d", p + 1);
            ImGui::PopStyleColor();
            // The alternate slot has no chip of its own (see the grid below);
            // the heading carries its only discoverable mention, so the button
            // row stays identical to the gamepad card's.
            if (!has_pad_src) {
                ImGui::SameLine();
                ImGui::TextColored(col(th.text_muted),
                    "  (right-click a bind for an alternate)");
            }
            ImGui::Spacing();

            if (!has_pad_src) {
                // Same shape as the gamepad grid below: a fixed 3x8, filled
                // column-major down kPsxGamepadBindOrder, one muted LABEL and
                // one bind CHIP per row. Reading a bind list is a vertical
                // scan, so the eye must go top->bottom within a column, not
                // left->right across three unrelated inputs; and switching
                // Input source must not reshuffle the vocabulary under the
                // player's cursor.
                //
                // The keyboard store keeps an ALTERNATE bind per input
                // (slot 1, also where a mouse button goes) which this grid no
                // longer shows -- one chip is what parity with the gamepad
                // card costs. Existing alternates in keybinds.ini are left
                // untouched and still assert at runtime; they are just not
                // editable from here.
                float label_col_w = px(90.0f);
                for (int i = 0; i < LNG_PSX_PAD_BUTTON_COUNT; ++i) {
                    float w = ImGui::CalcTextSize(spec.buttons[i].label).x + px(20.0f);
                    if (w > label_col_w) label_col_w = w;
                }
                const float chip_w = px(140.0f);
                const float cell_w = label_col_w + chip_w + px(16.0f);
                if (ImGui::BeginTable("psx_key_binds", LNG_PSX_GAMEPAD_BIND_COLS,
                                      ImGuiTableFlags_SizingFixedFit)) {
                    for (int c = 0; c < LNG_PSX_GAMEPAD_BIND_COLS; ++c)
                        ImGui::TableSetupColumn(nullptr,
                            ImGuiTableColumnFlags_WidthFixed, cell_w);
                    for (int row = 0; row < LNG_PSX_GAMEPAD_BIND_ROWS; ++row) {
                        for (int c = 0; c < LNG_PSX_GAMEPAD_BIND_COLS; ++c) {
                            const int order_i = c * LNG_PSX_GAMEPAD_BIND_ROWS + row;
                            const int b = kPsxGamepadBindOrder[order_i];
                            ImGui::TableNextColumn();
                            ImGui::PushID(b);
                            bind_row_label(spec.buttons[b].label,
                                           col(th.text_muted), label_col_w,
                                           px(6.0f));
                            const bool cap = m->capturing && !m->capture_pad &&
                                             m->capture_btn == b;
                            const bool cap_alt = cap && m->capture_slot == 1;
                            const char* lbl = m->binds[p][b];
                            if (!lbl || !lbl[0]) lbl = "(unbound)";
                            const char* alt = m->binds_alt[p][b];
                            const bool has_alt = alt && alt[0] &&
                                                 strcmp(alt, "(unbound)") != 0;
                            const char* chip = cap_alt ? "[ press an alt... ]"
                                             : cap     ? "[ press a key... ]"
                                                       : lbl;
                            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                            if (ImGui::Button(chip, ImVec2(chip_w, 0))) {
                                m->map_all_active = false;
                                launcher_model_begin_capture_slot(m, b, 0);
                            }
                            if (cap) ImGui::PopStyleColor();
                            // Right-click captures into slot 1 -- the ALTERNATE
                            // bind, and the only slot a mouse button can go in.
                            // Fire on RELEASE, not IsItemClicked (which fires on
                            // press): try_capture swallows every mouse event once
                            // capturing, so a press-triggered capture would eat
                            // its own button-up and leave ImGui believing the
                            // right button was held down forever.
                            if (ImGui::IsItemHovered() &&
                                ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                                m->map_all_active = false;
                                launcher_model_begin_capture_slot(m, b, 1);
                            }
                            // A bind with an alternate carries an accent dot in
                            // the chip's corner: without it the second bind is
                            // invisible, and a chip with no alternate stays
                            // pixel-identical to the gamepad card's.
                            if (has_alt && !cap) {
                                const ImVec2 mn = ImGui::GetItemRectMin();
                                const ImVec2 mx = ImGui::GetItemRectMax();
                                ImGui::GetWindowDrawList()->AddCircleFilled(
                                    ImVec2(mx.x - px(6.0f), mn.y + px(6.0f)),
                                    px(2.5f), ImGui::GetColorU32(col(th.accent2)));
                            }
                            if (ImGui::IsItemHovered()) {
                                if (has_alt)
                                    ImGui::SetTooltip(
                                        "Alternate: %s\n"
                                        "Right-click to rebind it (key or mouse button)",
                                        alt);
                                else
                                    ImGui::SetTooltip(
                                        "No alternate bound\n"
                                        "Right-click to set one (key or mouse button)");
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::Button("Map All Bindings"))
                    launcher_model_begin_map_all(m);
                ImGui::SameLine();
                if (ImGui::Button("Reset to Defaults"))
                    launcher_binds_reset_player(m, m->cfg_player + 1);
                // Save Profile -- keyboard captures already write keybinds.ini
                // on every rebind, so this is the explicit commit + flush.
                static double s_kb_profile_saved_until = 0.0;
                {
                    const float save_w = px(120.0f);
                    const float right = ImGui::GetWindowContentRegionMax().x;
                    ImGui::SameLine(right - save_w);
                    if (ImGui::Button("Save Profile", ImVec2(save_w, 0))) {
                        launcher_binds_save_psx_keyboard(m, p + 1);
                        s_kb_profile_saved_until = ImGui::GetTime() + 2.5;
                    }
                }
                if (m->capturing && !m->capture_pad) {
                    const char* label =
                        (m->capture_btn >= 0 &&
                         m->capture_btn < LNG_PSX_PAD_BUTTON_COUNT)
                            ? spec.buttons[m->capture_btn].label : "?";
                    ImGui::TextColored(col(th.warn),
                        "Map %s to %s (Esc cancels, Backspace unbinds)%s",
                        m->capture_slot == 1 ? "an alternate key / mouse button"
                                             : "a key",
                        label,
                        m->map_all_active ? " — Map All" : "");
                }
                if (ImGui::GetTime() < s_kb_profile_saved_until)
                    ImGui::TextColored(col(th.accent2), "Input Profile Saved!");
            } else {
                float label_col_w = px(90.0f);
                for (int i = 0; i < LNG_PSX_PAD_BUTTON_COUNT; ++i) {
                    float w = ImGui::CalcTextSize(spec.buttons[i].label).x + px(20.0f);
                    if (w > label_col_w) label_col_w = w;
                }
                const float chip_w = px(140.0f);
                const float cell_w = label_col_w + chip_w + px(16.0f);
                if (ImGui::BeginTable("psx_pad_binds", LNG_PSX_GAMEPAD_BIND_COLS,
                                      ImGuiTableFlags_SizingFixedFit)) {
                    for (int c = 0; c < LNG_PSX_GAMEPAD_BIND_COLS; ++c)
                        ImGui::TableSetupColumn(nullptr,
                            ImGuiTableColumnFlags_WidthFixed, cell_w);
                    for (int row = 0; row < LNG_PSX_GAMEPAD_BIND_ROWS; ++row) {
                        for (int c = 0; c < LNG_PSX_GAMEPAD_BIND_COLS; ++c) {
                            const int order_i = c * LNG_PSX_GAMEPAD_BIND_ROWS + row;
                            const int b = kPsxGamepadBindOrder[order_i];
                            ImGui::TableNextColumn();
                            ImGui::PushID(b);
                            bind_row_label(spec.buttons[b].label,
                                           col(th.text_muted), label_col_w,
                                           px(6.0f));
                            const bool cap = m->capturing && m->capture_pad &&
                                             m->capture_btn == b;
                            const bool wait_rel = cap && m->map_all_wait_release;
                            const char* pl = m->pad_binds[p][b][0]
                                               ? m->pad_binds[p][b]
                                               : "(unbound)";
                            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                            const char* chip = wait_rel ? "[ release button... ]"
                                              : cap     ? "[ press a button... ]"
                                                        : pl;
                            if (ImGui::Button(chip, ImVec2(chip_w, 0))) {
                                m->map_all_active = false;
                                m->map_all_wait_release = false;
                                launcher_model_begin_pad_capture(m, b);
                                // If they start a bind while still holding the
                                // prior input, wait for a full release first.
                                if (m->capturing && m->player_pad_id[p] &&
                                    !launcher_input_gamepad_at_rest(
                                        m->player_pad_id[p]))
                                    m->map_all_wait_release = true;
                            }
                            if (cap) ImGui::PopStyleColor();
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::Button("Map All Bindings")) {
                    launcher_model_begin_map_all(m);
                    if (m->capturing && m->player_pad_id[p] &&
                        !launcher_input_gamepad_at_rest(m->player_pad_id[p]))
                        m->map_all_wait_release = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset to Defaults"))
                    launcher_binds_reset_player(m, m->cfg_player + 1);
                // Save Profile — name, choice, mappings, and deadzone for this GUID.
                static double s_profile_saved_until = 0.0;
                {
                    const float save_w = px(120.0f);
                    const float right = ImGui::GetWindowContentRegionMax().x;
                    ImGui::SameLine(right - save_w);
                    if (ImGui::Button("Save Profile", ImVec2(save_w, 0))) {
                        launcher_binds_save_psx_gamepad(m, p + 1);
                        s_profile_saved_until = ImGui::GetTime() + 2.5;
                    }
                }
                if (m->capturing && m->capture_pad) {
                    const char* label =
                        (m->capture_btn >= 0 &&
                         m->capture_btn < LNG_PSX_PAD_BUTTON_COUNT)
                            ? spec.buttons[m->capture_btn].label : "?";
                    if (m->map_all_wait_release) {
                        ImGui::TextColored(col(th.warn),
                            "Release Button (Esc cancels, Backspace unbinds)%s",
                            m->map_all_active ? " — Map All" : "");
                    } else {
                        ImGui::TextColored(col(th.warn),
                            "Map an input to %s (Esc cancels, Backspace unbinds)%s",
                            label,
                            m->map_all_active ? " — Map All" : "");
                    }
                }
                if (ImGui::GetTime() < s_profile_saved_until)
                    ImGui::TextColored(col(th.accent2), "Input Profile Saved!");
            }
        } else {
        // Alternate binds per input (N64's input.cfg keeps two; SNES/GBA
        // keep one). 0 in the spec reads as 1 (older positional initializers).
        // Host-owned settings arrays have one keyboard and one controller
        // value per action, independent of a console bridge's alternate-slot
        // format (for example N64 input.cfg). Keep that opt-in store on its
        // own two-chip path instead of accidentally editing the native store.
        const bool settings_player_binds = m->settings_bindings && !cfg_psx;
        const int bpi = settings_player_binds
            ? 1 : (spec.binds_per_input < 1 ? 1 : spec.binds_per_input);

        // Re-read display strings on entry.
        //
        // This used to be done only for bpi>=2 stores, on the reasoning that
        // single-bind stores are per-player and cannot change behind the page.
        // That is not true of a store shared across players or across device
        // types -- N64's keyboard table is one table for every port -- and it
        // is not true of a per-GUID store either, where selecting a different
        // controller changes every label on the page. Refreshing is a pure
        // read of whatever is on disk, so it is correct for every console and
        // there is no longer a case to special-case.
        launcher_binds_refresh(m);

        // A pad-bind console (Genesis) offers a KEY chip AND a GAMEPAD chip per
        // row — the legacy launcher's "Set key" / "Set pad" pair. Otherwise the
        // grid is keyboard-only, exactly as before.
        const bool has_pad = spec.has_pad_binds != 0 || settings_player_binds;

        // Is this player actually driving the game with a pad?
        //
        // player_src == 2 is the gamepad source the Input source selector sets,
        // and is what the PSX gamepad panel already keys off. This deliberately
        // does NOT ask a console-specific "which store captures" question: an
        // earlier version did, and because that question was N64-only, a SNES
        // player on a controller was shown the KEYBOARD row and Auto Map
        // listened for keys, so pressing the controller did nothing at all.
        const bool pad_src = has_pad && m->s.player_src[p] == 2;

        // Heading uses accent2 so each console's title tints in ITS logo colour
        // (N64 blue; single-accent consoles set accent2 == accent).
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent2));
        if (has_pad) ImGui::Text("INPUT BINDINGS - PLAYER %d", p + 1);
        else         ImGui::Text("KEYBOARD BINDINGS - PLAYER %d", p + 1);
        ImGui::PopStyleColor(); ImGui::Spacing();

        // Rows shown follow the player's ACTIVE pad mode (Genesis 3-Button hides
        // X/Y/Z/Mode); non-mode systems get their full button_count.
        const int nbtn = launcher_model_active_button_count(m, p);

        // Label column width is sized to the WIDEST label this system's spec
        // actually uses (e.g. PSX's "Triangle") instead of a constant tuned
        // for SNES's shorter names ("Select") — otherwise longer per-system
        // vocab overlaps the bind-chip button next to it.
        float label_col_w = px(70.0f);
        for (int b = 0; b < nbtn; ++b) {
            float w = ImGui::CalcTextSize(spec.buttons[b].label).x + px(20.0f);
            if (w > label_col_w) label_col_w = w;
        }
        // Two chips per row when either the N64 keeps two binds per input
        // (bpi>=2) OR a pad-bind console pairs a KEY + GAMEPAD chip (has_pad);
        // narrower chips then. Single-chip cells keep the wider chip AND the
        // exact legacy cell width (label + 170) so non-pad/single-bind consoles
        // (SNES/GBA) pack columns byte-identically to before this existed.
        // ONE chip per row when the console keeps a single bind per input.
        //
        // A row used to carry a KEY chip and a GAMEPAD chip side by side, but
        // only one of them can act: the player's Input source decides whether a
        // key or a pad button drives that input, and the other chip maps
        // something nothing reads. Showing both put an inert control next to a
        // live one with nothing to tell them apart. The source selector sits
        // directly above, so switching it brings the other set back.
        //
        // N64 (bpi >= 2) is untouched: its two chips are primary and ALTERNATE
        // binds for the same input, and both are live.
        const bool two_chip = (bpi >= 2);
        const float chip_w   = two_chip ? px(118.0f) : px(160.0f);
        const float chip_gap = px(6.0f);
        const float cell_w = two_chip
            ? (label_col_w + chip_w + chip_gap + chip_w + px(16.0f))
            : (label_col_w + chip_w + px(16.0f));

        // Column-major when the console declares a panel order: a pad's
        // controls come in groups (D-pad, face, shoulders) and reading DOWN a
        // column keeps each group together. Filling across rows scatters them.
        const int* const order = spec.pad_bind_order;
        const int order_rows = order ? spec.pad_bind_rows : 0;
        const int order_cols = order ? spec.pad_bind_cols : 0;
        // Only when the declared grid matches the buttons actually shown: a pad
        // MODE can reduce the active count, and an order naming a hidden button
        // would put a control on screen for an input this mode does not have.
        const bool vertical = order && order_rows > 0 && order_cols > 0 &&
                              order_rows * order_cols == nbtn;

        int cols;
        if (vertical) {
            cols = order_cols;
        } else {
            cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
            if (cols < 1) cols = 1;
            if (cols > 4) cols = 4;
        }
        // Fixed-width columns, explicitly sized to cell_w: the default
        // stretch policy divides available width evenly across `cols`
        // regardless of our computed cell_w, which reintroduces the very
        // overlap/clip this sizing pass exists to avoid.
        if (ImGui::BeginTable("binds", cols, ImGuiTableFlags_SizingFixedFit)) {
            for (int c = 0; c < cols; ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cell_w);
            const int cell_count = vertical ? (order_rows * order_cols) : nbtn;
            for (int cell = 0; cell < cell_count; ++cell) {
                // Column-major: walk down column 0, then column 1, then 2. The
                // table itself is filled left-to-right, so the index is
                // transposed here rather than the table being reshaped.
                const int b = vertical
                    ? order[(cell % order_cols) * order_rows + (cell / order_cols)]
                    : cell;
                ImGui::TableNextColumn();
                ImGui::PushID(b);
                bind_row_label(spec.buttons[b].label, col(th.text_muted),
                               label_col_w, chip_gap);
                if (bpi >= 2) {
                    // Two chips per input: slot 0 primary, slot 1 alternate,
                    // both keyboard binds in the console's own store. No
                    // console ships this today — N64 left it when its gamepad
                    // half moved to the per-GUID store — but the store shape it
                    // serves (alternates per input) is not N64-specific.
                    for (int slot = 0; slot < bpi; ++slot) {
                        if (slot) ImGui::SameLine(0, chip_gap);
                        ImGui::PushID(slot);
                        const bool cap = m->capturing && m->capture_btn == b
                                                      && m->capture_slot == slot;
                        const char* txt = cap
                            ? "[ press a key... ]"
                            : (slot == 0 ? m->binds[p][b] : m->binds_alt[p][b]);
                        if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                        if (ImGui::Button(txt, ImVec2(chip_w, 0)))
                            launcher_model_begin_capture_slot(m, b, slot);
                        if (cap) ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                } else if (pad_src) {
                    // GAMEPAD chip only: the player's source is a pad, so a key
                    // bind on this row would map something nothing reads.
                    ImGui::PushID("pad");
                    const bool cap_pad = m->capturing && m->capture_pad && m->capture_btn == b;
                    char settings_pad[48];
                    settings_pad_label(m->s.player_pad_bind[p][b],
                                       settings_pad, sizeof settings_pad);
                    const char* pl = settings_player_binds
                        ? settings_pad
                        : (m->pad_binds[p][b][0]
                            ? m->pad_binds[p][b] : "(unbound)");
                    if (cap_pad) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                    if (ImGui::Button(cap_pad ? "[ press a button... ]" : pl, ImVec2(chip_w, 0)))
                        launcher_model_begin_pad_capture(m, b);
                    if (cap_pad) ImGui::PopStyleColor();
                    ImGui::PopID();
                } else {
                    // KEY chip only: keyboard is the source (or the console has
                    // no pad binds at all).
                    const bool cap_key = m->capturing && !m->capture_pad && m->capture_btn == b;
                    if (cap_key) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                    const char* key_text = settings_player_binds
                        ? settings_key_label(m->s.player_key_bind[p][b])
                        : m->binds[p][b];
                    if (ImGui::Button(cap_key ? "[ press a key... ]" : key_text, ImVec2(chip_w, 0)))
                        launcher_model_begin_capture(m, b);
                    if (cap_key) ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        /* ---- Auto Map All -------------------------------------------------
         *
         * Walks the panel in the order it is READ -- down column 1, then 2,
         * then 3 -- capturing each input in turn, so the player presses
         * buttons in the same sequence their eyes are already following.
         *
         * State lives here rather than in the model because it is a property
         * of this page: a run is abandoned the moment the page stops drawing
         * it, which is what should happen when the player navigates away
         * mid-sequence. */
        const bool automap_running =
            s_automap_i >= 0 && s_automap_player == p;
        const int automap_total = vertical ? (order_rows * order_cols) : nbtn;

        if (automap_running) {
            /* Esc abandons the whole run, not just the current capture. Checked
             * before advancing, or cancelling one capture would immediately
             * begin the next and Esc would appear to do nothing. */
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                s_automap_i = -1;
                s_automap_player = -1;
            } else if (!m->capturing) {
                /* Wait for the pad to come to REST between steps.
                 *
                 * Without this, one held control satisfies every remaining
                 * step: the capture commits, the next begins on the following
                 * frame, and the same still-held button commits that one too.
                 * A single resting D-pad direction (deadzone defaults to 0%)
                 * walked straight down the list and bound itself to eight
                 * inputs in a fraction of a second.
                 *
                 * PSX's Map All carries the same guard as map_all_wait_release
                 * -- "never bind while waiting for release". Mine did not. */
                const uint32_t pad_id = m->player_pad_id[p];
                if (pad_src && pad_id &&
                    !launcher_input_gamepad_at_rest(pad_id)) {
                    /* hold here; nothing is captured until the pad settles */
                } else if (s_automap_i >= automap_total) {
                    s_automap_i = -1;       /* finished the last column */
                    s_automap_player = -1;
                } else {
                    /* order[] is ALREADY in reading order -- down column 1,
                     * then 2, then 3 -- so the sequence walks it directly.
                     *
                     * The transposition below is for DRAWING: the table fills
                     * left-to-right, so a screen cell has to be converted into
                     * an order index. Reusing it here made Auto Map follow the
                     * screen's fill order instead of the column order, which is
                     * exactly the left-to-right walk that looked wrong. */
                    const int b = vertical ? order[s_automap_i] : s_automap_i;
                    s_automap_i++;
                    if (pad_src) launcher_model_begin_pad_capture(m, b);
                    else         launcher_model_begin_capture(m, b);
                }
            }
        }

        if (automap_running) {
            if (ImGui::Button("Cancel Auto Map")) {
                s_automap_i = -1;
                s_automap_player = -1;
            }
        } else if (ImGui::Button("Auto Map All")) {
            s_automap_i = 0;
            s_automap_player = p;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Defaults")) {
            if (settings_player_binds)
                launcher_model_reset_player_bindings(m, m->cfg_player);
            else
                launcher_binds_reset_player(m, m->cfg_player + 1);
        }
        if (automap_running) {
            /* Name the input being waited on: a bare "Listening..." during a
             * twelve-step sequence does not say which one. */
            const int cell = s_automap_i - 1;
            const int b = (cell >= 0 && cell < automap_total)
                ? (vertical ? order[cell] : cell)
                : 0;
            const uint32_t pad_id_now = m->player_pad_id[p];
            const bool settling = pad_src && pad_id_now && !m->capturing &&
                                  !launcher_input_gamepad_at_rest(pad_id_now);
            if (settling) {
                ImGui::TextColored(col(th.warn),
                                   "Auto Map - release the controller to "
                                   "continue");
            } else {
                ImGui::TextColored(col(th.accent), "Auto Map %d/%d - press %s%s",
                                   cell + 1, automap_total,
                                   spec.buttons[b].label,
                                   pad_src ? " on the controller" : "");
            }
        } else if (m->capturing) {
            ImGui::TextColored(col(th.warn), "Listening... (Esc cancels, Backspace unbinds)");
        }
        } // !is_psx
    } end_panel();

    // Zapper (light gun) block — NES Zapper games only (gi.zapper). The mouse
    // is the gun on a PC: position aims, left click pulls the trigger. Both
    // switches persist to keybinds.ini's [zapper] section immediately (same
    // file the game's runtime reads; the rest of the file is preserved).
    if (m->zapper) {
        if (begin_panel("cfg_zapper", 0)) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
            ImGui::TextUnformatted("ZAPPER (LIGHT GUN)");
            ImGui::PopStyleColor(); ImGui::Spacing();
            ImGui::TextColored(col(th.text_muted),
                "The mouse is the Zapper: move to aim, left-click to fire.");
            ImGui::Dummy(ImVec2(0, px(4)));
            bool mouse = m->zapper_mouse;
            if (ImGui::Checkbox("Mouse acts as the Zapper", &mouse))
                launcher_model_toggle_zapper_mouse(m);
            bool ch = m->zapper_crosshair;
            if (ImGui::Checkbox("Show crosshair (hides the OS cursor)", &ch))
                launcher_model_toggle_zapper_crosshair(m);
        } end_panel();
    }

    /* Named host actions stand on their own; see the note in
     * draw_assist_binding_editor. */
    if (m->assist_binding_count > 0 && m->assist_binding_labels) {
        if (begin_panel("cfg_assist_binds", 0)) {
            draw_controller_assist_shortcuts(m, th);
        } end_panel();
    }
}

void panel_controller_config_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_controller_config_view(m, *th);
}

// The CONTROLLER view composes whichever panel(s) this game's SystemProfile
// lists in panels_controller — today always the single "controller_config"
// page (source+deadzone card, then the bindings grid card), matching the
// architecture's "Binds ... page reached from the Controller panel's
// Configure" note.
void draw_controller(LauncherModel* m, const LauncherTheme& th) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const LauncherPanel* p = find_composed(prof->panels_controller, "controller_config", m);
    if (p) p->draw(m, &th);
}

const RecompLauncherCNetplayCallbacks* np_cb(LauncherModel* m) {
    return (m && m->netplay_supported) ? m->netplay : nullptr;
}

bool np_connected(LauncherModel* m) {
    const auto* np = np_cb(m);
    return np && np->connected && np->connected(np->ctx);
}

bool np_valid_port(const char* text) {
    if (!text || !text[0]) return false;
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10u + (unsigned)(*p - '0');
        if (value > 65535u) return false;
    }
    return value != 0;
}

bool np_prepare_guest_bind(char* out, size_t cap, char* status, size_t status_cap) {
    if (launcher_udp_prepare_guest_bind(out, cap) != 0) {
        if (status && status_cap)
            std::snprintf(status, status_cap, "No free UDP port near 7778. Try again.");
        return false;
    }
    return true;
}

void np_connect_and_list(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np) return;
    if (np->set_player_name && m->s.netplay_player_name[0])
        np->set_player_name(np->ctx, m->s.netplay_player_name);
    const bool already = np->connected && np->connected(np->ctx);
    const bool in_flight = np->connecting && np->connecting(np->ctx);
    if (np->connect && !already && !in_flight)
        (void)np->connect(np->ctx);
    if (np->request_list)
        np->request_list(np->ctx);
    m->netplay_list_fresh = true;
    if (!already)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Connecting to lobby server…");
}

/* Reload server lobby table + UDP-browse LAN hosts (BEACON) / file registry. */
void np_refresh_lobby_list(LauncherModel* m) {
    np_connect_and_list(m);
    m->netplay_selected_lobby = -1;
    /* Keep Connecting… status from np_connect_and_list; clear only when
     * the caller is an explicit Refresh after a prior error. */
}

static void mod_note_error(LauncherModel* m);
static bool mod_commit_launch(LauncherModel* m);
/* Defined with the Mods page; reused by the compact lobby mod picker. */
static void draw_mod_feature_option(LauncherModel* m,
                                    const RecompLauncherCModFeature& feature,
                                    const RecompLauncherCModOption& option);

/* Netplay commits through the provider's commit_netplay hook: it applies the
 * HOST's lobby mod plan (match_caps.mods) on every peer without touching the
 * player's persisted offline selection, and clears to vanilla when the host
 * published no mods. Skipping the hook entirely (no provider support) leaves
 * the session vanilla, which is the safe default. */
static bool mod_commit_netplay_launch(LauncherModel* m) {
    if (!m) return true;
    const auto* mods = m->mods;
    if (!mods) return true;
    if (mods->commit_netplay) {
        if (mods->commit_netplay(mods->ctx, launcher_model_rom_path(m)))
            return true;
        mod_note_error(m);
        /* A refused mod commit cancels the launch. mod_status only shows on
         * the Mods page, so mirror it where the player actually is — the
         * lobby — or the game just never starts with no explanation. */
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Cannot start: %s",
                      m->mod_status[0] ? m->mod_status
                                       : "this lobby's mods could not be "
                                         "applied on this machine.");
        return false;
    }
    return true;
}

void np_try_launch(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np || !np->fill_launch) return;
    RecompLauncherCNetplayLaunch launch{};
    if (!np->fill_launch(np->ctx, &launch) || !launch.enabled) return;
    if (mod_commit_netplay_launch(m)) {
        m->s.netplay_launch = launch;
        if (np->clear_launch_pending) np->clear_launch_pending(np->ctx);
        m->action = LNG_ACTION_LAUNCH;
    }
}

void np_refresh_host_ip(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np) return;
    m->netplay_local_address_count = 0;

    /* Enumerate local interfaces for the Host Lobby "Advertised IP Address"
     * dropdown (LAN-only and online). Online also uses the pick as host_bind /
     * preferred LAN advertise; STUN still publishes a public endpoint. */
    if (np->local_address_get) {
        for (int index = 0; index < LNG_NETPLAY_MAX_LOCAL_ADDRESSES; ++index) {
            RecompLauncherCNetplayLocalAddress candidate{};
            if (!np->local_address_get(np->ctx, index, &candidate)) break;
            candidate.address[sizeof(candidate.address) - 1] = '\0';
            candidate.label[sizeof(candidate.label) - 1] = '\0';
            if (!candidate.address[0]) continue;

            bool duplicate = false;
            for (int existing = 0; existing < m->netplay_local_address_count; ++existing) {
                if (std::strcmp(m->netplay_local_addresses[existing].address,
                                candidate.address) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                m->netplay_local_addresses[m->netplay_local_address_count++] = candidate;
            }
        }
    }

    // Older hosts expose one preferred address through local_ip only.
    if (m->netplay_local_address_count == 0 && np->local_ip) {
        RecompLauncherCNetplayLocalAddress candidate{};
        if (np->local_ip(np->ctx, candidate.address, sizeof(candidate.address)) &&
            candidate.address[0]) {
            candidate.address[sizeof(candidate.address) - 1] = '\0';
            std::snprintf(candidate.label, sizeof(candidate.label), "Local network");
            m->netplay_local_addresses[m->netplay_local_address_count++] = candidate;
        }
    }

    if (m->netplay_local_address_count > 0) {
        int selected = 0;
        const char* preferred = m->netplay_host_local_ip[0]
            ? m->netplay_host_local_ip : m->netplay_host_ip;
        for (int index = 0; index < m->netplay_local_address_count; ++index) {
            if (std::strcmp(preferred, m->netplay_local_addresses[index].address) == 0) {
                selected = index;
                break;
            }
        }
        std::snprintf(m->netplay_host_ip, sizeof(m->netplay_host_ip), "%s",
                      m->netplay_local_addresses[selected].address);
        std::snprintf(m->netplay_host_local_ip, sizeof(m->netplay_host_local_ip), "%s",
                      m->netplay_local_addresses[selected].address);
        return;
    }

    std::snprintf(m->netplay_host_ip, sizeof(m->netplay_host_ip), "Unavailable");
}

void np_format_local_address(const RecompLauncherCNetplayLocalAddress& address,
                             char* out, size_t out_len) {
    if (address.label[0])
        std::snprintf(out, out_len, "%s (%s)", address.label, address.address);
    else
        std::snprintf(out, out_len, "%s", address.address);
}

/* Matches snes_lobby_default_url() when SNES_NET_LOBBY_URL is unset. */
static const char kNpDefaultLobbyUrl[] =
    "ws://netplay.retcomm.net:8765";
/* Persisted next to guest netplay saves (cwd-relative). */
static const char kNpNetworkSettingsPath[] = "saves/netplay/network settings";

static void np_ensure_netplay_dir(void) {
#if defined(_WIN32)
    _mkdir("saves");
    _mkdir("saves\\netplay");
#else
    mkdir("saves", 0755);
    mkdir("saves/netplay", 0755);
#endif
}

/* Read-only + greyed, but still allows click-drag select / Ctrl+C. */
static void np_copyable_readonly_input(const char* id, char* buf, size_t buf_len,
                                       const LauncherTheme& th) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                          ImVec4(th.control.r * 0.65f, th.control.g * 0.65f,
                                 th.control.b * 0.65f, th.control.a));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                          ImVec4(th.control.r * 0.65f, th.control.g * 0.65f,
                                 th.control.b * 0.65f, th.control.a));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                          ImVec4(th.control.r * 0.65f, th.control.g * 0.65f,
                                 th.control.b * 0.65f, th.control.a));
    ImGui::InputText(id, buf, buf_len, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(4);
}

static void np_save_network_settings(const LauncherModel* m) {
    if (!m) return;
    np_ensure_netplay_dir();
    FILE* f = std::fopen(kNpNetworkSettingsPath, "wb");
    if (!f) return;
    std::fprintf(f, "lobby_url=%s\n", m->netplay_lobby_url);
    std::fprintf(f, "preferred_ip=%s\n",
                 m->netplay_host_local_ip[0] ? m->netplay_host_local_ip
                                             : m->netplay_host_ip);
    std::fprintf(f, "preferred_port=%s\n", m->netplay_host_port);
    std::fclose(f);
}

static void np_load_network_settings(LauncherModel* m) {
    if (!m) return;
    FILE* f = std::fopen(kNpNetworkSettingsPath, "rb");
    if (!f) return;
    char line[320];
    while (std::fgets(line, sizeof(line), f)) {
        char* nl = std::strchr(line, '\n');
        if (nl) *nl = '\0';
        char* cr = std::strchr(line, '\r');
        if (cr) *cr = '\0';
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if (std::strcmp(key, "lobby_url") == 0 && val[0]) {
            std::snprintf(m->netplay_lobby_url, sizeof(m->netplay_lobby_url), "%s",
                          val);
        } else if (std::strcmp(key, "preferred_ip") == 0 && val[0]) {
            std::snprintf(m->netplay_host_ip, sizeof(m->netplay_host_ip), "%s",
                          val);
            std::snprintf(m->netplay_host_local_ip,
                          sizeof(m->netplay_host_local_ip), "%s", val);
        } else if (std::strcmp(key, "preferred_port") == 0 && val[0]) {
            std::snprintf(m->netplay_host_port, sizeof(m->netplay_host_port),
                          "%s", val);
        }
        /* Legacy force_turn= lines are ignored — Lobby Settings owns relay. */
    }
    std::fclose(f);
    const auto* np = np_cb(m);
    if (np && np->set_lobby_url && m->netplay_lobby_url[0])
        np->set_lobby_url(np->ctx, m->netplay_lobby_url);
}

static void np_ensure_public_ip(LauncherModel* m) {
    if (!m || m->netplay_public_ip_resolved) return;
    const auto* np = np_cb(m);
    m->netplay_public_ip_resolved = true;
    if (np && np->external_ip &&
        np->external_ip(np->ctx, m->netplay_public_ip,
                        sizeof(m->netplay_public_ip)) &&
        m->netplay_public_ip[0]) {
        return;
    }
    std::snprintf(m->netplay_public_ip, sizeof(m->netplay_public_ip),
                  "Unavailable");
}

/* A name the word list refuses is not masked and not silently dropped: the
 * player is told, and asked for another one. The check belongs to the
 * backend (it owns recomp-net's list; duplicating it here would be a second
 * copy that cannot inherit fixes to the first), so this is a callback, and
 * it is only the courtesy half -- the lobby server refuses the name too, and
 * that refusal comes back through last_error into the same prompt. */
static bool np_name_refused(LauncherModel* m, const char* name) {
    const auto* np = np_cb(m);
    if (!np || !np->name_rejected || !name || !name[0]) return false;
    return np->name_rejected(np->ctx, name) != 0;
}

/* Signing in is optional, and this draws the whole of it.
 *
 * A build with no account callbacks, a lobby server whose operator never set
 * Discord up, and a player who simply does not sign in are all ordinary cases:
 * the section is not drawn, the name field below is the locally-typed player
 * name, and everything behaves as it did before Discord existed. Guest is not
 * a lesser state here -- it is the original one.
 *
 * Returns true when the player is signed in, which is what decides whether the
 * name field edits a LOCAL name or the server-owned handle. */
static bool draw_account_section(LauncherModel* m, const LauncherTheme& th) {
    const auto* np = np_cb(m);
    if (!np || !np->account_state || !np->account_available) return false;
    /* The server answers 503 to a login start when its operator has not
     * configured Discord. Offering a button that cannot work is worse than
     * offering none. */
    if (!np->account_available(np->ctx)) return false;

    const int st = np->account_state(np->ctx);
    const bool signed_in = st == RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN;

    if (signed_in) {
        const char* handle = np->account_handle ? np->account_handle(np->ctx) : "";
        const char* uname = np->account_username ? np->account_username(np->ctx) : "";
        char disp[128];
        emoji_display(handle && handle[0] ? handle : "Signed in", disp, sizeof(disp));
        ImGui::TextColored(col(th.text_muted), "Signed in as");
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(col(th.good), "%s", disp);
        if (uname && uname[0]) {
            /* Discord display names are NOT unique, so the @handle is what
             * tells two players with the same name apart. */
            ImGui::SameLine(0, px(6));
            ImGui::TextColored(col(th.text_muted), "@%s", uname);
        }
        if (ImGui::Button("Sign out", ImVec2(px(120), 0)) && np->account_sign_out) {
            np->account_sign_out(np->ctx);
            m->netplay_name_error[0] = '\0';
        }
    } else if (st == RECOMP_LAUNCHER_ACCOUNT_WAITING) {
        ImGui::TextColored(col(th.accent2), "Waiting for Discord…");
        ImGui::PushTextWrapPos(px(360));
        ImGui::TextColored(col(th.text_muted),
                           "Finish signing in on the page that opened in your "
                           "browser, then come back here.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(px(360));
        ImGui::TextColored(col(th.text_muted),
                           "Sign in with Discord so your name is yours across "
                           "sessions. Optional — you can play as a guest.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Sign in with Discord", ImVec2(px(200), 0)) &&
            np->account_login_begin) {
            if (np->account_login_begin(np->ctx) != 0) {
                const char* e = np->account_error ? np->account_error(np->ctx) : nullptr;
                std::snprintf(m->netplay_name_error, sizeof(m->netplay_name_error),
                              "%s", e && e[0] ? e : "Could not start the sign-in.");
            } else {
                m->netplay_name_error[0] = '\0';
            }
        }
        if (st == RECOMP_LAUNCHER_ACCOUNT_FAILED && np->account_error) {
            const char* e = np->account_error(np->ctx);
            if (e && e[0]) {
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextColored(col(th.warn), "%s", e);
                ImGui::PopTextWrapPos();
            }
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    return signed_in;
}

/* Defined with the other account helpers, below; used from here on. */
static const char* np_effective_name(LauncherModel* m);
static void np_open_name_modal(LauncherModel* m);

void draw_netplay_player_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->netplay_name_modal_open) ImGui::OpenPopup("Player Name");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Player Name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool signed_in = draw_account_section(m, th);
        /* Signed in, the field edits the handle the SERVER owns; as a guest it
         * edits the local player name. The box is seeded by whoever opened
         * this (np_open_name_modal), so it always starts on the right one. */
        ImGui::TextColored(col(th.text_muted),
                           signed_in ? "Display name (other players see this)"
                                     : "Player name");
        ImGui::SetNextItemWidth(px(320));
        bool save = ImGui::InputText("##player_name", m->netplay_name_edit,
                                     sizeof(m->netplay_name_edit),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        /* Editing is the retry: the complaint goes away the moment the player
         * starts typing a different name, rather than sitting under a field
         * that no longer says what it is complaining about. */
        if (ImGui::IsItemEdited()) m->netplay_name_error[0] = '\0';
        if (m->netplay_name_error[0]) {
            ImGui::PushTextWrapPos(px(320));
            ImGui::TextColored(col(th.warn), "%s", m->netplay_name_error);
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            /* "Do they still have no name?" must ask the EFFECTIVE name. A
             * signed-in player's name is the account handle; s.netplay_player_name
             * is only the guest/LAN fallback and is routinely empty for them. Asking
             * the fallback made Cancel read as "no name yet, netplay is not usable"
             * and bounce them to the dashboard — losing the lobby page behind the
             * modal — even though they were signed in as someone. */
            const char* effective = np_effective_name(m);
            const bool required_name = !effective || effective[0] == '\0';
            std::snprintf(m->netplay_name_edit, sizeof(m->netplay_name_edit), "%s",
                          effective ? effective : "");
            m->netplay_name_modal_open = false;
            m->netplay_name_error[0] = '\0';
            if (required_name) {
                m->netplay_name_prompted = false;
                launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const bool valid_name = m->netplay_name_edit[0] != '\0';
        ImGui::BeginDisabled(!valid_name);
        if ((ImGui::Button("Save", ImVec2(px(120), 0)) || save) && valid_name) {
            /* Refused: keep the modal open, say so, and keep the old name --
             * the player picks another. Nothing is sent, so a refused name
             * never reaches a peer even for the frame it took to notice. */
            if (np_name_refused(m, m->netplay_name_edit)) {
                std::snprintf(m->netplay_name_error, sizeof(m->netplay_name_error),
                              "That name can't be used. Please pick another one.");
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return;
            }
            if (signed_in) {
                /* The server owns an account's name, so this is a request, not
                 * a local write: it can be refused for the same reason a typed
                 * name is, and the player picks another. */
                const auto* npa = np_cb(m);
                if (npa && npa->account_set_handle &&
                    npa->account_set_handle(npa->ctx, m->netplay_name_edit) != 0) {
                    std::snprintf(m->netplay_name_error, sizeof(m->netplay_name_error),
                                  "That name can't be used. Please pick another one.");
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                    return;
                }
                /* Tell the lobby too. account_set_handle only changes what the
                 * ACCOUNT server holds; the players-online list is lobby presence,
                 * which keeps the name from the first hello until something
                 * re-announces it. snes_lobby_set_display_name re-queues hello
                 * exactly for this, but nothing on this path ever called it, so a
                 * signed-in rename showed the new name in this dialog and the old
                 * one in the list beside it until reconnect. The guest branch below
                 * has always done this; only the signed-in early-return skipped it. */
                if (npa && npa->set_player_name)
                    npa->set_player_name(npa->ctx, m->netplay_name_edit);
                m->netplay_name_modal_open = false;
                m->netplay_name_error[0] = '\0';
                ImGui::CloseCurrentPopup();
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return;
            }
            char previous_default[96];
            std::snprintf(previous_default, sizeof(previous_default), "%s's Lobby",
                          m->s.netplay_player_name);
            const bool update_lobby_name = !m->netplay_host_name[0] ||
                std::strcmp(m->netplay_host_name, previous_default) == 0;
            std::snprintf(m->s.netplay_player_name, sizeof(m->s.netplay_player_name), "%s",
                          m->netplay_name_edit);
            if (update_lobby_name)
                std::snprintf(m->netplay_host_name, sizeof(m->netplay_host_name),
                              "%s's Lobby", np_effective_name(m));
            const auto* np = np_cb(m);
            if (np && np->set_player_name) np->set_player_name(np->ctx, m->s.netplay_player_name);
            m->netplay_name_modal_open = false;
            m->netplay_name_error[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

/* Delay-sync only (no rollback / prediction runway). At ~60 Hz, one frame is
 * ~16.67 ms, so one-way RTT coverage is ceil(RTT_ms / 33.33) frames. Add a
 * jitter/TURN pad — Battleship's lower tiers assume prediction absorbs the
 * rest of the path and are too aggressive here. */
static int np_delay_frames_from_rtt_ms(int rtt_ms) {
    if (rtt_ms < 0) rtt_ms = 0;
    /* ceil(rtt / 33.333) via integer ceil: (rtt + 32) / 33, floor at 1. */
    int one_way_frames = (rtt_ms + 32) / 33;
    if (one_way_frames < 1) one_way_frames = 1;
    const int kJitterPad = 3; /* ICE/TURN variance + scheduling slack */
    int delay = one_way_frames + kJitterPad;
    if (delay < 3) delay = 3;   /* delay-only floor (above Battleship's D=2) */
    if (delay > 20) delay = 20;
    return delay;
}

/* Rollback D from measured lobby RTT (§59: WAN-aware tiers).
 *
 * Pre-§59 table was BattleShip-feel aggressive (50–80 ms → D=3). MotK §56/§57
 * soaks showed a TURN WAN link with lobby RTT in that band still needs D=5–6
 * once transit+jitter are counted (lead ≈ D−1−transit), and the session spent
 * its first minute invent-storming until arrival-driven auto-delay caught up.
 * Tiers are +1..+2 vs the old table; floor 3. Callers apply an extra TURN
 * floor (see Play) because lobby UDP RTT underestimates the game path.
 *
 * §60: +1 on every tier at RTT ≥ 80 so non-TURN WAN matches start closer to
 * arrival-driven D instead of invent-grace hitching for the first ~5s eval. */
static int np_rb_delay_frames_from_rtt_ms(int rtt_ms) {
    if (rtt_ms < 0) rtt_ms = 0;
    int d;
    if (rtt_ms < 20) d = 3;
    else if (rtt_ms < 50) d = 3;
    else if (rtt_ms < 80) d = 4;
    else if (rtt_ms < 120) d = 6;
    else if (rtt_ms < 160) d = 7;
    else if (rtt_ms < 200) d = 8;
    else if (rtt_ms < 260) d = 9;
    else d = 10;
    if (d < 3) d = 3;
    if (d > 12) d = 12;
    return d;
}

/* Invent runway: P = 4 + D (deterministic; matches the MotK RTT table). */
static int np_rb_prediction_frames_from_rtt_ms(int rtt_ms, int delay_frames) {
    (void)rtt_ms;
    if (delay_frames < 2) delay_frames = 2;
    int p = 4 + delay_frames;
    if (p < 6) p = 6;
    if (p > 16) p = 16;
    return p;
}

static int np_lobby_max_peer_rtt_ms(LauncherModel* m,
                                    const RecompLauncherCNetplayCallbacks* np) {
    int max_rtt = 0;
    if (!np || !np->member_count || !np->member_get) return 0;
    const int nmem = np->member_count(np->ctx);
    for (int mi = 0; mi < nmem; ++mi) {
        RecompLauncherCNetplayMember mem{};
        if (!np->member_get(np->ctx, mi, &mem)) continue;
        if (mem.is_local) continue;
        if (mem.latency_ms > max_rtt) max_rtt = mem.latency_ms;
    }
    (void)m;
    return max_rtt;
}

static int np_game_max_players(const LauncherModel* m) {
    int n = (m && m->player_count > 0) ? m->player_count : 2;
    if (n < 2) n = 2;
    if (n > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
        n = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
    return n;
}

static int np_clamp_host_max_players(LauncherModel* m) {
    const int game_max = np_game_max_players(m);
    /* Lobby/delay-sync ceiling: RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS / RNET_MAX_SLOTS. */
    const int sync_max = game_max < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS
                             ? game_max : RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
    int n = m->netplay_host_max_players;
    if (n < 2) n = 2;
    if (n > sync_max) n = sync_max;
    m->netplay_host_max_players = n;
    return n;
}

/* Join the queue. A refusal lands in netplay_status the way every other
 * lobby op's does -- the server's own line when it gave one (need_account,
 * cooldown, mods_not_pooled), because it says more than "failed" can. */
static void np_automatch_queue(LauncherModel* m, const char* ruleset_id) {
    const auto* np = np_cb(m);
    if (!np || !np->automatch_queue) return;
    if (np->automatch_queue(np->ctx, ruleset_id ? ruleset_id : "") == 0) {
        m->netplay_status[0] = '\0';
        return;
    }
    const char* why = np->automatch_error ? np->automatch_error(np->ctx) : nullptr;
    std::snprintf(m->netplay_status, sizeof(m->netplay_status), "%s",
                  why && why[0] ? why : "Could not join the automatch queue.");
}

static int np_automatch_state(const RecompLauncherCNetplayCallbacks* np) {
    return (np && np->automatch_state) ? np->automatch_state(np->ctx)
                                       : RECOMP_LAUNCHER_AUTOMATCH_IDLE;
}

/* The accept gate, and the queue-type picker when a server offers more than
 * one. Both live here rather than in the footer so they draw at root and are
 * not clipped by the page's child windows. */
void draw_netplay_automatch_modal(LauncherModel* m, const LauncherTheme& th) {
    const auto* np = np_cb(m);
    if (!np) return;

    /* ---- queue-type picker ---------------------------------------------- */
    if (m->netplay_automatch_picker_open) ImGui::OpenPopup("Automatch");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Automatch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Pick a queue. The server owns these settings for the whole match "
            "-- both players agreed to them by queueing here, so neither side "
            "can change them once a pair is found.");
        ImGui::Spacing();
        const int n = np->automatch_ruleset_count ? np->automatch_ruleset_count(np->ctx) : 0;
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayRuleset r;
            std::memset(&r, 0, sizeof(r));
            if (!np->automatch_ruleset_get || !np->automatch_ruleset_get(np->ctx, i, &r))
                continue;
            ImGui::PushID(i);
            if (ImGui::Button(r.label[0] ? r.label : r.id, ImVec2(px(280), px(34)))) {
                np_automatch_queue(m, r.id);
                m->netplay_automatch_picker_open = false;
                ImGui::CloseCurrentPopup();
            }
            if (r.caps_summary[0])
                ImGui::TextColored(col(th.text_muted), "%s", r.caps_summary);
            if (r.game_version[0])
                ImGui::TextColored(col(th.text_muted), "Release %s", r.game_version);
            ImGui::PopID();
            ImGui::Spacing();
        }
        if (n == 0)
            ImGui::TextColored(col(th.warn), "This server has no automatch queues.");
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            m->netplay_automatch_picker_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ---- accept gate ----------------------------------------------------
     * Opened and closed by automatch_state, not by a click: a peer that
     * declines, or a deadline that lapses, has to take this down on its own.
     * The flag only stops OpenPopup being called every frame. */
    const int st = np_automatch_state(np);
    const bool gate = st == RECOMP_LAUNCHER_AUTOMATCH_FOUND ||
                      st == RECOMP_LAUNCHER_AUTOMATCH_ACCEPTED;
    if (gate && !m->netplay_automatch_gate_open) {
        m->netplay_automatch_gate_open = true;
        ImGui::OpenPopup("Match found");
    }
    if (!gate) m->netplay_automatch_gate_open = false;

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Match found", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!gate) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        RecompLauncherCNetplayFound f;
        std::memset(&f, 0, sizeof(f));
        const bool have = np->automatch_found_get &&
                          np->automatch_found_get(np->ctx, &f);

        if (have) {
            np_draw_country_flag(th, f.country);
            ImGui::TextUnformatted(f.handle[0] ? f.handle : "Opponent");
            /* Discord display names are not unique. The @handle is the
             * disambiguator, which is the whole reason it is carried. */
            if (f.username[0])
                ImGui::TextColored(col(th.text_muted), "@%s", f.username);
            ImGui::Spacing();
            if (f.ruleset_label[0])
                ImGui::TextColored(col(th.text_muted), "Queue: %s", f.ruleset_label);
            /* An estimate, and labelled as one: it is each peer's round trip
             * to the relay added together, not a measured peer-to-peer ping. */
            if (f.est_rtt_ms >= 0)
                ImGui::TextColored(col(th.text_muted), "Estimated latency: ~%d ms",
                                   f.est_rtt_ms);
            else
                ImGui::TextColored(col(th.text_muted), "Estimated latency: unknown");
        } else {
            ImGui::TextUnformatted("An opponent is ready.");
        }
        ImGui::Spacing();

        if (st == RECOMP_LAUNCHER_AUTOMATCH_ACCEPTED) {
            ImGui::TextColored(col(th.accent2), "Waiting for %s to accept...",
                               have && f.handle[0] ? f.handle : "the other player");
            ImGui::Spacing();
            ImGui::TextColored(col(th.text_muted),
                               "If they decline you go back to the front of the "
                               "queue, not the back.");
        } else {
            /* "Accepting in 15s" said the opposite of what happens. Letting
             * this lapse DECLINES -- and takes the same cooldown strike a
             * click on Decline does -- so a player who read it as "it will
             * accept for me" and walked away was told the machine would do
             * the one thing it will not. Say which way it falls. */
            if (have)
                ImGui::TextColored(f.accept_secs_left <= 5 ? col(th.warn) : col(th.text),
                                   "Auto-declines in %ds", f.accept_secs_left);
            ImGui::Spacing();
            if (ImGui::Button("Accept", ImVec2(px(140), px(34)))) {
                if (np->automatch_accept) np->automatch_accept(np->ctx, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Decline", ImVec2(px(140), px(34)))) {
                if (np->automatch_accept) np->automatch_accept(np->ctx, 0);
            }
            /* Say the cost before it is paid. Declining is allowed; being
             * surprised by the cooldown afterwards is what is not. */
            ImGui::TextColored(col(th.text_muted),
                               "Declining or letting this lapse puts you on a "
                               "short queue cooldown.");
        }
        ImGui::EndPopup();
    }
}

/*
 * The review list. Without it the only way to undo a block is editing a file
 * by hand, which is not an undo -- and a moderation feature whose effects are
 * invisible is one you stop trusting.
 */
void draw_netplay_moderation_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->netplay_moderation_modal_open) ImGui::OpenPopup("Ignored & Blocked");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Ignored & Blocked", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const int n = recomp_moderation_count();
    if (n <= 0) {
        ImGui::TextColored(col(th.text_muted),
                           "Nobody is ignored or blocked.");
        ImGui::TextColored(col(th.text_muted),
                           "Right-click a player's name to add one.");
    } else {
        ImGui::TextColored(col(th.text_muted),
                           "Names are only labels -- the list follows the "
                           "account, so renaming does not escape it.");
        ImGui::Spacing();
        if (ImGui::BeginTable("##mod_list", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, px(90));
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(90));
            for (int i = 0; i < n; ++i) {
                char key[RECOMP_MOD_KEY_CAP], name[RECOMP_MOD_NAME_CAP];
                RecompModLevel lvl = RECOMP_MOD_NONE;
                if (!recomp_moderation_get(i, key, sizeof(key), name, sizeof(name), &lvl))
                    continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char disp[96];
                emoji_display(name[0] ? name : "(unknown)", disp, sizeof(disp));
                ImGui::TextUnformatted(disp);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(col(lvl == RECOMP_MOD_BLOCKED ? th.warn : th.text_muted),
                                   "%s", lvl == RECOMP_MOD_BLOCKED ? "Blocked" : "Ignored");
                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("Remove", ImVec2(px(84), 0))) {
                    recomp_moderation_set(key, name, RECOMP_MOD_NONE);
                    ImGui::PopID();
                    break;   /* the list shifted under us */
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(px(120), 0))) {
        m->netplay_moderation_modal_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* The report dialog. A category and an optional sentence -- and NOT the text
 * of the line, which the server already has and is the only copy that can be
 * trusted (docs/MODERATION.md, "The one design decision that matters"). */
void draw_netplay_report_modal(LauncherModel* m, const LauncherTheme& th) {
    static const struct { const char* id; const char* label; } kReasons[] = {
        { RECOMP_LAUNCHER_REPORT_HARASSMENT,     "Harassment" },
        { RECOMP_LAUNCHER_REPORT_HATE_SPEECH,    "Hate speech" },
        { RECOMP_LAUNCHER_REPORT_THREATS,        "Threats" },
        { RECOMP_LAUNCHER_REPORT_SEXUAL_CONTENT, "Sexual content" },
        { RECOMP_LAUNCHER_REPORT_SPAM,           "Spam" },
        { RECOMP_LAUNCHER_REPORT_CHEATING,       "Cheating claim" },
        { RECOMP_LAUNCHER_REPORT_OTHER,          "Something else" },
    };
    const int kReasonCount = (int)(sizeof(kReasons) / sizeof(kReasons[0]));

    if (m->netplay_report_modal_open) ImGui::OpenPopup("Report Message");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Report Message", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    char who[96];
    emoji_display(m->netplay_report_who, who, sizeof(who));
    ImGui::TextColored(col(th.text_muted), "Reporting a message from");
    ImGui::SameLine(0, px(5));
    ImGui::TextUnformatted(who);
    ImGui::Spacing();

    if (m->netplay_report_reason < 0 || m->netplay_report_reason >= kReasonCount)
        m->netplay_report_reason = 0;
    ImGui::TextColored(col(th.text_muted), "Reason");
    ImGui::SetNextItemWidth(px(260));
    if (ImGui::BeginCombo("##report_reason",
                          kReasons[m->netplay_report_reason].label)) {
        for (int i = 0; i < kReasonCount; ++i)
            if (ImGui::Selectable(kReasons[i].label,
                                  i == m->netplay_report_reason))
                m->netplay_report_reason = i;
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::TextColored(col(th.text_muted), "Anything to add (optional)");
    ImGui::SetNextItemWidth(px(460));
    ImGui::InputTextWithHint("##report_note",
                             "What happened, in a sentence",
                             m->netplay_report_note,
                             sizeof(m->netplay_report_note));

    ImGui::Spacing();
    /* Said plainly, because a reporter who thinks they are attaching their own
     * copy of the words will word the note as though the message were not
     * already in the record. */
    ImGui::PushTextWrapPos(px(460));
    ImGui::TextColored(col(th.text_muted),
                       "The server already has the message and records its own "
                       "copy, along with a little of the conversation around "
                       "it. Nothing is sent from here except the reason and "
                       "your note.");
    ImGui::PopTextWrapPos();

    if (m->netplay_report_status[0]) {
        ImGui::Spacing();
        ImGui::TextColored(col(th.warn), "%s", m->netplay_report_status);
    }

    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
        m->netplay_report_modal_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const auto* np = np_cb(m);
    const bool can_send = np && np->chat_report && m->netplay_report_mid[0];
    ImGui::BeginDisabled(!can_send);
    if (ImGui::Button("Send Report", ImVec2(px(150), 0))) {
        const char* mids[1] = { m->netplay_report_mid };
        const int rc = np->chat_report(np->ctx, mids, 1,
                                       kReasons[m->netplay_report_reason].id,
                                       m->netplay_report_note);
        if (rc == 0) {
            /* Handed over, which is not the same as accepted -- the server
             * still refuses an expired line, a guest's line, your own, and
             * anything over the rate limit. Saying "sent" rather than
             * "received" is the honest half of that. */
            m->netplay_report_modal_open = false;
            ImGui::CloseCurrentPopup();
            std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                          "Report sent to the moderation queue.");
        } else {
            std::snprintf(m->netplay_report_status,
                          sizeof(m->netplay_report_status),
                          "Could not send the report. Are you still connected?");
        }
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void draw_netplay_direct_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->netplay_direct_modal_open) ImGui::OpenPopup("Join Direct");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Join Direct", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Join a LAN/Direct IP lobby by IP (or pick a LAN row from the "
            "lobby list after Refresh — hosts announce via UDP broadcast). "
            "The host must create with LAN/Direct IP Only checked and keep "
            "the waiting room open. Use their LAN IP on the same network, or "
            "their Public IP with UDP port-forwarded to the host PC. Online "
            "(MotK) lobbies: join from the server list instead.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(px(280));
        ImGui::InputText("Host IP", m->netplay_direct_ip, sizeof(m->netplay_direct_ip));
        ImGui::SetNextItemWidth(px(160));
        ImGui::InputText("Port", m->netplay_direct_port, sizeof(m->netplay_direct_port),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SetNextItemWidth(px(280));
        ImGui::InputText("Password (optional)", m->netplay_password,
                         sizeof(m->netplay_password),
                         ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        if (m->netplay_status[0])
            ImGui::TextColored(col(th.warn), "%s", m->netplay_status);
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            m->netplay_direct_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const bool can_join = m->netplay_direct_ip[0] && np_valid_port(m->netplay_direct_port);
        ImGui::BeginDisabled(!can_join);
        if (ImGui::Button("Join Direct", ImVec2(px(140), 0))) {
            const auto* np = np_cb(m);
            if (np && np->join) {
                char lobby_id[96];
                char guest_bind[64];
                std::snprintf(lobby_id, sizeof(lobby_id), "lan:%s:%s",
                              m->netplay_direct_ip[0] ? m->netplay_direct_ip : "127.0.0.1",
                              m->netplay_direct_port[0] ? m->netplay_direct_port : "7777");
                if (np_prepare_guest_bind(guest_bind, sizeof(guest_bind),
                                          m->netplay_status, sizeof(m->netplay_status))) {
                const int rc = np->join(np->ctx, lobby_id, m->netplay_password, guest_bind);
                if (rc == 0) {
                    m->netplay_local_room = true;
                    std::snprintf(m->netplay_host_endpoint, sizeof(m->netplay_host_endpoint),
                                  "%s", lobby_id + 4);
                    m->netplay_lobby_max_slots = np_game_max_players(m);
                    m->netplay_status[0] = '\0';
                    m->netplay_direct_modal_open = false;
                    ImGui::CloseCurrentPopup();
                } else if (rc == -2) {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "Incorrect password.");
                } else if (rc == -3) {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "No response from that IP:port. Check the host is in "
                                  "the LAN waiting room, UDP is forwarded, and the "
                                  "firewall allows the game port.");
                } else {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "Could not join (lobby full, already started, or "
                                  "game/version mismatch).");
                }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void draw_netplay_network_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->netplay_network_modal_open) ImGui::OpenPopup("Network Settings");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Network Settings", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(col(th.text_muted), "Lobby server URL");
        ImGui::SetNextItemWidth(px(440));
        bool save = ImGui::InputText("##lobby_server_url", m->netplay_lobby_url,
                                     sizeof(m->netplay_lobby_url),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            const auto* np = np_cb(m);
            const char* current = np && np->default_url ? np->default_url(np->ctx) : "";
            std::snprintf(m->netplay_lobby_url, sizeof(m->netplay_lobby_url), "%s",
                          current ? current : "");
            m->netplay_network_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const bool valid_url = m->netplay_lobby_url[0] != '\0';
        ImGui::BeginDisabled(!valid_url);
        if ((ImGui::Button("Save", ImVec2(px(120), 0)) || save) && valid_url) {
            const auto* np = np_cb(m);
            if (np && np->set_lobby_url)
                np->set_lobby_url(np->ctx, m->netplay_lobby_url);
            np_save_network_settings(m);
            np_connect_and_list(m);
            m->netplay_network_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void draw_netplay_host_modal(LauncherModel* m, const LauncherTheme& th) {
    /* Create errors stay in this modal — not m->netplay_status (list banner). */
    static char host_create_status[160] = "";
    if (m->netplay_host_modal_open) ImGui::OpenPopup("Host Lobby");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Host Lobby", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(col(th.text_muted), "Lobby name");
        ImGui::SetNextItemWidth(px(430));
        ImGui::InputText("##host_lobby_name", m->netplay_host_name,
                         sizeof(m->netplay_host_name));
        /* Editing is the retry, as in the Player Name modal. */
        if (ImGui::IsItemEdited()) m->netplay_host_name_error[0] = '\0';
        if (m->netplay_host_name_error[0]) {
            ImGui::PushTextWrapPos(px(430));
            ImGui::TextColored(col(th.warn), "%s", m->netplay_host_name_error);
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        const RecompLauncherCNetplayCallbacks* np_cb_host = np_cb(m);
        const bool link_supported =
            np_cb_host && np_cb_host->link_lobby_supported &&
            np_cb_host->link_lobby_supported(np_cb_host->ctx);
        int lobby_kind = (link_supported && np_cb_host->lobby_kind_get)
                             ? np_cb_host->lobby_kind_get(np_cb_host->ctx)
                             : 0;
        if (link_supported) {
            ImGui::TextColored(col(th.text_muted), "Lobby Type");
            ImGui::SetNextItemWidth(px(220));
            /* A link-capable title has no multitap mode: one console seats
             * 2 players, four players means two linked consoles. So the type
             * IS the seat count — offering a 4-seat "Standard" room would put
             * P3/P4 on console A with a multitap the game cannot use. */
            const char* kind_label = lobby_kind == 1
                                         ? "PSX-Link — 2 consoles, 4 players"
                                         : "Standard — 1 console, 2 players";
            if (ImGui::BeginCombo("##host_lobby_kind", kind_label)) {
                if (ImGui::Selectable("Standard — 1 console, 2 players",
                                      lobby_kind == 0) &&
                    np_cb_host->lobby_kind_set)
                    np_cb_host->lobby_kind_set(np_cb_host->ctx, 0);
                if (ImGui::Selectable("PSX-Link — 2 consoles, 4 players",
                                      lobby_kind == 1) &&
                    np_cb_host->lobby_kind_set)
                    np_cb_host->lobby_kind_set(np_cb_host->ctx, 1);
                ImGui::EndCombo();
                lobby_kind = np_cb_host->lobby_kind_get
                                 ? np_cb_host->lobby_kind_get(np_cb_host->ctx)
                                 : lobby_kind;
            }
            if (lobby_kind == 1) {
                ImGui::SameLine();
                ImGui::TextColored(col(th.text_muted),
                                   "P1/P2 on console A, P3/P4 on console B");
                m->netplay_host_max_players = 4;
            } else {
                m->netplay_host_max_players = 2;
            }
            ImGui::Spacing();
        }
        {
            const int game_max = np_game_max_players(m);
            const int sync_max = game_max < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS
                                     ? game_max : RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
            const bool game_locked = sync_max <= 2 || link_supported;
            const int max_players = np_clamp_host_max_players(m);
            ImGui::TextColored(col(th.text_muted), "Max Players");
            ImGui::SetNextItemWidth(px(140));
            ImGui::BeginDisabled(game_locked);
            char preview[8];
            std::snprintf(preview, sizeof(preview), "%d", max_players);
            if (ImGui::BeginCombo("##host_max_players", preview)) {
                for (int n = 2; n <= sync_max; ++n) {
                    char label[8];
                    std::snprintf(label, sizeof(label), "%d", n);
                    const bool selected = n == max_players;
                    if (ImGui::Selectable(label, selected))
                        m->netplay_host_max_players = n;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (game_locked) {
                ImGui::SameLine();
                ImGui::TextColored(col(th.text_muted),
                                   link_supported
                                       ? (lobby_kind == 1
                                              ? "(set by lobby type)"
                                              : "(one console seats 2)")
                                       : "(this game is 2-player)");
            }
        }
        /* Spectators.
         *
         * Only offered where it can actually be enforced. A LAN / Direct-IP
         * room has no lobby server between the peers, so there is nothing to
         * drop a spectator's packets at -- and a spectator that is merely
         * asked not to send is not a spectator, it is a promise. */
        if (np_cb_host && np_cb_host->allow_spectators_set &&
            np_cb_host->allow_spectators_get && !m->netplay_lan_only) {
            ImGui::Spacing();
            bool allow_spec =
                np_cb_host->allow_spectators_get(np_cb_host->ctx) != 0;
            if (ImGui::Checkbox("Allow Spectators", &allow_spec))
                (void)np_cb_host->allow_spectators_set(np_cb_host->ctx,
                                                       allow_spec ? 1 : 0);
            ImGui::SameLine();
            ImGui::TextColored(col(th.text_muted),
                               "up to %d, on top of the players",
                               RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS);
        }
        ImGui::Spacing();
        /* No LAN/Direct-IP checkbox. The player already answered this question
         * on the way in -- the mode page is literally "LAN / Direct IP" or
         * "Online Netplay" -- and asking again inside the dialog offered them
         * a third answer that contradicts the first. It defaulted to OFF in
         * both modes, so hosting from the LAN page published an ONLINE room
         * nobody on the LAN could see. open_host() sets the flag from the mode
         * now, and this dialog only shows what follows from it.
         *
         * Spectators disappear with it in LAN mode, by the gate above: a
         * peer-to-peer room has no server between the peers to drop a
         * spectator's packets at, so the control was never honest there. */
        /* Advertised IP/Port: which NIC + port peers should use for LAN RTT /
         * Direct IP. Online still STUNs for a public endpoint; this pick is the
         * preferred LAN advertise / bind address. */
        if (ImGui::BeginTable("##host_lan_conn", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("ip", ImGuiTableColumnFlags_WidthFixed, px(300));
            ImGui::TableSetupColumn("port", ImGuiTableColumnFlags_WidthFixed, px(120));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(col(th.text_muted), "Advertised IP Address");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(col(th.text_muted), "Port");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1.0f);
            if (m->netplay_local_address_count > 1) {
                int selected = 0;
                for (int index = 0; index < m->netplay_local_address_count; ++index) {
                    if (std::strcmp(m->netplay_host_ip,
                                    m->netplay_local_addresses[index].address) == 0) {
                        selected = index;
                        break;
                    }
                }
                char preview[144];
                np_format_local_address(m->netplay_local_addresses[selected],
                                        preview, sizeof(preview));
                if (ImGui::BeginCombo("##host_ip", preview)) {
                    for (int index = 0; index < m->netplay_local_address_count; ++index) {
                        char choice[144];
                        np_format_local_address(m->netplay_local_addresses[index],
                                                choice, sizeof(choice));
                        const bool is_selected = index == selected;
                        if (ImGui::Selectable(choice, is_selected)) {
                            std::snprintf(m->netplay_host_ip, sizeof(m->netplay_host_ip),
                                          "%s", m->netplay_local_addresses[index].address);
                            std::snprintf(m->netplay_host_local_ip,
                                          sizeof(m->netplay_host_local_ip), "%s",
                                          m->netplay_local_addresses[index].address);
                            np_save_network_settings(m);
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::InputText("##host_ip", m->netplay_host_ip,
                                 sizeof(m->netplay_host_ip),
                                 ImGuiInputTextFlags_ReadOnly);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##host_port", m->netplay_host_port,
                             sizeof(m->netplay_host_port),
                             ImGuiInputTextFlags_CharsDecimal);
            if (ImGui::IsItemDeactivatedAfterEdit())
                np_save_network_settings(m);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextColored(col(th.text_muted), "Password (optional)");
        ImGui::SetNextItemWidth(px(430));
        ImGui::InputText("##host_password", m->netplay_host_password,
                         sizeof(m->netplay_host_password), ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            host_create_status[0] = '\0';
            m->netplay_host_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const bool can_create =
            np_valid_port(m->netplay_host_port) &&
            m->netplay_host_ip[0] &&
            std::strcmp(m->netplay_host_ip, "Unavailable") != 0 &&
            std::strcmp(m->netplay_host_ip, "Detecting...") != 0 &&
            launcher_model_netplay_disc_ok(m);
        ImGui::BeginDisabled(!can_create);
        if (ImGui::Button("Create Lobby", ImVec2(px(150), 0))) {
            /* A room title is refused, not masked: it sits in the lobby
             * browser in front of everyone shopping for a game. Caught here
             * so the host renames the room before it is created, rather than
             * after the server refuses the `create` (which it also does). */
            if (np_name_refused(m, m->netplay_host_name)) {
                std::snprintf(m->netplay_host_name_error,
                              sizeof(m->netplay_host_name_error),
                              "That lobby name can't be used. Please rename "
                              "the lobby.");
            } else if (!launcher_model_netplay_disc_ok(m)) {
                std::snprintf(host_create_status, sizeof(host_create_status), "%s",
                              m->verify.netplay_detail[0]
                                  ? m->verify.netplay_detail
                                  : "Disc TOC not valid for netplay.");
            } else {
            const auto* np = np_cb(m);
            if (np && np->create) {
                /* Online create needs the lobby WebSocket. LAN/Direct IP only
                 * publishes the local registry — do not connect or advertise
                 * there, or the same room would appear twice in the list. */
                if (!m->netplay_lan_only)
                    np_connect_and_list(m);
                /* Selected NIC:port is the advertised / preferred bind address
                 * for both LAN-only and online (online may bump the port). */
                char endpoint[96];
                const char* port_label = m->netplay_host_port[0]
                    ? m->netplay_host_port : "7777";
                std::snprintf(endpoint, sizeof(endpoint), "%s:%s",
                              m->netplay_host_ip[0] ? m->netplay_host_ip : "127.0.0.1",
                              port_label);
                /* LAN requires the exact UI port; online auto-picks nearby if
                 * the preferred port is busy. */
                const int want_port = launcher_endpoint_port(endpoint);
                bool port_ok = true;
                if (m->netplay_lan_only) {
                    if (!launcher_udp_port_available(want_port)) {
                        std::snprintf(host_create_status, sizeof(host_create_status),
                                      "Port %s is already in use. Choose a "
                                      "different port for this LAN lobby.",
                                      port_label);
                        port_ok = false;
                    }
                } else {
                    const int free_port =
                        launcher_udp_find_free_port(/*preferred=*/want_port, 32);
                    if (free_port < 0 ||
                        (free_port != want_port &&
                         launcher_endpoint_set_port(endpoint, sizeof(endpoint),
                                                    free_port) != 0)) {
                        std::snprintf(host_create_status, sizeof(host_create_status),
                                      "No free UDP port near %s. Try again.",
                                      port_label);
                        port_ok = false;
                    }
                }
                if (port_ok) {
                    const char* lobby = m->netplay_host_name[0]
                        ? m->netplay_host_name : "Netplay Lobby";
                    const int max_slots = np_clamp_host_max_players(m);
                    /* Seed host match_caps from UI defaults before create. */
                    if (np->rollback_set)
                        (void)np->rollback_set(np->ctx,
                                               m->netplay_rollback ? 1 : 0);
                    if (np->input_prediction_set)
                        (void)np->input_prediction_set(
                            np->ctx, m->netplay_lobby_input_prediction);
                    if (np->input_delay_set)
                        (void)np->input_delay_set(np->ctx,
                                                  m->netplay_lobby_input_delay);
                    const int rc = np->create(np->ctx, lobby, endpoint,
                                              m->netplay_host_password, &m->s,
                                              m->netplay_lan_only ? 1 : 0,
                                              max_slots);
                    if (rc == -4) {
                        std::snprintf(host_create_status, sizeof(host_create_status),
                                      m->netplay_lan_only
                                          ? "Port %s is already in use. Choose a "
                                            "different port for this LAN lobby."
                                          : "No free UDP port near %s. Try again.",
                                      port_label);
                    } else if (rc != 0) {
                        std::snprintf(host_create_status, sizeof(host_create_status),
                                      "Could not create lobby.");
                    } else {
                        if (const char* colon = std::strrchr(endpoint, ':')) {
                            std::snprintf(m->netplay_host_port,
                                          sizeof(m->netplay_host_port), "%s",
                                          colon + 1);
                        }
                        std::snprintf(m->netplay_host_endpoint,
                                      sizeof(m->netplay_host_endpoint), "%s",
                                      endpoint);
                        std::snprintf(m->netplay_host_local_ip,
                                      sizeof(m->netplay_host_local_ip), "%s",
                                      m->netplay_host_ip);
                        np_save_network_settings(m);
                        m->netplay_lobby_max_slots = max_slots;
                        host_create_status[0] = '\0';
                        /* LAN/Direct IP is a local room (file registry). Online
                         * create seats on the WebSocket lobby when connected. */
                        m->netplay_local_room =
                            m->netplay_lan_only || !np_connected(m);
                        m->netplay_host_modal_open = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            } /* else disc ok */
        }
        ImGui::EndDisabled();
        if (!launcher_model_netplay_disc_ok(m) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s",
                              m->verify.netplay_detail[0]
                                  ? m->verify.netplay_detail
                                  : "Mount the supported .cue dump for netplay.");
        if (host_create_status[0])
            ImGui::TextColored(col(th.warn), "%s", host_create_status);
        ImGui::EndPopup();
    } else {
        host_create_status[0] = '\0';
    }
}

void draw_netplay_password_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->netplay_password_modal_open) ImGui::OpenPopup("Join Lobby");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Join Lobby", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(px(320));
        ImGui::InputText("Password", m->netplay_password, sizeof(m->netplay_password),
                         ImGuiInputTextFlags_Password);
        if (m->netplay_status[0])
            ImGui::TextColored(col(th.warn), "%s", m->netplay_status);
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            m->netplay_password_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Join", ImVec2(px(120), 0))) {
            const auto* np = np_cb(m);
            RecompLauncherCNetplayLobby row{};
            if (np && np->list_get && np->join &&
                np->list_get(np->ctx, m->netplay_selected_lobby, &row)) {
                char guest_bind[64];
                if (np_prepare_guest_bind(guest_bind, sizeof(guest_bind),
                                          m->netplay_status, sizeof(m->netplay_status))) {
                int rc = np->join(np->ctx, row.lobby_id, m->netplay_password, guest_bind);
                if (rc == 0) {
                    if (strncmp(row.lobby_id, "lan:", 4) == 0) {
                        m->netplay_local_room = true;
                        std::snprintf(m->netplay_host_endpoint,
                                      sizeof(m->netplay_host_endpoint), "%s",
                                      row.lobby_id + 4);
                    } else {
                        /* Server list join — never inherit LAN header state. */
                        m->netplay_local_room = false;
                        m->netplay_host_endpoint[0] = '\0';
                    }
                    m->netplay_lobby_max_slots =
                        row.max_slots >= 2 ? row.max_slots : np_game_max_players(m);
                    m->netplay_password_modal_open = false;
                    m->netplay_status[0] = '\0';
                    ImGui::CloseCurrentPopup();
                } else if (rc == -2) {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "Incorrect password.");
                } else if (rc == -3) {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "No LAN/Direct IP lobby at that address.");
                } else {
                    std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                  "Could not join lobby.");
                }
                }
            }
        }
        ImGui::EndPopup();
    }
}

/* Vertically center the next widget inside a fixed-height table row. */
static void table_row_vcenter(float row_h, float content_h) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (row_h - content_h) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p.x, y));
}

static void np_ingest_last_error(LauncherModel* m, const RecompLauncherCNetplayCallbacks* np) {
    if (!m || !np || !np->last_error) return;
    const char* err = np->last_error(np->ctx);
    if (!err || !err[0]) return;
    if (std::strcmp(err, "name_rejected") == 0) {
        /* The server refused the display name, and it is the authority here:
         * a modified or older client can send what the local check stops, and
         * the word list lives with the backend, not with the UI. Reopen the
         * prompt rather than write the status line, because the only useful
         * next act is typing another name.
         *
         * The refused name is dropped from settings but LEFT in the edit box:
         * nothing refused is kept or sent again, while the player can still
         * see and fix what they typed. Clearing the stored name also puts the
         * modal back on its required-name footing, so Cancel returns to the
         * dashboard instead of leaving netplay with a name the server will
         * refuse again on the next op. */
        std::snprintf(m->netplay_name_error, sizeof(m->netplay_name_error),
                      "That name can't be used. Please pick another one.");
        m->s.netplay_player_name[0] = '\0';
        m->netplay_name_prompted = true;
        m->netplay_name_modal_open = true;
        if (np->clear_last_error) np->clear_last_error(np->ctx);
        return;
    }
    if (std::strcmp(err, "lobby_name_rejected") == 0) {
        /* The server refused the room title. Reopen Host Lobby with the name
         * still in the field so the host can rename it; nothing was created,
         * so there is nothing else to undo. */
        std::snprintf(m->netplay_host_name_error, sizeof(m->netplay_host_name_error),
                      "That lobby name can't be used. Please rename the lobby.");
        m->netplay_host_modal_open = true;
        if (np->clear_last_error) np->clear_last_error(np->ctx);
        return;
    }
    if (std::strcmp(err, "password_invalid") == 0) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "That password can't be used. Use a shorter one without "
                      "control characters.");
        if (np->clear_last_error) np->clear_last_error(np->ctx);
        return;
    }
    if (std::strcmp(err, "need_players") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Need two players before starting.");
    else if (std::strcmp(err, "missing_endpoints") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Peer connection info missing — have guests rejoin, then "
                      "retry Play.");
    else if (std::strcmp(err, "relay_unavailable") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Lobby UDP SFU is unavailable. Fix INPUT_RELAY_* on "
                      "the lobby server (online matches require it).");
    else if (std::strcmp(err, "host_slot_fixed") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Host stays in seat 1. Rearrange guests among the "
                      "other seats.");
    else if (std::strcmp(err, "not_host") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Only the host can start the match.");
    else if (std::strcmp(err, "not_all_ready") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Lobby server is outdated (ready gate). Retry Play, or "
                      "redeploy recomp-net-server main.");
    else if (std::strcmp(err, "connect_timeout_ice") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Online connection timed out. Allow the game through "
                      "your firewall, then rejoin and retry.");
    else if (std::strcmp(err, "connect_timeout_lan") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "LAN connection timed out. Check the address, firewall, "
                      "and that both players are still in the lobby.");
    else if (std::strcmp(err, "peer_disconnected") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "The other player stopped responding. Rejoin to retry.");
    else if (std::strcmp(err, "transport_unavailable") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Online transport could not start. This build must "
                      "include ICE/NAT traversal.");
    else if (std::strcmp(err, "netplay_start_failed") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Netplay could not start. Check the log and retry.");
    else if (std::strcmp(err, "disc_mismatch") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Disc dump does not match the host (TOC / tracks). "
                      "Mount the same supported .cue dump.");
    else if (std::strcmp(err, "version_mismatch") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Game version does not match the lobby.");
    else if (std::strcmp(err, "game_mismatch") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "This lobby is for a different game.");
    else if (std::strcmp(err, "peer_needs_mods") == 0)
        /* The host pressed Play while somebody in the room cannot run the
         * plan. Not an error in the lobby -- the whole point of letting them
         * sit here is that they can fix it -- so this says who is waiting on
         * what rather than reporting a fault. */
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Cannot start yet: a player is missing mods this lobby "
                      "uses. Open Mods to see who, or turn the mod off.");
    else if (std::strcmp(err, "need_mods") == 0)
        /* The server refused the seat because the host's plan names packages
         * this peer does not have. Being refused is the correct outcome -- a
         * peer missing a mod that patches guest memory cannot stay in sync --
         * so this reads as a next step, not as a fault. */
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "This lobby needs mods you do not have. Open Mods to see "
                      "which, install them, then join again.");
    else if (std::strcmp(err, "mod_offer_too_large") == 0)
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Too many mods installed to announce to the lobby. "
                      "Remove some and try again.");
    else if (std::strcmp(err, "cooldown") == 0) {
        /* A matchmaking cooldown is not a lobby fault, and "Lobby error:
         * cooldown" reads as one -- a bare protocol code in front of a player
         * who declined a match thirty seconds ago and has no way to connect
         * the two. The automatch layer has already phrased it WITH the
         * server's own retry_secs in it, so prefer that over anything
         * reconstructed here; the fallback only covers a server that sent no
         * number. */
        const char* am = np->automatch_error ? np->automatch_error(np->ctx) : "";
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Matchmaking: %s",
                      (am && am[0]) ? am : "Cooldown For Declining");
    } else
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Lobby error: %s", err);
    if (np->clear_last_error) np->clear_last_error(np->ctx);
}

/* One seat row of the lobby player table. Shared by the standard single
 * table and the PSX-Link two-console tables (identical columns; the caller
 * owns BeginTable/EndTable and the header rows). */
/* One rendered seat table.
 *
 * Players and spectators differ in where their rows live and what a seat is
 * called, and in nothing else. The index move_member / kick_member take is one
 * namespace covering both, so dragging a row from one table to the other needs
 * no translation -- which is the point: a promotion IS a move, and giving it
 * its own code path is how the two end up behaving differently. */
/* After "Keep my seat", every further ask -- from anyone -- is declined
 * unseen until this ImGui-clock time (seconds). */
static const double k_swap_decline_s = 30.0;
static double s_swap_decline_until = 0.0;

struct LobbySeatView {
    RecompLauncherCNetplayMember* rows; /* indexed by position within a table */
    bool* occupied;
    int   n;
    /* Wire index of position 0. Zero for players; the server-reported gallery
     * base for spectators, which is why the gallery view is only built when
     * the server actually reported one. */
    int   wire_base;
    bool  spectator;
    const char* label; /* "P" or "S" */
};

/* Resolve a wire seat index back to its row, whichever table it is in.
 * Returns null for a seat neither table covers -- a stale drag payload after
 * the gallery closed, say, which must do nothing rather than index anything. */
static const RecompLauncherCNetplayMember* lobby_seat_lookup(
    const LobbySeatView* views, int nviews, int wire, bool* occupied_out,
    bool* spectator_out) {
    for (int i = 0; i < nviews; ++i) {
        const int pos = wire - views[i].wire_base;
        if (pos < 0 || pos >= views[i].n) continue;
        if (occupied_out) *occupied_out = views[i].occupied[pos];
        if (spectator_out) *spectator_out = views[i].spectator;
        return &views[i].rows[pos];
    }
    if (occupied_out) *occupied_out = false;
    if (spectator_out) *spectator_out = false;
    return nullptr;
}

/* Bring-your-own memory card (PSX). -1 = not a memory-card system; else 1
 * when a slot-1 card is enabled locally. An empty path still counts: the
 * runtime formats <memcard_dir>/card1.mcd on demand, so the only "no card"
 * cases are a disabled slot or a picked file that failed inspection. */
static int np_local_memcard_has_card(const LauncherModel* m) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (!prof || !prof->id || std::strcmp(prof->id, "psx") != 0) return -1;
    if (!m->s.memcard_enabled[0]) return 0;
    if (m->s.memcard_path[0][0] && m->memcard_inspected[0] &&
        !m->memcard_valid[0])
        return 0;
    return 1;
}

/* A little PS1 memory card: body, label band, connector strip. Vector so it
 * reads at any DPI and in both themes (emoji fonts have uneven metrics). */
static void np_draw_memcard_glyph(ImDrawList* dl, ImVec2 mn, ImVec2 mx,
                                  ImU32 line, ImU32 fill, bool filled) {
    const float w = mx.x - mn.x;
    const float h = mx.y - mn.y;
    const float r = h * 0.18f;
    if (filled) dl->AddRectFilled(mn, mx, fill, r);
    dl->AddRect(mn, mx, line, r, 0, px(1.5f));
    const float inset = w * 0.16f;
    /* label band */
    dl->AddRectFilled(ImVec2(mn.x + inset, mn.y + h * 0.24f),
                      ImVec2(mx.x - inset, mn.y + h * 0.50f), line, r * 0.5f);
    /* connector strip along the bottom edge */
    dl->AddRectFilled(ImVec2(mn.x + inset, mx.y - h * 0.22f),
                      ImVec2(mn.x + inset + w * 0.42f, mx.y - px(1.5f)), line);
}

/* Seat 2's memory-card toggle, drawn beside P2's name. Lit when the match
 * will use P2's card as its slot-2 card: P2 offered it (has a slot-1 card
 * and opted in) AND the host allows it. P2 clicks its own opt-in; the host
 * clicks its allow flag; both sides therefore have to agree, and the tooltip
 * always names which side is holding it off. */
static void draw_lobby_memcard_toggle(LauncherModel* m, const LauncherTheme& th,
                                      const RecompLauncherCNetplayCallbacks* np,
                                      const RecompLauncherCNetplayMember& p2,
                                      bool is_host) {
    const bool self_row = p2.is_local != 0;
    const bool allow = np->guest_memcard_get
                           ? (np->guest_memcard_get(np->ctx) != 0) : true;
    const bool legacy = p2.memcard_offer_valid == 0;
    const bool offered = !legacy && p2.memcard_has_card != 0;
    const bool shared = offered && p2.memcard_share != 0;
    const bool active = shared && allow;
    const bool can_click =
        (self_row && offered) || (is_host && (active || !allow));

    ImGui::SameLine(0, px(10));
    const float gw = px(26);
    const float gh = px(18);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.y += (ImGui::GetTextLineHeight() - gh) * 0.5f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##p2_memcard", ImVec2(gw, gh));
    const bool hovered = ImGui::IsItemHovered();
    if (can_click && hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImU32 line;
    ImU32 fill = 0;
    if (active) {
        line = imcol(th.accent);
        ImVec4 f = col(th.accent);
        f.w *= hovered ? 0.55f : 0.35f;
        fill = ImGui::GetColorU32(f);
    } else if (offered || (!allow && !legacy)) {
        line = imcol(hovered && can_click ? th.text : th.text_muted);
    } else {
        line = imcol(th.border);
    }
    np_draw_memcard_glyph(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(),
                          ImGui::GetItemRectMax(), line, fill, active);

    if (can_click && ImGui::IsItemClicked()) {
        if (self_row) {
            const int has_card = np_local_memcard_has_card(m);
            (void)np->memcard_offer_set(np->ctx, has_card > 0 ? 1 : 0,
                                        p2.memcard_share ? 0 : 1);
        } else if (np->guest_memcard_set) {
            /* active → turn off; off because of us → allow again. Off
             * because P2 has not offered is P2's to change, not ours. */
            (void)np->guest_memcard_set(np->ctx, active ? 0 : 1);
        }
    }
    if (!hovered) return;
    if (active) {
        ImGui::SetTooltip(
            "P2's memory card is in: %s brings their slot-1 card and it becomes\n"
            "slot 2 for everyone this match. The host's slot-2 card is not used.%s",
            p2.display_name,
            can_click ? "\nClick to turn it off." : "");
    } else if (!allow && !legacy) {
        ImGui::SetTooltip(
            "Guest memory cards are turned off by the host — the match uses the\n"
            "host's slot choices only.%s",
            is_host ? "\nClick to allow P2's card." : "");
    } else if (legacy) {
        ImGui::SetTooltip(
            "%s's launcher cannot bring a memory card (older build).",
            p2.display_name);
    } else if (!offered) {
        if (self_row)
            ImGui::SetTooltip(
                "No memory card enabled in your slot 1. Enable one on the\n"
                "dashboard to bring it to the match.");
        else
            ImGui::SetTooltip("%s has no slot-1 memory card to bring.",
                              p2.display_name);
    } else if (self_row) {
        ImGui::SetTooltip(
            "Bring your memory card: your slot-1 card becomes slot 2 for\n"
            "everyone this match (needed for games like Yu-Gi-Oh! where each\n"
            "player duels from their own card). Click to turn it on.");
    } else {
        ImGui::SetTooltip(
            "%s has a memory card but has not offered it. Only %s can turn\n"
            "that on.",
            p2.display_name, p2.display_name);
    }
}

static void draw_lobby_seat_row(LauncherModel* m,
                                const LauncherTheme& th,
                                const RecompLauncherCNetplayCallbacks* np,
                                const LobbySeatView& view, int pos,
                                const LobbySeatView* views, int nviews,
                                bool is_host, float text_h) {
    const float member_row_h = px(42);
    /* `wire` is what the backend is told; `pos` is only where the row is
     * drawn. Keeping them separate is what lets one renderer serve both
     * tables without either one knowing the other's indices. */
    const int wire = view.wire_base + pos;
    RecompLauncherCNetplayMember& row = view.rows[pos];
    const bool occ = view.occupied[pos];
    ImGui::PushID(wire);

        ImGui::TableNextRow(ImGuiTableRowFlags_None, member_row_h);
        ImGui::TableSetColumnIndex(0);
        ImVec2 row_pos = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##member_row_drop", false,
                          ImGuiSelectableFlags_SpanAllColumns |
                          ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0, member_row_h));
        /* Any seat — including P1/host — is a valid drop target: the host's
         * seat identifies whoever is hosting (host_slot / host_player_id),
         * not a fixed index, so it can move or be traded into like any
         * other. */
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("NETPLAY_MEMBER_SLOT")) {
                const int from_wire = *(const int*)payload->Data;
                bool from_spectator = false;
                const RecompLauncherCNetplayMember* from_row =
                    lobby_seat_lookup(views, nviews, from_wire, nullptr,
                                      &from_spectator);
                const bool self_drag = from_row && from_row->is_local;
                const bool cross_table = from_spectator || view.spectator;
                /* The host may seat ITSELF in the gallery only when the
                 * backend can run the match from there (it keeps the host
                 * role and its save-state / overlay controls; its pad is
                 * muted). Elsewhere the host's own seat stays in play. */
                const bool host_self_gallery =
                    is_host && self_drag && cross_table && np->host_can_spectate &&
                    np->host_can_spectate(np->ctx) != 0;
                if (from_wire != wire && from_row) {
                    if (is_host && np->move_member && (!self_drag || host_self_gallery)) {
                        /* The one call that crosses tables. Promotion,
                         * demotion and a plain reorder are all this. A
                         * refusal is said out loud: a drag that silently
                         * does nothing reads as a bug. */
                        if (np->move_member(np->ctx, from_wire, wire) != 0)
                            std::snprintf(m->netplay_status,
                                          sizeof(m->netplay_status),
                                          "Could not move %s to %s%d.",
                                          from_row->display_name, view.label,
                                          pos + 1);
                    } else if (self_drag && cross_table) {
                        /* Moving yourself between watching and playing: an
                         * EMPTY seat on the other side is yours to take; a
                         * taken one is a trade the occupant must agree to,
                         * exactly as inside the player table. The one extra
                         * rule: a trade that lands the HOST in the gallery
                         * -- the host dragging itself there, or a spectator
                         * asking the host for its player seat -- needs a
                         * backend that can run the match that way. Say so
                         * when refused: a drag that silently does nothing
                         * reads as a bug. */
                        int rc = -1;
                        const bool host_to_gallery =
                            (is_host && view.spectator) ||
                            (!is_host && occ && row.is_host && from_spectator);
                        if (host_to_gallery &&
                            (!np->host_can_spectate ||
                             !np->host_can_spectate(np->ctx))) {
                            std::snprintf(m->netplay_status,
                                          sizeof(m->netplay_status),
                                          "This room cannot run the match with "
                                          "the host in the spectator table.");
                        } else if (!occ) {
                            if (np->seat_move_self)
                                rc = np->seat_move_self(np->ctx, wire);
                            if (rc != 0)
                                std::snprintf(m->netplay_status,
                                              sizeof(m->netplay_status),
                                              "Could not move to %s%d (seat "
                                              "refused by the host).",
                                              view.label, pos + 1);
                        } else if (np->seat_swap_request) {
                            rc = np->seat_swap_request(np->ctx, wire);
                            if (rc != 0)
                                std::snprintf(m->netplay_status,
                                              sizeof(m->netplay_status),
                                              "Could not ask %s to swap seats.",
                                              row.display_name);
                        } else {
                            std::snprintf(m->netplay_status,
                                          sizeof(m->netplay_status),
                                          "%s%d is taken.", view.label, pos + 1);
                        }
                    } else if (self_drag) {
                        /* Moving yourself: a free seat is yours to take; an
                         * occupied one needs that player's consent. Say so
                         * when the backend refuses — a drag that silently
                         * does nothing is indistinguishable from a bug. */
                        int rc = -1;
                        if (!occ) {
                            if (np->seat_move_self)
                                rc = np->seat_move_self(np->ctx, wire);
                            if (rc != 0)
                                std::snprintf(m->netplay_status,
                                              sizeof(m->netplay_status),
                                              "Could not move to %s%d (seat "
                                              "refused by the host).",
                                              view.label, pos + 1);
                        } else if (np->seat_swap_request) {
                            rc = np->seat_swap_request(np->ctx, wire);
                            if (rc != 0)
                                std::snprintf(m->netplay_status,
                                              sizeof(m->netplay_status),
                                              "Could not ask %s to swap seats.",
                                              row.display_name);
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetCursorScreenPos(row_pos);
        ImGui::InvisibleButton("##member_drag_handle", ImVec2(px(28), member_row_h));
        ImVec2 grip_min = ImGui::GetItemRectMin();
        ImVec2 grip_max = ImGui::GetItemRectMax();
        const float grip_cx = (grip_min.x + grip_max.x) * 0.5f;
        const float grip_cy = (grip_min.y + grip_max.y) * 0.5f;
        const bool self_row = occ && row.is_local;
        const int can_drag =
            occ &&
            ((is_host && np->move_member) ||
             (self_row && (np->seat_move_self || np->seat_swap_request)));
        ImU32 grip_col = imcol(can_drag ? th.text_muted : th.border);
        ImDrawList* grip_dl = ImGui::GetWindowDrawList();
        for (int line = -1; line <= 1; ++line) {
            const float y = grip_cy + px(4) * line;
            grip_dl->AddLine(ImVec2(grip_cx - px(7), y),
                             ImVec2(grip_cx + px(7), y), grip_col, px(1.5f));
        }
        if (can_drag) {
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("NETPLAY_MEMBER_SLOT", &wire, sizeof(wire));
                ImGui::BeginGroup();
                ImGui::Text("%s%d", view.label, pos + 1);
                ImGui::SameLine(0, px(28));
                ImGui::TextUnformatted(row.display_name);
                ImGui::SameLine(0, px(28));
                ImGui::TextColored(col(th.good), "%s",
                                   row.is_host ? "Host"
                                   : view.spectator ? "Watching" : "Connected");
                ImGui::EndGroup();
                ImGui::EndDragDropSource();
            }
        }
        ImGui::TableSetColumnIndex(1);
        table_row_vcenter(member_row_h, text_h);
        ImGui::Text("%s%d", view.label, pos + 1);
        ImGui::TableSetColumnIndex(2);
        table_row_vcenter(member_row_h, text_h);
        if (occ) np_draw_country_flag(th, row.country);
        if (!occ) ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
        ImGui::TextUnformatted(occ ? row.display_name
                                   : view.spectator ? "Open seat" : "Open slot");
        if (!occ) ImGui::PopStyleColor();
        /* The third surface: a seat in the room. Blocked players are NOT
         * hidden here -- you are already in a lobby with them, and a seat
         * that silently vanishes is a room you cannot reason about. Their
         * chat still goes, and the menu is how you get here from a name you
         * only meet once you are seated. */
        if (occ && !row.is_local) np_player_menu(m, th, row.account, row.display_name);
        /* Seat 1 (P2) — by seat, not by who hosts: seat 0 is always the sim
         * authority whose cards are the match cards. */
        if (!view.spectator && wire == 1 && occ && np->memcard_offer_set &&
            np_local_memcard_has_card(m) >= 0)
            draw_lobby_memcard_toggle(m, th, np, row, is_host);
        ImGui::TableSetColumnIndex(3);
        table_row_vcenter(member_row_h, text_h);
        if (occ && row.is_host)
            ImGui::TextColored(col(th.good), "Host");
        else if (occ && view.spectator)
            /* Not "Connected": a spectator IS connected, and the thing worth
             * saying about it is that it cannot touch the match. */
            ImGui::TextColored(col(th.text_muted), "Watching");
        else if (occ)
            ImGui::TextColored(col(th.good), "Connected");
        else
            ImGui::TextColored(col(th.text_muted), "Waiting");
        if (occ && view.spectator && ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs the match in sync. Its controllers do not "
                              "reach the game.");
        else if (occ && row.bios_offer_valid && ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "BIOS: %s%s",
                row.bios_prefer_openbios ? "OpenBIOS" : "SCPH-1001",
                row.bios_can_scph1001 ? "" : " (no SCPH dump)");
        }
        ImGui::TableSetColumnIndex(4);
        table_row_vcenter(member_row_h, text_h);
        /* RTT to that seat from local peer — never on the local row. */
        if (occ && !row.is_local && row.latency_ms >= 0) {
            ImGui::Text("%d ms", row.latency_ms);
        } else {
            ImGui::TextColored(col(th.text_muted), "—");
        }
        ImGui::TableSetColumnIndex(5);
        {
            const float kick_btn = px(34);
            const bool can_kick = is_host && occ &&
                                  !row.is_host && np->kick_member;
            ImVec2 cell = ImGui::GetCursorScreenPos();
            const float avail_x = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorScreenPos(ImVec2(
                cell.x + (avail_x - kick_btn) * 0.5f,
                cell.y + (member_row_h - kick_btn) * 0.5f));
            if (can_kick) {
                /* Empty label + manual glyph draw: emoji fonts have uneven
                 * metrics so ButtonTextAlign alone leaves the boot off-center. */
                const bool pressed =
                    ImGui::Button("##kick", ImVec2(kick_btn, kick_btn));
                {
                    const char* boot = u8"\U0001F97E";
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    const ImVec2 ts = ImGui::CalcTextSize(boot);
                    const ImVec2 tp((rmin.x + rmax.x - ts.x) * 0.5f,
                                    (rmin.y + rmax.y - ts.y) * 0.5f);
                    ImGui::GetWindowDrawList()->AddText(
                        tp, ImGui::GetColorU32(ImGuiCol_Text), boot);
                }
                if (pressed)
                    (void)np->kick_member(np->ctx, wire);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Kick player");
            } else {
                /* Non-interactive placeholder — BeginDisabled still animates. */
                ImGui::Dummy(ImVec2(kick_btn, kick_btn));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                dl->AddRect(rmin, rmax, ImGui::GetColorU32(ImGuiCol_Border),
                            ImGui::GetStyle().FrameRounding);
                const char* boot = u8"\U0001F97E";
                const ImVec2 ts = ImGui::CalcTextSize(boot);
                dl->AddText(ImVec2((rmin.x + rmax.x - ts.x) * 0.5f,
                                   (rmin.y + rmax.y - ts.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), boot);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (!is_host)
                        ImGui::SetTooltip("Only the host can kick");
                    else if (!occ)
                        ImGui::SetTooltip(view.spectator ? "Open seat"
                                                         : "Open slot");
                    else if (row.is_host)
                        ImGui::SetTooltip("Cannot kick the host");
                    else if (!np->kick_member)
                        ImGui::SetTooltip("Kick unavailable");
                    else
                        ImGui::SetTooltip("Open slot");
                }
            }
        }

    ImGui::PopID();
}

/* ---- Lobby room: a full-screen view ---------------------------------------
 *
 * The room used to be a fixed 640px popup drawn over whatever view was up.
 * It is now a view of its own (LNG_VIEW_LOBBY) that the frame enters and
 * leaves by SEAT STATE — seated in a lobby means you are looking at the room,
 * not seated means you are not — so every profile that opens a lobby gets the
 * same full-screen page and nothing has to remember to open or close it.
 *
 * Layout: the body is two columns. Left, the seat tables (players, PSX-Link
 * consoles, spectators) with the notices that belong to them. Right, the room
 * panel (where peers connect), the match settings inline (host edits, guests
 * see what the host chose), and the mod plan. The action row — Leave, Mods,
 * Play — lives in the fixed footer like every other view's primary actions,
 * so it never scrolls out of reach. Narrow windows stack the columns. */

struct LobbySnapshot {
    RecompLauncherCNetplayMember slots[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS];
    bool occupied[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS];
    RecompLauncherCNetplayMember specs[RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS];
    bool spec_occupied[RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS];
    int  max_slots;
    int  spectator_seats;
    int  spectator_base;
    int  seated_players;
    int  peers_not_ready;
    char not_ready_names[160];
    bool is_host;
    bool link_kind;
};

/* Prefer the backend seat; sticky local_room alone kept kicked LAN joiners in
 * the room. */
static bool np_lobby_seated(const LauncherModel* m,
                            const RecompLauncherCNetplayCallbacks* np) {
    if (!np) return false;
    return np->in_lobby ? (np->in_lobby(np->ctx) != 0) : m->netplay_local_room;
}

/* Host: the match settings panel edits model fields that mirror the backend.
 * Seed them once per room entry (not every frame, which would clobber a value
 * mid-edit); guests re-read every frame because theirs are read-only echoes
 * of the host's caps. */
static bool g_lobby_settings_synced = false;

static void np_lobby_snapshot(LauncherModel* m,
                              const RecompLauncherCNetplayCallbacks* np,
                              LobbySnapshot* s) {
    std::memset(s, 0, sizeof(*s));
    s->is_host = np->is_host && np->is_host(np->ctx);
    const int count = np->member_count ? np->member_count(np->ctx) : 0;
    /* Prefer backend room ceiling, then sticky create/join value, then game max. */
    int max_slots = 0;
    if (np->lobby_max_slots) {
        max_slots = np->lobby_max_slots(np->ctx);
        if (max_slots >= 2) m->netplay_lobby_max_slots = max_slots;
    }
    if (max_slots < 2)
        max_slots = m->netplay_lobby_max_slots > 0
                        ? m->netplay_lobby_max_slots
                        : np_game_max_players(m);
    if (max_slots < 2) max_slots = 2;
    if (max_slots > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
        max_slots = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
    s->max_slots = max_slots;
    /* The gallery, when the CURRENT lobby has one. Gated on the lobby rather
     * than on the callback existing: a build that supports spectators still
     * talks to hosts and servers that do not, and there the whole section has
     * to be absent, not empty. */
    const bool spectators_on =
        np->lobby_allow_spectators && np->lobby_allow_spectators(np->ctx) != 0;
    if (spectators_on) {
        if (np->lobby_max_spectators)
            s->spectator_seats = np->lobby_max_spectators(np->ctx);
        if (s->spectator_seats > RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS)
            s->spectator_seats = RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS;
        /* The base comes from the backend, never assumed: it is the server's
         * namespace. Without it the two views could overlap, and a drag would
         * resolve to the wrong row. */
        if (np->spectator_slot) s->spectator_base = np->spectator_slot(np->ctx, 0);
        if (s->spectator_base <= 0) s->spectator_seats = 0;
    }
    for (int i = 0; i < count; ++i) {
        RecompLauncherCNetplayMember mem{};
        if (!np->member_get || !np->member_get(np->ctx, i, &mem)) continue;
        if (mem.is_spectator) {
            const int pos = s->spectator_seats ? mem.slot - s->spectator_base : -1;
            if (pos < 0 || pos >= s->spectator_seats) continue;
            s->specs[pos] = mem;
            s->spec_occupied[pos] = mem.display_name[0] != '\0';
            continue;
        }
        if (mem.slot < 0 || mem.slot >= RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS) continue;
        s->slots[mem.slot] = mem;
        s->occupied[mem.slot] = mem.display_name[0] != '\0';
    }
    for (int slot = 0; slot < max_slots; ++slot)
        if (s->occupied[slot]) ++s->seated_players;
    /* Mod readiness: a seated peer publishes ready=0 while it is still
     * missing this lobby's mod plan, so the host can hold the launch until
     * everyone can actually run the match. The host's own seat is covered by
     * its local catalog (it is the source of the plan). */
    for (int slot = 0; slot < max_slots; ++slot) {
        if (!s->occupied[slot] || s->slots[slot].is_host) continue;
        if (s->slots[slot].ready) continue;
        ++s->peers_not_ready;
        /* Name them: "somebody is missing mods" is not actionable when the
         * player in question is looking at a green screen. */
        const size_t used = std::strlen(s->not_ready_names);
        std::snprintf(s->not_ready_names + used, sizeof(s->not_ready_names) - used,
                      "%s%s", used ? ", " : "", s->slots[slot].display_name);
    }
    s->link_kind =
        max_slots >= 4 &&
        (((np->lobby_kind_get && np->lobby_kind_get(np->ctx) == 1)) ||
         (np->link_lobby_supported && np->link_lobby_supported(np->ctx)));
}

/* Host pressed Play. */
static void np_lobby_start(LauncherModel* m,
                           const RecompLauncherCNetplayCallbacks* np) {
    /* Ensure engine match_caps.rollback matches UI before start. */
    if (np->rollback_set)
        (void)np->rollback_set(np->ctx, m->netplay_rollback ? 1 : 0);
    const bool use_rb = m->netplay_rollback;
    const int max_rtt = np_lobby_max_peer_rtt_ms(m, np);
    /* Auto D: rollback uses §59 WAN-aware tiers; delay-sync uses the padded
     * one-way formula. Manual keeps the match settings panel's value.
     *
     * No TURN penalty. force_turn is a delay-floor HINT that does not change
     * online transport (§108: online is always the lobby SFU), and TURN is
     * deployed co-located with that SFU — so relaying through it costs a
     * localhost hop, not a second WAN leg. The old padding (RTT floored to
     * 80ms, +40ms, then D floored at 6) compensated for a cost that is not
     * paid: it inflated D on every relayed session, and since P = 4 + D it
     * inflated the invent runway with it. The measured peer RTT already
     * traverses the relay in use, so it needs no fudge. */
    if (!m->netplay_manual_input_delay && np->input_delay_set) {
        int delay = use_rb
            ? np_rb_delay_frames_from_rtt_ms(max_rtt)
            : np_delay_frames_from_rtt_ms(max_rtt);
        m->netplay_lobby_input_delay = delay;
        (void)np->input_delay_set(np->ctx, delay);
    }
    /* Auto P (rollback only): invent runway from RTT + committed D. */
    if (use_rb && !m->netplay_manual_input_prediction &&
        np->input_prediction_set) {
        const int pred = np_rb_prediction_frames_from_rtt_ms(
            max_rtt, m->netplay_lobby_input_delay);
        m->netplay_lobby_input_prediction = pred;
        (void)np->input_prediction_set(np->ctx, pred);
    }
    if (np->set_ready)
        (void)np->set_ready(np->ctx,
                            np->lobby_mods_missing
                                ? (np->lobby_mods_missing(np->ctx) == 0)
                                : 1);
    const int rc = np->request_start ? np->request_start(np->ctx, &m->s) : -1;
    if (rc != 0) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Could not start lobby (need at least two "
                      "players, or server rejected start).");
    } else {
        m->netplay_status[0] = '\0';
        /* LAN arms launch_pending inside request_start so host can
         * leave this frame. Online must wait for the server's
         * op:launch (drawn frames keep calling launch_pending) so
         * every peer boots together — do not fill from lobby seat. */
        if (np->launch_pending && np->launch_pending(np->ctx))
            np_try_launch(m);
        else
            std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                          "Starting match…");
    }
}

static void np_lobby_leave(LauncherModel* m,
                           const RecompLauncherCNetplayCallbacks* np) {
    m->netplay_local_room = false;
    m->netplay_lobby_settings_open = false;
    m->netplay_lobby_mods_open = false;
    m->netplay_lobby_max_slots = 0;
    if (np->leave) (void)np->leave(np->ctx);
    /* Back to the browser now rather than on the next seat poll, so the
     * frame that shows "left" is the frame the button was pressed on. */
    launcher_model_set_view(m, LNG_VIEW_NETPLAY);
    m->netplay_list_fresh = false;
}

/* Where peers connect. Only file-backed LAN/Direct rooms show IP/Port.
 * Server-list joins always show the lobby URL — do not use Host Lobby's LAN
 * checkbox or a stale host_endpoint (e.g. from a prior LAN host) as a
 * signal. Stacked, not tabular: this sits in a side column. */
static void draw_lobby_room_panel(LauncherModel* m, const LauncherTheme& th,
                                  const RecompLauncherCNetplayCallbacks* np) {
    ImGui::TextColored(col(th.accent2), "ROOM");
    ImGui::Spacing();
    const bool show_lan_endpoint = m->netplay_local_room;
    if (show_lan_endpoint) {
        char room_ip[64] = "";
        char room_port[16] = "7777";
        if (m->netplay_host_endpoint[0]) {
            std::snprintf(room_ip, sizeof(room_ip), "%s", m->netplay_host_endpoint);
            char* colon = strrchr(room_ip, ':');
            if (colon) {
                std::snprintf(room_port, sizeof(room_port), "%s", colon + 1);
                *colon = '\0';
            }
        }
        np_ensure_public_ip(m);
        const float w = ImGui::GetContentRegionAvail().x;
        const float port_w = px(96);
        const float gap = px(8);
        ImGui::TextColored(col(th.text_muted), "LAN IP Address");
        ImGui::SameLine(w - port_w);
        ImGui::TextColored(col(th.text_muted), "Port");
        ImGui::SetNextItemWidth(w - port_w - gap);
        np_copyable_readonly_input("##lobby_ip", room_ip, sizeof(room_ip), th);
        ImGui::SameLine(0, gap);
        ImGui::SetNextItemWidth(port_w);
        np_copyable_readonly_input("##lobby_port", room_port, sizeof(room_port), th);
        ImGui::Spacing();
        ImGui::TextColored(col(th.text_muted), "Public IP Address");
        {
            const float help_sz = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(w - help_sz - gap);
            np_copyable_readonly_input("##lobby_public_ip", m->netplay_public_ip,
                                       sizeof(m->netplay_public_ip), th);
            ImGui::SameLine(0, gap);
            if (ImGui::Button("?", ImVec2(help_sz, help_sz))) {
                /* tooltip on hover only */
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(
                    "Direct IP over the internet needs UDP port forwarding on "
                    "your router.\n\n"
                    "1. On your router, forward the Port shown above (UDP) from "
                    "the Public IP Address to this PC's LAN IP Address.\n"
                    "2. Remote friends Join Direct with your Public IP and that "
                    "Port.\n"
                    "3. Players on your local network can keep using the LAN IP "
                    "Address.\n\n"
                    "Router menus differ (Port Forwarding, Virtual Server, or "
                    "NAT). Leave the lobby open while they connect.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    } else {
        char lobby_server[256];
        const char* url = np->default_url ? np->default_url(np->ctx) : "";
        if (!url || !url[0]) url = m->netplay_lobby_url;
        std::snprintf(lobby_server, sizeof(lobby_server), "%s",
                      (url && url[0]) ? url : "lobby server");
        ImGui::TextColored(col(th.text_muted), "Lobby Server");
        ImGui::SetNextItemWidth(-1.0f);
        np_copyable_readonly_input("##lobby_server", lobby_server,
                                   sizeof(lobby_server), th);
    }
}

/* Match settings, inline. The host edits; everyone else reads the host's
 * choices (the getters return host-authoritative caps for guests). This used
 * to be a "Lobby Settings" popup behind a button; with a full page there is
 * room to keep it in view, which is where a setting that changes everyone's
 * input lag belongs. */
static void draw_lobby_match_settings(LauncherModel* m, const LauncherTheme& th,
                                      const RecompLauncherCNetplayCallbacks* np,
                                      bool is_host) {
    ImGui::TextColored(col(th.accent2), "MATCH SETTINGS");
    if (!is_host) {
        ImGui::SameLine();
        ImGui::TextColored(col(th.text_muted), " (set by the host)");
    }
    ImGui::Spacing();
    if (!is_host || !g_lobby_settings_synced) {
        if (np->input_delay_get)
            m->netplay_lobby_input_delay = np->input_delay_get(np->ctx);
        if (m->netplay_lobby_input_delay < 2) m->netplay_lobby_input_delay = 2;
        if (m->netplay_lobby_input_delay > 20) m->netplay_lobby_input_delay = 20;
        if (np->input_prediction_get)
            m->netplay_lobby_input_prediction = np->input_prediction_get(np->ctx);
        if (m->netplay_lobby_input_prediction < 2)
            m->netplay_lobby_input_prediction = 2;
        if (m->netplay_lobby_input_prediction > 16)
            m->netplay_lobby_input_prediction = 16;
        if (np->rollback_get)
            m->netplay_rollback = np->rollback_get(np->ctx) != 0;
        if (m->netplay_local_room) {
            m->netplay_force_input_relay = false;
            m->netplay_force_turn = false;
        } else {
            if (np->force_input_relay_get)
                m->netplay_force_input_relay =
                    np->force_input_relay_get(np->ctx) != 0;
            if (np->force_turn_get)
                m->netplay_force_turn = np->force_turn_get(np->ctx) != 0;
        }
        g_lobby_settings_synced = true;
    }
    ImGui::BeginDisabled(!is_host);
    {
        /* Rollback is the match mode and is no longer a lobby toggle: it is
         * whatever the backend reports (default on). Delay-sync stays
         * reachable through the backend's env override for debugging. */
        ImGui::TextUnformatted("Manual Input Delay");
        ImGui::SameLine();
        ImGui::TextColored(col(th.text_muted), "(frames)");
        {
            bool manual = m->netplay_manual_input_delay;
            if (ImGui::Checkbox("##manual_input_delay", &manual))
                m->netplay_manual_input_delay = manual;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(
                    "Off (default): at match start the host sets input delay "
                    "from the highest peer latency in the lobby.\n\n"
                    "On: use the frame value to the right for every player.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!m->netplay_manual_input_delay);
            ImGui::SetNextItemWidth(px(140));
            int delay = m->netplay_lobby_input_delay;
            if (ImGui::InputInt("##lobby_input_delay", &delay, 1, 1)) {
                if (delay < 2) delay = 2;
                if (delay > 20) delay = 20;
                m->netplay_lobby_input_delay = delay;
                if (np->input_delay_set)
                    (void)np->input_delay_set(np->ctx, delay);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                delay = m->netplay_lobby_input_delay;
                if (delay < 2) delay = 2;
                if (delay > 20) delay = 20;
                m->netplay_lobby_input_delay = delay;
                if (np->input_delay_set)
                    (void)np->input_delay_set(np->ctx, delay);
            }
            ImGui::SameLine();
            {
                const float help_sz = ImGui::GetFrameHeight();
                if (ImGui::Button("?##input_delay_help", ImVec2(help_sz, help_sz))) {
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(px(360));
                    ImGui::TextUnformatted(
                        "Committed input delay D (send lead / buffer).\n\n"
                        "With rollback (Disable Rollback off), auto D from "
                        "max peer RTT (§59/§60 WAN-aware):\n"
                        "  0–50 ms → 3, 50–80 → 4, 80–120 → 6,\n"
                        "  120–160 → 7, then steps up (floor 3).\n"
                        "Forced TURN floors at 6 (lobby RTT underestimates "
                        "the relay path).\n\n"
                        "With delay-sync (Disable Rollback on), auto D covers "
                        "one-way RTT at 60 Hz plus a 3-frame jitter pad:\n"
                        "  D = ceil(RTT_ms / 33.3) + 3  (min 3, max 20)\n\n"
                        "Too low stalls when packets arrive late. Too high adds "
                        "input lag for everyone.");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Manual Input Prediction");
        ImGui::SameLine();
        ImGui::TextColored(col(th.text_muted), "(frames)");
        {
            const bool rb_on = m->netplay_rollback;
            ImGui::BeginDisabled(!rb_on);
            bool manual_p = m->netplay_manual_input_prediction;
            if (ImGui::Checkbox("##manual_input_prediction", &manual_p))
                m->netplay_manual_input_prediction = manual_p;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(
                    "Rollback invent runway P. Off (default): host sets P from "
                    "peer RTT at match start. On: use the frame value to the "
                    "right.\n\n"
                    "Locked when Disable Rollback is on (delay-sync).");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!rb_on || !m->netplay_manual_input_prediction);
            ImGui::SetNextItemWidth(px(140));
            int pred = m->netplay_lobby_input_prediction;
            if (ImGui::InputInt("##lobby_input_prediction", &pred, 1, 1)) {
                if (pred < 2) pred = 2;
                if (pred > 16) pred = 16;
                m->netplay_lobby_input_prediction = pred;
                if (np->input_prediction_set)
                    (void)np->input_prediction_set(np->ctx, pred);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                pred = m->netplay_lobby_input_prediction;
                if (pred < 2) pred = 2;
                if (pred > 16) pred = 16;
                m->netplay_lobby_input_prediction = pred;
                if (np->input_prediction_set)
                    (void)np->input_prediction_set(np->ctx, pred);
            }
            ImGui::SameLine();
            {
                const float help_sz = ImGui::GetFrameHeight();
                if (ImGui::Button("?##input_prediction_help",
                                 ImVec2(help_sz, help_sz))) {
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(px(360));
                    ImGui::TextUnformatted(
                        "How far ahead of the remote tip a peer may invent "
                        "hold-last inputs before stalling (phase_lock).\n\n"
                        "Tip: prediction should be 4 + delay.\n\n"
                        "Auto (checkbox off): P = 4 + D (clamped 6..16).\n"
                        "Manual: keep the same relationship unless you are "
                        "deliberately tuning.\n\n"
                        "Outside this window the fast peer freezes until the "
                        "buffer refills; sustained freezes raise delay.");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        if (launcher_model_multitap_analog_available(m)) {
            bool analog_hack = m->s.multitap_analog != 0;
            if (np->multitap_analog_get)
                analog_hack = np->multitap_analog_get(np->ctx) != 0;
            if (ImGui::Checkbox("Multitap analog (hack)", &analog_hack)) {
                m->s.multitap_analog = analog_hack ? 1 : 0;
                if (np->multitap_analog_set)
                    (void)np->multitap_analog_set(np->ctx, analog_hack ? 1 : 0);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(
                    "Host-enforced DualShock sticks on multitap tap seats for "
                    "this match (match_caps.multitap_analog). Off = faithful "
                    "digital taps. Peers apply the host setting at launch.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::Spacing();
        }
    }
    ImGui::EndDisabled();
}

#if RECOMP_UI_ENABLE_MODS
/* Lobby mod picker: the HOST owns the session plan. Every peer applies the
 * host's required-mod list at launch (match_caps.mods), so guests get a
 * read-only view of what they are about to run. Compact by design — the full
 * Mods page stays the place to install packages and read details. */
static void draw_lobby_mods_popup(LauncherModel* m, const LauncherTheme& th,
                                  const RecompLauncherCNetplayCallbacks* np,
                                  bool is_host) {
    /* Lobby mod picker: the HOST owns the session plan. Every peer applies
     * the host's required-mod list at launch (match_caps.mods), so guests get
     * a read-only view of what they are about to run. Compact by design — the
     * full Mods page stays the place to install packages and read details. */
    if (m->netplay_lobby_mods_open && m->mods)
        ImGui::OpenPopup("Lobby Mods");
    if (m->mods &&
        ImGui::BeginPopupModal("Lobby Mods", &m->netplay_lobby_mods_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* mods = m->mods;
        const int feature_count =
            mods->feature_count ? mods->feature_count(mods->ctx) : 0;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(520));
        if (is_host) {
            ImGui::TextColored(col(th.text_muted),
                               "Everyone in this lobby runs the mods you pick "
                               "here.");
        } else {
            ImGui::TextColored(col(th.text_muted),
                               "The host picks this lobby's mods. You will run "
                               "this set when the match starts.");
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        /* GUEST: the plan is the host's published package list, which may
         * name packages this peer does not have. Those must be visible (and
         * downloadable) — they are exactly why the match cannot start. */
        if (!is_host) {
            const int plan_n =
                np->lobby_mods_count ? np->lobby_mods_count(np->ctx) : 0;
            const int missing_n =
                np->lobby_mods_missing ? np->lobby_mods_missing(np->ctx) : 0;
            const int progress =
                np->mod_xfer_progress ? np->mod_xfer_progress(np->ctx) : -1;
            /* Asked once, before anything is drawn: a row's button and the
             * paragraph below it must agree about whether a transfer is
             * possible, and neither may find out by trying. */
            const bool can_dl =
                np->lobby_mods_can_download
                    ? np->lobby_mods_can_download(np->ctx) != 0
                    : true;   /* unknown: keep the old click-to-find-out */
            /* Every paragraph in this panel wraps to the width of the list
             * above it. Unwrapped, one long sentence sets the dialog's width
             * and stretches it past the window. */
            const float kModTextWrap = px(520);
            /* One row per package: [status] name  version  — reason   [get]
             * The download control is per row rather than one button for the
             * whole plan, so a peer missing one package of five does not
             * re-fetch four it already has, and so the control sits next to
             * the thing it acts on. */
            if (ImGui::BeginChild("##lobby_plan_list", ImVec2(px(520), px(260)),
                                  ImGuiChildFlags_Borders)) {
                const float icon = ImGui::GetTextLineHeight() + px(4);
                for (int i = 0; i < plan_n; ++i) {
                    RecompLauncherCNetplayLobbyMod lm{};
                    if (!np->lobby_mods_get || !np->lobby_mods_get(np->ctx, i, &lm))
                        continue;
                    const int row_prog = np->lobby_mods_progress_one
                        ? np->lobby_mods_progress_one(np->ctx, i) : -1;
                    const bool in_flight = row_prog >= 0 && row_prog < 100;
                    const bool row_failed = row_prog == -2;

                    ImGui::PushID(i);
                    if (lm.installed) {
                        ImGui::TextColored(col(th.good), "OK");
                    } else {
                        ImGui::TextColored(col(th.warn), "--");
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(lm.name);
                    ImGui::SameLine();
                    /* The note is shown whenever there is one, installed or
                     * not: an installed row can still carry "host runs 1.0.1;
                     * you have another version", which is not a blocker here
                     * but is the first thing worth knowing if the match later
                     * refuses to start. */
                    const char* note = row_failed ? "download failed"
                                     : lm.reason[0] ? lm.reason
                                     : lm.installed ? ""
                                                    : "not installed";
                    ImGui::TextColored(col(th.text_muted), "%s%s%s%s",
                                       lm.version,
                                       note[0] ? "  — " : "",
                                       note,
                                       lm.builtin ? "  [built-in]" : "");

                    /* Right-align the control so the rows form a column
                     * regardless of how long the names are.
                     *
                     * Offered on INSTALLED rows too, as a re-fetch. A local
                     * copy can be stale or incomplete -- an older transfer of
                     * the same version, a half-finished install -- and the row
                     * only knows the package is present, not that it is the
                     * host's. Without a control there is no way to repair it
                     * from inside the game. The transfer replaces a copy at
                     * the same version, so this is a repair, not a duplicate. */
                    if (true) {
                        const float right =
                            ImGui::GetWindowContentRegionMax().x - icon;
                        if (right > ImGui::GetCursorPosX()) {
                            ImGui::SameLine();
                            ImGui::SetCursorPosX(right);
                        } else {
                            ImGui::SameLine();
                        }
                        if (in_flight) {
                            /* Occupies the same cell as the button, so the row
                             * does not jump when a transfer starts. */
                            ImGui::ProgressBar(row_prog / 100.0f,
                                               ImVec2(icon, icon), "");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Downloading… %d%%", row_prog);
                        } else {
                            char tip[224];
                            std::snprintf(tip, sizeof(tip),
                                          !can_dl
                                              ? "%s %s cannot be downloaded "
                                                "from the host in this build"
                                          : lm.installed
                                              ? "Get %s %s from the host again "
                                                "(replaces your copy — use this "
                                                "if the mod is not working)"
                                              : "Download %s %s from the host",
                                          lm.name, lm.version);
                            if (download_icon_button("##get", icon, can_dl, th,
                                                     tip)) {
                                const int rc = np->lobby_mods_download_one
                                    ? np->lobby_mods_download_one(np->ctx, i)
                                    : -1;
                                if (rc == -2)
                                    /* Not a failure: one transfer runs at a
                                     * time, so this names the click that was
                                     * ignored and why, rather than reporting
                                     * a fault that is not there. */
                                    std::snprintf(m->mod_status,
                                                  sizeof(m->mod_status),
                                                  "Already downloading — let "
                                                  "it finish, then get %s.",
                                                  lm.name);
                                else if (rc != 0)
                                    std::snprintf(m->mod_status,
                                                  sizeof(m->mod_status),
                                                  "Could not start the download "
                                                  "for %s.", lm.name);
                                else
                                    m->mod_status[0] = '\0';
                            }
                        }
                    }
                    /* The host's configuration for this package, indented
                     * under it. This is what the guest will actually run --
                     * its own settings are replaced with the host's before
                     * launch -- and it refreshes whenever the host republishes
                     * its caps, so changing a dropdown on the host shows up
                     * here without the guest touching anything. */
                    if (lm.options[0]) {
                        ImGui::Indent(px(24));
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                               px(460));
                        /* One line per entry: a mod with several features, or
                         * several dropdowns, is a list and reads as one. */
                        const char* line = lm.options;
                        while (*line) {
                            const char* nl = std::strchr(line, '\n');
                            const int len = nl ? (int)(nl - line)
                                               : (int)std::strlen(line);
                            ImGui::TextColored(col(th.text_muted),
                                               "host runs: %.*s", len, line);
                            if (!nl) break;
                            line = nl + 1;
                        }
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent(px(24));
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (plan_n == 0) {
                    ImGui::TextColored(col(th.text_muted),
                                       "The host has not enabled any mods — "
                                       "vanilla match.");
                }
            }
            ImGui::EndChild();

            if (missing_n > 0) {
                /* Distinguish "you can fix this with Download" from "your
                 * disc dump is different", which no download can repair. */
                bool image_mismatch = false;
                for (int i = 0; i < plan_n; ++i) {
                    RecompLauncherCNetplayLobbyMod lm{};
                    if (!np->lobby_mods_get || !np->lobby_mods_get(np->ctx, i, &lm))
                        continue;
                    if (!lm.installed && std::strstr(lm.reason, "image"))
                        image_mismatch = true;
                }
                ImGui::Spacing();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kModTextWrap);
                ImGui::TextColored(col(th.warn),
                                   "%d mod(s) unavailable. The match cannot "
                                   "start until they are.", missing_n);
                ImGui::PopTextWrapPos();
                if (image_mismatch) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kModTextWrap);
                    ImGui::TextColored(col(th.warn),
                        "One or more mods target a different dump of this game "
                        "than yours. Downloading will not help — the host and "
                        "you need the same disc image.");
                    ImGui::PopTextWrapPos();
                }
                if (!can_dl) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kModTextWrap);
                    ImGui::TextColored(col(th.text_muted),
                        "This build cannot download mods from the host yet. "
                        "Install them yourself to play. You can stay in the "
                        "lobby — the match simply will not start until every "
                        "player has them.");
                    ImGui::PopTextWrapPos();
                } else {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kModTextWrap);
                    ImGui::TextColored(col(th.text_muted),
                                       "Download All fetches the %d missing "
                                       "mod(s) one after another; the arrow "
                                       "beside a row fetches just that one. "
                                       "Downloading runs the host's code on "
                                       "your machine — only accept mods from "
                                       "a host you trust.", missing_n);
                    ImGui::PopTextWrapPos();
                    if (progress >= 0 && progress < 100) {
                        ImGui::ProgressBar(progress / 100.0f,
                                           ImVec2(px(320), 0));
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel", ImVec2(px(100), 0)) &&
                            np->mod_xfer_cancel) {
                            np->mod_xfer_cancel(np->ctx);
                        }
                    } else if (ImGui::Button(missing_n > 1 ? "Download All"
                                                          : "Download",
                                             ImVec2(px(200), 0))) {
                        const int rc = np->lobby_mods_download
                            ? np->lobby_mods_download(np->ctx) : -1;
                        if (rc != 0) {
                            /* Do not guess at a cause here: whoever refused
                             * knows why and says so through mod_xfer_failed,
                             * printed just below. */
                            std::snprintf(m->mod_status, sizeof(m->mod_status),
                                          "The download did not start. Leave "
                                          "and re-join to be offered these "
                                          "mods, or install them yourself.");
                        } else {
                            m->mod_status[0] = '\0';
                        }
                    }
                }
                /* 256, matching the lobby client's own error buffer. At 160
                 * the relay size refusal -- which names the mod, its size, the
                 * cap and what to do instead -- lost its last sentence, so the
                 * one line telling the player where to get the mod was the
                 * part that got cut. */
                char xfer_err[256];
                if (np->mod_xfer_failed &&
                    np->mod_xfer_failed(np->ctx, xfer_err, sizeof(xfer_err)) &&
                    xfer_err[0]) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kModTextWrap);
                    ImGui::TextColored(col(th.warn), "Transfer failed: %s",
                                       xfer_err);
                    ImGui::PopTextWrapPos();
                }
            } else if (plan_n > 0) {
                ImGui::Spacing();
                ImGui::TextColored(col(th.good),
                                   "You have every mod this lobby needs.");
            }
        }

        ImGui::BeginDisabled(!is_host);
        if (is_host &&
            ImGui::BeginChild("##lobby_mod_list", ImVec2(px(520), px(300)),
                              ImGuiChildFlags_Borders)) {
            int shown = 0;
            for (int i = 0; i < feature_count; ++i) {
                RecompLauncherCModFeature f{};
                if (!mods->feature_get(mods->ctx, i, &f)) continue;
                if (f.hidden && !f.enabled) continue;
                ++shown;
                ImGui::PushID(f.package_id);
                ImGui::PushID(f.id);

                bool enabled = f.enabled != 0;
                if (ImGui::Checkbox("##en", &enabled)) {
                    if (mods->feature_enable(mods->ctx, f.package_id, f.id,
                                             enabled ? 1 : 0)) {
                        /* Publish immediately: peers must see the host's plan
                         * without waiting for an unrelated settings change. */
                        if (np->push_match_caps) np->push_match_caps(np->ctx);
                    } else {
                        mod_note_error(m);
                    }
                }
                ImGui::SameLine();
                if (f.has_error) {
                    ImGui::TextColored(col(th.warn), "!");
                    if (ImGui::IsItemHovered() && f.status[0])
                        ImGui::SetTooltip("%s", f.status);
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(f.name);
                if (f.package_name[0] || f.package_version[0]) {
                    ImGui::SameLine();
                    ImGui::TextColored(col(th.text_muted), "(%s %s)",
                                       f.package_name[0] ? f.package_name
                                                         : f.package_id,
                                       f.package_version);
                }

                /* Options of an ENABLED feature are part of the plan, so they
                 * belong here — a divergent option value is exactly the kind
                 * of mismatch that only shows up as a mid-race desync. */
                if (enabled && f.option_count > 0 && mods->feature_option_get) {
                    ImGui::Indent(px(24));
                    for (int o = 0; o < f.option_count; ++o) {
                        RecompLauncherCModOption opt{};
                        if (!mods->feature_option_get(mods->ctx, f.package_id,
                                                      f.id, o, &opt))
                            continue;
                        draw_mod_feature_option(m, f, opt);
                    }
                    ImGui::Unindent(px(24));
                }
                ImGui::PopID();
                ImGui::PopID();
                ImGui::Separator();
            }
            if (shown == 0) {
                ImGui::TextColored(col(th.text_muted),
                                   feature_count
                                       ? "No mods enabled — vanilla match."
                                       : "No mod features installed.");
            }
        }
        if (is_host) ImGui::EndChild();
        ImGui::EndDisabled();

        if (m->mod_status[0]) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(520));
            ImGui::TextColored(col(th.warn), "%s", m->mod_status);
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(px(120), 0))) {
            m->netplay_lobby_mods_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* The plan summary + readiness, and the way into the picker. */
[[maybe_unused]] static void draw_lobby_mods_panel(LauncherModel* m, const LauncherTheme& th,
                                  const RecompLauncherCNetplayCallbacks* np,
                                  const LobbySnapshot& s) {
    if (!m->mods) return;
    ImGui::TextColored(col(th.accent2), "MODS");
    ImGui::Spacing();
    /* Summarize the plan every peer will run (host-authoritative). */
    const auto* lmods = m->mods;
    const int lfc = lmods->feature_count ? lmods->feature_count(lmods->ctx) : 0;
    int enabled_n = 0;
    char first_name[128] = {0};   /* RecompLauncherCModFeature::name */
    for (int i = 0; i < lfc; ++i) {
        RecompLauncherCModFeature f{};
        if (!lmods->feature_get(lmods->ctx, i, &f) || !f.enabled) continue;
        if (!enabled_n) std::snprintf(first_name, sizeof(first_name), "%s", f.name);
        ++enabled_n;
    }
    if (enabled_n == 0)
        ImGui::TextColored(col(th.text_muted), "Vanilla match.");
    else if (enabled_n == 1)
        ImGui::TextColored(col(th.accent2), "%s", first_name);
    else
        ImGui::TextColored(col(th.accent2), "%s +%d more", first_name, enabled_n - 1);
    /* Mods — host picks the lobby's set; guests get the read-only view of
     * what they will run (the host's plan is authoritative at launch). */
    if (ImGui::Button(s.is_host ? "Choose mods…" : "View mods…",
                      ImVec2(px(150), px(30))))
        m->netplay_lobby_mods_open = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(s.is_host
            ? "Pick the mods everyone in this lobby will run"
            : "See the mods the host has enabled for this lobby");
    /* Readiness: the host cannot start until every peer can run the plan,
     * so say plainly which state the room is in — and warn about what
     * downloading a mod actually means (it is code from another player). */
    if (np->lobby_mods_count) {
        const int lobby_mod_n = np->lobby_mods_count(np->ctx);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        if (lobby_mod_n <= 0) {
            ImGui::TextColored(col(th.text_muted),
                               "No mods required — vanilla match.");
        } else if (s.peers_not_ready == 0) {
            ImGui::TextColored(col(th.good),
                               "All players have this lobby's mods installed "
                               "and are ready.");
        } else {
            ImGui::TextColored(col(th.warn),
                               "Waiting on: %s — this lobby's mods are not "
                               "confirmed installed there yet. They can open "
                               "'View mods' to download them from the host. "
                               "The match cannot start until then.",
                               s.not_ready_names[0] ? s.not_ready_names
                                                    : "another player");
        }
        if (lobby_mod_n > 0) {
            ImGui::TextColored(col(th.text_muted),
                               "Mods are code that runs on your machine. Only "
                               "download them from a host you trust.");
        }
        ImGui::PopTextWrapPos();
    }
}
#endif

/* The seat tables and everything that hangs off them. */
static void draw_lobby_seats(LauncherModel* m, const LauncherTheme& th,
                             const RecompLauncherCNetplayCallbacks* np,
                             LobbySnapshot& s) {
    ImGui::TextColored(col(th.accent2), "PLAYERS");
    ImGui::SameLine();
    ImGui::TextColored(col(th.text_muted), "  %d / %d seated",
                       s.seated_players, s.max_slots);
    if (m->netplay_status[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(col(th.warn), "%s", m->netplay_status);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();

    const float text_h = ImGui::GetTextLineHeight();
    LobbySeatView views[2];
    int nviews = 0;
    const int player_view = nviews;
    views[nviews++] = LobbySeatView{s.slots, s.occupied, s.max_slots, 0, false, "P"};
    if (s.spectator_seats > 0)
        views[nviews++] = LobbySeatView{s.specs, s.spec_occupied, s.spectator_seats,
                                        s.spectator_base, true, "S"};
    auto seat_table = [&](const char* id, const char* title, int vi, int lo,
                          int hi) {
        if (title) ImGui::TextColored(col(th.text_muted), "%s", title);
        if (ImGui::BeginTable(id, 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##move", ImGuiTableColumnFlags_WidthFixed, px(32));
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, px(40));
            ImGui::TableSetupColumn(views[vi].spectator ? "Spectator" : "Player",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, px(100));
            ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, px(72));
            ImGui::TableSetupColumn("Kick", ImGuiTableColumnFlags_WidthFixed, px(56));
            ImGui::TableHeadersRow();
            for (int pos = lo; pos < hi; ++pos)
                draw_lobby_seat_row(m, th, np, views[vi], pos, views, nviews,
                                    s.is_host, text_h);
            ImGui::EndTable();
        }
    };
    if (s.link_kind) {
        /* PSX-Link: two consoles over the serial cable. The host can drag
         * players between the tables; seats 0/1 race on console A, 2/3 on
         * console B. Slot 0 (host / sim authority) stays on console A. */
        ImGui::TextColored(col(th.accent), "PSX-Link lobby");
        ImGui::SameLine();
        ImGui::TextColored(col(th.text_muted),
                           " — two linked consoles, 2 players each");
        seat_table("lobby_players_a", "Console A — Players 1 & 2", player_view,
                   0, 2);
        ImGui::Spacing();
        seat_table("lobby_players_b", "Console B — Players 3 & 4", player_view,
                   2, s.max_slots < 4 ? s.max_slots : 4);
        {
            bool b_occupied = false;
            for (int i = 2; i < 4 && i < s.max_slots; ++i)
                if (s.occupied[i]) b_occupied = true;
            if (!b_occupied)
                ImGui::TextColored(col(th.text_muted),
                    "Console B is empty — the match will start as a standard "
                    "2-player race.");
        }
    } else {
        seat_table("lobby_players",
                   s.spectator_seats > 0 ? "Players" : nullptr, player_view, 0,
                   s.max_slots);
    }
    /* Bring-your-own memory card summary: one line, only when it is on, so
     * every peer sees the same thing the launch will do. */
    if (s.max_slots > 1 && s.occupied[1] && np->memcard_offer_set &&
        np_local_memcard_has_card(m) >= 0 && s.slots[1].memcard_offer_valid &&
        s.slots[1].memcard_has_card && s.slots[1].memcard_share &&
        (!np->guest_memcard_get || np->guest_memcard_get(np->ctx))) {
        ImGui::TextColored(col(th.text_muted),
                           "P2 (%s) brings their memory card — it is slot 2 for "
                           "everyone this match.",
                           s.slots[1].display_name);
    }
    if (s.spectator_seats > 0) {
        ImGui::Spacing();
        seat_table("lobby_spectators", "Spectators", nviews - 1, 0,
                   s.spectator_seats);
        /* The host watching from the gallery keeps hosting. Say what that
         * means before Play, not after. */
        bool host_in_gallery = false;
        for (int i = 0; i < s.spectator_seats; ++i)
            if (s.spec_occupied[i] && s.specs[i].is_host) host_in_gallery = true;
        if (host_in_gallery) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(col(th.text_muted),
                               s.is_host
                                   ? "You are hosting from the spectator table: "
                                     "you keep save states and the host "
                                     "controls; your controller is not in the "
                                     "game."
                                   : "The host is spectating and still runs the "
                                     "match.");
            ImGui::PopTextWrapPos();
        }
    }
    /* Session BIOS notice (OpenBIOS vs SCPH1001). Keep copy plain — hosts care
     * about save-state compatibility, not kernel-RAM details.
     * Orange OpenBIOS override only when a peer cannot run SCPH; host retail
     * preference otherwise settles SCPH (even if a guest prefers OpenBIOS).
     *
     * PSX only. A BIOS choice exists on no other console this launcher serves,
     * and the block below reads a MISSING offer as "prefers OpenBIOS" -- which
     * on an SNES lobby, where nobody ever sends one, printed "Host has selected
     * OpenBIOS for this session" over a Gundam Wing seat table. The notice is
     * derived from PSX data; where that data cannot exist, so cannot the
     * notice. */
    const SystemProfile* bios_prof = (const SystemProfile*)m->profile;
    if (bios_prof && bios_prof->id && std::strcmp(bios_prof->id, "psx") == 0) {
        int host_prefer_open = 0;
        int host_found = 0;
        int all_can_scph = 1;
        int saw = 0;
        for (int slot = 0; slot < s.max_slots; ++slot) {
            if (!s.occupied[slot]) continue;
            ++saw;
            const int offer_ok = s.slots[slot].bios_offer_valid;
            const int prefer_open = !offer_ok || s.slots[slot].bios_prefer_openbios;
            const int can_scph = offer_ok && s.slots[slot].bios_can_scph1001;
            if (s.slots[slot].is_host) {
                host_found = 1;
                host_prefer_open = prefer_open ? 1 : 0;
            }
            if (!can_scph) all_can_scph = 0;
        }
        if (saw >= 1 && host_found) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            if (host_prefer_open) {
                ImGui::TextColored(
                    col(th.good),
                    "Host has selected OpenBIOS for this session.\n"
                    "Note: Save states are not cross compatible with SCPH1001 "
                    "and OpenBIOS sessions");
            } else if (all_can_scph) {
                ImGui::TextColored(
                    col(th.good),
                    "All users agree on proprietary BIOS SCPH1001.bin for this "
                    "session.\n"
                    "Note: Save states are not cross compatible with SCPH1001 "
                    "and OpenBIOS sessions");
            } else {
                ImGui::TextColored(
                    col(th.warn),
                    "1 or more users lacks proprietary BIOS, using OpenBIOS for "
                    "this session instead.\n"
                    "Note: Save states are not cross compatible with SCPH1001 "
                    "and OpenBIOS sessions");
            }
            ImGui::PopTextWrapPos();
        }
    }
    /* Outcome of a swap this player asked for. */
    if (np->seat_swap_outgoing) {
        const int st = np->seat_swap_outgoing(np->ctx);
        if (st == 1) {
            ImGui::TextColored(col(th.text_muted),
                               "Waiting for the other player to accept the seat "
                               "swap…");
        } else if (st == -1) {
            ImGui::TextColored(col(th.warn),
                               "That player kept their seat.");
            ImGui::SameLine();
            if (ImGui::SmallButton("OK##swapres") && np->seat_swap_clear)
                np->seat_swap_clear(np->ctx);
        } else if (st == 2) {
            if (np->seat_swap_clear) np->seat_swap_clear(np->ctx);
        }
    }
    if (ImGui::GetTime() < s_swap_decline_until) {
        ImGui::TextColored(col(th.text_muted),
                           "Declining seat requests for another %d s.",
                           (int)(s_swap_decline_until - ImGui::GetTime()) + 1);
    }
    if (s.is_host && (np->move_member || np->kick_member)) {
        ImGui::Spacing();
        ImGui::TextColored(col(th.text_muted),
                           s.spectator_seats > 0
                               ? "Drag a row to move a player; drag between "
                                 "the tables to move somebody in or out of play."
                               : "Drag a row to move a player to another seat.");
    }
}

/* "Vanilla match." / "Widescreen (16:9) +1 more" — the plan every peer will
 * run (host-authoritative). False when this build has no mod provider, so
 * the caller can fall back to a plain label. */
static bool np_lobby_mods_summary(const LauncherModel* m, char* out, size_t cap) {
#if RECOMP_UI_ENABLE_MODS
    if (!m->mods || !out || cap == 0) return false;
    const auto* lmods = m->mods;
    const int lfc = lmods->feature_count ? lmods->feature_count(lmods->ctx) : 0;
    int enabled_n = 0;
    char first_name[128] = {0};   /* RecompLauncherCModFeature::name */
    for (int i = 0; i < lfc; ++i) {
        RecompLauncherCModFeature f{};
        if (!lmods->feature_get(lmods->ctx, i, &f) || !f.enabled) continue;
        if (!enabled_n) std::snprintf(first_name, sizeof(first_name), "%s", f.name);
        ++enabled_n;
    }
    if (enabled_n == 0)
        std::snprintf(out, cap, "Mods: vanilla match");
    else if (enabled_n == 1)
        std::snprintf(out, cap, "Mods: %s", first_name);
    else
        std::snprintf(out, cap, "Mods: %s +%d more", first_name, enabled_n - 1);
    return true;
#else
    (void)m; (void)out; (void)cap;
    return false;
#endif
}

/* Lobby chat: the whole right column. Every seated player and spectator sees
 * every line; the backend's ring is the only source (the UI never appends
 * its own send, so what you see is what the room saw, in the room's order).
 * Enter sends and keeps the box focused, so a conversation does not need the
 * mouse. */
/* One chat panel, two rooms: the lobby's chat and the browser page's
 * per-game server chat draw through this. The log scrolls to a NEW line
 * only; the input holds display form (atlas codepoints for emoji) so the
 * box matches the log, and emoji_restore turns it back into UTF-8 to send.
 * Lines arrive already masked by the backend (see docs, "Chat filtering");
 * this panel adds nothing and removes nothing. */
struct ChatPanelIo {
    void* ctx;
    int (*send)(void* ctx, const char* text);
    int (*count)(void* ctx);
    int (*get)(void* ctx, int index, RecompLauncherCNetplayChatMessage* out);
};

static void draw_chat_panel(LauncherModel* m, const LauncherTheme& th,
                            const ChatPanelIo& io, const char* id,
                            const char* title, const char* empty_hint,
                            const char* input_hint, const char* send_fail,
                            char* edit, size_t edit_cap, uint32_t* seen_seq,
                            bool* focus) {
    ImGui::PushID(id);
    ImGui::TextColored(col(th.accent2), "%s", title);
    ImGui::Spacing();
    const float input_h = ImGui::GetFrameHeight() + px(10);
    const float list_h = ImGui::GetContentRegionAvail().y - input_h;
    const int n = io.count(io.ctx);
    uint32_t newest = 0;
    if (ImGui::BeginChild("##chat_log", ImVec2(0, list_h > px(80) ? list_h : px(80)),
                          ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0.0f);
        if (n <= 0) ImGui::TextColored(col(th.text_muted), "%s", empty_hint);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayChatMessage msg{};
            if (!io.get(io.ctx, i, &msg)) continue;
            newest = msg.seq;
            if (msg.is_system) {
                char sys_disp[640];
                emoji_display(msg.text, sys_disp, sizeof(sys_disp));
                ImGui::TextColored(col(th.text_muted), "%s", sys_disp);
                continue;
            }
            /* An ignored player's line is not drawn at all -- not greyed,
             * not collapsed to "message hidden". The point of ignoring
             * somebody is to stop seeing them, and a placeholder per line is
             * still a conversation you are being made to watch. `newest` is
             * updated above regardless, so a hidden line does not keep
             * re-scrolling the log. Never your own. */
            if (!msg.is_local && recomp_moderation_is_ignored(msg.account))
                continue;
            char from_disp[128];
            char text_disp[640];
            emoji_display(msg.from[0] ? msg.from : "?", from_disp, sizeof(from_disp));
            emoji_display(msg.text, text_disp, sizeof(text_disp));
            ImGui::TextColored(col(msg.is_local ? th.good : th.accent), "%s", from_disp);
            /* Right-click the NAME on a chat line, same as in the player
             * list: it is the thing you point at when you mean "them". */
            if (!msg.is_local) np_player_menu(m, th, msg.account, msg.from, msg.mid);
            ImGui::SameLine(0, px(6));
            ImGui::TextUnformatted(text_disp);
        }
        ImGui::PopTextWrapPos();
        /* Scroll to a NEW line only; a reader who scrolled up to re-read is
         * left where they are until the next line lands. */
        if (newest != *seen_seq) {
            ImGui::SetScrollHereY(1.0f);
            *seen_seq = newest;
        }
    }
    ImGui::EndChild();

    const float send_w = px(72);
    const float gap = px(8);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send_w - gap);
    if (*focus) {
        ImGui::SetKeyboardFocusHere();
        *focus = false;
    }
    bool send = ImGui::InputTextWithHint("##chat_edit", input_hint, edit, edit_cap,
                                         ImGuiInputTextFlags_EnterReturnsTrue |
                                             ImGuiInputTextFlags_CallbackEdit,
                                         emoji_input_callback);
    if (send) *focus = true;
    ImGui::SameLine(0, gap);
    if (ImGui::Button(ui_text("Send"), ImVec2(send_w, 0))) send = true;
    if (send) {
        /* Trim; an empty or all-space line is not a message. */
        char* t = edit;
        while (*t == ' ') ++t;
        size_t len = std::strlen(t);
        while (len > 0 && t[len - 1] == ' ') t[--len] = '\0';
        if (len > 0) {
            char raw[1024];
            emoji_restore(t, raw, sizeof(raw));
            if (io.send(io.ctx, raw) == 0)
                edit[0] = '\0';
            else
                std::snprintf(m->netplay_status, sizeof(m->netplay_status), "%s", send_fail);
        }
    }
    ImGui::PopID();
}

static void draw_lobby_chat(LauncherModel* m, const LauncherTheme& th,
                            const RecompLauncherCNetplayCallbacks* np) {
    if (!np->chat_send || !np->chat_count || !np->chat_get) {
        /* This tests OUR OWN callback table, so it says nothing whatsoever
         * about the host. It read "the host runs an older build", which sent
         * players to look at the wrong machine -- on a title that simply does
         * not implement chat it appeared for every lobby, including one this
         * player was hosting themselves. */
        ImGui::TextColored(col(th.accent2), "CHAT");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(col(th.text_muted),
                           "This build does not have lobby chat.");
        ImGui::PopTextWrapPos();
        return;
    }
    const ChatPanelIo io{np->ctx, np->chat_send, np->chat_count, np->chat_get};
    draw_chat_panel(m, th, io, "lobby_chat", "CHAT",
                    "Say hello — everyone in the room sees this.",
                    "Message the lobby…", "Chat is not available in this room.",
                    m->netplay_chat_edit, sizeof(m->netplay_chat_edit),
                    &m->netplay_chat_seen_seq, &m->netplay_chat_focus);
}

/* The browser page's per-game server chat: everyone on the lobby server
 * playing this title, seated or not. Drawn only when the backend has the
 * callbacks and is online (a LAN-only session has no wider room). */
static bool np_server_chat_available(const RecompLauncherCNetplayCallbacks* np) {
    return np->server_chat_send && np->server_chat_count && np->server_chat_get &&
           np->connected && np->connected(np->ctx);
}

static void draw_server_chat(LauncherModel* m, const LauncherTheme& th,
                             const RecompLauncherCNetplayCallbacks* np) {
    const ChatPanelIo io{np->ctx, np->server_chat_send, np->server_chat_count,
                         np->server_chat_get};
    draw_chat_panel(m, th, io, "server_chat", "SERVER CHAT",
                    "Everyone online for this game sees this — no history, "
                    "only what is said while you are here.",
                    "Message everyone playing this game…",
                    "Server chat is not available right now.",
                    m->netplay_schat_edit, sizeof(m->netplay_schat_edit),
                    &m->netplay_schat_seen_seq, &m->netplay_schat_focus);
}

/* Lobby Settings popup: the room address, the match settings, and the mod
 * plan — the things a host tunes, and a guest looks up. Opened from the
 * footer. The mod picker is its own modal, and ImGui does not nest two
 * modals, so choosing mods from here closes this one first. */
static void draw_lobby_settings_popup(LauncherModel* m, const LauncherTheme& th,
                                      const RecompLauncherCNetplayCallbacks* np,
                                      const LobbySnapshot& s) {
    if (m->netplay_lobby_settings_open)
        ImGui::OpenPopup("Lobby Settings");
    ImGui::SetNextWindowSize(ImVec2(px(520), 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Lobby Settings", &m->netplay_lobby_settings_open,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    draw_lobby_room_panel(m, th, np);
    ImGui::Dummy(ImVec2(0, px(14)));
    draw_lobby_match_settings(m, th, np, s.is_host);
    /* Mods have their own footer button and picker; they do not repeat here. */
    (void)s;
    ImGui::Spacing();
    if (ImGui::Button(ui_text("Close"), ImVec2(px(120), 0))) {
        m->netplay_lobby_settings_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* The lobby view body. */
void draw_lobby(LauncherModel* m, const LauncherTheme& th) {
    const auto* np = np_cb(m);
    if (!np) return;
    /* Keep membership live while the room is up (join/leave/move/kick). */
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    /* Bring-your-own memory card: keep the backend's view of THIS peer's
     * slot-1 card current (the dashboard can change it while the room is
     * open). The opt-in itself is toggled from the seat row. */
    if (np->memcard_offer_set) {
        const int has_card = np_local_memcard_has_card(m);
        if (has_card >= 0) (void)np->memcard_offer_set(np->ctx, has_card, -1);
    }
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);
    if (!np_lobby_seated(m, np)) {
        /* The frame flips the view on its next pass; say why this is blank
         * for the frame in between rather than drawing a stale room. */
        ImGui::TextColored(col(th.text_muted), "Leaving lobby…");
        return;
    }

    LobbySnapshot s;
    np_lobby_snapshot(m, np, &s);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float gap = px(20);
    const float side_w = px(380);
    /* Two columns when the seat table keeps a comfortable width beside the
     * side panel; otherwise stack, seats first. */
    const bool two_col = avail_w >= side_w + gap + px(520);
    const float left_w = two_col ? avail_w - side_w - gap : avail_w;

    begin_container("lobby_seats", ImVec2(left_w, two_col ? 0.0f : 0.0f),
                    two_col ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
    draw_lobby_seats(m, th, np, s);
    end_container();

    if (two_col) ImGui::SameLine(0, gap);
    else ImGui::Dummy(ImVec2(0, px(12)));
    /* Stacked: the chat still needs a real height to be usable, so it takes
     * a fixed band rather than auto-sizing to its (empty) contents. */
    begin_container("lobby_side", ImVec2(two_col ? side_w : avail_w,
                                         two_col ? 0.0f : px(320)));
    draw_lobby_chat(m, th, np);
    end_container();

    draw_lobby_settings_popup(m, th, np, s);

    /* Seat trade: somebody asked to swap with this player. Modal, because
     * agreeing moves them out of the seat they chose. "Keep my seat" also
     * answers every ask from ANYONE for the next while (k_swap_decline_s)
     * without showing the prompt: one "no" should end the nagging, not
     * invite a retry the moment the popup closes. */
    if (np->seat_swap_incoming) {
        char who[64] = {0};
        int from_slot = -1;
        if (np->seat_swap_incoming(np->ctx, who, sizeof(who), &from_slot)) {
            if (ImGui::GetTime() < s_swap_decline_until) {
                if (np->seat_swap_respond)
                    (void)np->seat_swap_respond(np->ctx, 0);
            } else {
                ImGui::OpenPopup("Swap seats?");
            }
            if (ImGui::BeginPopupModal("Swap seats?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s wants to swap seats with you.",
                            who[0] ? who : "Another player");
                if (from_slot >= 0) {
                    /* Their seat, named the way the tables name it: a
                     * gallery seat is "S<n>", a player seat "P<n>". */
                    const bool from_gallery =
                        s.spectator_base > 0 && from_slot >= s.spectator_base;
                    const int from_pos =
                        from_gallery ? from_slot - s.spectator_base : from_slot;
                    ImGui::TextColored(col(th.text_muted),
                                       from_gallery
                                           ? "They are watching from S%d; you "
                                             "would move there and they would "
                                             "take your seat."
                                           : "They are in P%d; you would move "
                                             "there.",
                                       from_pos + 1);
                }
                ImGui::Spacing();
                if (ImGui::Button("Swap", ImVec2(px(120), 0))) {
                    if (np->seat_swap_respond)
                        (void)np->seat_swap_respond(np->ctx, 1);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep my seat", ImVec2(px(140), 0))) {
                    if (np->seat_swap_respond)
                        (void)np->seat_swap_respond(np->ctx, 0);
                    s_swap_decline_until = ImGui::GetTime() + k_swap_decline_s;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }
#if RECOMP_UI_ENABLE_MODS
    draw_lobby_mods_popup(m, th, np, s.is_host);
#endif
}

/* The lobby view's footer band: Leave (left), Mods (beside it), Play (right,
 * host only). Drawn by draw_footer in the fixed band every view shares. */
static void draw_lobby_footer(LauncherModel* m, const LauncherTheme& th,
                              ImVec2 origin, float cta_y, float play_h,
                              float fullw) {
    const auto* np = np_cb(m);
    if (!np) return;
    LobbySnapshot s;
    np_lobby_snapshot(m, np, &s);
    const float leave_w = px(150);
    const float mods_w = px(130);
    const float play_w = px(210);
    const float gap = px(10);

    /* Leave — red, pinned left. */
    const LngColor leave_bg = {0.72f, 0.20f, 0.24f, 1.0f};
    const LngColor leave_hov = {0.84f, 0.28f, 0.32f, 1.0f};
    const LngColor leave_act = {0.58f, 0.14f, 0.18f, 1.0f};
    ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y));
    ImGui::PushStyleColor(ImGuiCol_Button, col(leave_bg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(leave_hov));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(leave_act));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(ui_text("Leave Lobby"), ImVec2(leave_w, play_h)))
        np_lobby_leave(m, np);
    ImGui::PopStyleColor(4);

    /* Settings — the room address, match settings and mod plan. The host
     * edits there; a guest gets the same page read-only, which is where it
     * finds the room's address and what the host has chosen. */
    const float settings_w = px(130);
    float next_x = origin.x + leave_w + gap;
    ImGui::SetCursorScreenPos(ImVec2(next_x, cta_y));
    if (ImGui::Button(s.is_host ? ui_text("Settings") : ui_text("Room Info"),
                      ImVec2(settings_w, play_h))) {
        g_lobby_settings_synced = false;
        m->netplay_lobby_settings_open = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(s.is_host
            ? "Room address, match settings, and the mod plan"
            : "Room address and the settings the host chose");
    next_x += settings_w + gap;
#if RECOMP_UI_ENABLE_MODS
    if (m->mods) {
        ImGui::SetCursorScreenPos(ImVec2(next_x, cta_y));
        if (ImGui::Button(s.is_host ? ui_text("Mods") : ui_text("View Mods"),
                          ImVec2(mods_w, play_h)))
            m->netplay_lobby_mods_open = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(s.is_host
                ? "Pick the mods everyone in this lobby will run"
                : "See the mods the host has enabled for this lobby");
        next_x += mods_w + gap;
    }
#else
    (void)mods_w;
#endif
    (void)next_x;

    if (s.is_host) {
        /* Require two seated players, without waiting for every open seat.
         * Count only visible game slots: a host sitting alone in P2 after a
         * seat swap must not satisfy the start gate. */
        const bool can_start = s.seated_players >= 2 && s.peers_not_ready == 0;
        const char* blocked_why =
            s.seated_players < 2
                ? "Waiting for another player to join"
                : (s.peers_not_ready > 0
                       ? "Waiting for every player to install this "
                         "lobby's mods"
                       : nullptr);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + fullw - play_w, cta_y));
        if (neon_cta("##lobby_play", ui_text("PLAY"), ImVec2(play_w, play_h),
                     can_start))
            np_lobby_start(m, np);
        /* Tooltips do not fire on a disabled item unless we allow it. */
        else if (blocked_why &&
                 ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", blocked_why);
    } else {
        ImGui::SetCursorScreenPos(ImVec2(
            origin.x + fullw - play_w,
            cta_y + (play_h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(col(th.text_muted), "Waiting for the host to start…");
    }
    (void)th;
}

void np_join_selected(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (!np) return;
    if (m->netplay_selected_lobby < 0) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Select a lobby from the server list.");
        return;
    }
    RecompLauncherCNetplayLobby row{};
    if (!np->list_get || !np->list_get(np->ctx, m->netplay_selected_lobby, &row)) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Selected lobby is no longer available.");
        return;
    }
    if (row.has_password) {
        m->netplay_password[0] = '\0';
        m->netplay_password_modal_open = true;
    } else if (np->join) {
        char guest_bind[64];
        if (!np_prepare_guest_bind(guest_bind, sizeof(guest_bind),
                                   m->netplay_status, sizeof(m->netplay_status)))
            return;
        const int rc = np->join(np->ctx, row.lobby_id, "", guest_bind);
        if (rc == 0) {
            if (strncmp(row.lobby_id, "lan:", 4) == 0) {
                m->netplay_local_room = true;
                std::snprintf(m->netplay_host_endpoint, sizeof(m->netplay_host_endpoint),
                              "%s", row.lobby_id + 4);
            } else {
                m->netplay_local_room = false;
                m->netplay_host_endpoint[0] = '\0';
            }
            m->netplay_lobby_max_slots =
                row.max_slots >= 2 ? row.max_slots : np_game_max_players(m);
        } else if (rc == -3) {
            std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                          "No LAN/Direct IP lobby at that address. If the host "
                          "is online, join from the server list.");
        } else if (rc != 0) {
            std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                          "Could not join lobby.");
        }
    }
}

/* "PLAYERS ONLINE": everyone connected to the lobby server, flag before the
 * name, with where they are -- a room name (hosting or seated) or nothing
 * for a browser. Reads the backend each frame; the server refreshes the
 * list about once a second with the lobby list. */
static void draw_netplay_online_panel(LauncherModel* m, const LauncherTheme& th,
                                      const RecompLauncherCNetplayCallbacks* np) {
    (void)m;
    const int n = np->online_count(np->ctx);
    ImGui::TextColored(col(th.accent2), "PLAYERS ONLINE");
    ImGui::SameLine();
    ImGui::TextColored(col(th.text_muted), "%d", n);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Players on the lobby server for this game.");
    ImGui::Spacing();
    if (n <= 0) {
        ImGui::TextColored(col(th.text_muted), "Nobody else is online for this game.");
        return;
    }
    const float row_h = px(26);
    const float text_h = ImGui::GetTextLineHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8), px(3)));
    if (ImGui::BeginTable("netplay_online_table", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, px(72));
        for (int i = 0; i < n; ++i) {
            RecompLauncherCNetplayOnlinePlayer p{};
            if (!np->online_get(np->ctx, i, &p)) continue;
            /* Blocked players are not shown at all -- that is what block
             * means here. Never the local row, whatever the file says. */
            if (!p.is_local && recomp_moderation_is_blocked(p.account)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            ImGui::TableSetColumnIndex(0);
            table_row_vcenter(row_h, text_h);
            np_draw_country_flag(th, p.country);
            char disp[96];
            emoji_display(p.display_name[0] ? p.display_name : "Player", disp,
                          sizeof(disp));
            if (p.is_local) {
                ImGui::TextColored(col(th.accent2), "%s", disp);
                ImGui::SameLine(0, px(4));
                ImGui::TextColored(col(th.text_muted), "(you)");
            } else {
                ImGui::TextUnformatted(disp);
                /* Right-click the NAME, which is the thing a player points at
                 * when they mean "that person". Never on your own row. */
                np_player_menu(m, th, p.account, p.display_name);
                if (recomp_moderation_is_ignored(p.account)) {
                    ImGui::SameLine(0, px(4));
                    ImGui::TextColored(col(th.text_muted), "(ignored)");
                }
            }
            ImGui::TableSetColumnIndex(1);
            table_row_vcenter(row_h, text_h);
            /* A word, not the room name: the panel is narrow and a name
             * would clip. The room is the tooltip. */
            if (p.in_lobby) {
                ImGui::TextColored(col(p.hosting ? th.good : th.text),
                                   p.hosting ? "Hosting" : "In lobby");
                if (ImGui::IsItemHovered() && p.lobby_name[0]) {
                    char where[96];
                    emoji_display(p.lobby_name, where, sizeof(where));
                    ImGui::SetTooltip("%s", where);
                }
            } else {
                ImGui::TextColored(col(th.text_muted), "Browsing");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

/* ---- the netplay fork --------------------------------------------------
 * NETPLAY lands here, not on the lobby browser. The two kinds of netplay
 * want different things and always did -- a LAN player has no use for a
 * lobby list, and an online player has no use for a direct-IP box -- but the
 * page used to show both to everyone and let them work it out.
 *
 * Signing in belongs on THIS side of the fork: it is only needed for online
 * play, so a LAN player is never asked for a Discord account, and neither is
 * anyone whose server offers no logins. */

/* The name other players actually see for THIS client -- and it is not one
 * value.
 *
 * Online it is the handle the SERVER owns, tied to the Discord account and
 * defaulted from the Discord name; that is what appears in the seat table,
 * the players-online panel and every chat line, because the server publishes
 * it rather than trusting whatever the client sent. On LAN there is no
 * account and no server, so it is the locally typed player name.
 *
 * Keeping them apart is the point: signing in must not overwrite the name
 * somebody uses for LAN games, and renaming for LAN must not rename the
 * account. The online one is stored server-side rather than here, which is
 * also what lets it follow the player to another device with their key. */
static const char* np_effective_name(LauncherModel* m) {
    const auto* np = np_cb(m);
    if (np && np->account_state && np->account_handle &&
        np->account_state(np->ctx) == RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN) {
        const char* h = np->account_handle(np->ctx);
        if (h && h[0]) return h;
    }
    return m->s.netplay_player_name;
}

/* Open the name editor seeded from whichever name it is about to edit. The
 * openers used to stuff the LAN name in unconditionally, so a signed-in
 * player opening it saw their LAN name over the top of their account. */
static void np_open_name_modal(LauncherModel* m) {
    std::snprintf(m->netplay_name_edit, sizeof(m->netplay_name_edit), "%s",
                  np_effective_name(m));
    m->netplay_name_error[0] = '\0';
    m->netplay_name_modal_open = true;
}

/* True when the player is already signed in, so the sign-in page is skipped. */
static bool np_account_signed_in(LauncherModel* m) {
    const auto* np = np_cb(m);
    return np && np->account_state &&
           np->account_state(np->ctx) == RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN;
}

/* True when this build and this server can sign anybody in at all. When they
 * cannot, ONLINE still works -- as a guest, exactly as it always has. */
static bool np_account_offered(LauncherModel* m) {
    const auto* np = np_cb(m);
    return np && np->account_state && np->account_login_begin &&
           np->account_available && np->account_available(np->ctx);
}

/* Go to the lobby browser, in the mode the player chose. */
static void np_enter_netplay(LauncherModel* m, int mode) {
    m->netplay_mode = mode;
    m->netplay_list_fresh = false;
    if (mode == 2) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Connecting to lobby server…");
    } else {
        m->netplay_status[0] = '\0';
    }
    launcher_model_set_view(m, LNG_VIEW_NETPLAY);
}

/* One big choice card. Sized to be hit with a controller, not just a mouse. */
static bool np_mode_card(const LauncherTheme& th, const char* id, const char* title,
                         const char* body, float w, bool accent) {
    ImGui::PushID(id);
    const float h = px(150);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      imcol(hot ? th.panel_hovered : th.panel), px(10));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                imcol(hot ? (accent ? th.accent : th.focus_ring) : th.border),
                px(10), 0, hot ? 2.0f : 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + px(20), p.y + px(18)));
    ImGui::PushStyleColor(ImGuiCol_Text, col(accent ? th.accent2 : th.text));
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p.x + px(20), p.y + px(50)));
    /* PushTextWrapPos takes a WINDOW-LOCAL x, and this was handing it a screen
     * one (p.x is from GetCursorScreenPos). The two differ by the window's
     * origin, so the wrap boundary sat that many pixels to the RIGHT of the
     * card and the body text ran to the border and past it.
     *
     * Derived from the cursor instead, which is already local, so it cannot
     * drift from where the text actually starts. The right inset is a little
     * wider than the left on purpose: a wrap point is where a word may still
     * begin, so the last glyph of a long word lands beyond it. */
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (w - px(20) - px(28)));
    ImGui::TextColored(col(th.text_muted), "%s", body);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + px(14)));
    ImGui::PopID();
    return pressed;
}

void draw_netplay_mode_page(LauncherModel* m, const LauncherTheme& th) {
    /* Pump here too, not only on the browser page. The backend does its own
     * lazy setup from the pump -- parsing the lobby host, loading a stored
     * device key, redeeming it -- and this page asks it questions ("is
     * sign-in offered?", "are we signed in?") before the browser page has
     * ever been drawn. Without this the first visit answered from an
     * uninitialised backend. */
    {
        const auto* npp = np_cb(m);
        if (npp && npp->pump) npp->pump(npp->ctx);
    }
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float card_w = avail_w > px(900) ? px(420) : (avail_w - px(30)) * 0.5f;

    ImGui::TextColored(col(th.accent2), "NETPLAY");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(avail_w > px(760) ? px(760) : avail_w);
    ImGui::TextColored(col(th.text_muted),
                       "How do you want to play? You can change this any time "
                       "by coming back here.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(18)));

    const ImVec2 row = ImGui::GetCursorScreenPos();
    if (np_mode_card(th, "lan", "LAN / Direct IP",
                     "Play with someone on your network, or connect straight to "
                     "an address they give you. No account, no lobby server.",
                     card_w, false)) {
        np_enter_netplay(m, 1);
    }

    ImGui::SetCursorScreenPos(ImVec2(row.x + card_w + px(24), row.y));
    const bool offered = np_account_offered(m);
    const auto* npa = np_cb(m);
    const int acct = (npa && npa->account_state) ? npa->account_state(npa->ctx)
                                                 : RECOMP_LAUNCHER_ACCOUNT_GUEST;

    /* Hold ONLINE until the quiet sign-in has actually answered.
     *
     * The pump above redeems a stored netplay_secret in the background, and
     * that takes a round-trip. Clicking during it read as "not signed in" and
     * sent the player to the Discord sign-in page -- to sign in again, on a
     * machine that already had a valid credential and was a moment away from
     * using it. WAITING is exactly "an answer is coming"; GUEST after the
     * pump means there was no secret to redeem, and that needs no wait.
     *
     * Bounded, because an unanswerable network must not lock anyone out of
     * netplay entirely. Five seconds is generous for one HTTP round-trip with
     * nobody in the loop -- unlike the sign-in page's ten, which is waiting on
     * a human in a browser. After that the card unlocks and says the sign-in
     * is still going; the player can go on as a guest, and if it lands later
     * the lobby picks up the name from the pump anyway. */
    static double quiet_signin_since = 0.0;
    const double now_t = ImGui::GetTime();
    const bool awaiting = offered && acct == RECOMP_LAUNCHER_ACCOUNT_WAITING;
    if (!awaiting) quiet_signin_since = 0.0;
    else if (quiet_signin_since <= 0.0) quiet_signin_since = now_t;
    const bool stalled = awaiting && (now_t - quiet_signin_since > 5.0);
    const bool hold_online = awaiting && !stalled;

    const char* online_body =
        !offered ? "Find players on the lobby server. This server does not "
                   "offer sign-in, so you will play as a guest."
        : hold_online ? "Checking your saved sign-in\u2026"
        : "Find players on the lobby server. Sign in with Discord "
          "so your name is yours across sessions.";

    ImGui::BeginDisabled(hold_online);
    const bool online_hit = np_mode_card(th, "online", "Online Netplay",
                                         online_body, card_w, true);
    ImGui::EndDisabled();
    if (online_hit) {
        /* Skip the sign-in page when there is nothing to sign in to, or when
         * this machine is already signed in -- which is the normal case after
         * the first time, because the stored device key is redeemed silently
         * at startup. */
        if (!offered || np_account_signed_in(m)) {
            np_enter_netplay(m, 2);
        } else {
            launcher_model_set_view(m, LNG_VIEW_NETPLAY_SIGNIN);
        }
    }

    /* Say which of the three it is, rather than only the happy one. A player
     * who is a guest because the redemption FAILED was previously told
     * nothing at all, and had no way to tell that apart from never having
     * signed in. */
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + px(190)));
    if (offered && acct == RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN) {
        const char* h = npa->account_handle ? npa->account_handle(npa->ctx) : "";
        char disp[128];
        emoji_display(h && h[0] ? h : "", disp, sizeof(disp));
        ImGui::TextColored(col(th.text_muted), "Signed in as");
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(col(th.good), "%s", disp);
    } else if (awaiting) {
        ImGui::TextColored(col(th.text_muted),
                           stalled ? "Still checking your saved sign-in \u2014 you can "
                                     "continue as a guest."
                                   : "Checking your saved sign-in\u2026");
    } else if (offered && acct == RECOMP_LAUNCHER_ACCOUNT_FAILED) {
        const char* err = npa->account_error ? npa->account_error(npa->ctx) : "";
        ImGui::PushTextWrapPos(px(760));
        ImGui::TextColored(col(th.warn), "Not signed in%s%s",
                           (err && err[0]) ? " \u2014 " : "",
                           (err && err[0]) ? err : "");
        ImGui::PopTextWrapPos();
    }
}

/* The sign-in page. Only ever reached on the way to online play. */
/* When the current "waiting for Discord" began, so the page can offer a way
 * out if it drags. Zero means "not waiting". */
static double s_signin_waiting_since = 0.0;

void draw_netplay_signin_page(LauncherModel* m, const LauncherTheme& th) {
    const auto* np = np_cb(m);
    /* Same reason as the chooser, plus this page needs the backend live to
     * notice the sign-in completing. */
    if (np && np->pump) np->pump(np->ctx);
    const int st = (np && np->account_state) ? np->account_state(np->ctx)
                                             : RECOMP_LAUNCHER_ACCOUNT_GUEST;
    const float wrap = px(640);

    /* Signed in -- by this page, or by the device key redeeming in the
     * background while the player was reading it. Either way there is nothing
     * left to do here. */
    if (st == RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN) {
        np_enter_netplay(m, 2);
        return;
    }

    ImGui::TextColored(col(th.accent2), "SIGN IN");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextColored(col(th.text),
                       "Signing in with Discord gives you a name that is yours "
                       "across sessions and devices, on this server and any "
                       "other that uses it.");
    ImGui::Spacing();
    ImGui::TextColored(col(th.text_muted),
                       "Online play on this server needs an account. LAN and "
                       "Direct IP do not — go back and pick those to play "
                       "without signing in.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(16)));

    if (st == RECOMP_LAUNCHER_ACCOUNT_WAITING) {
        /* Offer a way out after a few seconds, but keep waiting underneath:
         * a person needs longer than this to actually sign in, so the button
         * is an escape from a login that went wrong, not a deadline. */
        const double now = ImGui::GetTime();
        if (s_signin_waiting_since <= 0.0) s_signin_waiting_since = now;
        const bool stalled = now - s_signin_waiting_since > 10.0;

        ImGui::TextColored(col(th.accent2), "Waiting for Discord…");
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextColored(col(th.text_muted),
                           "Finish signing in on the page that opened in your "
                           "browser. This screen will move on by itself.");
        if (stalled) {
            ImGui::Spacing();
            ImGui::TextColored(col(th.text_muted),
                               "Still waiting. If the browser never opened, or "
                               "you closed the page, start again:");
        }
        ImGui::PopTextWrapPos();
        if (stalled) {
            ImGui::Spacing();
            if (ImGui::Button("Retry Discord Sign In", ImVec2(px(240), px(36))) &&
                np && np->account_login_begin) {
                s_signin_waiting_since = now; /* the new attempt gets its own clock */
                np->account_login_begin(np->ctx);
            }
        }
    } else {
        /* Not waiting: reset the clock so the next attempt starts fresh. */
        s_signin_waiting_since = 0.0;
        if (ImGui::Button("Sign in with Discord", ImVec2(px(240), px(40))) &&
            np && np->account_login_begin) {
            np->account_login_begin(np->ctx);
        }
        if (st == RECOMP_LAUNCHER_ACCOUNT_FAILED && np && np->account_error) {
            const char* e = np->account_error(np->ctx);
            if (e && e[0]) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(wrap);
                ImGui::TextColored(col(th.warn), "%s", e);
                ImGui::PopTextWrapPos();
            }
        }
    }

    ImGui::Dummy(ImVec2(0, px(20)));
    /* No "continue as guest". Online play means signing in, when this server
     * offers sign-in at all -- a server with no Discord configured never
     * reaches this page, because the chooser sends it straight through. Back
     * is the way out, and it leads to LAN, which needs no account. */
    if (ImGui::Button("Back", ImVec2(px(120), px(34))))
        launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
}

void draw_netplay(LauncherModel* m, const LauncherTheme& th) {
    const auto* np = np_cb(m);
    if (!np) return;
    static bool network_settings_loaded = false;
    if (!network_settings_loaded) {
        network_settings_loaded = true;
        np_load_network_settings(m);
        np_refresh_host_ip(m);
    }
    /* Only nag for a name when there is genuinely none. A signed-in player
     * already has one -- the server's -- so asking them to invent a second is
     * the bug this used to cause. */
    if (!np_effective_name(m)[0] && !m->netplay_name_modal_open &&
        !m->netplay_name_prompted) {
        m->netplay_name_prompted = true;
        np_open_name_modal(m);
    }
    if (!m->netplay_list_fresh)
        np_refresh_lobby_list(m);
    if (np->pump) np->pump(np->ctx);
    np_ingest_last_error(m, np);
    if (np->launch_pending && np->launch_pending(np->ctx))
        np_try_launch(m);

    const bool np_online = np->connected && np->connected(np->ctx);
    const bool np_connecting = np->connecting && np->connecting(np->ctx);
    if (np_online && m->netplay_status[0] &&
        std::strncmp(m->netplay_status, "Connecting", 10) == 0) {
        m->netplay_status[0] = '\0';
    } else if (!np_online && !np_connecting && m->netplay_list_fresh &&
               m->netplay_status[0] &&
               std::strncmp(m->netplay_status, "Connecting", 10) == 0) {
        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                      "Could not reach lobby server.");
    }

    /* Two columns when the backend reports who is online: the lobby table
     * on the left, the players panel on the right. A LAN-only backend has
     * no presence and keeps the full width. */
    /* A LAN player picked LAN. The lobby server's half of this page -- the
     * players-online panel and the per-game server chat -- is about people on
     * a server they are not using, so it is not drawn. The lobby list, Host
     * Lobby and Join Direct stay: those are how a LAN room is made. */
    const bool lan_mode = m->netplay_mode == 1;
    const bool has_online = !lan_mode && np->online_count && np->online_get;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float side_gap = px(20);
    const float side_w = px(300);
    const bool two_col = has_online && avail_w >= side_w + side_gap + px(560);
    const float list_w = two_col ? avail_w - side_w - side_gap : avail_w;
    /* The per-game server chat takes a band across the bottom; the list and
     * the players panel share what is above it. */
    const bool has_schat = !lan_mode && np_server_chat_available(np);
    const float schat_h = px(230);
    const float schat_gap = px(10);
    const float avail_h = ImGui::GetContentRegionAvail().y;
    /* Stack: top row + gap + band must fit exactly, or the page grows a
     * scrollbar and the band's input row slides under the footer. The gap
     * is drawn as a Dummy between two children, so it carries two item
     * spacings of its own. */
    const float item_sp = ImGui::GetStyle().ItemSpacing.y;
    const float schat_gap_total = schat_gap > 2.0f * item_sp ? schat_gap : 2.0f * item_sp;
    const float top_h = has_schat && avail_h > schat_h + px(200)
                            ? avail_h - schat_h - schat_gap_total - px(2)
                            : 0.0f;

    begin_container("netplay_lobbies", ImVec2(list_w, top_h), ImGuiChildFlags_None);
    ImGui::TextColored(col(th.accent2), "LOBBIES");
    if (m->netplay_status[0])
        ImGui::TextColored(col(th.warn), "%s", m->netplay_status);
    ImGui::Spacing();
    np_push_blocks(m);
    /* Tell the backend which fork the player took before asking it anything.
     * It merges its LAN registry and beacon rows with the server's list and
     * has no other way to know: a LAN player was being shown online rooms
     * they had no connection for, and an online player was shown LAN rooms
     * from their own machine. */
    if (np->list_scope_set) {
        np->list_scope_set(np->ctx,
                           m->netplay_mode == 1 ? RECOMP_LAUNCHER_LIST_SCOPE_LAN
                                                : RECOMP_LAUNCHER_LIST_SCOPE_ONLINE);
    }
    int rows = np->list_count ? np->list_count(np->ctx) : 0;
    /* Slim rows: a browser is a list to scan, not a form. The Join button
     * sets the floor. */
    const float join_btn_w = px(64);
    const float join_btn_h = px(24);
    const float lobby_row_h = px(32);
    const float text_h = ImGui::GetTextLineHeight();
    /* Extra left inset so Lobby column text isn't flush with the panel edge. */
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(14), px(3)));
    if (ImGui::BeginTable("netplay_lobby_table", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_SizingStretchProp)) {
        /* Lobby stretches; the counts, latency and Join stay fixed. The game
         * is implied -- the list is already filtered to this title. */
        ImGui::TableSetupColumn("Lobby", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, px(64));
        ImGui::TableSetupColumn("Spectators", ImGuiTableColumnFlags_WidthFixed, px(82));
        ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, px(64));
        ImGui::TableSetupColumn("Join", ImGuiTableColumnFlags_WidthFixed, px(80));
        ImGui::TableHeadersRow();
        if (rows <= 0) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, lobby_row_h);
            ImGui::TableSetColumnIndex(0);
            table_row_vcenter(lobby_row_h, text_h);
            ImGui::Text("No lobbies yet - host one.");
            for (int c = 1; c < 5; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted("");
            }
        }
        for (int i = 0; i < rows; ++i) {
            RecompLauncherCNetplayLobby row{};
            if (!np->list_get || !np->list_get(np->ctx, i, &row)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, lobby_row_h);
            ImGui::TableSetColumnIndex(0);
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            const bool selected = (m->netplay_selected_lobby == i);
            /* Anonymous selectable for hit-testing only — a labeled Selectable
             * was rendering like a full-width text field ("Lobby"). */
            if (ImGui::Selectable("##lobby_row", selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap |
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, lobby_row_h))) {
                m->netplay_selected_lobby = i;
                m->netplay_status[0] = '\0';
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    np_join_selected(m);
            }
            ImGui::SetCursorScreenPos(row_pos);
            table_row_vcenter(lobby_row_h, text_h);
            np_draw_country_flag(th, row.host_country);
            char lobby_label[96];
            std::snprintf(lobby_label, sizeof(lobby_label), "%s%s",
                          row.name[0] ? row.name : "Unnamed lobby",
                          row.lobby_kind == 1
                              ? (row.has_password ? "  [PSX-Link] [locked]"
                                                  : "  [PSX-Link]")
                              : (row.has_password ? "  [locked]" : ""));
            ImGui::TextUnformatted(lobby_label);
            ImGui::TableSetColumnIndex(1);
            table_row_vcenter(lobby_row_h, text_h);
            ImGui::Text("%d/%d", row.player_count, row.max_slots);
            ImGui::TableSetColumnIndex(2);
            table_row_vcenter(lobby_row_h, text_h);
            /* "No" when the host opened no gallery; else watching/seats. */
            if (row.allow_spectators && row.max_spectators > 0)
                ImGui::Text("%d/%d", row.spectator_count, row.max_spectators);
            else
                ImGui::TextColored(col(th.text_muted), "No");
            ImGui::TableSetColumnIndex(3);
            table_row_vcenter(lobby_row_h, text_h);
            if (row.latency_ms >= 0)
                ImGui::Text("%d ms", row.latency_ms);
            else
                ImGui::TextColored(col(th.text_muted), "—");
            ImGui::TableSetColumnIndex(4);
            {
                ImVec2 cell = ImGui::GetCursorScreenPos();
                const float avail_x = ImGui::GetContentRegionAvail().x;
                /* Same origin and formula as table_row_vcenter for the text
                 * cells, so the button and the text share one centre line. */
                ImGui::SetCursorScreenPos(ImVec2(
                    cell.x + (avail_x - join_btn_w) * 0.5f,
                    row_pos.y + (lobby_row_h - join_btn_h) * 0.5f));
                /* A button shorter than text + 2*FramePadding pins its label
                 * to the top padding instead of centring it; give the slim
                 * button a padding that fits. */
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(ImGui::GetStyle().FramePadding.x,
                                           (join_btn_h - text_h) * 0.5f));
                ImGui::BeginDisabled(!launcher_model_netplay_disc_ok(m));
                if (ImGui::Button("Join", ImVec2(join_btn_w, join_btn_h))) {
                    if (!launcher_model_netplay_disc_ok(m)) {
                        std::snprintf(m->netplay_status, sizeof(m->netplay_status),
                                      "%s",
                                      m->verify.netplay_detail[0]
                                          ? m->verify.netplay_detail
                                          : "Mount the supported .cue dump before joining.");
                    } else {
                        m->netplay_selected_lobby = i;
                        m->netplay_status[0] = '\0';
                        np_join_selected(m);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    end_container();

    if (two_col) {
        ImGui::SameLine(0, side_gap);
        begin_container("netplay_online", ImVec2(side_w, top_h), ImGuiChildFlags_None);
        draw_netplay_online_panel(m, th, np);
        end_container();
    }
    if (has_schat) {
        ImGui::Dummy(ImVec2(0, schat_gap_total - 2.0f * item_sp));
        begin_container("netplay_server_chat", ImVec2(avail_w, schat_h),
                        ImGuiChildFlags_None);
        draw_server_chat(m, th, np);
        end_container();
    }
}

static bool mod_text_matches(const char* search, const RecompLauncherCModPackage& package) {
    if (!search || !search[0]) return true;
    std::string needle(search), haystack = std::string(package.name) + " " +
        package.id + " " + package.author + " " + package.description;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return haystack.find(needle) != std::string::npos;
}

/* One channel vocabulary, used everywhere a feature is named.
 *
 * A stable feature gets NO tag. The absence is the stable case, and tagging
 * every row would bury the two that actually need saying something. The short
 * form is for the list, where rows are 330px wide and already carry a name and
 * a package; the long form and the explanation are for the detail pane. Same
 * word stem and same colour in both, so the two read as one marker. */
static const char* mod_channel_tag_short(int channel) {
    switch (channel) {
        case RECOMP_MOD_CHANNEL_EXPERIMENTAL: return ui_text("EXP");
        case RECOMP_MOD_CHANNEL_DEVELOPER:    return ui_text("DEV");
        default: return NULL;
    }
}

static const char* mod_channel_tag(int channel) {
    switch (channel) {
        case RECOMP_MOD_CHANNEL_EXPERIMENTAL: return ui_text("EXPERIMENTAL");
        case RECOMP_MOD_CHANNEL_DEVELOPER:    return ui_text("DEVELOPER");
        default: return NULL;
    }
}

static const char* mod_channel_blurb(int channel) {
    switch (channel) {
        case RECOMP_MOD_CHANNEL_EXPERIMENTAL:
            return ui_text("Works, but has not been validated on this game. "
                           "Expect rough edges, and turn it off if something "
                           "looks wrong.");
        case RECOMP_MOD_CHANNEL_DEVELOPER:
            return ui_text("A work-in-progress instrument rather than a player "
                           "feature. Released builds do not include it -- you "
                           "are seeing it because this is a local build.");
        default:
            return NULL;
    }
}

/* Amber is the theme's documented "unverified / caution" colour, which is
 * exactly what experimental means. Developer is not a caution, it is a
 * category, so it takes the secondary accent instead. */
static LngColor mod_channel_color(int channel, const LauncherTheme& th) {
    return channel == RECOMP_MOD_CHANNEL_DEVELOPER ? th.accent2 : th.warn;
}

static bool mod_feature_text_matches(const char* search,
                                     const RecompLauncherCModFeature& feature) {
    if (!search || !search[0]) return true;
    std::string needle(search), haystack = std::string(feature.name) + " " +
        feature.id + " " + feature.group + " " + feature.author + " " +
        feature.description + " " + feature.package_name + " " +
        feature.package_id + " " + feature.status;
    /* Searching "experimental" should find the experimental features. */
    if (const char* channel_tag = mod_channel_tag(feature.channel))
        haystack += std::string(" ") + channel_tag;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return haystack.find(needle) != std::string::npos;
}

static void mod_note_error(LauncherModel* m) {
    const auto* mods = m ? m->mods : nullptr;
    const char* error = mods && mods->last_error ? mods->last_error(mods->ctx) : nullptr;
    std::snprintf(m->mod_status, sizeof(m->mod_status), "%s",
                  error && error[0] ? error : "The mod operation failed.");
}

static bool mod_commit_launch(LauncherModel* m) {
    if (!m || !m->mods || !m->mods->commit ||
        m->mods->commit(m->mods->ctx, launcher_model_effective_rom_path(m))) {
        return true;
    }
    mod_note_error(m);
    return false;
}

struct ModIntegerEditState {
    int64_t value = 0;
    std::string provider_value;
};

static void draw_linkified_mod_author(
    const char* author_text,
    const RecompLauncherCModAuthorLink* author_links,
    int author_link_count,
    const LauncherTheme& th) {
    if (!author_text || !author_text[0]) return;
    const std::string author(author_text);
    size_t cursor = 0;
    ImGui::TextColored(col(th.text_muted), "by: ");
    ImGui::SameLine(0, 0);
    while (cursor < author.size()) {
        size_t next = std::string::npos;
        const RecompLauncherCModAuthorLink* link = nullptr;
        for (int i = 0; i < author_link_count; ++i) {
            const auto& candidate = author_links[i];
            if (!candidate.name[0] || !candidate.url[0]) continue;
            const size_t found = author.find(candidate.name, cursor);
            if (found < next) {
                next = found;
                link = &candidate;
            }
        }
        if (!link) {
            ImGui::TextColored(col(th.text_muted), "%s", author.c_str() + cursor);
            break;
        }
        if (next > cursor) {
            const std::string prefix = author.substr(cursor, next - cursor);
            ImGui::TextColored(col(th.text_muted), "%s", prefix.c_str());
            ImGui::SameLine(0, 0);
        }
        ImGui::TextLinkOpenURL(link->name, link->url);
        cursor = next + std::strlen(link->name);
        if (cursor < author.size()) ImGui::SameLine(0, 0);
    }
}

/* Free-text option row shared by the package and feature option paths.
 * Immediate-mode text editing with external state uses ONE shared edit
 * buffer: every inactive row re-mirrors its model value before drawing
 * (so it always displays truth), while the row that owns the keyboard
 * keeps its typing state. Commit happens when the field deactivates after
 * an edit; the caller pushes the committed text through set_option, whose
 * rejection reverts the row to the model value on the next frame. */
static bool draw_mod_text_option(const RecompLauncherCModOption& option,
                                 char* next, size_t next_size) {
    ImGui::TextUnformatted(option.label);
    ImGui::SameLine(px(260));
    ImGui::SetNextItemWidth(px(230));
    static char edit[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    static ImGuiID owner = 0;
    const ImGuiID id = ImGui::GetID("##text");
    if (owner != id)
        std::snprintf(edit, sizeof(edit), "%s", option.value);
    ImGui::InputText("##text", edit, sizeof(edit));
    if (ImGui::IsItemActivated()) owner = id;
    bool changed = false;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::snprintf(next, next_size, "%s", edit);
        changed = true;
    }
    if (ImGui::IsItemDeactivated() && owner == id) owner = 0;
    return changed;
}

static bool draw_mod_integer_option(const RecompLauncherCModOption& option,
                                    char* next, size_t next_size) {
    ImGui::TextUnformatted(option.label);
    ImGui::SameLine(px(260));
    ImGui::SetNextItemWidth(px(230));

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(option.value, &end, 10);
    int64_t provider_value =
        errno == 0 && end && *end == '\0'
            ? static_cast<int64_t>(parsed)
            : option.min_value;
    provider_value = std::max(
        option.min_value, std::min(option.max_value, provider_value));

    static std::unordered_map<ImGuiID, ModIntegerEditState> edits;
    const ImGuiID id = ImGui::GetID("##integer");
    ModIntegerEditState& edit = edits[id];
    if (edit.provider_value != option.value) {
        edit.value = provider_value;
        edit.provider_value = option.value;
    }

    const int64_t step = option.step > 0 ? option.step : 1;
    const bool edited = ImGui::InputScalar(
        "##integer", ImGuiDataType_S64, &edit.value, &step, nullptr, nullptr,
        ImGuiInputTextFlags_None);
    const bool commit = ImGui::IsItemDeactivatedAfterEdit() ||
                        (edited && !ImGui::IsItemActive());
    if (!commit) return false;

    edit.value = std::max(
        option.min_value, std::min(option.max_value, edit.value));
    const uint64_t distance =
        static_cast<uint64_t>(edit.value) -
        static_cast<uint64_t>(option.min_value);
    edit.value -= static_cast<int64_t>(
        distance % static_cast<uint64_t>(step));
    std::snprintf(next, next_size, "%lld",
                  static_cast<long long>(edit.value));
    edit.provider_value = next;
    return true;
}

static void draw_mod_packages(LauncherModel* m, const LauncherTheme& th) {
    const auto* mods = m ? m->mods : nullptr;
    if (!mods || !mods->package_count || !mods->package_get) return;
    const bool feature_provider =
        mods->feature_count && mods->feature_get &&
        mods->feature_option_get && mods->feature_enable &&
        mods->feature_set_option;

    const char* archive_extension =
        mods->archive_extension && mods->archive_extension[0]
            ? mods->archive_extension : ".psxmod";
    const char* archive_description =
        mods->archive_description && mods->archive_description[0]
            ? mods->archive_description
            : "PSXRecomp mod package (.psxmod)";
    char install_label[96];
    char archive_pattern[64];
    std::snprintf(install_label, sizeof(install_label),
                  "%s %s", ui_text("Install"), archive_extension);
    std::snprintf(archive_pattern, sizeof(archive_pattern),
                  "*%s", archive_extension);
    if (ImGui::Button(install_label)) {
        ui_pick_file(m, ui_text("Install Mod Package"), {archive_pattern},
                     archive_description, [m, mods](const char* path) {
            if (!path) return;
            if (!mods->install_archive || !mods->install_archive(mods->ctx, path))
                mod_note_error(m);
            else
                std::snprintf(m->mod_status, sizeof(m->mod_status),
                              "Package installed. Changes apply when you press PLAY.");
        });
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(300));
    ImGui::InputTextWithHint("##mod_search", "Search mods and options...",
                             m->mod_search, sizeof(m->mod_search));
    if (m->mod_status[0]) {
        ImGui::SameLine();
        ImGui::TextColored(col(th.warn), "%s", m->mod_status);
    }
    ImGui::Spacing();

    const float list_w = px(300);
    if (ImGui::BeginChild("##mod_list", ImVec2(list_w, 0), ImGuiChildFlags_Borders)) {
        const int count = mods->package_count(mods->ctx);
        int visible = 0;
        for (int i = 0; i < count; ++i) {
            RecompLauncherCModPackage package{};
            if (!mods->package_get(mods->ctx, i, &package) ||
                !mod_text_matches(m->mod_search, package)) continue;
            visible++;
            ImGui::PushID(i);
            const bool selected = m->mod_package_selected == i;
            char label[196];
            if (feature_provider) {
                std::snprintf(label, sizeof(label), "%s\n%s",
                              package.name, package.version);
            } else {
                std::snprintf(label, sizeof(label), "%s\n%s  %s",
                              package.name, package.version,
                              package.enabled ? "[enabled]" : "[disabled]");
            }
            if (ImGui::Selectable(label, selected, 0, ImVec2(0, px(52))))
                m->mod_package_selected = i;
            if (package.has_error)
                ImGui::TextColored(col(th.warn), "%s", package.status);
            ImGui::PopID();
        }
        if (!visible) ImGui::TextColored(col(th.text_muted), "No matching packages.");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##mod_detail", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        RecompLauncherCModPackage package{};
        if (mods->package_get(mods->ctx, m->mod_package_selected, &package)) {
            ImGui::TextColored(col(th.accent2), "%s", package.name);
            ImGui::SameLine();
            ImGui::TextColored(col(th.text_muted), "%s", package.version);
            if (package.author[0])
                draw_linkified_mod_author(
                    package.author, package.author_links,
                    package.author_link_count, th);
            if (package.description[0]) ImGui::TextWrapped("%s", package.description);
            if (package.source_url[0]) {
                ImGui::TextColored(col(th.text_muted), "Source: ");
                ImGui::SameLine(0, 0);
                ImGui::TextLinkOpenURL(
                    package.source_name[0] ? package.source_name : "Project page",
                    package.source_url);
            }
            if (package.license[0])
                ImGui::TextColored(col(th.text_muted), "License: %s", package.license);
            ImGui::Spacing();

            if (!feature_provider) {
                bool enabled = package.enabled != 0;
                if (ImGui::Checkbox(ui_text("Enabled"), &enabled)) {
                    if (!mods->set_enabled ||
                        !mods->set_enabled(mods->ctx, package.id, enabled ? 1 : 0))
                        mod_note_error(m);
                }
            }
            if (mods->version_count && mods->version_get && mods->select_version) {
                const int version_count = mods->version_count(mods->ctx, package.id);
                if (version_count > 1) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(px(170));
                    if (ImGui::BeginCombo("##mod_version", package.version)) {
                        for (int version_index = 0; version_index < version_count;
                             ++version_index) {
                            RecompLauncherCModVersion version{};
                            if (!mods->version_get(mods->ctx, package.id,
                                                   version_index, &version)) continue;
                            if (ImGui::Selectable(version.version,
                                                  version.selected != 0)) {
                                if (!mods->select_version(
                                        mods->ctx, package.id, version.version))
                                    mod_note_error(m);
                            }
                            if (version.selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Select an installed version (rollback)");
                }
            }
            if (package.removable) {
                ImGui::SameLine();
                if (ImGui::Button("Remove")) ImGui::OpenPopup("Remove mod package?");
                if (ImGui::BeginPopupModal("Remove mod package?", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped("Remove %s %s from this installation?",
                                       package.name, package.version);
        if (ImGui::Button(ui_text("Cancel"), ImVec2(px(110), 0)))
                        ImGui::CloseCurrentPopup();
                    ImGui::SameLine();
                    if (ImGui::Button("Remove", ImVec2(px(110), 0))) {
                        if (!mods->remove_package ||
                            !mods->remove_package(mods->ctx, package.id, package.version))
                            mod_note_error(m);
                        else if (m->mod_package_selected > 0)
                            m->mod_package_selected--;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::Separator();

            std::string last_group;
            for (int i = 0; !feature_provider && i < package.option_count; ++i) {
                RecompLauncherCModOption option{};
                if (!mods->option_get ||
                    !mods->option_get(mods->ctx, package.id, i, &option)) continue;
                if (m->mod_search[0]) {
                    RecompLauncherCModPackage searchable = package;
                    std::snprintf(searchable.name, sizeof(searchable.name), "%s", option.label);
                    std::snprintf(searchable.description, sizeof(searchable.description),
                                  "%s %s", option.description, option.group);
                    if (!mod_text_matches(m->mod_search, searchable)) continue;
                }
                if (last_group != option.group) {
                    last_group = option.group;
                    ImGui::Spacing();
                    ImGui::TextColored(col(th.accent), "%s",
                                       last_group.empty() ? "General" : last_group.c_str());
                    ImGui::Separator();
                }
                ImGui::PushID(option.id);
                bool changed = false;
                char next[RECOMP_LAUNCHER_MOD_VALUE_MAX];
                std::snprintf(next, sizeof(next), "%s", option.value);
                if (option.type == RECOMP_MOD_OPTION_BOOLEAN) {
                    bool value = std::strcmp(option.value, "true") == 0;
                    if (ImGui::Checkbox(option.label, &value)) {
                        std::snprintf(next, sizeof(next), "%s", value ? "true" : "false");
                        changed = true;
                    }
                } else if (option.type == RECOMP_MOD_OPTION_CHOICE) {
                    ImGui::TextUnformatted(option.label);
                    ImGui::SameLine(px(260));
                    ImGui::SetNextItemWidth(px(230));
                    char preview[128];
                    std::snprintf(preview, sizeof(preview), "%s", option.value);
                    for (int c = 0; c < option.choice_count; ++c) {
                        RecompLauncherCModChoice choice{};
                        if (mods->choice_get &&
                            mods->choice_get(mods->ctx, package.id, option.id,
                                             c, &choice) &&
                            std::strcmp(choice.value, option.value) == 0) {
                            std::snprintf(preview, sizeof(preview), "%s",
                                          choice.label[0] ? choice.label
                                                          : choice.value);
                            break;
                        }
                    }
                    if (ImGui::BeginCombo("##choice", preview)) {
                        for (int c = 0; c < option.choice_count; ++c) {
                            RecompLauncherCModChoice choice{};
                            if (!mods->choice_get ||
                                !mods->choice_get(mods->ctx, package.id, option.id, c, &choice))
                                continue;
                            const bool selected = std::strcmp(choice.value, option.value) == 0;
                            if (ImGui::Selectable(choice.label, selected)) {
                                std::snprintf(next, sizeof(next), "%s", choice.value);
                                changed = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else if (option.type == RECOMP_MOD_OPTION_TEXT) {
                    changed = draw_mod_text_option(
                        option, next, sizeof(next));
                } else {
                    changed = draw_mod_integer_option(
                        option, next, sizeof(next));
                }
                if (ImGui::IsItemHovered() && option.description[0])
                    ImGui::SetTooltip("%s", option.description);
                if (changed && (!mods->set_option ||
                    !mods->set_option(mods->ctx, package.id, option.id, next)))
                    mod_note_error(m);
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(col(th.text_muted),
                               "Install or select a package to configure it.");
        }
    }
    ImGui::EndChild();
}

struct ModFeatureListItem {
    int index;
    RecompLauncherCModFeature feature;
};

static bool mod_feature_less(const ModFeatureListItem& lhs,
                             const ModFeatureListItem& rhs) {
    const int group = std::strcmp(lhs.feature.group, rhs.feature.group);
    if (group != 0) return group < 0;
    const int name = std::strcmp(lhs.feature.name, rhs.feature.name);
    if (name != 0) return name < 0;
    const int package = std::strcmp(lhs.feature.package_id, rhs.feature.package_id);
    if (package != 0) return package < 0;
    return std::strcmp(lhs.feature.id, rhs.feature.id) < 0;
}

static void draw_mod_feature_option(LauncherModel* m,
                                    const RecompLauncherCModFeature& feature,
                                    const RecompLauncherCModOption& option) {
    const auto* mods = m->mods;
    ImGui::PushID(option.id);
    bool changed = false;
    char next[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    std::snprintf(next, sizeof(next), "%s", option.value);

    /* Another option currently overrides this one (e.g. "Instant" ticked makes
     * a speed box meaningless). Show it, greyed, so the player can see the
     * value they will get back when they untick -- hiding it would make the
     * row jump around and lose the setting from view. */
    const bool inert = option.disabled != 0;
    if (inert) ImGui::BeginDisabled();

    if (option.type == RECOMP_MOD_OPTION_BOOLEAN) {
        bool value = std::strcmp(option.value, "true") == 0;
        if (ImGui::Checkbox(option.label, &value)) {
            std::snprintf(next, sizeof(next), "%s", value ? "true" : "false");
            changed = true;
        }
    } else if (option.type == RECOMP_MOD_OPTION_CHOICE) {
        ImGui::TextUnformatted(option.label);
        ImGui::SameLine(px(260));
        ImGui::SetNextItemWidth(px(230));
        char preview[128];
        std::snprintf(preview, sizeof(preview), "%s", option.value);
        for (int choice_index = 0; choice_index < option.choice_count;
             ++choice_index) {
            RecompLauncherCModChoice choice{};
            if (mods->feature_choice_get &&
                mods->feature_choice_get(mods->ctx, feature.package_id,
                                          feature.id, option.id,
                                          choice_index, &choice) &&
                std::strcmp(choice.value, option.value) == 0) {
                std::snprintf(preview, sizeof(preview), "%s",
                              choice.label[0] ? choice.label : choice.value);
                break;
            }
        }
        if (ImGui::BeginCombo("##choice", preview)) {
            for (int choice_index = 0; choice_index < option.choice_count;
                 ++choice_index) {
                RecompLauncherCModChoice choice{};
                if (!mods->feature_choice_get ||
                    !mods->feature_choice_get(mods->ctx, feature.package_id,
                                              feature.id, option.id,
                                              choice_index, &choice)) {
                    continue;
                }
                const bool selected =
                    std::strcmp(choice.value, option.value) == 0;
                if (ImGui::Selectable(choice.label, selected)) {
                    std::snprintf(next, sizeof(next), "%s", choice.value);
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (option.type == RECOMP_MOD_OPTION_TEXT) {
        changed = draw_mod_text_option(option, next, sizeof(next));
    } else {
        changed = draw_mod_integer_option(option, next, sizeof(next));
    }

    const bool hovered = ImGui::IsItemHovered();
    if (inert) ImGui::EndDisabled();

    if (hovered && option.description[0])
        ImGui::SetTooltip("%s", option.description);
    /* A disabled control cannot report a change, but guard anyway so a future
     * widget that stays interactive can never write through an inert option. */
    if (changed && !inert &&
        !mods->feature_set_option(mods->ctx, feature.package_id, feature.id,
                                  option.id, next)) {
        mod_note_error(m);
    }
    ImGui::PopID();
}

static void draw_mod_feature_diagnostics(
    LauncherModel* m, const LauncherTheme& th,
    const RecompLauncherCModFeature& feature) {
    const auto* mods = m->mods;
    if (feature.status[0]) {
        ImGui::Spacing();
        ImGui::TextColored(feature.has_error ? col(th.warn) : col(th.text_muted),
                           "%s", feature.status);
    }
    if (!mods->diagnostic_count || !mods->diagnostic_get) return;

    const int count = mods->diagnostic_count(
        mods->ctx, feature.package_id, feature.id);
    if (count <= 0) return;
    ImGui::Spacing();
    ImGui::TextColored(col(th.accent), "Validation");
    ImGui::Separator();
    for (int index = 0; index < count; ++index) {
        RecompLauncherCModDiagnostic diagnostic{};
        if (!mods->diagnostic_get(mods->ctx, feature.package_id, feature.id,
                                  index, &diagnostic)) {
            continue;
        }
        const ImVec4 color = diagnostic.severity == RECOMP_MOD_DIAGNOSTIC_ERROR
            ? col(th.warn) : col(th.text_muted);
        ImGui::PushID(index);
        ImGui::TextColored(color, "%s", diagnostic.message);
        if (diagnostic.resource[0])
            ImGui::TextColored(col(th.text_muted), "Resource: %s",
                               diagnostic.resource);
        if (diagnostic.related_feature_id[0]) {
            ImGui::TextColored(
                col(th.text_muted), "Also involved: %s%s%s",
                diagnostic.related_package_id,
                diagnostic.related_package_id[0] ? " / " : "",
                diagnostic.related_feature_id);
        }
        ImGui::PopID();
    }
}

static bool set_all_mod_features(LauncherModel* m, bool enabled) {
    const auto* mods = m ? m->mods : nullptr;
    if (!mods || !mods->feature_count || !mods->feature_get ||
        !mods->feature_enable) {
        return false;
    }

    const int count = mods->feature_count(mods->ctx);
    std::vector<RecompLauncherCModFeature> features;
    features.reserve(count > 0 ? (size_t)count : 0);
    for (int index = 0; index < count; ++index) {
        RecompLauncherCModFeature feature{};
        if (!mods->feature_get(mods->ctx, index, &feature)) {
            mod_note_error(m);
            return false;
        }
        features.push_back(feature);
    }

    std::vector<size_t> changed;
    changed.reserve(features.size());
    for (size_t index = 0; index < features.size(); ++index) {
        const RecompLauncherCModFeature& feature = features[index];
        if ((feature.enabled != 0) == enabled) continue;
        if (!mods->feature_enable(mods->ctx, feature.package_id, feature.id,
                                  enabled ? 1 : 0)) {
            char failure[sizeof(m->mod_status)] = {};
            const char* error =
                mods->last_error ? mods->last_error(mods->ctx) : nullptr;
            if (error && error[0])
                std::snprintf(failure, sizeof(failure), "%s", error);
            // Treat the bulk action as one edit. Best-effort rollback prevents
            // a failed feature from leaving an unexpected half-toggled set.
            for (auto rollback = changed.rbegin(); rollback != changed.rend();
                 ++rollback) {
                const RecompLauncherCModFeature& prior = features[*rollback];
                mods->feature_enable(mods->ctx, prior.package_id, prior.id,
                                     prior.enabled ? 1 : 0);
            }
            if (failure[0]) {
                std::snprintf(m->mod_status, sizeof(m->mod_status), "%s",
                              failure);
            } else {
                mod_note_error(m);
            }
            return false;
        }
        changed.push_back(index);
    }
    std::snprintf(m->mod_status, sizeof(m->mod_status),
                  enabled ? "All mod features enabled. Changes apply on PLAY."
                          : "All mod features disabled. Changes apply on PLAY.");
    return true;
}

/* Packages that never loaded, at the top of the page.
 *
 * A package whose manifest will not parse has no row in the list below -- it
 * has no features, because it has no manifest -- so if this is not drawn the
 * only symptom is a mod that is silently absent. That was the whole defect:
 * an author's typo produced a mod that did not exist, with nothing anywhere
 * saying why. It goes above the list rather than in the detail pane because
 * there is no row to select to reach it. */
static void draw_mod_catalog_diagnostics(LauncherModel* m,
                                         const LauncherTheme& th) {
    const auto* mods = m ? m->mods : nullptr;
    if (!mods || !mods->catalog_diagnostic_count ||
        !mods->catalog_diagnostic_get) {
        return;
    }
    const int count = mods->catalog_diagnostic_count(mods->ctx);
    if (count <= 0) return;

    char heading[128];
    std::snprintf(heading, sizeof(heading),
                  count == 1 ? ui_text("%d mod package could not be loaded")
                             : ui_text("%d mod packages could not be loaded"),
                  count);
    if (ImGui::CollapsingHeader(heading, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(
            col(th.text_muted), "%s",
            ui_text("These are not installed. Fix the manifest and restart, "
                    "or run: psxmod validate <path>"));
        for (int index = 0; index < count; ++index) {
            RecompLauncherCModDiagnostic diagnostic{};
            if (!mods->catalog_diagnostic_get(mods->ctx, index, &diagnostic))
                continue;
            ImGui::PushID(index);
            ImGui::TextColored(col(th.warn), "%s", diagnostic.message);
            if (diagnostic.resource[0])
                ImGui::TextWrapped("%s", diagnostic.resource);
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::Spacing();
}

static void draw_mod_features(LauncherModel* m, const LauncherTheme& th) {
    const auto* mods = m ? m->mods : nullptr;
    if (!mods || !mods->feature_count || !mods->feature_get ||
        !mods->feature_option_get || !mods->feature_enable ||
        !mods->feature_set_option) {
        return;
    }

    const int feature_count = mods->feature_count(mods->ctx);
    const char* archive_extension =
        mods->archive_extension && mods->archive_extension[0]
            ? mods->archive_extension : ".psxmod";
    const char* archive_description =
        mods->archive_description && mods->archive_description[0]
            ? mods->archive_description
            : "PSXRecomp mod package (.psxmod)";
    char install_label[96];
    char archive_pattern[64];
    std::snprintf(install_label, sizeof(install_label),
                  "Install %s", archive_extension);
    std::snprintf(archive_pattern, sizeof(archive_pattern),
                  "*%s", archive_extension);
    if (ImGui::Button(install_label)) {
        ui_pick_file(m, "Install Mod Package", {archive_pattern},
                     archive_description, [m, mods](const char* path) {
            if (!path) return;
            if (!mods->install_archive ||
                !mods->install_archive(mods->ctx, path)) {
                mod_note_error(m);
            } else {
                std::snprintf(m->mod_status, sizeof(m->mod_status),
                              "%s", ui_text("Package installed. Changes apply when you press PLAY."));
            }
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_text("Enable all")))
        set_all_mod_features(m, true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ui_text("Enable every installed mod feature"));
    ImGui::SameLine();
    if (ImGui::Button(ui_text("Disable all")))
        set_all_mod_features(m, false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ui_text("Disable every installed mod feature"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(300));
    ImGui::InputTextWithHint("##mod_search", ui_text("Search features, groups, packages..."),
                             m->mod_search, sizeof(m->mod_search));
    if (m->mod_status[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.warn));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", m->mod_status);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (feature_count <= 0) m->mod_selected = 0;
    else if (m->mod_selected >= feature_count)
        m->mod_selected = feature_count - 1;

    std::vector<ModFeatureListItem> visible_features;
    visible_features.reserve(feature_count > 0 ? (size_t)feature_count : 0);
    for (int index = 0; index < feature_count; ++index) {
        ModFeatureListItem item{};
        item.index = index;
        if (!mods->feature_get(mods->ctx, index, &item.feature) ||
            (item.feature.hidden && !item.feature.enabled) ||
            !mod_feature_text_matches(m->mod_search, item.feature)) {
            continue;
        }
        visible_features.push_back(item);
    }
    std::stable_sort(visible_features.begin(), visible_features.end(),
                     mod_feature_less);

    const float list_w = px(330);
    if (ImGui::BeginChild("##mod_feature_list", ImVec2(list_w, 0),
                          ImGuiChildFlags_Borders)) {
        size_t first = 0;
        while (first < visible_features.size()) {
            const char* raw_group = visible_features[first].feature.group;
            const char* group = raw_group[0] ? raw_group : ui_text("General");
            size_t last = first + 1;
            while (last < visible_features.size()) {
                const char* candidate = visible_features[last].feature.group;
                if (std::strcmp(raw_group, candidate) != 0) break;
                ++last;
            }

            ImGui::PushID(raw_group[0] ? raw_group : "##general");
            if (m->mod_search[0])
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            const bool open = ImGui::CollapsingHeader(
                group, ImGuiTreeNodeFlags_DefaultOpen);
            if (open) {
                for (size_t item_index = first; item_index < last; ++item_index) {
                    const ModFeatureListItem& item = visible_features[item_index];
                    const RecompLauncherCModFeature& feature = item.feature;
                    ImGui::PushID(feature.package_id);
                    ImGui::PushID(feature.id);

                    bool enabled = feature.enabled != 0;
                    if (ImGui::Checkbox("##enabled", &enabled)) {
                        if (!mods->feature_enable(
                                mods->ctx, feature.package_id, feature.id,
                                enabled ? 1 : 0)) {
                            mod_note_error(m);
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s %s",
                                          enabled ? ui_text("Disable") : ui_text("Enable"),
                                          feature.name);
                    ImGui::SameLine();
                    if (feature.has_error) {
                        ImGui::TextColored(col(th.warn), "!");
                        if (ImGui::IsItemHovered() && feature.status[0])
                            ImGui::SetTooltip("%s", feature.status);
                        ImGui::SameLine();
                    }
                    if (const char* channel_tag =
                            mod_channel_tag_short(feature.channel)) {
                        ImGui::TextColored(
                            col(mod_channel_color(feature.channel, th)), "%s",
                            channel_tag);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s\n%s",
                                              mod_channel_tag(feature.channel),
                                              mod_channel_blurb(feature.channel));
                        ImGui::SameLine();
                    }

                    char label[320];
                    std::snprintf(label, sizeof(label), "%s\n%s%s%s",
                                  feature.name,
                                  feature.package_name[0] ? feature.package_name
                                                          : feature.package_id,
                                  feature.package_version[0] ? "  " : "",
                                  feature.package_version);
                    if (ImGui::Selectable(
                            label, m->mod_selected == item.index, 0,
                            ImVec2(0, px(44)))) {
                        m->mod_selected = item.index;
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
            first = last;
        }
        if (visible_features.empty()) {
            ImGui::TextColored(col(th.text_muted),
                               "%s",
                               feature_count ? ui_text("No matching features.")
                                             : ui_text("No mod features installed."));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##mod_feature_detail", ImVec2(0, 0),
                          ImGuiChildFlags_Borders)) {
        RecompLauncherCModFeature feature{};
        if (feature_count > 0 &&
            mods->feature_get(mods->ctx, m->mod_selected, &feature)) {
            ImGui::TextColored(col(th.accent2), "%s", feature.name);
            if (const char* channel_tag = mod_channel_tag(feature.channel)) {
                ImGui::SameLine();
                ImGui::TextColored(col(mod_channel_color(feature.channel, th)),
                                   "%s", channel_tag);
            }
            if (feature.group[0]) {
                ImGui::SameLine();
                ImGui::TextColored(col(th.text_muted), "%s", feature.group);
            }
            ImGui::TextColored(
                col(th.text_muted), "%s %s%s%s", ui_text("From"),
                feature.package_name[0] ? feature.package_name
                                        : feature.package_id,
                feature.package_version[0] ? " " : "",
                feature.package_version);
            if (feature.author[0])
                draw_linkified_mod_author(
                    feature.author, feature.author_links,
                    feature.author_link_count, th);
            if (feature.source_url[0]) {
                ImGui::TextColored(col(th.text_muted), "%s", ui_text("Source: "));
                ImGui::SameLine(0, 0);
                ImGui::TextLinkOpenURL(
                    feature.source_name[0] ? feature.source_name : ui_text("Project page"),
                    feature.source_url);
            }
            if (const char* channel_blurb = mod_channel_blurb(feature.channel)) {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    col(mod_channel_color(feature.channel, th)));
                ImGui::TextWrapped("%s", channel_blurb);
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
            if (feature.description[0])
                ImGui::TextWrapped("%s", feature.description);
            ImGui::Spacing();

            if (feature.camera_controls) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(col(th.accent), "%s", ui_text("3D camera controls"));
                ImGui::TextWrapped(
                    "%s",
                    feature.enabled
                        ? ui_text("This mod adds live camera input. Review the current right-stick and keyboard bindings before playing.")
                        : ui_text("Enable this feature to expose its camera bindings on the Controller page."));
                if (feature.enabled &&
                    ImGui::Button(ui_text("Review Camera Bindings"))) {
                    launcher_model_open_config(m, 0);
                }
                ImGui::Spacing();
            }

            // The list-row checkbox is the single enable/disable control.
            // The detail pane owns configuration values only.
            if (mods->feature_resource_count &&
                mods->feature_resource_get &&
                mods->feature_resource_set_path) {
                const int resource_count = mods->feature_resource_count(
                    mods->ctx, feature.package_id, feature.id);
                if (resource_count > 0) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextColored(col(th.accent), "%s", ui_text("Files"));
                    for (int resource_index = 0;
                         resource_index < resource_count; ++resource_index) {
                        RecompLauncherCModResource resource{};
                        if (!mods->feature_resource_get(
                                mods->ctx, feature.package_id, feature.id,
                                resource_index, &resource))
                            continue;
                        ImGui::PushID(resource.id);
                        ImGui::TextUnformatted(resource.label);
                        if (resource.description[0])
                            ImGui::TextWrapped("%s", resource.description);
                        ImGui::TextColored(
                            resource.verified ? col(th.accent2) :
                                !resource.required && !resource.path[0] ? col(th.text_muted) : col(th.warn),
                            "%s", resource.status[0]
                                      ? resource.status : ui_text("Not selected"));
                        if (resource.path[0]) {
                            const char* basename = resource.path;
                            for (const char* p = resource.path; *p; ++p)
                                if (*p == '/' || *p == '\\') basename = p + 1;
                            ImGui::TextColored(col(th.text_muted), "%s", basename);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", resource.path);
                        }
                        const bool directory_resource =
                            std::strcmp(resource.format, "directory") == 0 ||
                            std::strcmp(resource.format, "folder") == 0;
                        if (ImGui::Button(
                                resource.path[0]
                                    ? (directory_resource ? ui_text("Change folder")
                                                          : ui_text("Change file"))
                                    : (directory_resource ? ui_text("Select folder")
                                                          : ui_text("Select file")))) {
                            std::vector<std::string> owned_patterns;
                            std::vector<const char*> patterns;
                            std::string remaining = resource.file_patterns;
                            size_t start = 0;
                            while (start <= remaining.size()) {
                                const size_t comma = remaining.find(',', start);
                                std::string pattern = remaining.substr(
                                    start, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - start);
                                if (!pattern.empty())
                                    owned_patterns.push_back(pattern);
                                if (comma == std::string::npos) break;
                                start = comma + 1;
                            }
                            for (const std::string& pattern : owned_patterns)
                                patterns.push_back(pattern.c_str());
                            /* The ids and label are copied into the closure:
                             * on the built-in-browser path this runs many
                             * frames later, when the RecompLauncherCMod*
                             * structs this loop walks are long gone. */
                            const std::string pkg_id = feature.package_id;
                            const std::string feat_id = feature.id;
                            const std::string res_id = resource.id;
                            const std::string res_label = resource.label;
                            auto apply_resource = [m, mods, pkg_id, feat_id,
                                                   res_id,
                                                   res_label](const char* path) {
                                if (!path) return;
                                if (!mods->feature_resource_set_path(
                                        mods->ctx, pkg_id.c_str(),
                                        feat_id.c_str(), res_id.c_str(),
                                        path)) {
                                    mod_note_error(m);
                                } else {
                                    std::snprintf(
                                        m->mod_status,
                                        sizeof(m->mod_status),
                                        "%s verified. Changes apply on PLAY.",
                                        res_label.c_str());
                                }
                            };
                            if (directory_resource) {
                                ui_pick_folder(m, resource.label,
                                               apply_resource);
                            } else {
                                ui_pick_file(m, resource.label, owned_patterns,
                                             resource.file_description[0]
                                                 ? resource.file_description
                                                 : nullptr,
                                             apply_resource);
                            }
                        }
                        if (!resource.required && resource.path[0]) {
                            ImGui::SameLine();
                            if (ImGui::Button(ui_text("Clear selection"))) {
                                if (!mods->feature_resource_set_path(
                                        mods->ctx, feature.package_id,
                                        feature.id, resource.id, "")) {
                                    mod_note_error(m);
                                } else {
                                    std::snprintf(m->mod_status, sizeof(m->mod_status),
                                        "%s selection cleared. Changes apply on PLAY.", resource.label);
                                }
                            }
                        }
                        ImGui::PopID();
                        ImGui::Spacing();
                    }
                }
            }
            draw_mod_feature_diagnostics(m, th, feature);
            if (feature.option_count > 0) {
                ImGui::Spacing();
                ImGui::Separator();
            }
            std::string last_group;
            for (int index = 0; index < feature.option_count; ++index) {
                RecompLauncherCModOption option{};
                if (!mods->feature_option_get(
                        mods->ctx, feature.package_id, feature.id,
                        index, &option)) {
                    continue;
                }
                if (last_group != option.group) {
                    last_group = option.group;
                    ImGui::Spacing();
                    ImGui::TextColored(
                        col(th.accent), "%s",
                        last_group.empty() ? ui_text("Configuration")
                                           : last_group.c_str());
                    ImGui::Separator();
                }
                draw_mod_feature_option(m, feature, option);
            }
        } else {
            ImGui::TextColored(col(th.text_muted),
                               "%s", ui_text("Install or select a feature to configure it."));
        }
    }
    ImGui::EndChild();
}

static void draw_rom_patch(LauncherModel* m, const LauncherTheme& th) {
    if (!m || !m->rom_patch_supported) return;

    if (ImGui::BeginChild("##rom_patch", ImVec2(0, px(170)),
                          ImGuiChildFlags_Borders)) {
        eyebrow("ROM PATCH");
        bool enabled = m->s.rom_patch_enabled != 0;
        if (ImGui::Checkbox(ui_text("Enable ROM patch"), &enabled))
            launcher_model_toggle_rom_patch(m);

        const float clear_w = px(70);
        const float browse_w = px(95);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - browse_w - clear_w -
                        px(8));
        if (ImGui::Button(ui_text("Browse..."), ImVec2(browse_w, px(28)))) {
            ui_pick_file(m, "Select ROM patch", {"*.ips", "*.ips32", "*.bps"},
                         "ROM patches", [m](const char* path) {
                             if (path) launcher_model_set_rom_patch(m, path);
                         });
        }
        ImGui::SameLine(0, px(8));
        ImGui::BeginDisabled(!m->s.rom_patch_path[0]);
        if (ImGui::Button(ui_text("Clear"), ImVec2(clear_w, px(28))))
            launcher_model_clear_rom_patch(m);
        ImGui::EndDisabled();

        const char* path = m->s.rom_patch_path[0]
            ? m->s.rom_patch_path : ui_text("No patch selected");
        char elided[384];
        elide_left(path, ImGui::GetContentRegionAvail().x,
                   elided, sizeof(elided));
        ImGui::TextColored(col(th.text_muted), "%s", elided);
        if (ImGui::IsItemHovered() && m->s.rom_patch_path[0])
            ImGui::SetTooltip("%s", m->s.rom_patch_path);
        if (m->rom_patch_note && m->rom_patch_note[0])
            ImGui::TextWrapped("%s", m->rom_patch_note);
        if (m->rom_patch_status[0])
            ImGui::TextColored(col(th.accent), "%s", m->rom_patch_status);
    }
    ImGui::EndChild();
}

static void draw_mod_presets(LauncherModel* m, const LauncherTheme& th) {
    const int count = launcher_model_preset_count(m);
    if (!count) return;
    const char* current = m->mods->preset_current(m->mods->ctx, &m->s);
    RecompLauncherCModPreset selected{};
    std::snprintf(selected.name, sizeof(selected.name), "%s", "Custom");
    for (int i = 0; i < count; ++i) {
        RecompLauncherCModPreset item{};
        if (m->mods->preset_get(m->mods->ctx, i, &item) && current && !std::strcmp(current, item.id)) selected = item;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(280));
    if (ImGui::BeginCombo("##mod_preset", selected.name)) {
        for (int i = 0; i < count; ++i) {
            RecompLauncherCModPreset item{};
            if (!m->mods->preset_get(m->mods->ctx, i, &item)) continue;
            if (ImGui::Selectable(item.name, current && !std::strcmp(current, item.id)))
                launcher_model_apply_preset(m, item.id);
            if (ImGui::IsItemHovered() && item.description[0]) ImGui::SetTooltip("%s", item.description);
        }
        ImGui::EndCombo();
    }
    ImGui::TextColored(col(th.text_muted), "Individual options remain editable.");
    ImGui::Spacing();
}

void draw_mods(LauncherModel* m, const LauncherTheme& th) {
    const auto* mods = m ? m->mods : nullptr;
    if (!m || (!mods && !m->rom_patch_supported)) return;
    if (m->rom_patch_supported) {
        draw_rom_patch(m, th);
        if (!mods) return;
        ImGui::Dummy(ImVec2(0, px(8)));
    }
    /* Above the Features/Packages switch: a package that failed to parse is
     * missing from BOTH views, so reporting it inside either one would leave
     * the other silent. */
    draw_mod_presets(m, th);
    draw_mod_catalog_diagnostics(m, th);

    const bool feature_provider =
        mods->feature_count && mods->feature_get &&
        mods->feature_option_get && mods->feature_enable &&
        mods->feature_set_option;
    if (!feature_provider) m->mod_show_packages = true;

    if (feature_provider) {
        if (ImGui::RadioButton(ui_text("Features"), !m->mod_show_packages))
            m->mod_show_packages = false;
        ImGui::SameLine();
        if (ImGui::RadioButton(ui_text("Installed packages"), m->mod_show_packages))
            m->mod_show_packages = true;
        ImGui::Spacing();
    }

    if (m->mod_show_packages)
        draw_mod_packages(m, th);
    else
        draw_mod_features(m, th);
}

// ---- panel registry: id -> {view, slot, available, draw} --------------------
// The single implementation table for every panel this backend draws. A
// SystemProfile's panels_dashboard/panels_settings/panels_controller arrays
// (launcher_system.h) list which of these ids compose into each view, in
// slot order; draw_dashboard/draw_settings/draw_controller above look each id
// up here via find_composed() and draw whatever's both listed and available().
// Online identity card (dashboard, under the controller card). Double
// opt-in like the BIOS row: the console profile must LIST "identity" in
// panels_dashboard AND the game must set GameInfo.has_player_name — most
// titles have no online play and never see this card (owner directive:
// shared launcher features are per-game opt-in).
int avail_identity(const LauncherModel* m) { return m->has_player_name; }
void panel_identity_draw(LauncherModel* m, const LauncherTheme* th) {
    if (!begin_panel("identity")) { end_panel(); return; }
    eyebrow("ONLINE");
    const float cw = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(ui_text("Player name"));
    ImGui::SetNextItemWidth(cw);
    ImGui::InputTextWithHint("##identity_name", ui_text("console default"),
                             m->s.player_name, sizeof(m->s.player_name));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The name other players see online (the console nickname). "
            "Leave empty for the default.");
    if (m->identity_detail && m->identity_detail[0]) {
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, col(th->text_muted));
        ImGui::TextWrapped("%s", m->identity_detail);
        ImGui::PopStyleColor();
    }
    end_panel();
}

const LauncherPanel kPanelRegistry[] = {
    { "game",              LNG_VIEW_DASHBOARD,  LNG_SLOT_MAIN, nullptr,      panel_game_draw },
    { "controller",        LNG_VIEW_DASHBOARD,  LNG_SLOT_SIDE, nullptr,      panel_controller_draw },
    { "identity",          LNG_VIEW_DASHBOARD,  LNG_SLOT_SIDE, avail_identity, panel_identity_draw },
    { "save",              LNG_VIEW_DASHBOARD,  LNG_SLOT_WIDE, avail_save,   panel_save_draw },
    { "tpak",              LNG_VIEW_DASHBOARD,  LNG_SLOT_WIDE, avail_tpak,   panel_tpak_draw },
    { "video",             LNG_VIEW_SETTINGS,   LNG_SLOT_MAIN, nullptr,      panel_video_draw },
    { "audio",             LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, nullptr,      panel_audio_draw },
    { "input",             LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, avail_input,  panel_input_draw },
    { "system",            LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, avail_system, panel_system_draw },
    { "solar",             LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, avail_solar,  panel_solar_draw },
    { "hotkeys",           LNG_VIEW_SETTINGS,   LNG_SLOT_WIDE, nullptr,      panel_hotkeys_draw },
    { "controller_config", LNG_VIEW_CONTROLLER, LNG_SLOT_WIDE, nullptr,      panel_controller_config_draw },
    { nullptr,              LNG_VIEW_DASHBOARD,  0,             nullptr,      nullptr },   // sentinel
};

// Footer: a fixed-height band with the neon divider pinned to its TOP and the
// CTA vertically centred inside it. Laid out from an explicit origin (not the
// running cursor) so it is pixel-identical on every view and the CTA's glow
// always has clearance below the divider — Settings has less body content, and
// a cursor-relative footer let the glow ride up into the rule.
void draw_footer(LauncherModel* m, const LauncherTheme& th, float footer_h) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  fullw  = ImGui::GetContentRegionAvail().x;
    const float  play_w = px(210), play_h = px(46);

    // divider at the very top of the band
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        origin, ImVec2(origin.x + fullw, origin.y + px(1.5f)),
        imcol(th.border, 0.2f), imcol(th.accent2, 0.7f),
        imcol(th.accent2, 0.7f), imcol(th.border, 0.2f));

    // CTA centred in the remaining band height (glow clears the rule on both sides)
    const float band_y = origin.y + px(1.5f);
    const float band_h = footer_h - px(1.5f);
    const float cta_y  = band_y + (band_h - play_h) * 0.5f;

    const ImVec2 win = ImGui::GetWindowPos();
    if (m->view == LNG_VIEW_DASHBOARD) {
        bool skip = m->s.skip_launcher != 0;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y + (play_h - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Checkbox(ui_text("Skip launcher on boot"), &skip))
            launcher_model_request_skip_toggle(m);
    } else if (m->view == LNG_VIEW_SETTINGS &&
               launcher_model_can_restore_defaults(m)) {
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x, cta_y + (play_h - px(34.0f)) * 0.5f));
        if (ImGui::Button(ui_text("Restore Defaults"), ImVec2(px(150.0f), px(34.0f))))
            launcher_model_request_restore_defaults(m);
    }
    if (m->view == LNG_VIEW_LOBBY) {
        draw_lobby_footer(m, th, origin, cta_y, play_h, fullw);
        return;
    }
    if (m->view == LNG_VIEW_NETPLAY) {
        /* Same split as draw_netplay's page body (netplay_mode: 1 = LAN /
         * Direct IP, 2 = online). Join Direct is how a LAN room is reached
         * and stays there; online replaces it with Automatch, which is the
         * online-only way to reach a room without picking one. */
        const bool lan_mode = m->netplay_mode == 1;
        const auto* np_am = np_cb(m);
        /* Two different "no", and the button must not confuse them.
         *
         * A NULL automatch_available means THIS BUILD has no automatch
         * backend wired -- the launcher never asks the server anything, and
         * would answer the same way against a server running the feature
         * perfectly. Reporting that as "the server does not offer automatch"
         * sends whoever reads it to go and check a server that was never
         * consulted. A present callback answering 0 is the real server
         * answer: reachable, asked, and it has no rulesets loaded for this
         * title. */
        const bool am_wired = np_am && np_am->automatch_available != NULL;
        const bool automatch_ok = am_wired &&
                                  np_am->automatch_available(np_am->ctx);
        const int am_state = np_automatch_state(np_am);
        const bool am_queued = am_state == RECOMP_LAUNCHER_AUTOMATCH_QUEUED;
        /* While the accept gate is up the modal owns the interaction; the
         * footer must not offer a second way to leave underneath it. */
        const bool am_gated = am_state == RECOMP_LAUNCHER_AUTOMATCH_FOUND ||
                              am_state == RECOMP_LAUNCHER_AUTOMATCH_ACCEPTED;
        const int am_pool = (np_am && np_am->automatch_pool)
                                ? np_am->automatch_pool(np_am->ctx) : 0;
        const int am_secs = (np_am && np_am->automatch_queued_secs)
                                ? np_am->automatch_queued_secs(np_am->ctx) : 0;
        /* The label owns text the launcher itself writes, so the ⚡ goes
         * through the atlas the same way a chat line's emoji does -- without
         * this it draws as the merged OpenMoji outline. The emoji is kept out
         * of the translated string so a translator carries words, not a
         * codepoint. */
        char automatch_label[80];
        {
            char raw[64];
            if (am_queued) {
                /* The elapsed time IS the button: a queue with no visible
                 * clock reads as a hang, and this is also the only control
                 * that leaves the queue. */
                std::snprintf(raw, sizeof(raw), "⚡ %s %d:%02d", ui_text("Queued"),
                              am_secs / 60, am_secs % 60);
            } else {
                std::snprintf(raw, sizeof(raw), "⚡ %s", ui_text("Automatch"));
            }
            emoji_display(raw, automatch_label, sizeof(automatch_label));
        }
        /* Queued: leave. Otherwise pick a queue when there is a choice, and
         * skip straight past the picker when there is only one. */
        auto automatch_click = [&]() {
            if (!np_am) return;
            if (am_queued) {
                if (np_am->automatch_cancel) np_am->automatch_cancel(np_am->ctx);
                return;
            }
            const int n = np_am->automatch_ruleset_count
                              ? np_am->automatch_ruleset_count(np_am->ctx) : 0;
            if (n > 1) {
                m->netplay_automatch_picker_open = true;
                return;
            }
            np_automatch_queue(m, "");
        };
        auto automatch_tip = [&]() {
            if (!am_wired)    return "This build has no automatch support yet";
            if (!automatch_ok) return "This lobby server does not offer automatch";
            if (am_gated)     return "Answer the match offer to continue";
            if (am_queued)    return am_pool > 0
                                  ? "Others are waiting in this queue - click to leave"
                                  : "Nobody else is waiting yet - click to leave";
            return "Queue for a match against anyone else waiting";
        };
        const float action_w = px(190.0f);
        const float settings_w = px(170.0f);
        const float refresh_w = px(120.0f);
        const float gap = px(10);
        const float row_need =
            action_w + gap + settings_w + gap + refresh_w + gap + action_w;
        const bool compact = fullw < row_need + px(8);

        auto open_host = [&]() {
            if (!launcher_model_netplay_disc_ok(m)) {
                const char* why = m->verify.netplay_detail[0]
                                      ? m->verify.netplay_detail
                                      : "Mount the supported .cue dump before hosting.";
                std::snprintf(m->netplay_status, sizeof(m->netplay_status), "%s", why);
                return;
            }
            if (!np_effective_name(m)[0]) {
                np_open_name_modal(m);
                return;
            }
            /* The mode IS the answer: LAN hosts a LAN room, online hosts an
             * online one. Connecting is online-only -- a LAN host dialling the
             * lobby server would advertise the same room twice, once in the
             * file registry and once on the server. */
            m->netplay_lan_only = (m->netplay_mode == 1);
            if (!m->netplay_lan_only) np_connect_and_list(m);
            np_refresh_host_ip(m);
            if (!m->netplay_host_name[0]) {
                std::snprintf(m->netplay_host_name, sizeof(m->netplay_host_name),
                              "%s's Lobby", np_effective_name(m));
            }
            m->netplay_host_password[0] = '\0';
            m->netplay_host_modal_open = true;
        };
        auto open_network = [&]() {
            const auto* np = np_cb(m);
            const char* current = np && np->default_url ? np->default_url(np->ctx) : "";
            std::snprintf(m->netplay_lobby_url, sizeof(m->netplay_lobby_url), "%s",
                          current ? current : "");
            m->netplay_network_modal_open = true;
        };

        if (!compact) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y));
            /* Host Lobby and Automatch carry the CTA treatment: on this page
             * they ARE the two ways to get into a game, and drawing them in
             * the same neutral grey as Refresh made the screen read as four
             * equal utilities with no obvious way in. Network Settings and
             * Refresh stay neutral, which is what gives the other two their
             * weight -- accenting everything would say nothing. */
            if (neon_cta("##np_host", ui_text("Host Lobby"),
                         ImVec2(action_w, play_h), true, /*arrow*/false))
                open_host();
            ImGui::SetCursorScreenPos(ImVec2(origin.x + action_w + gap, cta_y));
            if (ImGui::Button(ui_text("Network Settings"), ImVec2(settings_w, play_h)))
                open_network();
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x + action_w + gap + settings_w + gap, cta_y));
            if (ImGui::Button(ui_text("Refresh"), ImVec2(refresh_w, play_h)))
                np_refresh_lobby_list(m);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reload server lobbies and rescan LAN/Direct IP");
            ImGui::SetCursorScreenPos(ImVec2(origin.x + fullw - action_w, cta_y));
            if (lan_mode) {
                if (ImGui::Button(ui_text("Join Direct"), ImVec2(action_w, play_h)))
                    m->netplay_direct_modal_open = true;
            } else {
                if (neon_cta("##np_automatch", automatch_label,
                             ImVec2(action_w, play_h),
                             automatch_ok && !am_gated, /*arrow*/false))
                    automatch_click();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", automatch_tip());
                /* The population sits under the button rather than in the
                 * tooltip: it is the number that says whether waiting is
                 * worth it, and a tooltip is not where you look for that. */
                if (am_queued) {
                    /* Painted straight onto the draw list rather than placed
                     * as a widget. The footer is a fixed-height band whose
                     * controls are positioned by hand, and an ImGui item below
                     * the button still counts toward the window's content
                     * size -- so this one line pushed the content past the
                     * band and the whole page grew a scrollbar for the sake of
                     * eight characters. There is room to draw it; there was no
                     * room to lay it out. */
                    char pool_text[32];
                    std::snprintf(pool_text, sizeof(pool_text), "%d waiting",
                                  am_pool);
                    const float tw = ImGui::CalcTextSize(pool_text).x;
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(origin.x + fullw - action_w + (action_w - tw) * 0.5f,
                               cta_y + play_h + px(2)),
                        imcol(th.text_muted), pool_text);
                }
            }
        } else {
            /* Narrow window: collapse into a scrollable Actions menu. */
            const float menu_btn_w = px(160.0f);
            ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y));
            if (ImGui::Button(ui_text("Actions##np_footer"), ImVec2(menu_btn_w, play_h)))
                ImGui::OpenPopup("##netplay_footer_menu");
            if (ImGui::BeginPopup("##netplay_footer_menu")) {
                const float menu_w = px(220.0f);
                const float menu_h = px(168.0f);
                if (ImGui::BeginChild("##np_footer_scroll", ImVec2(menu_w, menu_h),
                                      ImGuiChildFlags_None,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
                    if (ImGui::Selectable(ui_text("Host Lobby"))) {
                        open_host();
                    }
                    if (ImGui::Selectable(ui_text("Network Settings"))) {
                        open_network();
                    }
                    if (ImGui::Selectable(ui_text("Refresh"))) {
                        np_refresh_lobby_list(m);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Reload server lobbies and rescan LAN/Direct IP");
                    if (lan_mode) {
                        if (ImGui::Selectable(ui_text("Join Direct"))) {
                            m->netplay_direct_modal_open = true;
                        }
                    } else {
                        ImGui::BeginDisabled(!automatch_ok || am_gated);
                        if (ImGui::Selectable(automatch_label)) automatch_click();
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("%s", automatch_tip());
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
        return;
    }
    // Back/Cancel (Circle/O on a DualSense, B on Xbox, Backspace on keyboard) AND
    // Start re-home the focus ring to PLAY. Directional nav through the card child
    // windows is easy to wander out of with no way back; these are the forcing
    // functions that snap the highlight back to the primary action. Start
    // deliberately only HIGHLIGHTS PLAY (does not launch), so it can't fire the
    // game by accident — the launch is the activate button (A/Cross) on the
    // focused PLAY, or a mouse click. SetKeyboardFocusHere() targets the NEXT
    // submitted item — PLAY's button.
    // Not while the player is typing, and not over an open modal --
    // rui_nav_rehome_to_play() carries the why.
    if (rui_nav_rehome_to_play())
        ImGui::SetKeyboardFocusHere();
    float play_x = origin.x + fullw - play_w;
    if (m->netplay_supported &&
        (m->view == LNG_VIEW_DASHBOARD || m->view == LNG_VIEW_SETTINGS ||
         m->view == LNG_VIEW_CONTROLLER)) {
        const float net_w = px(170.0f);
        ImGui::SetCursorScreenPos(ImVec2(play_x - net_w - px(12.0f), cta_y));
        if (ImGui::Button(ui_text("NETPLAY"), ImVec2(net_w, play_h))) {
            /* The fork first. Connecting used to start here, which meant a LAN
             * player dialled a lobby server they were never going to use --
             * np_enter_netplay does it now, and only for online. */
            launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
        }
    }
    /* The netplay MODE picker is a fork in the road, not a launch screen: the
     * player is answering "LAN or online?", and a PLAY button there offers to
     * start the game instead of answering it -- which is both the wrong action
     * and the most prominent thing on the page. Every other view keeps it. */
    if (m->view == LNG_VIEW_NETPLAY_MODE) return;

    ImGui::SetCursorScreenPos(ImVec2(play_x, cta_y));
    const bool can_play = launcher_model_can_launch(m);
    const bool bios_block = launcher_model_bios_blocks_play(m);
    const bool play_enabled = can_play || bios_block;
    if (neon_cta("##play", ui_text("PLAY"), ImVec2(play_w, play_h), play_enabled)) {
        /* Prefer mismatch prompt over launch even if can_play races true. */
        if (bios_block)
            launcher_model_bios_play_prompt(m);
        else if (!launcher_model_prepare_rom_patch(m)) {
            /* The effective image could not be prepared. Send the user to
             * the page that owns the patch selection instead of launching
             * an image the runtime's identity gate would reject. */
            if (m->rom_patch_supported && m->s.rom_patch_enabled)
                launcher_model_set_view(m, LNG_VIEW_MODS);
        } else if (mod_commit_launch(m))
            m->action = LNG_ACTION_LAUNCH;
    } else if (!play_enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* noun = m->rom_noun ? m->rom_noun : "ROM";
        if (m->has_bios && !m->setup_bios_ok) {
            ImGui::SetTooltip(
                "Select a valid BIOS first (or Use OpenBIOS when this build "
                "allows it).");
        } else if (!m->rom_present || strcmp(m->rom_size, "--") == 0) {
            ImGui::SetTooltip("Select a valid %s first", noun);
        } else if (m->profile && m->profile->verify.mode == 1 &&
                   (m->verify.verdict == 0 || m->verify.verdict == 3)) {
            ImGui::SetTooltip("Select a verified %s first", noun);
        } else if (m->setup_preparing) {
            ImGui::SetTooltip("Wait for the current setup job to finish");
        } else {
            ImGui::SetTooltip("Select a valid %s first", noun);
        }
    }
    if (!play_enabled && ImGui::IsItemClicked() && m->setup_wizard_supported)
        m->setup_wizard_open = true;
    ImGui::SetItemDefaultFocus();   // gamepad/keyboard start on the primary action
    (void)win;
}

/* Compact progress-only modal while a setup job runs — closes the full
 * first-run form so nothing else is interactive. */
enum SetupPlatKind {
    SETUP_PLAT_GENERIC = 0,
    SETUP_PLAT_PSX,
    SETUP_PLAT_GBA,
    SETUP_PLAT_SNES,
};

static bool setup_plat_contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static SetupPlatKind setup_platform_kind(const char* platform) {
    if (setup_plat_contains_ci(platform, "PLAYSTATION") ||
        setup_plat_contains_ci(platform, "PSX") ||
        setup_plat_contains_ci(platform, "PS1"))
        return SETUP_PLAT_PSX;
    if (setup_plat_contains_ci(platform, "GAME BOY ADVANCE") ||
        setup_plat_contains_ci(platform, "GBA"))
        return SETUP_PLAT_GBA;
    if (setup_plat_contains_ci(platform, "SUPER NINTENDO") ||
        setup_plat_contains_ci(platform, "SNES"))
        return SETUP_PLAT_SNES;
    return SETUP_PLAT_GENERIC;
}

static const char* kRetcommToolchainsUrl =
    "https://github.com/TechnicallyComputers/retcomm-toolchains";
static const char* kRetcommToolchainsReleasesUrl =
    "https://github.com/TechnicallyComputers/retcomm-toolchains/releases";

/* TextLinkOpenURL is 1.91+; HOST_IMGUI may be older — fall back to a button. */
static void setup_url_link(const char* label, const char* url) {
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19100
    ImGui::TextLinkOpenURL(label, url);
#else
    if (ImGui::SmallButton(label))
        SDL_OpenURL(url);
#endif
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", url);
}

/* Compact progress-only modal while a setup job runs — closes the full
 * first-run form so nothing else is interactive. */
static void draw_setup_progress_modal(LauncherModel* m, const LauncherTheme& th) {
    static const char* kProgPopup = "Setup progress";
    ImGui::OpenPopup(kProgPopup);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(480), 0), ImGuiCond_Always);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::BeginPopupModal(kProgPopup, nullptr, flags))
        return;

    const float wrap_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const char* game =
        (m->game_name && m->game_name[0]) ? m->game_name : "this game";
    const char* job =
        (m->setup_progress_title[0])
            ? m->setup_progress_title
            : ((m->prepare_disc_label && m->prepare_disc_label[0])
                   ? m->prepare_disc_label
                   : "Setup");
    ImGui::TextColored(col(th.accent), "%s", job);
    ImGui::PushTextWrapPos(wrap_x);
    ImGui::TextColored(col(th.text_muted),
                       "Please wait — %s is working. Do not close this window.",
                       game);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(14)));

    /* The status line is fed every compiler invocation during a rebuild.
     * Those lines are long, so the text wrapped to a second line, the modal
     * grew to fit, the next short line shrank it back -- a window that
     * bounced in height on every ninja step. Reserve exactly two lines and
     * elide anything that would need a third, so the frame never moves. */
    const char* st = m->setup_status[0] ? m->setup_status : "Working…";
    const bool warn_st =
        (strncmp(st, "WARNING", 7) == 0) ||
        (strstr(st, "Do not close") != nullptr) ||
        (strstr(st, "DO NOT") != nullptr);
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float two_lines = ImGui::GetTextLineHeight() * 2.0f + 1.0f;
    char shown[sizeof(m->setup_status) + 4];
    std::snprintf(shown, sizeof(shown), "%s", st);
    if (ImGui::CalcTextSize(shown, nullptr, false, wrap_w).y > two_lines) {
        /* Longest prefix that still fits in two lines with an ellipsis.
         * Cut only on UTF-8 code point boundaries. */
        size_t lo = 0, hi = std::strlen(st);
        char probe[sizeof(shown)];
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            while (mid > 0 && mid < hi && (static_cast<unsigned char>(st[mid]) & 0xC0) == 0x80)
                --mid;
            if (mid <= lo) { hi = lo; break; }
            std::snprintf(probe, sizeof(probe), "%.*s…", static_cast<int>(mid), st);
            if (ImGui::CalcTextSize(probe, nullptr, false, wrap_w).y <= two_lines)
                lo = mid;
            else
                hi = mid - 1;
        }
        std::snprintf(shown, sizeof(shown), "%.*s…", static_cast<int>(lo), st);
    }
    const ImVec2 status_pos = ImGui::GetCursorPos();
    ImGui::PushTextWrapPos(wrap_x);
    if (warn_st) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.warn));
        ImGui::TextWrapped("%s", shown);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextColored(col(th.accent), "%s", shown);
    }
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(status_pos.x, status_pos.y + two_lines));
    ImGui::Dummy(ImVec2(0, px(8)));
    const float bar = (m->setup_prepare_fraction >= 0.0f)
                          ? m->setup_prepare_fraction
                          : m->setup_prepare_pulse;
    ImGui::ProgressBar(bar, ImVec2(-1.0f, px(14)), "");

    if (m->setup_error[0]) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.warn), "%s", m->setup_error);
        ImGui::PopTextWrapPos();
    }

    ImGui::EndPopup();
    /* Esc / outside click must not dismiss while the job is running. */
    if (m->setup_preparing && !ImGui::IsPopupOpen(kProgPopup))
        ImGui::OpenPopup(kProgPopup);
}

/* A hover "?" that parks a long explanation off-screen until asked for.
 * The wizard's disc note is the tallest block on the page and is read once,
 * on a first run; the disc rows underneath it are read every time. */
static void setup_help_marker(const LauncherTheme& th, const char* text) {
    ImGui::TextColored(col(th.text_muted), "(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/* Multi-disc media section: one row per disc of the set, inside a single
 * bordered frame.
 *
 * The wizard previously asked for one image, because persist_setup carried one
 * path -- so a 3-disc set left the player to discover in the middle of disc 1's
 * ending that discs 2 and 3 were never recorded. Asking for all of them up
 * front is the fix; asking for them as three copies of the old full-height
 * block is not, because the wizard is a fixed-height modal and three of those
 * do not fit.
 *
 * So: the explanation collapses to a (?), the verdict block is drawn once for
 * the set rather than once per disc, and each disc costs exactly one frame-high
 * row. A 3-disc set is shorter here than the single-disc layout it replaces.
 *
 * Only disc 1 gates progress. Generate runs off the boot image, and discs 2..N
 * matter later, at swap time -- blocking the wizard on all of them would strand
 * a player who has disc 1 to hand and the rest in a drawer. */
/* Multi-disc media section: one row per disc of the set, in one bordered frame.
 *
 * The wizard previously asked for one image, because persist_setup carried one
 * path -- so a 3-disc set left the player to discover mid-game that discs 2 and
 * 3 were never recorded. Asking for all of them is the fix; asking as N copies
 * of the old full-height block is not, because the wizard is a fixed-height
 * modal.
 *
 * A row is: "Disc N", the file, and Browse. There is deliberately no status
 * column -- the path IS the status (a located disc shows its file, an unlocated
 * one shows the greyed name to look for), and a separate OK/Needed column both
 * repeated that and cost the width the path needed.
 *
 * Every disc is required. Building one disc at a time is not supported, so a
 * partially located set cannot produce a working install and the wizard should
 * not let one through.
 */
static void draw_setup_disc_frame(LauncherModel* m, const LauncherTheme& th,
                                  const char* const* patterns,
                                  int pattern_count, const char* pattern_desc,
                                  const char* long_note) {
    const int count = launcher_model_disc_count(m);
    const int ready = launcher_model_discs_ready_count(m);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(ready >= count ? th.good : th.warn),
                       "%d of %d located", ready, count);
    ImGui::SameLine();
    ImGui::TextColored(col(th.text_muted), "- all discs are required.");
    ImGui::SameLine();
    setup_help_marker(th, long_note);

    /* Size the frame to hold every row. Getting this wrong scrolls the last
     * disc out of sight, which on a 3-disc set hides a required field. */
    const float row_h = ImGui::GetFrameHeightWithSpacing();
    const float pad = ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::BeginChild("##setup_discs", ImVec2(0, row_h * (float)count + pad),
                      true, ImGuiWindowFlags_NoScrollbar);
    const float btn_w = px(88);
    for (int i = 0; i < count; ++i) {
        const bool ok = launcher_model_disc_ready(m, i);
        const char* dp = launcher_model_disc_path(m, i);
        ImGui::PushID(i);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(th.text), "%s", launcher_model_disc_label(m, i));
        ImGui::SameLine(px(72));

        /* Reserve the button's column first so a long path is elided into the
         * space that is actually left, instead of running under the button. */
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float text_w = row_w - btn_w - px(12);

        char elided[260];
        ImGui::AlignTextToFramePadding();
        if (ok) {
            elide_left(dp, text_w, elided, sizeof(elided));
            ImGui::TextColored(col(th.text), "%s", elided);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dp);
        } else {
            /* Not located: show the file NAME the project was built from. It
             * is the one useful hint we have -- the player is looking for their
             * own copy of that image, and the developer's absolute path is
             * meaningless on their machine while the file name is not. */
            const char* hint = launcher_model_disc_suggested_name(m, i);
            if (hint && hint[0]) {
                elide_left(hint, text_w, elided, sizeof(elided));
                ImGui::TextColored(col(th.text_muted), "%s", elided);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Look for a file named:\n%s", hint);
            } else {
                ImGui::TextColored(col(th.text_muted), "(not selected)");
            }
        }

        ImGui::SameLine();
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > btn_w)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btn_w);
            if (ImGui::Button("Browse", ImVec2(btn_w, 0))) {
                char title[96];
                std::snprintf(title, sizeof(title), "Select %s (.cue/.bin/.car)",
                              launcher_model_disc_label(m, i));
                request_disc_slot_picker(m, i, title, patterns, pattern_count,
                                         pattern_desc, true);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    /* Sets almost always carry the disc number in the path, so one browse is
     * normally enough. Offered rather than automatic: it binds paths the player
     * did not choose, and a wrong guess that appears unasked-for is worse than
     * a button that was not pressed. */
    const bool can_autofill = ready > 0 && ready < count;
    ImGui::BeginDisabled(!can_autofill);
    if (ImGui::Button("Find other discs", ImVec2(px(150), px(28)))) {
        const int filled = launcher_model_autofill_sibling_discs(m);
        std::snprintf(m->setup_status, sizeof(m->setup_status),
                      filled > 0 ? "Located %d more disc image(s)."
                                 : "No sibling disc images found next to the "
                                   "one you picked -- browse for them.",
                      filled);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !can_autofill && ready == 0)
        ImGui::SetTooltip("Pick one disc first, then this looks for the rest.");
}

void draw_setup_wizard_modal(LauncherModel* m, const LauncherTheme& th) {
    if (!m->setup_wizard_supported) return;
    if (!m->setup_wizard_open && !m->setup_preparing) return;
    launcher_model_poll_prepare_disc(m);

    /* While a prepare/rebuild/toolchain job runs, show only the progress window.
     * Skipping BeginPopupModal on "First-run setup" lets ImGui dismiss it. */
    if (m->setup_preparing) {
        draw_setup_progress_modal(m, th);
        return;
    }

    ImGui::OpenPopup("First-run setup");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Auto-sized setup content changes height as validation details appear.
    // Recenter against its current size every frame so it cannot drift or clip.
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    /* Prefer content height (AlwaysAutoResize). Clamp to the work area. */
    const float max_h = vp->WorkSize.y * 0.92f;
    ImGui::SetNextWindowSize(
        ImVec2(px(g_picker.active && g_picker.from_setup ? 720.0f : 640.0f), 0),
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(520), 0),
                                        ImVec2(FLT_MAX, max_h));
    if (!ImGui::BeginPopupModal("First-run setup", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove))
        return;

    const float wrap_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const char* noun = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
    const char* game = (m->game_name && m->game_name[0]) ? m->game_name : "this game";
    if (g_picker.active && g_picker.from_setup) {
        draw_builtin_rom_picker_contents(m, th, false);
        ImGui::EndPopup();
        return;
    }

    /* ---- Page 0: portable toolchain ------------------------------------ */
    if (m->setup_needs_toolchain && m->setup_page == 0) {
        const bool want_update =
            m->setup_tc_ready && m->setup_tc_update_available &&
            !m->setup_tc_update_skipped;
        ImGui::TextColored(col(th.accent),
                           want_update ? "Toolchain update" : "Build tools");
        ImGui::PushTextWrapPos(wrap_x);
        if (want_update) {
            ImGui::TextColored(col(th.text_muted),
                "%s found a newer portable cmake/clang pack. Update now "
                "(recommended), keep your current install for this session, "
                "or install from an offline zip.",
                game);
        } else {
            ImGui::TextColored(col(th.text_muted),
                "%s builds game sources on your machine. Install the portable "
                "cmake/clang pack (cmake-clang-v1), or provide a matching zip "
                "for offline setup.",
                game);
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, px(12)));

        ImGui::TextUnformatted("1. Portable toolchain");
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.text_muted),
            "Automatic download pulls the latest cmake-clang-v1 release and "
            "caches it for reuse across titles (no per-game version pin). "
            "Uncheck to browse a local zip instead "
            "(for offline machines, grab a release asset from the repo below).");
        ImGui::PopTextWrapPos();
        if (want_update || m->setup_tc_local_ver[0] ||
            m->setup_tc_remote_ver[0]) {
            ImGui::Dummy(ImVec2(0, px(4)));
            char ver_line[160];
            std::snprintf(
                ver_line, sizeof(ver_line), "Installed: %s   Latest: %s",
                m->setup_tc_local_ver[0] ? m->setup_tc_local_ver : "—",
                m->setup_tc_remote_ver[0] ? m->setup_tc_remote_ver : "—");
            ImGui::TextColored(col(want_update ? th.warn : th.good), "%s",
                               ver_line);
        }
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::TextColored(col(th.text_muted), "Source / manual download:");
        ImGui::SameLine();
        setup_url_link("TechnicallyComputers/retcomm-toolchains",
                       kRetcommToolchainsUrl);
        ImGui::Dummy(ImVec2(0, px(6)));

        if (ImGui::Checkbox(want_update
                                ? "Download toolchain update##tc"
                                : "Download latest portable toolchain##tc",
                            &m->setup_tc_auto)) {
            if (m->setup_tc_auto)
                m->setup_error[0] = '\0';
        }

        if (!m->setup_tc_auto) {
            ImGui::Dummy(ImVec2(0, px(8)));
            ImGui::TextUnformatted("Toolchain zip");
            ImGui::PushTextWrapPos(wrap_x);
            ImGui::TextColored(col(th.text_muted),
                "Select a cmake-clang-v1-*.zip from "
                "TechnicallyComputers/retcomm-toolchains releases.");
            ImGui::PopTextWrapPos();
            setup_url_link("Open retcomm-toolchains releases",
                           kRetcommToolchainsReleasesUrl);
            ImGui::Dummy(ImVec2(0, px(4)));
            const char* zp = m->setup_tc_zip[0] ? m->setup_tc_zip
                                                : "(none selected)";
            char zelided[220];
            elide_left(zp, px(320), zelided, sizeof(zelided));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(m->setup_tc_zip[0] ? th.good : th.warn), "%s",
                               m->setup_tc_zip[0] ? "OK" : "Needed");
            ImGui::SameLine();
            ImGui::TextColored(
                col(m->setup_tc_zip[0] ? th.text : th.text_muted), "%s",
                zelided);
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(6)));
            if (ImGui::Button("Browse zip…##tc", ImVec2(px(128), px(32)))) {
                PickRequest req;
                req.title = "Select cmake-clang-v1 toolchain zip";
                req.patterns = {"*.zip"};
                req.description = "Toolchain zip archives";
                if (m->setup_tc_zip[0]) req.start_path = m->setup_tc_zip;
                req.from_setup = true;
                req.on_pick = [m](const char* path) {
                    if (!path) return;
                    std::snprintf(m->setup_tc_zip, sizeof(m->setup_tc_zip),
                                  "%s", path);
                    m->setup_error[0] = 0;
                };
                ui_pick(m, std::move(req));
            }
            ImGui::PopStyleVar();
        }

        if (m->setup_status[0]) {
            ImGui::Dummy(ImVec2(0, px(8)));
            ImGui::PushTextWrapPos(wrap_x);
            ImGui::TextColored(col(th.good), "%s", m->setup_status);
            ImGui::PopTextWrapPos();
        }
        if (m->setup_error[0]) {
            ImGui::Dummy(ImVec2(0, px(6)));
            ImGui::PushTextWrapPos(wrap_x);
            ImGui::TextColored(col(th.warn), "%s", m->setup_error);
            ImGui::PopTextWrapPos();
        }

        ImGui::Dummy(ImVec2(0, px(14)));
        const bool can_next = launcher_model_can_advance_toolchain(m);
        if (!can_next) ImGui::BeginDisabled();
        if (ImGui::Button(want_update ? "Update##tc_next" : "Next##tc_next",
                          ImVec2(px(140), px(34)))) {
            launcher_model_start_ensure_toolchain(m);
            if (m->setup_preparing) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                draw_setup_progress_modal(m, th);
                return;
            }
        }
        if (!can_next) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    "Enable automatic download or select a toolchain zip");
        }
        if (want_update) {
            ImGui::SameLine();
            if (ImGui::Button("Skip for now##tc_skip", ImVec2(px(140), px(34))))
                launcher_model_skip_toolchain_update(m);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Keep the installed toolchain for this session.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(px(100), px(34))))
            m->action = LNG_ACTION_QUIT;

        if (m->setup_wizard_open && !ImGui::IsPopupOpen("First-run setup"))
            ImGui::OpenPopup("First-run setup");
        ImGui::EndPopup();
        return;
    }

    /* ---- Page 1: BIOS / disc / generate -------------------------------- */
    const SetupPlatKind plat = setup_platform_kind(m->platform);
    const bool media_confirm = launcher_model_setup_media_confirm_only(m);

    /* A retail BIOS was staged in section 1 that this binary has no backend
     * for: the wizard stays up (disc rows and all) and its primary button
     * becomes Generate & rebuild. */
    const bool bios_regen = launcher_model_setup_needs_bios_regen(m);
    /* The dedicated "Generate from disc" section below is drawn on a full
     * first run only. When it is there it already carries a live Generate &
     * rebuild button, so the footer must not grow a second one. */
    const bool prep_section_shown =
        !media_confirm && (m->prepare_disc_cb || m->prepare_with_progress_cb);

    if (bios_regen) {
        ImGui::TextColored(col(th.accent), "Rebuild required for this BIOS");
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.text_muted),
            "%s was built against a different BIOS. Keep your selection below "
            "and choose Generate & rebuild to compile this one in (you need a "
            "disc image for that), or switch back to OpenBIOS to play now.",
            game);
        ImGui::PopTextWrapPos();
    } else if (media_confirm) {
        // "Confirm disc" on a cartridge console asked a SNES player to confirm
        // a disc. The profile's rom_noun names the medium ("ROM", "Disc").
        const char* medium = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
        ImGui::TextColored(col(th.accent),
                           m->has_bios ? "Confirm BIOS and %s" : "Confirm %s",
                           medium);
        ImGui::PushTextWrapPos(wrap_x);
        if (plat == SETUP_PLAT_PSX && m->has_bios) {
            ImGui::TextColored(col(th.text_muted),
                "This build is already generated. Select a PlayStation BIOS "
                "(or keep OpenBIOS) and a Redump-style .cue with sibling .bin "
                "tracks so %s can launch. Your previous disc/BIOS picks were "
                "cleared from settings.",
                game);
        } else if (m->has_bios) {
            ImGui::TextColored(col(th.text_muted),
                "This build is already ready. Confirm a BIOS image and a "
                "playable %s below — previous picks were cleared from settings.",
                noun);
        } else {
            ImGui::TextColored(col(th.text_muted),
                "This build is already ready. Confirm a playable %s below — "
                "the previous pick was cleared from settings.",
                noun);
        }
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(col(th.accent), "Setup required");
        ImGui::PushTextWrapPos(wrap_x);
        if (plat == SETUP_PLAT_PSX && m->has_bios) {
            /* The BIOS and disc sections below each answer this for themselves,
             * so a paragraph restating both only pushes the controls off a
             * fixed-height modal. The detail it used to carry lives behind the
             * disc section's (?). */
            ImGui::TextColored(col(th.text_muted),
                "%s needs a %s to launch. You must legally own the dumps.",
                game, noun);
        } else if (plat == SETUP_PLAT_GBA && m->has_bios) {
            ImGui::TextColored(col(th.text_muted),
                "%s needs a Game Boy Advance BIOS dump and a playable %s before "
                "you can launch. Pick both below (you must legally own these dumps).",
                game, noun);
        } else if (plat == SETUP_PLAT_SNES) {
            ImGui::TextColored(col(th.text_muted),
                "%s needs a playable Super Nintendo %s before you can launch. "
                "Pick your file below (you must legally own this dump).",
                game, noun);
        } else if (m->has_bios) {
            ImGui::TextColored(col(th.text_muted),
                "%s needs a BIOS image and a playable %s before you can launch. "
                "Pick both below (you must legally own these dumps).",
                game, noun);
        } else {
            ImGui::TextColored(col(th.text_muted),
                "%s needs a playable %s before you can launch. Pick your file below "
                "(you must legally own this dump).",
                game, noun);
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    if (m->has_bios) {
        const bool has_pick = m->s.bios_path[0] != 0;
        const bool offers_bundled = (plat == SETUP_PLAT_PSX);
        const char* bios_section =
            (plat == SETUP_PLAT_GBA) ? "1. GBA BIOS (required)"
            : (plat == SETUP_PLAT_PSX) ? "1. PlayStation BIOS (optional)"
                                      : "1. BIOS";
        const char* bios_help =
            (plat == SETUP_PLAT_GBA)
                ? "Browse for your gba_bios.bin dump (16 KB, SHA-1 "
                  "300c20df… — dumped from a Game Boy Advance). Setup packages "
                  "do not ship a redistributable GBA BIOS."
            : (plat == SETUP_PLAT_PSX)
                ? "Optional \u2014 OpenBIOS is used unless you browse for a "
                  "retail dump (exactly 512 KB)."
                : "Browse for a BIOS image required by this console.";
        const char* empty_bios_label =
            offers_bundled ? "OpenBIOS" : "(none selected)";
        const char* bios_picker =
            (plat == SETUP_PLAT_GBA) ? "Select GBA BIOS (gba_bios.bin)"
            : (plat == SETUP_PLAT_PSX) ? "Select PlayStation BIOS (SCPH1001.BIN)"
                                      : "Select BIOS file";

        ImGui::TextUnformatted(bios_section);
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.text_muted), "%s", bios_help);
        ImGui::PopTextWrapPos();
        const char* bp = has_pick ? m->s.bios_path : empty_bios_label;
        char belided[220];
        elide_left(bp, px(300), belided, sizeof(belided));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(m->setup_bios_ok ? th.good : th.warn), "%s",
                           m->setup_bios_ok ? "OK" : "Needed");
        ImGui::SameLine();
        ImGui::TextColored(col(has_pick ? th.text : th.text_muted), "%s", belided);
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(6)));
        if (ImGui::Button("Browse BIOS##setup", ImVec2(px(120), px(32)))) {
            request_bios_picker(m, bios_picker, true);
        }
        if (offers_bundled) {
            ImGui::SameLine();
            /* Always visible; greyed out when OpenBIOS is already selected. */
            if (!has_pick) ImGui::BeginDisabled();
            if (ImGui::Button("Use OpenBIOS##setup", ImVec2(px(128), px(32))))
                launcher_model_request_bios_path(m, "");
            if (!has_pick) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(has_pick
                                      ? "Switch to bundled OpenBIOS (no rebuild)."
                                      : "OpenBIOS is already selected.");
        }
        ImGui::PopStyleVar();
        if (m->setup_bios_detail[0]) {
            /* "not compiled into this build" is the reason the footer button
             * changed — it must not read as a muted aside. */
            const bool detail_warn =
                m->setup_bios_warn || m->setup_bios_needs_regen;
            ImGui::PushTextWrapPos(wrap_x);
            ImGui::TextColored(col(detail_warn ? th.warn : th.text_muted),
                               "%s", m->setup_bios_detail);
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, px(12)));
    }

    /* A multi-disc set asks for every image, in one compact frame, and parks the
     * long note behind a (?). See draw_setup_disc_frame. */
    const bool multi_disc = launcher_model_disc_count(m) > 1;
    static const char* kPsxDiscNote =
        "psxrecomp games require a .cue + .bin dump of the disc, or an "
        "official re-release image (.car, e.g. from the Steam Special "
        "Edition).\n\n"
        "A single-track .bin or .iso cannot be converted to multitrack. "
        "Redump-style dumps are the usual source; you can make your own from "
        "the original disc with redumper "
        "(https://github.com/superg/redumper).\n\n"
        "Always point Generate at the .cue when one exists.";

    if (multi_disc)
        ImGui::Text("%s. Disc images (%d)", m->has_bios ? "2" : "1",
                    launcher_model_disc_count(m));
    else
        ImGui::Text("%s. %s image", m->has_bios ? "2" : "1", noun);
    ImGui::PushTextWrapPos(wrap_x);
    if (multi_disc) {
        ImGui::PopTextWrapPos();
        static const char* kDiscPatterns[] = {"*.cue", "*.bin", "*.img",
                                              "*.iso", "*.car"};
        draw_setup_disc_frame(m, th, kDiscPatterns, 5,
                              "Disc image (.cue .bin .img .iso .car)",
                              kPsxDiscNote);
        ImGui::PushTextWrapPos(wrap_x);
    } else if (plat == SETUP_PLAT_PSX) {
        /* Keep the note as one wrapped TextColored block — SameLine +
         * TextLinkOpenURL after a wrapped line leaves a huge empty gap in
         * BeginPopupModal (Windows first-run wizard regression). */
        ImGui::TextColored(col(th.text_muted),
            "NOTE: psxrecomp games require a .cue + .bin dump of the disc, "
            "or an official re-release image (.car, e.g. from the Steam "
            "Special Edition). "
            "Note the number of tracks required by this project; multitrack "
            "discs are often Redump-formatted dumps. You can generate your own "
            "from the original disc with redumper "
            "(https://github.com/superg/redumper). You cannot convert a "
            "single-track .bin or .iso to multitrack.");
        ImGui::PopTextWrapPos();
        setup_url_link("Open redumper", "https://github.com/superg/redumper");
        ImGui::PushTextWrapPos(wrap_x);
    } else {
        const char* media_help =
            (plat == SETUP_PLAT_GBA)
                ? "Select your verified Game Boy Advance ROM (.gba)."
            : (plat == SETUP_PLAT_SNES)
                ? "Select your Super Nintendo ROM (.sfc / .smc)."
                : "Select your game ROM file.";
        ImGui::TextColored(col(th.text_muted), "%s", media_help);
    }
    ImGui::PopTextWrapPos();
    if (!multi_disc) {
        const char* rp = m->rom_present ? m->rom_full : "(none selected)";
        char relided[220];
        elide_left(rp, px(360), relided, sizeof(relided));
        const bool rom_ok = m->rom_present && strcmp(m->rom_size, "--") != 0 &&
                            !(m->profile && m->profile->verify.mode == 1 &&
                              (m->verify.verdict == 0 || m->verify.verdict == 3));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(rom_ok ? th.good : th.warn), "%s",
                           rom_ok ? "OK" : "Needed");
        ImGui::SameLine();
        ImGui::TextColored(col(th.text), "%s", relided);
        ImGui::SameLine();
        char browse_lbl[48];
        std::snprintf(browse_lbl, sizeof(browse_lbl), "Browse %s##setup", noun);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(6)));
        if (ImGui::Button(browse_lbl, ImVec2(px(128), px(32)))) {
            const SystemProfile* prof = (const SystemProfile*)m->profile;
            char title[96];
            if (plat == SETUP_PLAT_PSX)
                std::snprintf(title, sizeof(title),
                              "Select %s (.cue/.bin/.car)", noun);
            else
                std::snprintf(title, sizeof(title), "Select %s", noun);
            if (prof && prof->rom_filter.pattern_count > 0)
                request_rom_picker(m, title, prof->rom_filter.patterns,
                                   prof->rom_filter.pattern_count,
                                   prof->rom_filter.desc, true);
            else
                request_rom_picker(m, title, NULL, 0, NULL, true);
        }
        ImGui::PopStyleVar();
        if (m->profile && m->profile->verify.mode == 1)
            draw_verdict_block(m, th, ImGui::GetContentRegionAvail().x);
        if (plat == SETUP_PLAT_PSX && m->rom_present && m->rom_full[0]) {
            /* .car is exempt: official Steam re-release payloads (e.g. Tomba!
             * Special Edition's t_data_u.car) are complete single-file raw
             * images that never ship with a cue sheet. */
            const char* ext = strrchr(m->rom_full, '.');
            if (ext && (lps_streq_ci(ext, ".bin") || lps_streq_ci(ext, ".img"))) {
                ImGui::PushTextWrapPos(wrap_x);
                ImGui::TextColored(col(th.warn),
                    "You picked a track image (%s). Prefer the matching .cue "
                    "in the same folder so multitrack discs are handled "
                    "correctly.",
                    ext);
                ImGui::PopTextWrapPos();
            }
        }
    }

    /* Full first-run only: Generate & rebuild. Media-confirm (cleared disc/
     * BIOS with sources already present) skips this — regenerate stays in
     * Settings → SYSTEM when the host exposes it. */
    if (!media_confirm &&
        (m->prepare_disc_cb || m->prepare_with_progress_cb)) {
        ImGui::Dummy(ImVec2(0, px(12)));
        char section_buf[128];
        const char* section;
        if (m->prepare_section_title && m->prepare_section_title[0]) {
            std::snprintf(section_buf, sizeof(section_buf), "%s. %s",
                          m->has_bios ? "3" : "2", m->prepare_section_title);
            section = section_buf;
        } else if (plat == SETUP_PLAT_PSX) {
            section = m->has_bios ? "3. Generate from disc"
                                  : "2. Generate from disc";
        } else {
            section = m->has_bios ? "3. Convert raw dump (optional)"
                                  : "2. Convert raw dump (optional)";
        }
        ImGui::TextUnformatted(section);
        ImGui::PushTextWrapPos(wrap_x);
        const char* prep_note =
            (m->prepare_disc_note && m->prepare_disc_note[0])
                ? m->prepare_disc_note
                : (plat == SETUP_PLAT_PSX
                       ? "Uses the selected disc image (.cue + track .bin "
                         "files, or a single-file .bin/.car) with the "
                         "local SDK, then "
                         "verifies digests against the game identity. Always "
                         "point Generate at the .cue when one exists."
                       : "If your disc image is a raw dump that this game "
                         "cannot boot directly, convert it here. Output is "
                         "written next to the game.");
        ImGui::TextColored(col(th.text_muted), "%s", prep_note);
        ImGui::PopTextWrapPos();
        const char* prep_lbl = (m->prepare_disc_label && m->prepare_disc_label[0])
                                   ? m->prepare_disc_label
                                   : ((m->rebuild_after_prepare &&
                                       m->rebuild_with_progress_cb)
                                          ? "Generate & rebuild…"
                                          : (plat == SETUP_PLAT_PSX
                                                 ? "Prepare disc…"
                                                 : "Convert raw dump…"));
        const bool use_selected = m->prepare_use_selected_rom;
        const bool can_prep_selected = use_selected && m->rom_present &&
                                       m->rom_full[0] &&
                                       strcmp(m->rom_size, "--") != 0;
        if (use_selected && !can_prep_selected) ImGui::BeginDisabled();
        if (ImGui::Button(prep_lbl, ImVec2(px(240), px(32)))) {
            if (use_selected) {
                if (can_prep_selected)
                    launcher_model_start_prepare_disc(m, m->rom_full);
            } else {
                /* First-run setup on a host with no working zenity/kdialog
                 * used to dead-end right here, before the player had a game. */
                PickRequest req;
                req.title = (plat == SETUP_PLAT_PSX)
                                ? "Select disc (.cue/.bin/.car)"
                                : "Select raw disc dump to convert";
                req.patterns = (plat == SETUP_PLAT_PSX)
                                   ? std::vector<std::string>{"*.cue", "*.bin",
                                                              "*.car"}
                                   : std::vector<std::string>{
                                         "*.cue", "*.iso", "*.bin", "*.img",
                                         "*.car", "*.chd", "*.*"};
                req.description = (plat == SETUP_PLAT_PSX)
                                      ? "PlayStation disc (.cue/.bin/.car)"
                                      : "Disc dump";
                req.from_setup = true;
                req.on_pick = [m](const char* path) {
                    if (path) launcher_model_start_prepare_disc(m, path);
                };
                ui_pick(m, std::move(req));
            }
        }
        if (use_selected && !can_prep_selected) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Select a verified %s first", noun);
        }
        if (m->setup_preparing) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            draw_setup_progress_modal(m, th);
            return;
        }
    }

    if (m->setup_status[0]) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.good), "%s", m->setup_status);
        ImGui::PopTextWrapPos();
    }
    if (m->setup_error[0]) {
        ImGui::Dummy(ImVec2(0, px(6)));
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextColored(col(th.warn), "%s", m->setup_error);
        ImGui::PopTextWrapPos();
    }

    ImGui::Dummy(ImVec2(0, px(14)));
    if (m->setup_needs_toolchain) {
        if (ImGui::Button("Back", ImVec2(px(100), px(34)))) {
            m->setup_page = 0;
            /* Re-offer the update UI if a newer pack is still available. */
            m->setup_tc_update_skipped = false;
            m->setup_error[0] = '\0';
            m->setup_status[0] = '\0';
        }
        ImGui::SameLine();
    }
    if (bios_regen && !prep_section_shown) {
        /* The staged BIOS has no linked backend, so Confirm/Continue could
         * never succeed. Same button, same place — it just does the thing the
         * player was told they need. */
        const char* blocker = launcher_model_setup_bios_regen_blocker(m);
        if (blocker) ImGui::BeginDisabled();
        if (ImGui::Button("Generate & rebuild", ImVec2(px(220), px(34))))
            launcher_model_setup_start_bios_regen(m);
        if (blocker) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", blocker);
        }
        ImGui::SameLine();
        if (ImGui::Button("Use OpenBIOS", ImVec2(px(130), px(34))))
            launcher_model_request_bios_path(m, "");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play now with the bundled OpenBIOS (no rebuild).");
        ImGui::SameLine();
    } else if (!m->prepare_required_before_continue) {
        const bool ready = launcher_model_can_finish_setup(m);
        char confirm_lbl[64];
        {
            const char* medium = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
            snprintf(confirm_lbl, sizeof(confirm_lbl),
                     m->has_bios ? "Confirm BIOS and %s" : "Confirm %s", medium);
        }
        const char* continue_lbl = media_confirm ? confirm_lbl : "Continue to launcher";
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button(continue_lbl, ImVec2(px(220), px(34)))) {
            launcher_model_finish_setup(m);
            ImGui::CloseCurrentPopup();
        }
        if (!ready) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!m->rom_present || !m->rom_full[0])
                    ImGui::SetTooltip("Select a %s first", noun);
                else if (m->setup_preparing)
                    ImGui::SetTooltip("Wait for the current job to finish");
                else if (m->has_bios && m->setup_bios_needs_regen)
                    ImGui::SetTooltip("Generate & rebuild with this BIOS "
                                      "first, or switch to OpenBIOS");
                else if (m->has_bios && !m->setup_bios_ok)
                    ImGui::SetTooltip("BIOS check required");
                else if (m->netplay_supported &&
                         m->profile && m->profile->verify.mode == 1 &&
                         !m->verify.netplay_ok)
                    ImGui::SetTooltip("Disc mount / track layout not accepted");
            }
        }
        ImGui::SameLine();
    } else if (launcher_model_can_finish_setup(m) &&
               m->action != LNG_ACTION_RELAUNCH) {
        launcher_model_finish_setup(m);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("Quit", ImVec2(px(100), px(34))))
        m->action = LNG_ACTION_QUIT;

    if (m->setup_wizard_open && !ImGui::IsPopupOpen("First-run setup"))
        ImGui::OpenPopup("First-run setup");
    ImGui::EndPopup();
}


static const char* generate_disabled_reason(const LauncherModel* m) {
    if (!m) return "Generate is unavailable.";
    const bool has_prep =
        m->prepare_with_progress_cb != nullptr || m->prepare_disc_cb != nullptr;
    if (!has_prep) {
        return "Generate is unavailable (project/SDK not found).\n"
               "Launch from Retro Launcher, or run the game from its source tree "
               "(src/current).";
    }
    if (!m->rom_present || !m->rom_full[0])
        return "Select a disc image first";
    return "Generate is unavailable.";
}

void draw_bios_confirm_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->bios_confirm_open) ImGui::OpenPopup("Switch BIOS?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Switch BIOS?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(420));
    ImGui::TextWrapped(
        "This retail BIOS is not compiled into the current build yet. "
        "Generate & rebuild will emit its BIOS C and rebuild the game binary "
        "with your current disc and toolchain. OpenBIOS switches never need "
        "this — use Use OpenBIOS instead.");
    if (m->bios_pending_path[0]) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Selected:");
        ImGui::TextWrapped("%s", m->bios_pending_path);
    } else {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Selected: OpenBIOS");
    }
    if (m->setup_bios_detail[0]) {
        ImGui::Dummy(ImVec2(0, px(6)));
        ImGui::TextColored(col(th.warn), "%s", m->setup_bios_detail);
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(12)));
    const bool can_gen = m->rom_present && m->rom_full[0] &&
                         (m->prepare_with_progress_cb || m->prepare_disc_cb);
    if (!can_gen) ImGui::BeginDisabled();
    if (ImGui::Button("Generate & rebuild…", ImVec2(px(200), px(32))))
        launcher_model_bios_confirm_accept(m);
    if (!can_gen) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", generate_disabled_reason(m));
    }
    ImGui::SameLine();
    if (ImGui::Button("Use OpenBIOS", ImVec2(px(130), px(32)))) {
        launcher_model_bios_confirm_cancel(m);
        launcher_model_request_bios_path(m, "");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100), px(32))))
        launcher_model_bios_confirm_cancel(m);
    if (!m->bios_confirm_open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_bios_play_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->bios_play_modal_open) ImGui::OpenPopup("BIOS not ready to Play");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("BIOS not ready to Play", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(440));
    ImGui::TextWrapped(
        "The selected retail BIOS is not compiled into this build. Generate & "
        "rebuild with it (same disc and toolchain), switch to OpenBIOS to Play "
        "without rebuilding, or cancel and keep the current selection.");
    if (m->s.bios_path[0]) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Current selection:");
        ImGui::TextWrapped("%s", m->s.bios_path);
    } else {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Current selection: OpenBIOS");
    }
    if (m->setup_bios_detail[0]) {
        ImGui::Dummy(ImVec2(0, px(6)));
        ImGui::TextColored(col(th.warn), "%s", m->setup_bios_detail);
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(12)));
    const bool can_gen = m->rom_present && m->rom_full[0] &&
                         (m->prepare_with_progress_cb || m->prepare_disc_cb);
    if (!can_gen) ImGui::BeginDisabled();
    if (ImGui::Button("Generate & rebuild…", ImVec2(px(200), px(32))))
        launcher_model_bios_play_generate(m);
    if (!can_gen) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", generate_disabled_reason(m));
    }
    ImGui::SameLine();
    if (ImGui::Button("Use OpenBIOS", ImVec2(px(130), px(32))))
        launcher_model_bios_play_use_openbios(m);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100), px(32))))
        launcher_model_bios_play_cancel(m);
    if (!m->bios_play_modal_open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_pgo_confirm_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->pgo_confirm_open) ImGui::OpenPopup("Optimize FMV Playback?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Optimize FMV Playback?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(460));
    ImGui::TextWrapped(
        "This will rebuild an instrumented binary, run the intro FMV in PGO "
        "training mode (video window open — do not close or interact with it), "
        "then rebuild the game with those profiles. Existing generated C is "
        "reused; the setup wizard is not run.");
    ImGui::Dummy(ImVec2(0, px(8)));
    ImGui::TextColored(col(th.warn),
                       "Expect several minutes. Keep this launcher open until "
                       "training finishes, then the optimized binary relaunches.");
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::TextColored(col(th.text_muted),
                       "Each peer may run this independently for rollback "
                       "netplay. Digests must still match.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(12)));
    if (ImGui::Button("Start training", ImVec2(px(160), px(32))))
        launcher_model_pgo_confirm_accept(m);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100), px(32))))
        launcher_model_pgo_confirm_cancel(m);
    if (!m->pgo_confirm_open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_fmv_timing_confirm_modal(LauncherModel* m, const LauncherTheme& th) {
    if (m->fmv_timing_confirm_open)
        ImGui::OpenPopup("Apply FMV Timing Opt?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Apply FMV Timing Opt?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(460));
    ImGui::TextWrapped(
        "This regenerates game C from game.toml (including "
        "load_charge_batch for VLC leaves), then rebuilds. It does not "
        "disable load-delay and does not run PGO training.");
    ImGui::Dummy(ImVec2(0, px(8)));
    ImGui::TextColored(col(th.warn),
                       "Expect a few minutes for generate + rebuild. The "
                       "launcher will relaunch the new binary when done.");
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::TextColored(col(th.text_muted),
                       "Both peers need the same load_charge_batch setting "
                       "for matching rollback digests.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, px(12)));
    if (ImGui::Button("Generate & rebuild", ImVec2(px(180), px(32))))
        launcher_model_fmv_timing_confirm_accept(m);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100), px(32))))
        launcher_model_fmv_timing_confirm_cancel(m);
    if (!m->fmv_timing_confirm_open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_skip_modal(LauncherModel* m) {
    if (m->skip_modal_open) ImGui::OpenPopup("Skip the launcher on boot?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Skip the launcher on boot?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("The launcher will no longer appear - the game boots straight in. "
                           "Run with \"--launcher\" or set \"SkipLauncher = 0\" in config.ini "
                           "to bring it back.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            launcher_model_skip_cancel(m); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip on Boot", ImVec2(px(140), 0))) {
            launcher_model_skip_confirm(m); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_restore_defaults_modal(LauncherModel* m) {
    if (m->defaults_modal_open) ImGui::OpenPopup("Restore default settings?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restore default settings?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "This resets display, audio, controller, and launcher choices. "
            "Your selected ROM, SRAM, and save states are not changed.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            launcher_model_cancel_restore_defaults(m);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore Defaults", ImVec2(px(150), 0))) {
            launcher_model_restore_defaults(m);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_ui(LauncherModel* m, const LauncherTheme& th, int logical_w, int logical_h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    // CRT ground + scanlines behind everything.
    draw_crt_background(vp->Pos, vp->Size);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // let CRT show
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    /* The lobby room is a full-screen view keyed to seat state: seated in a
     * lobby means the room, whatever view was up before (join from the
     * browser, or a soft-return from a match); not seated means never the
     * room. Done here, before the header, so the header and footer draw for
     * the view the body will actually show. */
    if (m->netplay_supported) {
        const auto* np = np_cb(m);
        const bool seated = np_lobby_seated(m, np);
        if (seated && m->view != LNG_VIEW_LOBBY) {
            launcher_model_set_view(m, LNG_VIEW_LOBBY);
            g_lobby_settings_synced = false;
        } else if (!seated && m->view == LNG_VIEW_LOBBY) {
            m->netplay_local_room = false;
            m->netplay_lobby_settings_open = false;
            m->netplay_lobby_mods_open = false;
            g_lobby_settings_synced = false;
            launcher_model_set_view(m, LNG_VIEW_NETPLAY);
            m->netplay_list_fresh = false;
        }
    }

    // ---- Marquee header: brand · GAME TITLE · subtitle .......... [nav] ----
    ImVec2 hp = ImGui::GetCursorScreenPos();
    // Vertically center the brand mark against the two-line title block (game
    // title at 1.55x + platform at 1.0x). Computed from the mark's fitted height
    // so a short wide mark (e.g. the NES "Nintendo" pill, ~4:1) sits centered
    // between the two lines rather than hugging the first.
    float hdr_top = ImGui::GetCursorPosY();
    // Header brand mark. Drawn only when the profile ships one: a profile can
    // opt out (empty brand, e.g. N64 which leads with its wordmark), in which
    // case g_brand never loads and the title block starts flush-left with no
    // reserved gap. When present, the shared default is a 44x33 square mark; a
    // console that ships its OWN (typically wide) wordmark (SystemProfile.brand
    // — the NES pill, the Genesis logo) draws it larger, sized to a target
    // HEIGHT with proportional width so the logo reads clearly.
    if (g_brand.id && g_brand.w > 0) {
        const SystemProfile* hprof = (const SystemProfile*)m->profile;
        const bool own_brand = hprof && hprof->brand && hprof->brand[0];
        float brand_w = 44.0f, brand_h = 33.0f;
        if (own_brand && g_brand.h > 0) {
            brand_h = 32.0f;
            brand_w = brand_h * (float)g_brand.w / (float)g_brand.h;   // preserve aspect
            if (brand_w > 130.0f) brand_w = 130.0f;                    // sane upper bound
        }
        // Vertically center against the two-line title block (game title at
        // 1.55x + platform at 1.0x), from the fitted height.
        float line_h  = ImGui::GetTextLineHeight();
        float block_h = line_h * 1.55f + ImGui::GetStyle().ItemSpacing.y + line_h;
        float fit_h = px(brand_h);
        if (g_brand.h > 0) {
            float s = (px(brand_w) / g_brand.w < px(brand_h) / g_brand.h)
                        ? px(brand_w) / (float)g_brand.w : px(brand_h) / (float)g_brand.h;
            fit_h = g_brand.h * s;
        }
        ImGui::SetCursorPosY(hdr_top + (block_h - fit_h) * 0.5f);
        image_fit(g_brand, brand_w, brand_h); ImGui::SameLine(0, px(12));
        ImGui::SetCursorPosY(hdr_top);
    }
    ImGui::BeginGroup();
        ImGui::SetWindowFontScale(1.55f);
        ImGui::TextUnformatted(m->game_name ? m->game_name : "(null)");
        ImGui::SetWindowFontScale(1.0f);
        // Platform lockup: the wordmark image when a host supplied one
        // (SystemProfile.wordmark_image), else the plain platform text.
        if (g_wordmark.id && g_wordmark.w > 0) {
            ImGui::Dummy(ImVec2(0, px(2)));
            image_fit(g_wordmark, 240, 20);
        } else if (m->platform && m->platform[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
            ImGui::TextUnformatted(m->platform);
            ImGui::PopStyleColor();
        }
    ImGui::EndGroup();
    {   // right-aligned top-level navigation
        const float gap = px(6.0f);
        const float resume_x = ImGui::GetCursorPosX();
        const float resume_y = ImGui::GetCursorPosY();
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float y = hdr_top + px(6.0f);
        if (m->view == LNG_VIEW_DASHBOARD) {
            const int count = 1 + (m->mods ? 1 : 0) +
                              (m->has_assist_tools ? 1 : 0) +
                              ((m->credits_text && m->credits_text[0]) ? 1 : 0);
            const float w = px(110.0f);
            const float total = w * count + gap * (count - 1);
            ImGui::SetCursorPos(ImVec2(right - total, y));
            if (ImGui::Button(ui_text("Settings"), ImVec2(w, px(34))))
                launcher_model_set_view(m, LNG_VIEW_SETTINGS);
            if (m->mods) {
                ImGui::SameLine(0, gap);
                if (ImGui::Button(ui_text("Mods"), ImVec2(w, px(34))))
                    launcher_model_set_view(m, LNG_VIEW_MODS);
            }
            if (m->has_assist_tools) {
                ImGui::SameLine(0, gap);
                if (ImGui::Button(ui_text("Assist Tools"), ImVec2(w, px(34))))
                    launcher_model_set_view(m, LNG_VIEW_ASSIST_TOOLS);
            }
            if (m->credits_text && m->credits_text[0]) {
                ImGui::SameLine(0, gap);
                if (ImGui::Button(ui_text("Credits"), ImVec2(w, px(34))))
                    launcher_model_set_view(m, LNG_VIEW_CREDITS);
            }
        } else {
            const float w = px(110.0f);
            const float name_w = px(170.0f);
            if ((m->view == LNG_VIEW_NETPLAY ||
                 m->view == LNG_VIEW_NETPLAY_MODE) && m->netplay_supported) {
                ImGui::SetCursorPos(ImVec2(right - w - name_w - gap, y));
                /* Signed in, this is the account's handle, not the LAN name:
                 * it is what other players are actually seeing. */
                const char* eff = np_effective_name(m);
                char lbl[96];
                emoji_display(eff && eff[0] ? eff : ui_text("Set player name"),
                              lbl, sizeof(lbl));
                if (ImGui::Button(lbl, ImVec2(name_w, px(34))))
                    np_open_name_modal(m);
            }
            if (m->view == LNG_VIEW_LOBBY) {
                /* No Back: you are seated, and the only way out of a seat is
                 * the footer's Leave Lobby — a Back that quietly kept the seat
                 * would leave a ghost in the room. */
                char summary[192];
                const char* label =
                    np_lobby_mods_summary(m, summary, sizeof(summary))
                        ? summary : ui_text("LOBBY");
                const float tw = ImGui::CalcTextSize(label).x;
                ImGui::SetCursorPos(ImVec2(
                    right - tw, y + (px(34) - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::TextColored(col(th.accent2), "%s", label);
                (void)w;
            } else {
                ImGui::SetCursorPos(ImVec2(right - w, y));
                if (ImGui::Button(ui_text("< Back"), ImVec2(w, px(34)))) {
                    /* Retrace the netplay fork rather than dropping to the
                     * dashboard from the middle of it: the browser and the
                     * sign-in page were both reached through the chooser. */
                    if (m->view == LNG_VIEW_NETPLAY ||
                        m->view == LNG_VIEW_NETPLAY_SIGNIN)
                        launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE);
                    else
                        launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
                }
            }
        }
        // Absolute placement prevents three dashboard buttons from mutating
        // the title group's line state and wrapping Settings into the body.
        ImGui::SetCursorPos(ImVec2(resume_x, resume_y));
    }
    // marquee underline: neon gradient rule under the header
    ImGui::Dummy(ImVec2(0, px(8.0f)));
    {
        ImVec2 u = ImGui::GetCursorScreenPos();
        float fw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(u, ImVec2(u.x + fw, u.y + px(2.0f)),
            imcol(th.accent2, 0.9f), imcol(th.accent2, 0.15f),
            imcol(th.accent2, 0.15f), imcol(th.accent2, 0.9f));
        glow_rect(dl, u, ImVec2(u.x + fw*0.5f, u.y + px(2.0f)), 0, th.accent2, 0.5f, 3);
    }
    ImGui::Dummy(ImVec2(0, px(12.0f)));
    (void)hp;

    // Body: fixed-height child that scrolls when content overflows, so nothing
    // is ever clipped out of reach. The footer below stays fixed (in the fold).
    const float footer_h = px(92.0f);   // divider + clearance + CTA + its glow
    float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    if (body_h < px(80.0f)) body_h = px(80.0f);
    begin_container("body", ImVec2(0, body_h));
    switch (m->view) {
        case LNG_VIEW_DASHBOARD:  draw_dashboard(m, th, logical_w); break;
        case LNG_VIEW_SETTINGS:   draw_settings(m, th);             break;
        case LNG_VIEW_CONTROLLER: draw_controller(m, th);           break;
        case LNG_VIEW_NETPLAY:    draw_netplay(m, th);              break;
        case LNG_VIEW_NETPLAY_MODE:   draw_netplay_mode_page(m, th);   break;
        case LNG_VIEW_NETPLAY_SIGNIN: draw_netplay_signin_page(m, th); break;
        case LNG_VIEW_MODS:       draw_mods(m, th);                 break;
        case LNG_VIEW_ASSIST_TOOLS: draw_assist_tools(m, th);        break;
        case LNG_VIEW_CREDITS:      draw_credits(m, th);             break;
        case LNG_VIEW_LOBBY:        draw_lobby(m, th);               break;
    }
    end_container();

    draw_footer(m, th, footer_h);
    draw_setup_wizard_modal(m, th);
    draw_bios_confirm_modal(m, th);
    draw_bios_play_modal(m, th);
    draw_pgo_confirm_modal(m, th);
    draw_fmv_timing_confirm_modal(m, th);
    draw_standalone_builtin_rom_picker(m, th);
    draw_skip_modal(m);
    draw_netplay_player_modal(m, th);
    draw_netplay_network_modal(m, th);
    draw_netplay_host_modal(m, th);
    draw_netplay_password_modal(m, th);
    draw_netplay_direct_modal(m, th);
    draw_netplay_automatch_modal(m, th);
    draw_netplay_moderation_modal(m, th);
    draw_netplay_report_modal(m, th);
    draw_restore_defaults_modal(m);
    // Transfer Pak config modal (N64): opened by any tile, dashboard or
    // Controller page. Drawn at root so it isn't clipped by a card child.
    if (m->tpak_slots > 0) draw_tpak_modal(m, th);
    ImGui::End();
    (void)logical_h;
}

bool is_modifier_scancode(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ||
           sc == SDL_SCANCODE_LALT  || sc == SDL_SCANCODE_RALT  ||
           sc == SDL_SCANCODE_LSHIFT|| sc == SDL_SCANCODE_RSHIFT ||
           sc == SDL_SCANCODE_LGUI  || sc == SDL_SCANCODE_RGUI;
}

// Keyboard capture for the rebind editors. Player buttons persist a SCANCODE to
// keybinds.ini; system hotkeys persist a KEYCODE+mods to config.ini [KeyMap].
/* The SDL2-only raw_input_is_mapped() helper stood here. Its one caller was
 * the N64 field-capture path, which bound a raw joystick button or axis for a
 * pad whose SDL_GameController mapping could not express the input (PSR issue
 * #15, the 8BitDo 64's C-buttons). N64 gamepad binds now live in the shared
 * per-GUID store, whose vocabulary is SDL gamepad names, so a raw field can no
 * longer be captured or written — see consoles/n64/n64_pad_binds.h. Kept as a
 * note rather than dead code, because "why can I not bind this pad's C-buttons
 * any more" deserves an answer at the place the answer used to live. */

bool try_capture(LauncherModel* m, const SDL_Event& ev) {
    if (!m->capturing && !m->hk_capturing &&
        !m->camera_capturing)
        return false;

    // ESC cancels any capture — keyboard, pad, or hotkey.
    if (ev.type == SDL_EVENT_KEY_DOWN && LNG_EVKEY(ev) == SDLK_ESCAPE) {
        launcher_model_cancel_capture(m);
        launcher_model_cancel_hk_capture(m);
        launcher_model_cancel_camera_capture(m);
        return true;
    }

    /* Backspace UNBINDS the input being captured -- every capture kind, so a
     * mapping button never needs a separate clear gesture. Each branch is the
     * same commit path a real press takes, handed the store's own "nothing"
     * value: scancode 0 for a keyboard slot, LNG_PADBIND_NONE for a pad slot,
     * keycode 0 for a hotkey. Inside a Map All / Auto Map run this clears the
     * current input and moves to the next, which is how you skip one. */
    if (ev.type == SDL_EVENT_KEY_DOWN && LNG_EVKEY(ev) == SDLK_BACKSPACE &&
        !LNG_EVKEYREPEAT(ev)) {
        if (m->camera_capturing) {
            launcher_binds_set_camera(m, m->capture_camera, 0);
            launcher_model_cancel_camera_capture(m);
            return true;
        }
        if (m->hk_capturing) {
            launcher_binds_set_hotkey(m, m->capture_hk, 0, 0);
            launcher_model_cancel_hk_capture(m);
            return true;
        }
        if (m->capturing && m->capture_pad) {
            if (m->capture_assist)
                launcher_model_set_captured_pad(m, 0);
            else
                launcher_binds_set_pad_button(m, m->cfg_player + 1, m->capture_btn,
                                              LNG_PADBIND_NONE, 0, 0);
        } else if (m->capturing) {
            const SystemProfile* prof = (const SystemProfile*)m->profile;
            if (m->capture_assist)
                launcher_model_set_captured_key(m, 0);
            else if (prof && prof->controller.binds_per_input >= 2 && prof->id &&
                     !strcmp(prof->id, "psx"))
                launcher_binds_set_button_slot(m, m->cfg_player + 1, m->capture_btn,
                                               m->capture_slot, 0);
            else if (prof && prof->controller.binds_per_input >= 2)
                launcher_binds_set_field(m, m->cfg_player + 1, m->capture_btn,
                                         m->capture_slot, RUI_N64_FIELD_NONE, -1);
            else
                launcher_binds_set_button(m, m->cfg_player + 1, m->capture_btn, 0);
        } else {
            return true;
        }
        if (m->map_all_active) launcher_model_map_all_advance(m);
        else                   launcher_model_cancel_capture(m);
        return true;
    }

    if (m->camera_capturing) {
        if (ev.type != SDL_EVENT_KEY_DOWN) return true;
        launcher_binds_set_camera(
            m, m->capture_camera, (int)LNG_EVSCAN(ev));
        launcher_model_cancel_camera_capture(m);
        return true;
    }

    // ---- GAMEPAD bind capture (capture_pad: Genesis + PSX Gamepad Bindings)
    // Persist a button/axis through the console's native bridge. Swallow
    // everything while listening; a controller button press or a decisive
    // axis push (past a dead threshold) commits. PSX only accepts events from
    // the player's selected Input source device.
    if (m->capturing && m->capture_pad) {
        if (m->capture_assist) {
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const int button = (int)LNG_EVGBTN(ev);
                uint32_t mask = launcher_input_gamepad_button_mask(
                    (uint32_t)LNG_EVGBTNWHICH(ev));
                if (button >= 0 && button < 32)
                    mask |= (uint32_t)1u << button;
                if (button_mask_count(mask) >= 2) {
                    launcher_model_set_captured_pad(
                        m, RECOMP_LAUNCHER_PAD_BUTTON_COMBO((int)mask));
                    launcher_model_cancel_capture(m);
                    return true;
                }
                if (settings_pad_button_is_select(button))
                    return true;
                launcher_model_set_captured_pad(
                    m, RECOMP_LAUNCHER_PAD_BUTTON(button));
                launcher_model_cancel_capture(m);
                return true;
            }
            if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                const int val = (int)LNG_EVGAXISVAL(ev);
                if (val >= 20000 || val <= -20000) {
                    launcher_model_set_captured_pad(
                        m, RECOMP_LAUNCHER_PAD_AXIS(
                               (int)LNG_EVGAXIS(ev), val > 0));
                    launcher_model_cancel_capture(m);
                }
                return true;
            }
            return true;
        }
        const SystemProfile* cap_prof = (const SystemProfile*)m->profile;
        const bool psx_cap = cap_prof && cap_prof->id && !strcmp(cap_prof->id, "psx");
        /* Consoles whose gamepad binds are stored per GUID. They must refuse a
         * bind whose selected device has not resolved to a live pad, or the
         * mapping lands in some other controller's profile. */
        const bool guid_store = psx_cap ||
            (cap_prof && cap_prof->id && !strcmp(cap_prof->id, "n64"));
        /* Which SDL device may bind.
         *
         * player_pad_id is set when the player picks a pad from the Input
         * source list, but a selection RESTORED from settings only carries the
         * GUID -- the id is re-resolved by a sync that runs for PSX alone. On
         * every other console the id was therefore 0 after a restart, and a
         * filter keyed on it would have let any pad bind. Resolve the GUID
         * against the live device list here so the rule holds from a restored
         * selection too. */
        uint32_t want_id = m->player_pad_id[m->cfg_player];
        if (!want_id && m->s.player_src[m->cfg_player] == 2) {
            const char* want_guid = m->s.player_gamepad_guid[m->cfg_player];
            if (want_guid[0]) {
                for (int i = 0; i < g_pad_count; ++i) {
                    if (g_pads[i].guid[0] &&
                        std::strcmp(g_pads[i].guid, want_guid) == 0) {
                        want_id = g_pads[i].id;
                        break;
                    }
                }
            }
        }
        auto from_selected = [&](uint32_t which) -> bool {
            /* A binding belongs to the device the player SELECTED.
             *
             * This used to require psx_cap, so every other console bound
             * whatever pad spoke first: with two controllers attached, the one
             * that was not chosen could capture the mapping, and the player had
             * no way to tell which had won. If a device is selected, only that
             * device may bind.
             *
             * want_id == 0 means no specific device is chosen (the console
             * offers a generic "Gamepad" source), and any pad is accepted --
             * which is the behaviour Genesis had and keeps. A per-GUID store
             * additionally refuses when its selection has not resolved to a
             * live device, because the binding would otherwise be written
             * against the wrong profile. */
            if (!want_id) return !guid_store;
            return which == want_id;
        };
        auto try_clear_release_wait = [&](uint32_t which) {
            if (!m->map_all_wait_release) return;
            if (!from_selected(which)) return;
            // Require the whole pad at rest — clearing on a single idle axis
            // let a held stick re-commit on every subsequent AXIS_MOTION.
            if (launcher_input_gamepad_at_rest(which))
                m->map_all_wait_release = false;
        };
        auto commit_pad = [&](int kind, int code, int axis_dir) {
            launcher_binds_set_pad_button(m, m->cfg_player + 1, m->capture_btn,
                                          kind, code, axis_dir);
            if (m->map_all_active)
                launcher_model_map_all_advance(m);
            else
                launcher_model_cancel_capture(m);
        };
        auto expected_dpad_button = [](int slot) -> int {
#if defined(LNG_SDL3)
            switch (slot) {
            case 0: return (int)SDL_GAMEPAD_BUTTON_DPAD_UP;
            case 1: return (int)SDL_GAMEPAD_BUTTON_DPAD_DOWN;
            case 2: return (int)SDL_GAMEPAD_BUTTON_DPAD_LEFT;
            case 3: return (int)SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
            default: return -1;
            }
#else
            switch (slot) {
            case 0: return (int)SDL_CONTROLLER_BUTTON_DPAD_UP;
            case 1: return (int)SDL_CONTROLLER_BUTTON_DPAD_DOWN;
            case 2: return (int)SDL_CONTROLLER_BUTTON_DPAD_LEFT;
            case 3: return (int)SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
            default: return -1;
            }
#endif
        };
        auto accepts_pad_button = [&](int slot, int button) -> bool {
            if (!psx_cap) return true;
            if (slot >= 0 && slot <= 3)
                return button == expected_dpad_button(slot);
            // Face/shoulders: ignore stray D-pad edges.
            const int expect0 = expected_dpad_button(0);
            const int expect3 = expected_dpad_button(3);
            if (button >= expect0 && button <= expect3) return false;
            return true;
        };
        if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (m->map_all_wait_release) {
                try_clear_release_wait((uint32_t)LNG_EVGBTNWHICH(ev));
                return true;  // never bind while waiting for release
            }
            if (from_selected((uint32_t)LNG_EVGBTNWHICH(ev))) {
                const int btn = (int)LNG_EVGBTN(ev);
                if (!accepts_pad_button(m->capture_btn, btn))
                    return true;
                commit_pad(LNG_PADBIND_BUTTON, btn, 0);
            }
            return true;
        }
        if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            try_clear_release_wait((uint32_t)LNG_EVGBTNWHICH(ev));
            return true;
        }
        if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            if (from_selected((uint32_t)LNG_EVGAXISWHICH(ev))) {
                if (m->map_all_wait_release) {
                    try_clear_release_wait((uint32_t)LNG_EVGAXISWHICH(ev));
                    return true;
                }
                const int val = (int)LNG_EVGAXISVAL(ev);
                if (val < 20000 && val > -20000) return true; // rest/jitter
                const int axis = (int)LNG_EVGAXIS(ev);
                // PSX: every pad slot is a digital bit. Stick axes belong on
                // L-Stick/R-Stick direction rows (indices 16..23); ignore them
                // on face/shoulder slots so a twitchy stick cannot steal
                // L1/Cross/etc. Trigger axes (L2/R2) are always OK — the
                // runtime thresholds them to digital. Buttons still win when
                // SDL emits GAMEPAD_BUTTON_DOWN for the same physical control.
                if (psx_cap) {
                    const int slot = m->capture_btn;
                    const bool stick_dir_slot = (slot >= 16 && slot < 24);
#if defined(LNG_SDL3)
                    const bool stick_axis =
                        axis == (int)SDL_GAMEPAD_AXIS_LEFTX ||
                        axis == (int)SDL_GAMEPAD_AXIS_LEFTY ||
                        axis == (int)SDL_GAMEPAD_AXIS_RIGHTX ||
                        axis == (int)SDL_GAMEPAD_AXIS_RIGHTY;
#else
                    const bool stick_axis =
                        axis == (int)SDL_CONTROLLER_AXIS_LEFTX ||
                        axis == (int)SDL_CONTROLLER_AXIS_LEFTY ||
                        axis == (int)SDL_CONTROLLER_AXIS_RIGHTX ||
                        axis == (int)SDL_CONTROLLER_AXIS_RIGHTY;
#endif
                    if (!stick_dir_slot && stick_axis) return true;
                }
                commit_pad(LNG_PADBIND_AXIS, axis, val < 0 ? -1 : +1);
            }
            return true;
        }
        return true;   // swallow all other input while capturing a pad bind
    }

    /* Mouse buttons are bindable inputs on stores that keep alternates
     * (PSX): bind-a-mouse-button is the whole point of the alt slot. ImGui
     * opens capture on release, so the next down event is deliberate. */
    if (m->capturing && !m->capture_pad) {
        const SystemProfile* mprof = (const SystemProfile*)m->profile;
        const bool alt_store = mprof && mprof->controller.binds_per_input >= 2;
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) return true;   /* swallow */
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!alt_store || !m->capture_mouse_armed) return true;
            int btn = (int)ev.button.button;      /* SDL: 1 L, 2 M, 3 R, 4/5 side */
            if (btn >= 1 && btn <= 5) {
                launcher_binds_set_button_slot(m, m->cfg_player + 1, m->capture_btn,
                                               m->capture_slot, 512 + btn);
                if (m->map_all_active) launcher_model_map_all_advance(m);
                else                   launcher_model_cancel_capture(m);
            }
            return true;
        }
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return true;   // swallow non-key input while capturing
    // A held key auto-repeats. Harmless for a single rebind (it commits the
    // same scancode twice) but during a keyboard Map All each repeat would
    // advance a step, racing 24 buttons past on one keypress.
    if (m->capturing && m->map_all_active && LNG_EVKEYREPEAT(ev)) return true;
    if (m->capturing) {
        // A store that keeps alternates per input must honour capture_slot.
        // N64's input.cfg is the one such store addressed by field rather than
        // by scancode, so it is named here; every other alternate-slot store
        // (PSX, Game Boy) goes through the slot-aware scancode setter, which
        // dispatches per profile. Single-bind stores (SNES/GBA/NES/Genesis) use
        // the legacy setter — capture_slot is always 0 for them.
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const bool alt_slots = prof && prof->controller.binds_per_input >= 2;
        if (m->capture_assist)
            launcher_model_set_captured_key(m, (int)LNG_EVSCAN(ev));
        else if (alt_slots && prof->id && !strcmp(prof->id, "n64"))
            launcher_binds_set_field(m, m->cfg_player + 1, m->capture_btn, m->capture_slot,
                                     RUI_N64_FIELD_KEY, (int)LNG_EVSCAN(ev));
        else if (alt_slots)
            launcher_binds_set_button_slot(m, m->cfg_player + 1, m->capture_btn,
                                           m->capture_slot, (int)LNG_EVSCAN(ev));
        else
            launcher_binds_set_button(m, m->cfg_player + 1, m->capture_btn, (int)LNG_EVSCAN(ev));
        if (m->map_all_active) launcher_model_map_all_advance(m);
        else                   launcher_model_cancel_capture(m);
        return true;
    }
    // hotkey capture: wait past a bare modifier press for the real key
    if (is_modifier_scancode((SDL_Scancode)LNG_EVSCAN(ev))) return true;
    launcher_binds_set_hotkey(m, m->capture_hk, (int)LNG_EVKEY(ev), (int)LNG_EVMOD(ev));
    launcher_model_cancel_hk_capture(m);
    return true;
}

bool is_absolute_path(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    if (path.size() >= 3 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        (path[2] == '/' || path[2] == '\\')) return true;
    return false;
}

std::string normalized_path(const char* path) {
    std::string out = path ? path : "";
    for (char& ch : out)
        if (ch == '\\') ch = '/';
    return out;
}

std::string asset(const char* rel) {
    std::string path = normalized_path(rel);
    if (is_absolute_path(path)) return path;

#if defined(__ANDROID__)
    const char* base = SDL_AndroidGetInternalStoragePath();
#else
    const char* base = SDL_GetBasePath();
#endif
    std::string out = normalized_path(base ? base : "");
    if (!out.empty() && !path.empty() && out.back() != '/')
        out.push_back('/');
    return out + path;
}

} // namespace

// ---- launcher_panels.h contract implementation -------------------------------
// The panel registry is defined above (kPanelRegistry, anonymous namespace) —
// only this backend's draw functions can populate it (they call ImGui::*), so
// this is the sole implementation, matching today's single-backend reality.
extern "C" const LauncherPanel* launcher_panels_all(void) {
    return kPanelRegistry;   // {id=NULL}-sentinel-terminated
}

extern "C" const LauncherPanel* launcher_panel_find(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; kPanelRegistry[i].id; ++i)
        if (strcmp(kPanelRegistry[i].id, id) == 0) return &kPanelRegistry[i];
    return nullptr;
}

extern "C" bool launcher_panel_available(const LauncherPanel* p, const LauncherModel* m) {
    if (!p) return false;
    return p->available ? (p->available(m) != 0) : true;
}

extern "C" LngAction launcher_backend_run(LauncherPlatform* p,
                                          LauncherModel* m,
                                          const LauncherTheme* th) {
    launcher_boot_timing_mark("rui:backend_run:begin");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /* Gamepad nav is enabled only once a real pad event has arrived -- see the
     * arming note in the event loop. Keyboard nav is unconditional: a stuck
     * KEY would have to be physically held, and the window would not have
     * focus to receive it. */
    s_pad_nav_armed = false;
#if defined(__ANDROID__)
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
#endif
    io.IniFilename = nullptr;
    // Test hook: force the focus ring always-on so scripted screenshots can
    // verify nav rendering without a physical pad. Off => normal auto behaviour
    // (ring appears on pad/keyboard, hides on mouse).
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19140
    // ConfigNavCursorVisibleAlways is 1.91.4+; a pre-1.91.4 host (rt64 1.90.x)
    // simply keeps the default auto-visibility for the test hook.
    if (const char* nv = SDL_getenv("LNG_NAV_ALWAYS"); nv && nv[0] == '1')
        io.ConfigNavCursorVisibleAlways = true;
#endif

    // Force `th` to be materialized before the store. Under the host -Os build,
    // GCC 15.2 otherwise miscompiled `g_th = th` (the value never reached the
    // global — read back as NULL). This barrier + the `volatile` on g_th make
    // the store reliably observable. Do not remove without re-verifying on the
    // gb-recompiled (-Os + ANGLE) build.
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    g_th = th;
    LNG_ImplSDL_InitForOpenGL(p->window, p->gl);
#ifdef LNG_GLES2
    ImGui_ImplOpenGL3_Init("#version 100");   // GLES 2 GLSL (host ANGLE backend)
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif
    launcher_boot_timing_mark("rui:imgui_gl_ready");

    // Box art: per-game path from the ABI when given (multi-variant repos
    // stage one file per variant in a shared build dir), else the default.
    g_boxart = launcher_texture_load(
        asset(m->boxart_path && m->boxart_path[0] ? m->boxart_path
                                                  : "assets/img/boxart.tga").c_str());
    // Controller art comes from the active SystemProfile's ControllerSpec —
    // never hardcoded console filenames in this common backend. Conventions:
    // a 32-bit TGA carries real alpha -> the plain alpha-respecting loader;
    // a 24-bit TGA has a flat backdrop baked in -> keyed out (top-left pixel)
    // so the art sits transparently on the panel. `image_analog`/
    // `image_digital` (the optional PSX-style mode-swap pair) are 32-bit.
    {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const char* pad_img = (prof && prof->controller.image)
                                ? prof->controller.image : "pad.tga";
        std::string pad_path = asset((std::string("assets/img/") + pad_img).c_str());
        // TGA header byte 16 = bits per pixel: pick the loader by depth.
        int bpp = 0;
        if (FILE* f = fopen(pad_path.c_str(), "rb")) {
            unsigned char hdr[18];
            if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) bpp = hdr[16];
            fclose(f);
        }
        g_pad = (bpp == 32)
            ? launcher_texture_load(pad_path.c_str())
            : launcher_texture_load_colorkey(pad_path.c_str(), 24);
        if (prof && prof->controller.image_analog)
            g_pad_analog = launcher_texture_load(
                asset((std::string("assets/img/") + prof->controller.image_analog).c_str()).c_str());
        if (prof && prof->controller.image_digital)
            g_pad_digital = launcher_texture_load(
                asset((std::string("assets/img/") + prof->controller.image_digital).c_str()).c_str());
    }
    // Header brand mark: the active SystemProfile's own (N64 -> the four-color
    // logo, NES/Genesis their wordmark), falling back to the shared recomp-ui
    // mark for consoles that don't set one. Keeps the top-left mark matched to
    // the system on screen.
    {
        const SystemProfile* bprof = (const SystemProfile*)m->profile;
        // NULL brand => the shared dots; a non-NULL EMPTY string => the profile
        // opts out of a corner emblem entirely (N64 leads with its wordmark).
        // Only load when there's an actual filename.
        const char* brand_file = bprof && bprof->brand
                                   ? bprof->brand : "brand_mark.tga";
        if (brand_file && brand_file[0])
            g_brand = launcher_texture_load(
                asset((std::string("assets/img/") + brand_file).c_str()).c_str());
        // Optional platform wordmark: loads only if the profile names one AND
        // the file is present (recomp-ui ships none — a console wordmark may be
        // a third-party trademark). Missing file => header falls back to text.
        if (bprof && bprof->wordmark_image && bprof->wordmark_image[0])
            g_wordmark = launcher_texture_load(
                asset((std::string("assets/img/") + bprof->wordmark_image).c_str()).c_str());
    }
    // Transfer Pak cartridge art (only a tpak game needs it): real GB cart PNGs
    // keyed by cart_kind, empty shell at index 0 (see g_cart / draw_tpak_cart).
    if (m->tpak_slots > 0) {
        static const char* const kCartFiles[5] = {
            "cart_empty.tga", "cart_red.tga", "cart_blue.tga",
            "cart_yellow.tga", "cart_green.tga",
        };
        for (int i = 0; i < 5; ++i)
            g_cart[i] = launcher_texture_load(
                asset((std::string("assets/img/") + kCartFiles[i]).c_str()).c_str());
    }
    g_verdict_ok    = launcher_texture_load(asset("assets/img/verdict_ok.tga").c_str());
    g_verdict_warn  = launcher_texture_load(asset("assets/img/verdict_warn.tga").c_str());
    g_verdict_bad   = launcher_texture_load(asset("assets/img/verdict_bad.tga").c_str());
    g_verdict_none  = launcher_texture_load(asset("assets/img/verdict_none.tga").c_str());
    // memcard.tga is already 32-bit with real alpha (no colorkey backdrop),
    // same as pad_analog.tga/pad_digital.tga above.
    g_memcard = launcher_texture_load(asset("assets/img/memcard.tga").c_str());
    /* Country flags come from a bundled sheet on every platform (Segoe UI
     * Emoji has none). A missing sheet is not fatal: the OS provider is
     * asked instead, and where that cannot draw one the UI prints letters. */
    if (!recomp_emoji_flags_load(asset("assets/img/flags.png").c_str()))
        std::fprintf(stderr, "[rui] no flag sheet at assets/img/flags.png; "
                             "flags fall back to the emoji provider\n");
    launcher_boot_timing_mark("rui:textures_loaded");

    std::string font_path = asset("assets/fonts/LatoLatin-Regular.ttf");
    // Optional Japanese subset, merged over the Latin base when present (PMS-J).
    // Games that don't ship it stay Latin-only (fopen in apply_scale fails
    // silently), so this path is inert for every other console.
    std::string jp_font_path = asset("assets/fonts/NotoSansJP-Subset.ttf");
    std::string symbols_font_path =
        asset("assets/fonts/NotoSansSymbols2-Regular.ttf");
    std::string emoji_font_path =
        asset("assets/fonts/OpenMoji-black-glyf.ttf");
    float applied_scale = 0.0f;
    launcher_debug_init();
    /* The ignore/block list, from beside the executable. Loaded once here
     * rather than lazily on the netplay page: the file is the authority for
     * whether a chat line is drawn, and a page that renders before the first
     * load would show a line the player has already asked never to see. */
    recomp_moderation_load(asset("").c_str());

    long smoke_frames = 0, frame = 0;
    if (const char* sf = SDL_getenv("LNG_SMOKE_FRAMES")) smoke_frames = SDL_atoi(sf);

    // HiDPI is driven by the platform layer's logical/pixel split, which is
    // real on macOS/Wayland and synthesized from the display scale on Windows
    // and X11 (see launcher_platform_refresh_metrics). LNG_FORCE_SCALE pins
    // that scale so the path can be exercised on a 100% display; nothing here
    // reads the env var any more.
    bool first_present_marked = false;

    while (m->action == LNG_ACTION_NONE && !p->should_quit) {
        if (smoke_frames > 0 && ++frame > smoke_frames) { m->action = LNG_ACTION_QUIT; break; }

        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 16)) do {
            /* Window coordinates -> logical units, before anything reads the
             * event. No-op unless the platform layer synthesized the split. */
            scale_mouse_event(ev, p->input_scale);
            if (ev.type == SDL_EVENT_QUIT) p->should_quit = true;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->should_quit = true;
            if (try_capture(m, ev)) continue;
            /* Arm gamepad navigation on the first REAL pad input.
             *
             * A control that is already asserted when the launcher opens
             * produces no event -- SDL reports a steady state, and events
             * only arrive on a transition. A human pressing a button always
             * produces one. That difference is the whole test, and it is why
             * this is done here on the event queue rather than by polling
             * state in NewFrame, where the two are indistinguishable.
             *
             * Measured on a host with a Sunshine "Mouse passthrough
             * (absolute)" virtual joystick [0xbeef/0xdead] whose axes sit
             * pinned at -32767: ImGui gamepad nav is on and PLAY holds
             * default focus, so the stuck axis activated PLAY about two
             * seconds after the window opened and the launcher was gone
             * before anyone could use it. Every port on that host booted
             * straight into the game and looked like it was ignoring the
             * launcher entirely. */
            switch (ev.type) {
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                /* Buttons only. SDL reports every axis's CURRENT value as a
                 * motion event when it opens a device, so a stick that is
                 * merely resting -- or wedged at its limit -- arrives as
                 * motion and would arm this on the very first frame, which is
                 * precisely the case this exists to catch. A button press is
                 * unambiguous; an axis value is not. */
                s_pad_nav_armed = true;
                break;
            default:
                break;
            }
            LNG_ImplSDL_ProcessEvent(&ev);
        } while (SDL_PollEvent(&ev));

        launcher_platform_refresh_metrics(p);
        /* Also when a never-seen emoji was registered last frame: the atlas
         * is static, so it is rebuilt once here, before NewFrame. */
        if (applied_scale != p->display_scale || g_emoji_atlas_dirty) {
            apply_scale(*th, p->display_scale, font_path.c_str(),
                        jp_font_path.c_str(), symbols_font_path.c_str(),
                        emoji_font_path.c_str());
            applied_scale = p->display_scale;
            if (!first_present_marked)
                launcher_boot_timing_mark("rui:fonts_built");
        }

        // Re-poll connected gamepads every frame so hot-plugged pads (e.g. a
        // DualSense powered on after launch) appear without a relaunch.
        g_pad_count = launcher_input_poll(
            g_pads, LNG_MAX_PADS, m->has_gyro_controls ? 1 : 0);

        // PSX: keep Input source labels on concrete pad names (live SDL name
        // or saved [gamepads] registry), never the generic "Gamepad" placeholder.
        launcher_binds_sync_psx_pad_sources(m, g_pads, g_pad_count);

        // SNES: same purpose, from this console's own profile store. Run every
        // frame so a pad plugged in after start-up picks up its label too.
        launcher_binds_hydrate_snes_pad_names(m, g_pads, g_pad_count);

        // Pad capture release-gate: clear once the selected pad is fully at
        // rest (covers the case where SDL stops sending AXIS_MOTION at rest).
        if (m->capturing && m->capture_pad && m->map_all_wait_release) {
            const uint32_t id = m->player_pad_id[m->cfg_player];
            if (id && launcher_input_gamepad_at_rest(id))
                m->map_all_wait_release = false;
        }

        // PSX D-pad poll fallback: some Windows Xbox backends miss
        // GAMEPAD_BUTTON_DOWN for a cardinal; commit from live button state
        // when capturing Up/Down/Left/Right and that exact bit is held.
        if (m->capturing && m->capture_pad && !m->map_all_wait_release &&
            m->capture_btn >= 0 && m->capture_btn <= 3) {
            const SystemProfile* poll_prof = (const SystemProfile*)m->profile;
            if (poll_prof && poll_prof->id && !strcmp(poll_prof->id, "psx")) {
                const uint32_t id = m->player_pad_id[m->cfg_player];
                if (id) {
#if defined(LNG_SDL3)
                    static const int kExpect[4] = {
                        (int)SDL_GAMEPAD_BUTTON_DPAD_UP,
                        (int)SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                        (int)SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                        (int)SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                    };
#else
                    static const int kExpect[4] = {
                        (int)SDL_CONTROLLER_BUTTON_DPAD_UP,
                        (int)SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                        (int)SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                        (int)SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                    };
#endif
                    const int expect = kExpect[m->capture_btn];
                    const uint32_t mask = launcher_input_gamepad_button_mask(id);
                    if (expect >= 0 && expect < 32 &&
                        (mask & (uint32_t)(1u << expect))) {
                        launcher_binds_set_pad_button(
                            m, m->cfg_player + 1, m->capture_btn,
                            LNG_PADBIND_BUTTON, expect, 0);
                        if (m->map_all_active)
                            launcher_model_map_all_advance(m);
                        else
                            launcher_model_cancel_capture(m);
                    }
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        /* Suspend ImGui's GAMEPAD navigation while a bind capture is open.
         *
         * The SDL backend POLLS gamepad state in NewFrame (see
         * ImGui_ImplSDL3_UpdateGamepads) rather than reading the SDL event
         * queue, so swallowing pad events in try_capture cannot stop the pad
         * from driving the menu: press a button to bind it and the focus
         * jumps instead. Clearing the flag for the frame is the only thing
         * that actually suppresses it. Keyboard nav stays on so Esc, arrows
         * and Enter still work while listening. */
        {
            ImGuiIO& nav_io = ImGui::GetIO();
            if (m->capturing || m->hk_capturing || m->camera_capturing ||
                automap_in_progress() || !s_pad_nav_armed)
                nav_io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
            else
                nav_io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        }
        LNG_ImplSDL_NewFrame();
        apply_logical_display(p);   // logical DisplaySize + pixel-density frame
        ImGui::NewFrame();
        draw_ui(m, *th, p->logical_w, p->logical_h);
        ImGui::Render();

        glViewport(0, 0, p->pixel_w, p->pixel_h);
        const LngColor bg = th->background;
        glClearColor(bg.r, bg.g, bg.b, bg.a);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        launcher_debug_step(p, m);   // script/screenshot: after draw, before swap
        launcher_platform_present(p);
        if (!first_present_marked) {
            launcher_boot_timing_mark("rui:first_swap");
            first_present_marked = true;
        }
    }

    launcher_input_shutdown();
    launcher_texture_free(&g_boxart);
    launcher_texture_free(&g_pad);
    launcher_texture_free(&g_pad_analog);
    launcher_texture_free(&g_pad_digital);
    launcher_texture_free(&g_brand);
    launcher_texture_free(&g_verdict_ok);
    launcher_texture_free(&g_verdict_warn);
    launcher_texture_free(&g_verdict_bad);
    launcher_texture_free(&g_verdict_none);
    launcher_texture_free(&g_memcard);
    launcher_texture_free(&g_wordmark);
    for (int i = 0; i < 5; ++i) launcher_texture_free(&g_cart[i]);
    ImGui_ImplOpenGL3_Shutdown();
    LNG_ImplSDL_Shutdown();
    ImGui::DestroyContext();

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    // Persist selected gamepads (defaults if never remapped) so PLAY remembers
    // the pad in input.ini / settings even without an explicit Save click.
    if (m->action == LNG_ACTION_LAUNCH || m->action == LNG_ACTION_RELAUNCH)
        launcher_binds_prepare_psx_launch(m, g_pads, g_pad_count);
    return m->action;
}
