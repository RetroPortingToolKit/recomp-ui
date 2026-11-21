// psx_menu_provider_imgui.cpp -- SDL input and Dear ImGui presentation for
// recomp-ui's psxrecomp menu provider (recomp_ui_psx_menu.h). The model and
// its wiring to the runtime live in psx_menu_provider.cpp.
#if defined(__has_include)
#if __has_include("psx_menu_host.h")
#define RUI_PSX_HAS_MENU_HOST 1
#endif
#endif

#if defined(RUI_PSX_HAS_MENU_HOST)
#include "psx_menu_provider_internal.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#if defined(LNG_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <cfloat>
#include <cstdint>

namespace rui_psx {
namespace {

ImGuiContext *g_ctx = nullptr;   // GL thread
bool g_gl_ready = false;
uint64_t g_last_ns = 0;
unsigned g_pad_held = 0;         // main thread

#if defined(LNG_SDL3)
constexpr unsigned kBack = 1u << SDL_GAMEPAD_BUTTON_BACK;
constexpr unsigned kL3 = 1u << SDL_GAMEPAD_BUTTON_LEFT_STICK;
#endif

int key_input(int key) {
    switch (key) {
    case SDLK_UP: return RECOMP_RUNTIME_UI_INPUT_UP;
    case SDLK_DOWN: return RECOMP_RUNTIME_UI_INPUT_DOWN;
    case SDLK_LEFT: return RECOMP_RUNTIME_UI_INPUT_LEFT;
    case SDLK_RIGHT: return RECOMP_RUNTIME_UI_INPUT_RIGHT;
    case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: return RECOMP_RUNTIME_UI_INPUT_ACCEPT;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return RECOMP_RUNTIME_UI_INPUT_BACK;
    default: return -1;
    }
}

}  // namespace

int sdl_event(const void *p) {
    if (!p) return 0;
    const SDL_Event &e = *static_cast<const SDL_Event *>(p);
    std::lock_guard<std::mutex> lk(lock());
    RecompRuntimeUi *ui = model();
    if (!ui) return 0;
    const bool open = recomp_runtime_ui_is_open(ui) != 0;
#if defined(LNG_SDL3)
    if (e.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode k = e.key.key;
        const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
        if (!e.key.repeat && (k == SDLK_F10 || (ctrl && k == SDLK_L))) {
            input(RECOMP_RUNTIME_UI_INPUT_TOGGLE);
            return 1;
        }
        if (!open) return 0;
        const int in = key_input((int)k);
        if (in >= 0) recomp_runtime_ui_handle_input(ui, (RecompRuntimeUiInput)in, 1, e.key.repeat ? 1 : 0);
        return 1;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        const unsigned bit = 1u << e.gbutton.button;
        const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const unsigned before = g_pad_held;
        g_pad_held = down ? (g_pad_held | bit) : (g_pad_held & ~bit);
        if (down && (g_pad_held & (kBack | kL3)) == (kBack | kL3) && (before & (kBack | kL3)) != (kBack | kL3)) {
            input(RECOMP_RUNTIME_UI_INPUT_TOGGLE);
            return 1;
        }
        if (!open) return 0;
        if (down) {
            int in = -1;
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: in = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: in = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: in = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: in = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: in = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            case SDL_GAMEPAD_BUTTON_EAST: in = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            default: break;
            }
            if (in >= 0) recomp_runtime_ui_handle_input(ui, (RecompRuntimeUiInput)in, 1, 0);
        }
        return 1;
    }
    if (!open) return 0;
    switch (e.type) {  // the game sees no input while the menu is open
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        return 1;
    default:
        return 0;
    }
#else
    (void)e; (void)open;
    return 0;  // SDL2 hosts: drive it with recomp_ui_psx_menu_input()
#endif
}

void draw(int w, int h) {
    std::lock_guard<std::mutex> lk(lock());
    RecompRuntimeUi *ui = model();
    if (!ui || !recomp_runtime_ui_is_open(ui) || w <= 0 || h <= 0) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    if (!g_ctx) {
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGui::GetIO().IniFilename = nullptr;
    }
    ImGui::SetCurrentContext(g_ctx);
    if (!g_gl_ready) g_gl_ready = ImGui_ImplOpenGL3_Init(nullptr);
    if (!g_gl_ready) { ImGui::SetCurrentContext(prev); return; }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    // Keyboard/controller driven: no mouse position, so hover never moves the
    // model's selection under the player.
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    const uint64_t now = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    io.DeltaTime = g_last_ns && now > g_last_ns && frequency
        ? (float)((double)(now - g_last_ns) / (double)frequency) : 1.0f / 60.0f;
    g_last_ns = now;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui::SetCurrentContext(prev);
}

void release_gl() {
    std::lock_guard<std::mutex> lk(lock());
    if (!g_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    if (g_gl_ready) ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(g_ctx);
    ImGui::SetCurrentContext(prev == g_ctx ? nullptr : prev);
    g_ctx = nullptr;
    g_gl_ready = false;
    g_last_ns = 0;
}

}  // namespace rui_psx

#endif  // RUI_PSX_HAS_MENU_HOST
