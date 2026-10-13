// Modern in-game presentation for the renderer-independent runtime UI model.
// The host owns the ImGui context/frame/submission. This file only draws.

#include "recomp_runtime_ui_internal.h"
#include "launcher_theme.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

ImVec4 col(const LngColor &c, float alpha = 1.0f) {
    return ImVec4(c.r, c.g, c.b, c.a * alpha);
}

ImU32 u32(const LngColor &c, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(col(c, alpha));
}

void value_text(RecompRuntimeUi *ui, const RecompRuntimeUiItem *item,
                char *out, size_t out_size) {
    if (item->type == RECOMP_RUNTIME_UI_ACTION) {
        std::snprintf(out, out_size, "OPEN");
        return;
    }
    if (item->type == RECOMP_RUNTIME_UI_TEXT) {
        char buf[128];
        recomp_runtime_ui_current_text(ui, item, buf, sizeof(buf));
        std::snprintf(out, out_size, "%s", buf[0] ? buf : "(not set)");
        return;
    }
    int value = 0;
    if (!recomp_runtime_ui_current_value(ui, item, &value)) {
        std::snprintf(out, out_size, "Unavailable");
    } else if (item->type == RECOMP_RUNTIME_UI_BOOL) {
        std::snprintf(out, out_size, "%s", value ? "On" : "Off");
    } else if (item->type == RECOMP_RUNTIME_UI_CHOICE && item->choices) {
        size_t index = item->choice_count;
        if (item->choice_values) {
            for (size_t i = 0; i < item->choice_count; ++i)
                if (item->choice_values[i] == value) { index = i; break; }
        } else if (value >= item->minimum &&
                   static_cast<size_t>(value - item->minimum) < item->choice_count) {
            index = static_cast<size_t>(value - item->minimum);
        }
        if (index < item->choice_count)
            std::snprintf(out, out_size, "%s", item->choices[index]);
        else
            std::snprintf(out, out_size, "Unavailable");
    } else if (item->key &&
               std::strcmp(item->key,
                           RECOMP_RUNTIME_UI_KEY_GYRO_SENSITIVITY) == 0) {
        std::snprintf(out, out_size, "%.2fx",
                      static_cast<float>(value) / 100.0f);
    } else {
        std::snprintf(out, out_size, "%d", value);
    }
}

void draw_sections(RecompRuntimeUi *ui, const LauncherTheme &theme,
                   bool enter_on_click) {
    for (size_t index = 0; index < ui->section_count; ++index) {
        ImGui::PushID(static_cast<int>(index));
        const bool selected = index == ui->section_index;
        const bool clicked = ImGui::Selectable(ui->sections[index], selected, 0,
                                               ImVec2(0.0f, theme.row_height));
        if ((clicked || ImGui::IsItemHovered()) &&
            (index != ui->section_index || (clicked && enter_on_click))) {
            const int was_in_section = ui->in_section;
            recomp_runtime_ui_enter_section(ui, index);
            if (!(clicked && enter_on_click)) ui->in_section = was_in_section;
        }
        ImGui::PopID();
    }
}

