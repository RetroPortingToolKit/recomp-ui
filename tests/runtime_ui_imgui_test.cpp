#include "recomp_runtime_ui.h"
#include "imgui.h"
#include "imgui_internal.h"
/* Include the production drawer to drive its item/section mouse seams directly,
 * without depending on the outer menu's pixel layout. */
#include "../src/common/backends/imgui/runtime_ui_imgui.cpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int get_value(void *context, const RecompRuntimeUiItem *, int *out) {
    *out = *static_cast<int *>(context);
    return 1;
}

int set_value(void *context, const RecompRuntimeUiItem *, int value) {
    *static_cast<int *>(context) = value;
    return 1;
}

void render_case(uint32_t presentation_flags, ImVec2 display,
                 float minimum_width, float minimum_height,
                 float maximum_width) {
    static const char *const modes[] = { "Standard (4:3)", "16:9", "Adaptive" };
    static const RecompRuntimeUiItem items[] = {
        { "view", "Display", "View mode", "Choose the visible game area.",
          RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, modes, 3, nullptr },
    };
    int value = 0;
    RecompRuntimeUiConfig config{};
    config.title = "Runtime UI";
    config.subtitle = "NINTENDO 64";
    config.items = items;
    config.item_count = 1;
    config.theme = "n64";
    config.callbacks.context = &value;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    config.presentation_flags = presentation_flags;

    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    assert(ui != nullptr);
    recomp_runtime_ui_open(ui);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = display;
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *font_pixels = nullptr;
    int font_w = 0, font_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_w, &font_h);

    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGuiWindow *window = ImGui::FindWindowByName("##recomp-runtime-ui");
    assert(window != nullptr);
    std::fprintf(stderr, "runtime-ui test display=%.0fx%.0f window=%.0fx%.0f\n",
                 display.x, display.y, window->Size.x, window->Size.y);
    assert(window->Size.x >= minimum_width);
    assert(window->Size.y >= minimum_height);
    assert(window->Size.x <= maximum_width);
    const float first_frame_font_scale = window->FontWindowScale;
    ImGui::Render();
    assert(ImGui::GetDrawData()->CmdListsCount > 0);

    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    window = ImGui::FindWindowByName("##recomp-runtime-ui");
    assert(window != nullptr);
    assert(std::fabs(window->FontWindowScale - first_frame_font_scale) <
           0.001f);
    ImGui::Render();

    ImGui::DestroyContext();
    recomp_runtime_ui_destroy(ui);
}

// A toast draws with the menu closed (its own window, no menu window), and
// both draw when the menu is open.
void toast_case() {
    static const RecompRuntimeUiItem items[] = {
        { "x", "Display", "X", nullptr, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr },
    };
    int value = 0;
    RecompRuntimeUiConfig config{};
    config.items = items;
    config.item_count = 1;
    config.theme = "nes";
    config.callbacks.context = &value;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    assert(ui != nullptr);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(768.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *font_pixels = nullptr;
    int font_w = 0, font_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_w, &font_h);

    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);                  // nothing to draw
    ImGui::Render();
    assert(ImGui::GetDrawData()->CmdListsCount == 0);

    recomp_runtime_ui_set_toast(ui, "DISK 1 SIDE A", "MOTOR OFF\nD AGAIN: DISK 1 SIDE B");
    for (int frame = 0; frame < 2; ++frame) {            // auto-resize settles on frame 2
        ImGui::NewFrame();
        recomp_runtime_ui_render_imgui(ui);
        ImGui::Render();
    }
    ImGuiWindow *toast = ImGui::FindWindowByName("##recomp-runtime-ui-toast");
    assert(toast != nullptr && toast->Active);
    ImGuiWindow *menu = ImGui::FindWindowByName("##recomp-runtime-ui");
    assert(menu == nullptr || !menu->Active);
    assert(toast->Pos.y < 60.0f && toast->Size.y > 30.0f);
    assert((toast->Flags & ImGuiWindowFlags_NoInputs) != 0);

    recomp_runtime_ui_open(ui);
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGui::Render();
    assert(ImGui::FindWindowByName("##recomp-runtime-ui")->Active);
    assert(ImGui::FindWindowByName("##recomp-runtime-ui-toast")->Active);

    recomp_runtime_ui_close(ui);
    recomp_runtime_ui_set_toast(ui, nullptr, nullptr);
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGui::Render();
    assert(ImGui::GetDrawData()->CmdListsCount == 0);
    ImGui::DestroyContext();
    recomp_runtime_ui_destroy(ui);
}

ImVec2 nested_frame(RecompRuntimeUi *ui, bool sections = false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(600, 400));
    ImGui::Begin("nested mouse test");
    const LauncherTheme theme = launcher_theme_default();
    if (sections) draw_sections(ui, theme, false);
    else draw_items(ui, theme, false);
    const ImRect last = GImGui->LastItemData.Rect;
    const ImVec2 center = last.GetCenter();
    ImGui::End();
    ImGui::Render();
    return center;
}

