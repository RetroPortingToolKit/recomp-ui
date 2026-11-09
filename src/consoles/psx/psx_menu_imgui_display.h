#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace rui_psx {

// The host supplies framebuffer pixels. Keep layout/font metrics logical and
// apply the scale once when submitting to that framebuffer, as the launcher does.
// Limit scaling in small windows so the footer and navigation remain reachable.
inline bool configure_menu_display(int width, int height, float display_scale) {
    ImGuiIO &io = ImGui::GetIO();
    if (!std::isfinite(display_scale) || display_scale <= 0) display_scale = 1;
    const float resolution_scale = std::min(width / 1280.0f, height / 720.0f);
    const float fit_scale = std::min(width / 640.0f, height / 480.0f);
    const float scale = std::max(0.1f, std::min(
        std::max(display_scale, resolution_scale), fit_scale));
    const bool rebuild = !io.Fonts->Fonts.Size ||
        std::fabs(io.DisplayFramebufferScale.x - scale) > 0.001f;
    io.DisplaySize = ImVec2(width / scale, height / scale);
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    if (rebuild) {
        io.Fonts->Clear();
        ImFontConfig cfg;
        cfg.SizePixels = 18.0f;
#if IMGUI_VERSION_NUM >= 19060
        cfg.RasterizerDensity = scale;
#endif
        io.Fonts->AddFontDefault(&cfg);
        io.Fonts->Build();
    }
    return rebuild;
}

} // namespace rui_psx