void draw_items(RecompRuntimeUi *ui, const LauncherTheme &theme,
                bool touch_friendly) {
    const size_t count = recomp_runtime_ui_page_entry_count(ui);
    for (size_t index = 0; index < count; ++index) {
        char folder[96];
        const RecompRuntimeUiItem *item = recomp_runtime_ui_page_entry(
            ui, index, folder, sizeof(folder));
        if (!item) {
            /* Folder row: opens a nested page. */
            if (!folder[0]) continue;
            ImGui::PushID(static_cast<int>(index));
            const bool selected = ui->in_section && index == ui->row_index;
            char label[112];
            std::snprintf(label, sizeof(label), "%s  >", folder);
            if (ImGui::Selectable(label, selected, 0,
                                  ImVec2(0.0f, theme.row_height))) {
                ui->row_index = index;
                ui->in_section = 1;
                if (recomp_runtime_ui_enter_folder(ui, index)) {
                    /* The remaining rows and hover belong to the parent page.
                     * Draw the new page on the next frame. */
                    ImGui::PopID();
                    return;
                }
            }
            if (ImGui::IsItemHovered()) {
                ui->row_index = index;
                ui->in_section = 1;
            }
            ImGui::PopID();
            continue;
        }

        const bool enabled = recomp_runtime_ui_item_enabled(ui, item) != 0;
        const bool selected = ui->in_section && index == ui->row_index;
        char value[128];
        value_text(ui, item, value, sizeof(value));

        ImGui::PushID(static_cast<int>(index));
        if (!enabled) ImGui::BeginDisabled();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float row_h = item->description && item->description[0]
                                ? theme.row_height + theme.spacing_sm
                                : theme.row_height;
        const float step_button_w =
            touch_friendly ? std::max(76.0f, row_h * 0.90f) : 38.0f;
        const float step_gap = touch_friendly ? 12.0f : 6.0f;
        const float step_controls_w =
            item->type == RECOMP_RUNTIME_UI_INT
                ? step_button_w * 2.0f + step_gap + theme.spacing_sm
                : 0.0f;
        // INT rows carry -/+ buttons and TEXT rows an edit field, both drawn on
        // top of the row selectable; without AllowOverlap the selectable would
        // swallow the clicks meant for them.
        const ImGuiSelectableFlags row_flags =
            (item->type == RECOMP_RUNTIME_UI_INT ||
             item->type == RECOMP_RUNTIME_UI_TEXT)
                ? ImGuiSelectableFlags_AllowOverlap : 0;
        if (ImGui::Selectable("##setting", selected, row_flags,
                              ImVec2(0.0f, row_h))) {
            ui->row_index = index;
            ui->in_section = 1;
            if (item->type != RECOMP_RUNTIME_UI_INT)
                recomp_runtime_ui_adjust_current(ui, 1, 1, 0);
        }
        if (ImGui::IsItemHovered()) {
            ui->row_index = index;
            ui->in_section = 1;
        }

        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImVec2 end = ImGui::GetItemRectMax();
        const ImVec2 next = ImGui::GetCursorScreenPos();
        const ImU32 label_color = u32(enabled ? theme.text : theme.text_muted,
                                      enabled ? 1.0f : 0.65f);
        draw->AddText(ImVec2(start.x + theme.spacing_md,
                            start.y + theme.spacing_sm),
                      label_color, item->label ? item->label : "");
        const bool stepped_value = item->type == RECOMP_RUNTIME_UI_INT;
        const bool editing_this = item->type == RECOMP_RUNTIME_UI_TEXT &&
                                  ui->editing_text && selected;
        if (!editing_this) {
            const ImVec2 value_size = ImGui::CalcTextSize(value);
            draw->AddText(ImVec2(end.x - theme.spacing_md - value_size.x -
                                    step_controls_w,
                                start.y + theme.spacing_sm),
                          u32(selected && enabled ? theme.accent2
                                                  : theme.text_muted),
                          value);
        }
        if (item->description && item->description[0]) {
            draw->AddText(ImVec2(start.x + theme.spacing_md,
                                start.y + theme.spacing_sm +
                                    ImGui::GetTextLineHeight() + 2.0f),
                          u32(theme.text_muted), item->description);
        }
        if (editing_this && enabled) {
            const float field_w = touch_friendly
                ? std::clamp((end.x - start.x) * 0.42f, 240.0f, 520.0f)
                : 200.0f;
            const float field_h = touch_friendly
                ? std::max(ImGui::GetFrameHeight(), row_h * 0.72f)
                : ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(
                ImVec2(end.x - theme.spacing_md - field_w,
                       start.y + (row_h - field_h) * 0.5f));
            ImGui::SetNextItemWidth(field_w);
            // Nothing is active on the first frame of an edit, which is exactly
            // when focus should land in the field.
            if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool submitted = ImGui::InputText(
                "##edit", ui->edit_buffer, sizeof(ui->edit_buffer),
                ImGuiInputTextFlags_EnterReturnsTrue |
                    ImGuiInputTextFlags_AutoSelectAll);
            const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (submitted) {
                recomp_runtime_ui_commit_text(ui, item, ui->edit_buffer);
                ui->editing_text = 0;
            } else if (cancelled) {
                ui->editing_text = 0;
            } else if (!ImGui::IsItemActive() && ImGui::IsAnyItemActive()) {
                // Focus moved elsewhere (a click on another row): treat it as
                // abandoning the edit rather than silently committing.
                ui->editing_text = 0;
            }
            ImGui::SetCursorScreenPos(next);
        }
        if (stepped_value && enabled) {
            const float button_h = touch_friendly
                ? std::max(ImGui::GetFrameHeight(), row_h * 0.72f)
                : ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(
                ImVec2(end.x - theme.spacing_sm -
                           step_button_w * 2.0f - step_gap,
                       start.y + (row_h - button_h) * 0.5f));
            if (ImGui::Button("-", ImVec2(step_button_w, button_h))) {
                ui->row_index = index;
                ui->in_section = 1;
                recomp_runtime_ui_adjust_current(ui, -1, 1, 0);
            }
            ImGui::SameLine(0.0f, step_gap);
            if (ImGui::Button("+", ImVec2(step_button_w, button_h))) {
                ui->row_index = index;
                ui->in_section = 1;
                recomp_runtime_ui_adjust_current(ui, 1, 1, 0);
            }
            ImGui::SetCursorScreenPos(next);
        }
        if (!enabled) ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void push_runtime_style(const LauncherTheme &theme, bool touch_friendly,
                        float opacity) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme.radius_sm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme.radius_sm);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme.spacing_lg, theme.spacing_lg));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(theme.spacing_sm, theme.spacing_sm));
    ImGui::PushStyleVar(
        ImGuiStyleVar_ScrollbarSize,
        touch_friendly ? std::max(30.0f, theme.spacing_lg * 1.5f)
                       : ImGui::GetStyle().ScrollbarSize);
    ImGui::PushStyleVar(
        ImGuiStyleVar_GrabMinSize,
        touch_friendly ? std::max(30.0f, theme.spacing_lg * 1.5f)
                       : ImGui::GetStyle().GrabMinSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(theme.background, 0.98f * opacity));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(theme.panel, 0.96f * opacity));
    ImGui::PushStyleColor(ImGuiCol_Border, col(theme.border));
    ImGui::PushStyleColor(ImGuiCol_Text, col(theme.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, col(theme.text_muted));
    ImGui::PushStyleColor(ImGuiCol_Header, col(theme.control_hovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col(theme.panel_hovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, col(theme.accent_dim));
    ImGui::PushStyleColor(ImGuiCol_Button, col(theme.control));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(theme.control_hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(theme.accent_dim));
}

void pop_runtime_style() {
    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(7);
}

// The toast: a borderless panel centred near the top of the display, drawn
// with or without the menu. It takes no input and no focus.
void draw_toast(RecompRuntimeUi *ui, const LauncherTheme &theme) {
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, theme.spacing_lg),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f * ui->opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme.spacing_lg, theme.spacing_md));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(theme.panel));
    ImGui::PushStyleColor(ImGuiCol_Border, col(theme.accent2, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, col(theme.text));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##recomp-runtime-ui-toast", nullptr, flags)) {
        if (ui->toast_title[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
            ImGui::TextUnformatted(ui->toast_title);
            ImGui::PopStyleColor();
        }
        if (ui->toast_body[0]) {
            if (ui->toast_title[0]) ImGui::Separator();
            ImGui::TextUnformatted(ui->toast_body);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

} // namespace

extern "C" void recomp_runtime_ui_render_imgui(RecompRuntimeUi *ui) {
    if (!ui || ImGui::GetCurrentContext() == nullptr) return;
    if (!ui->open) {
        if (recomp_runtime_ui_toast_visible(ui))
            draw_toast(ui, launcher_theme_by_name(ui->config.theme));
        return;
    }

    LauncherTheme theme = launcher_theme_by_name(ui->config.theme);
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;
    const bool touch_friendly =
        (ui->config.presentation_flags &
         RECOMP_RUNTIME_UI_PRESENTATION_TOUCH_FRIENDLY) != 0;

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), display,
        IM_COL32(0, 0, 0, static_cast<int>(255.0f * ui->dim + 0.5f)));

    const float short_axis = std::min(display.x, display.y);
    const float touch_row_height =
        std::clamp(short_axis * 0.085f, 92.0f, 124.0f);
    const float metric_scale =
        touch_friendly ? touch_row_height / theme.row_height : 1.0f;
    if (touch_friendly) {
        theme.row_height *= metric_scale;
        theme.spacing_sm *= metric_scale;
        theme.spacing_md *= metric_scale;
        theme.spacing_lg *= metric_scale;
        theme.radius_sm *= metric_scale;
        theme.radius_lg *= metric_scale;
    }
    const float margin = touch_friendly
        ? std::max(20.0f, short_axis * 0.025f)
        : 24.0f;
    const float max_width = touch_friendly ? display.x : 780.0f;
    const float max_height = touch_friendly ? display.y : 680.0f;
    const float width = display.x - margin * 2.0f < max_width
                            ? display.x - margin * 2.0f
                            : max_width;
    const float height = display.y - margin * 2.0f < max_height
                             ? display.y - margin * 2.0f
                             : max_height;
    const bool wide = width >= (touch_friendly ? 900.0f : 640.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

    push_runtime_style(theme, touch_friendly, ui->opacity);
    // The host drives selection through recomp_runtime_ui_handle_input. ImGui's
    // own keyboard/gamepad navigation would move a second, hidden cursor over
    // the same rows, and Enter / A would activate whichever row that one is
    // on. A text row being edited still needs the keyboard.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   (ui->editing_text ? 0 : ImGuiWindowFlags_NoNavInputs);
    if (ImGui::Begin("##recomp-runtime-ui", nullptr, flags)) {
        if (touch_friendly) {
            // Scale toward a phone-readable physical size regardless of any
            // host-wide DPI scale already applied to the ImGui context.
            const float target_font_px =
                std::clamp(short_axis * 0.036f, 34.0f, 48.0f);
            const float base_font_px =
                ImGui::GetFont()->FontSize * ImGui::GetFont()->Scale *
                io.FontGlobalScale;
            const float font_scale =
                std::clamp(target_font_px / base_font_px, 1.0f, 4.0f);
            ImGui::SetWindowFontScale(font_scale);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
        ImGui::TextUnformatted(ui->config.title ? ui->config.title : "Settings");
        ImGui::PopStyleColor();
        if (ui->config.subtitle && ui->config.subtitle[0]) {
            ImGui::SameLine();
            const float subtitle_w = ImGui::CalcTextSize(ui->config.subtitle).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                                 ImGui::GetStyle().WindowPadding.x - subtitle_w);
            ImGui::TextDisabled("%s", ui->config.subtitle);
        }
        ImGui::Separator();

        const float close_h = touch_friendly
            ? std::max(ImGui::GetFrameHeight(), theme.row_height * 0.82f)
            : 0.0f;
        const char *close_label = "Resume";
        const float available_w = ImGui::GetContentRegionAvail().x;
        const float wanted_close_w = touch_friendly
            ? std::max(320.0f, ImGui::CalcTextSize(close_label).x + theme.spacing_lg * 2.0f)
            : ImGui::CalcTextSize(close_label).x + theme.spacing_lg * 2.0f;
        const float close_w = std::min(wanted_close_w, available_w * 0.4f);
        const float text_w = std::max(1.0f, available_w - close_w - ImGui::GetStyle().ItemSpacing.x);
        char hint[384];
        if (touch_friendly) {
            std::snprintf(hint, sizeof hint, "Tap a section or setting");
        } else {
            std::snprintf(hint, sizeof hint, "%s  Select    %s  Back    D-pad / Arrows  Navigate",
                ui->config.accept_label ? ui->config.accept_label : "Select",
                ui->config.back_label ? ui->config.back_label : "Back");
        }
        float text_h = ImGui::CalcTextSize(hint, nullptr, false, text_w).y;
        if (ui->status_frames)
            text_h += ImGui::GetStyle().ItemSpacing.y +
                      ImGui::CalcTextSize(ui->status, nullptr, false, text_w).y;
        const float footer_content_h = std::max(text_h,
            touch_friendly ? close_h : ImGui::GetFrameHeight());
        const float footer_h = footer_content_h + theme.spacing_lg + 2.0f;
        const float content_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer_h);
        if (wide) {
            const float sidebar_w = touch_friendly
                ? std::clamp(width * 0.20f, 280.0f, 520.0f)
                : 190.0f;
            ImGui::BeginChild("##sections", ImVec2(sidebar_w, content_h), true);
            draw_sections(ui, theme, false);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##items", ImVec2(0.0f, content_h), true);
            char page_title[256];
            recomp_runtime_ui_page_title(ui, page_title, sizeof(page_title));
            if (ui->page[0]) {
                if (ImGui::SmallButton("< Back")) recomp_runtime_ui_leave_folder(ui);
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
            ImGui::TextUnformatted(page_title);
            ImGui::PopStyleColor();
            ImGui::Separator();
            draw_items(ui, theme, touch_friendly);
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("##content", ImVec2(0.0f, content_h), true);
            if (ui->in_section) {
                if (ImGui::SmallButton("< Back") &&
                    !recomp_runtime_ui_leave_folder(ui))
                    recomp_runtime_ui_leave_section(ui);
                ImGui::SameLine();
                char page_title[256];
                recomp_runtime_ui_page_title(ui, page_title, sizeof(page_title));
                ImGui::TextUnformatted(page_title);
                ImGui::Separator();
                draw_items(ui, theme, touch_friendly);
            } else {
                draw_sections(ui, theme, true);
            }
            ImGui::EndChild();
        }

        ImGui::Separator();
        // Text owns its own wrapping region; it cannot run behind Resume.
        ImGui::BeginChild("##footer", ImVec2(text_w, footer_content_h), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", hint);
        if (ui->status_frames) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
            ImGui::TextUnformatted(ui->status);
            ImGui::PopStyleColor();
            --ui->status_frames;
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::Button(close_label, ImVec2(close_w, close_h)))
            recomp_runtime_ui_close(ui);
    }
    ImGui::End();
    pop_runtime_style();
    // Over the menu too: a shortcut can still answer while it is open.
    if (recomp_runtime_ui_toast_visible(ui)) draw_toast(ui, theme);
}
