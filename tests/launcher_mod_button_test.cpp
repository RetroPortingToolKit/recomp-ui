/* Real Dear ImGui widgets without a platform backend or window. Hover metadata
 * is supplied directly to the unit fixture: no OS input/events or pointer moves. */
#include "backends/imgui/launcher_mod_button.h"
#include "imgui_internal.h"
#include "backends/imgui/launcher_mod_integer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static unsigned draw_case(bool enabled, bool hovered) {
    ImGui::NewFrame();
    ImGui::Begin("button-fixture");
    ImGuiContext& g = *GImGui;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    int start = draw->VtxBuffer.Size;
    assert(!rui_mods_button(enabled, "Mods", ImVec2(110, 34)));
    assert(((g.LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0) == !enabled);
    unsigned alpha = 0;
    for (int i = start; i < draw->VtxBuffer.Size; ++i) {
        unsigned a = (draw->VtxBuffer[i].col >> IM_COL32_A_SHIFT) & 255;
        if (a > alpha) alpha = a;
    }
    assert(alpha > 0); // The disabled button remains visible.
    assert(!(g.CurrentItemFlags & ImGuiItemFlags_Disabled)); // Other controls unaffected.
    assert(!rui_mods_button_tooltip(enabled)); // No hover -> no message.
    if (hovered) {
        g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_HoveredRect |
                                      ImGuiItemStatusFlags_HoveredWindow;
        if (!enabled) assert(!ImGui::IsItemHovered()); // Ordinary hover rejects disabled widgets.
        ImGui::LogToBuffer();
        assert(rui_mods_button_tooltip(enabled) == !enabled);
        if (!enabled)
            assert(std::strstr(g.LogBuffer.c_str(), "Please select a valid game disc first."));
        else
            assert(!std::strstr(g.LogBuffer.c_str(), "Please select a valid game disc first."));
        ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
    return alpha;
}

static void integer_cases() {
    ImGui::NewFrame(); ImGui::Begin("integer-fixture");
    const int64_t low = std::numeric_limits<int64_t>::min();
    const int64_t high = std::numeric_limits<int64_t>::max();
    for (int64_t divisor : {int64_t(1), int64_t(100)}) {
        ImGui::PushID((int)divisor);
        int64_t value = high;
        assert(!rui_mod_integer_control(&value, low, high, 1, true, divisor));
        assert(value == high);
        ImGui::PopID();
    }
    int64_t scaled = 125;
    ImGui::LogToBuffer();
    assert(!rui_mod_integer_control(&scaled, 100, 250, 1, true, 100));
    assert(scaled == 125 && std::strstr(GImGui->LogBuffer.c_str(), "1.25"));
    ImGui::LogFinish();
    ImGui::End(); ImGui::Render();
}

int main() {
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720); io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    integer_cases();
    draw_case(false, false); // First-frame window layout.
    const unsigned disabled = draw_case(false, true);
    const unsigned enabled = draw_case(true, true);
    assert(disabled < enabled); // Real disabled styling, not just a click guard.
    ImGui::DestroyContext();
    std::puts("PASS real ImGui Mods controls: button state/tooltip; full-width integer ranges preserved; scaled 1.25 display; no desktop/input automation");
}
