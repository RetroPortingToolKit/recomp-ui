/* Backspace in a launcher text field must not steal the focus ring.
 *
 * Reported on the Rename Controller dialog: typing a name, one Backspace
 * deleted a character and the field went dead -- the footer's "Back/Start
 * re-homes the focus ring to PLAY" shortcut treats Backspace as Back, and it
 * fired while an InputText was active, moving keyboard focus to the PLAY
 * button behind the modal. The same shortcut let a pad's Back or Start reach
 * past an open modal to the page underneath.
 *
 * Drives real ImGui frames headlessly (the runtime_ui_imgui test does the
 * same) so the predicate is exercised against actual widget state rather than
 * a description of it.
 */
#include "backends/imgui/launcher_nav.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>

namespace {

int fails;

void expect(bool cond, const char* what) {
    if (cond) { std::printf("ok: %s\n", what); return; }
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

void begin_frame(bool backspace) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Backspace, backspace);
    ImGui::NewFrame();
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);

    char name[64] = "Apollo DualSense";
    bool rehome_plain = false, rehome_typing = false, rehome_modal = false;
    bool rehome_after_close = false;

    /* 1. No text field, no modal: Backspace IS the Back shortcut. */
    begin_frame(false);
    ImGui::Begin("page");
    ImGui::Button("PLAY");
    ImGui::End();
    ImGui::Render();

    begin_frame(true);
    ImGui::Begin("page");
    ImGui::Button("PLAY");
    rehome_plain = rui_nav_rehome_to_play();
    ImGui::End();
    ImGui::Render();
    expect(rehome_plain, "Backspace re-homes the focus ring on an ordinary page");

    /* 2. An active text field: Backspace belongs to the field.
     *
     * Two frames matter. On the FIRST frame the field is active, io.WantTextInput
     * still reads the previous frame's false (it is computed at EndFrame), and
     * a Backspace arriving right then used to steal the focus anyway -- so the
     * guard is checked on that frame too, not just once the flag has caught up. */
    begin_frame(false);
    ImGui::Begin("page");
    ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##name", name, sizeof(name));
    ImGui::End();
    ImGui::Render();

    begin_frame(true);
    ImGui::Begin("page");
    ImGui::InputText("##name", name, sizeof(name));
    const bool first_active_frame_want = io.WantTextInput;
    const bool rehome_first_active = rui_nav_rehome_to_play();
    ImGui::End();
    ImGui::Render();
    expect(!first_active_frame_want,
           "WantTextInput lags one frame behind the field becoming active");
    expect(!rehome_first_active,
           "and Backspace on that very frame still does NOT re-home");

    /* Release, so the next frame is a fresh PRESS rather than a held key. */
    begin_frame(false);
    ImGui::Begin("page");
    ImGui::InputText("##name", name, sizeof(name));
    ImGui::End();
    ImGui::Render();

    begin_frame(true);
    ImGui::Begin("page");
    ImGui::InputText("##name", name, sizeof(name));
    const bool typing = io.WantTextInput;
    rehome_typing = rui_nav_rehome_to_play();
    ImGui::End();
    ImGui::Render();
    expect(typing, "the text field reports itself active (WantTextInput)");
    expect(!rehome_typing, "and Backspace does NOT re-home while typing");

    /* 3. An open modal, nothing focused in it: Back/Start stay inside. */
    begin_frame(false);
    ImGui::Begin("page");
    ImGui::OpenPopup("Rename Controller");
    if (ImGui::BeginPopupModal("Rename Controller", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Button("OK");
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::Render();

    begin_frame(true);
    ImGui::Begin("page");
    if (ImGui::BeginPopupModal("Rename Controller", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Button("OK");
        ImGui::EndPopup();
    }
    rehome_modal = rui_nav_rehome_to_play();
    ImGui::End();
    ImGui::Render();
    expect(!rehome_modal, "and does NOT reach past an open modal");

    /* 4. Modal closed again: the shortcut comes back. */
    begin_frame(false);
    ImGui::Begin("page");
    ImGui::CloseCurrentPopup();
    ImGui::Button("PLAY");
    ImGui::End();
    ImGui::Render();

    begin_frame(true);
    ImGui::Begin("page");
    ImGui::Button("PLAY");
    rehome_after_close = rui_nav_rehome_to_play();
    ImGui::End();
    ImGui::Render();
    expect(rehome_after_close, "and returns once the modal is gone");

    ImGui::DestroyContext();
    if (fails) { std::fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    std::puts("launcher_nav_backspace_test: ok");
    return 0;
}
