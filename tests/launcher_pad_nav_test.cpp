// A pad button already held when gamepad navigation turns on must
// not drive the menu until it is released (launcher_pad_nav.h).
//
// The reported case: an Xbox pad whose X (WEST, ImGui FaceLeft =
// NavGamepadMenu) read as held for good. Once the launcher armed pad nav,
// ImGui saw a fresh press of the menu key, opened its gamepad window switcher
// (the "(Untitled)" list) and kept it open for as long as the key read down,
// that is, forever.
#include "imgui.h"
#include "imgui_internal.h"
#include "launcher_pad_nav.h"

#include <cstdio>

static int fails;

static void expect(bool cond, const char* what) {
    if (cond) { std::printf("ok: %s\n", what); return; }
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static void new_context() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
}

// One launcher-like frame: a single window with a focusable button.
static void frame() {
    ImGui::NewFrame();
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Button("PLAY");
    ImGui::End();
    ImGui::EndFrame();
}

static void frames(int n) { for (int i = 0; i < n; ++i) frame(); }

static bool switcher_open() { return GImGui->NavWindowingTarget != nullptr; }

int main() {
    // Without the lock: the held button opens the switcher and it stays open.
    new_context();
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    frames(60);
    expect(switcher_open(), "baseline: a held FaceLeft opens the window switcher (the bug)");
    ImGui::DestroyContext();

    // With the lock taken as pad nav starts, while the button is already held.
    new_context();
    frame();
    launcher_pad_nav_lock_held_keys();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    frames(60);
    expect(!switcher_open(), "a button held when pad nav starts does not open the switcher");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    frames(3);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    frames(30);
    expect(switcher_open(), "after a release, a real press works again");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    frames(3);
    expect(!switcher_open(), "releasing it closes the switcher");
    ImGui::DestroyContext();

    // Keys that are up when the lock is taken are not affected at all.
    new_context();
    frame();
    launcher_pad_nav_lock_held_keys();
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    frames(30);
    expect(switcher_open(), "a button pressed after nav starts works immediately");
    ImGui::DestroyContext();

    if (fails) { std::fprintf(stderr, "launcher_pad_nav_test: %d failure(s)\n", fails); return 1; }
    std::printf("launcher_pad_nav_test: all checks passed\n");
    return 0;
}
