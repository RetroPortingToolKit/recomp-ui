#pragma once
#include "imgui.h"

/* Only the dashboard entry is gated. The existing Mods page is unchanged. */
static inline bool rui_mods_button_tooltip(bool enabled) {
    if (!enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", "Please select a valid game disc first.");
        return true;
    }
    return false;
}

static inline bool rui_mods_button(bool enabled, const char* label, ImVec2 size) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    rui_mods_button_tooltip(enabled);
    return enabled && pressed;
}