void nested_mouse_case() {
    static const RecompRuntimeUiItem items[] = {
        { "root", "Mods", "Root", nullptr, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr },
        { "child", "Mods/Child", "Child item", nullptr, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr },
        { "video", "Video", "Video item", nullptr, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr },
    };
    int value = 0;
    RecompRuntimeUiConfig config{};
    config.items = items;
    config.item_count = 3;
    config.callbacks.context = &value;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    assert(ui != nullptr);
    recomp_runtime_ui_open(ui);
    recomp_runtime_ui_enter_section(ui, 0);
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    nested_frame(ui);
    const ImVec2 folder = nested_frame(ui);
    io.AddMousePosEvent(folder.x, folder.y);
    nested_frame(ui);
    io.AddMouseButtonEvent(0, true);
    nested_frame(ui);
    io.AddMouseButtonEvent(0, false);
    nested_frame(ui);
    assert(std::strcmp(ui->page, "Child") == 0);
    assert(ui->row_index == 0);
    recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
    assert(value == 1);  // the only child is immediately usable after the click
    recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_BACK, 1, 0);
    assert(ui->row_index == 1);  // Back still selects the parent folder
    recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
    assert(std::strcmp(ui->page, "Child") == 0);

    io.AddMousePosEvent(-100, -100);
    nested_frame(ui, true);
    const ImVec2 video = nested_frame(ui, true);
    io.AddMousePosEvent(video.x, video.y);
    nested_frame(ui, true);
    assert(ui->section_index == 1 && ui->page[0] == '\0');
    assert(ui->page_depth == 0 && ui->row_index == 0 && ui->in_section);
    recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
    assert(value == 0);  // Video's root item is usable without backing out

    ImGui::DestroyContext();
    recomp_runtime_ui_destroy(ui);
}


void footer_case(ImVec2 display, uint32_t flags) {
    static const RecompRuntimeUiItem item = {
        "disc", "Disc", "Selected disc", "Choose a disc.",
        RECOMP_RUNTIME_UI_INT, 1, 4, 1, nullptr, 0, nullptr };
    int value = 1;
    RecompRuntimeUiConfig config{};
    config.title = "Game menu";
    config.items = &item;
    config.item_count = 1;
    config.accept_label = "Enter / Cross";
    config.back_label = "Esc / Circle";
    config.callbacks.context = &value;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    config.presentation_flags = flags;
    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    recomp_runtime_ui_open(ui);
    recomp_runtime_ui_set_status(ui, "Could not open that disc. Check its image files and try again.");
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = display;
    io.DeltaTime = 1.0f / 60.0f;
    ImFontConfig font;
    font.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&font);
    unsigned char *pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        recomp_runtime_ui_render_imgui(ui);
        ImGui::Render();
    }
    ImGuiWindow *menu = ImGui::FindWindowByName("##recomp-runtime-ui");
    ImGuiWindow *footer = nullptr;
    for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows)
        if (window->ParentWindow == menu && std::strstr(window->Name, "##footer")) footer = window;
    assert(footer && menu);
    /* Hints and status fit vertically and leave a usable button region. */
    assert(footer->Pos.x + footer->Size.x <= menu->InnerRect.Max.x - 70.0f);
    assert(footer->Pos.y + footer->Size.y <= menu->InnerRect.Max.y + 1.0f);
    assert(footer->ScrollMax.y < 1.0f);
    const ImVec2 resume(menu->WorkRect.Max.x - 20.0f, footer->Pos.y + 10.0f);
    io.AddMousePosEvent(resume.x, resume.y);
    for (int frame = 0; frame < 3; ++frame) {
        if (frame == 1) io.AddMouseButtonEvent(0, true);
        if (frame == 2) io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
        recomp_runtime_ui_render_imgui(ui);
        ImGui::Render();
    }
    assert(!recomp_runtime_ui_is_open(ui));
    ImGui::DestroyContext();
    recomp_runtime_ui_destroy(ui);
}

} // namespace

int main() {
    footer_case(ImVec2(640, 480), 0);
    footer_case(ImVec2(1280, 720), 0);
    footer_case(ImVec2(1200, 900), RECOMP_RUNTIME_UI_PRESENTATION_TOUCH_FRIENDLY);
    nested_mouse_case();
    toast_case();
    render_case(0, ImVec2(1280.0f, 720.0f), 779.0f, 671.0f, 781.0f);
    render_case(RECOMP_RUNTIME_UI_PRESENTATION_TOUCH_FRIENDLY,
                ImVec2(3088.0f, 1440.0f), 3000.0f, 1300.0f, 3020.0f);
    return 0;
}
