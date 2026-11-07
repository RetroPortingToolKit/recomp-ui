#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

inline bool rui_mod_integer_control(int64_t* value, int64_t min_value,
                                    int64_t max_value, int64_t step,
                                    bool slider, int64_t display_divisor) {
    const bool scaled = slider && display_divisor > 1;
    constexpr int64_t exact_double_max = (int64_t(1) << 53) - 1;
    if (scaled && min_value >= -exact_double_max && max_value <= exact_double_max) {
        const double divisor = static_cast<double>(display_divisor);
        double shown = static_cast<double>(*value) / divisor;
        const double lo = static_cast<double>(min_value) / divisor;
        const double hi = static_cast<double>(max_value) / divisor;
        int decimals = 0;
        for (int64_t n = display_divisor; n > 1; n /= 10) ++decimals;
        char format[16];
        std::snprintf(format, sizeof(format), "%%.%df", decimals);
        const bool edited = ImGui::SliderScalar("##integer", ImGuiDataType_Double,
            &shown, &lo, &hi, format, ImGuiSliderFlags_AlwaysClamp);
        if (edited) *value = static_cast<int64_t>(std::llround(shown * divisor));
        return edited;
    }
    // Dear ImGui S64 sliders require half-range bounds. Scaled values beyond
    // exact double integers use the full-range integer editor too.
    const bool integer_slider = slider && !scaled &&
        min_value >= std::numeric_limits<int64_t>::min() / 2 &&
        max_value <= std::numeric_limits<int64_t>::max() / 2;
    return integer_slider
        ? ImGui::SliderScalar("##integer", ImGuiDataType_S64, value,
                              &min_value, &max_value, "%lld")
        : ImGui::InputScalar("##integer", ImGuiDataType_S64, value,
                             &step, nullptr, nullptr, ImGuiInputTextFlags_None);
}
