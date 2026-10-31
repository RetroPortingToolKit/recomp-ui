// launcher_player_tabs.h - the tab strips of the dashboard: one tab per player
// over the controller card, and one tab per memory card over the card shown.
//
// The cards used to stand side by side. A title with four players then needed
// two rows of them, and the second row did not fit above the memory cards:
// "PLAYER 4" was cut off and the section scrolled. With a tab per player the
// section is one card high whatever the player count.
//
// A strip is rows of tabs. Players 1 to 4 are the first row and players 5 to 8
// the second (a PlayStation title reaches eight players with two multitaps).
// A label is never shortened: when four full labels do not fit the width, the
// rows hold two tabs each, and then one.
//
// In a header, as launcher_nav.h and launcher_pad_nav.h are, so that
// tests/launcher_player_tabs_strip_test.cpp can run it in real ImGui frames
// with no window.
//
// How a tab is selected:
//   - a click;
//   - focus (d-pad or arrow keys along and between the rows), then activate
//     (Cross / A, Enter, Space). Focus alone selects nothing: the focus ring
//     crosses the strip on its way up or down the page, and that must not
//     change which player is shown;
//   - L1 / R1 from anywhere on the page, for the player strip only: the
//     previous / next player, with the focus ring put on that tab.
#pragma once

#include "imgui.h"

#include <cstdio>

// Colours of the active theme and sizes in logical units.
struct RuiPlayerTabsLook {
    ImU32 tab, tab_hovered, tab_selected;   // fills
    ImU32 border;                           // outline of the selected tab
    ImU32 text, text_selected;
    ImU32 accent;                           // bar under the selected tab
    ImU32 dot_on, dot_off;                  // the player has a device / has none
    float height, gap, rounding;
};

// Players in one row of the player strip.
enum { kRuiPlayerTabsRow = 4 };
// Tabs a strip can hold: the eight players of two multitaps.
enum { kRuiTabsMax = 8 };

// L1 / R1 as "previous / next player": -1, +1 or 0.
//
// Not while a field is edited or an item is held, and not over an open list
// or dialog (the rule of rui_nav_rehome_to_play). Not while face-left is down
// either: that is ImGui's window-switcher button, and it gives L1 / R1 another
// meaning. IsKeyPressed honours the lock that launcher_pad_nav_lock_held_keys()
// takes, so a shoulder button that is held when pad navigation starts does
// nothing until it is released; and the backend feeds no pad key at all while
// pad navigation is off.
inline int rui_player_tabs_shoulder_step() {
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return 0;
    const char* any = nullptr;   // typed: imgui_internal.h adds an ImGuiID overload
    if (ImGui::IsPopupOpen(any, ImGuiPopupFlags_AnyPopupId |
                                ImGuiPopupFlags_AnyPopupLevel))
        return 0;
    if (ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft)) return 0;
    return (int)ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) -
           (int)ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
}

// Width a tab needs for its dot and `label`.
inline float rui_tab_label_width(const char* label) {
    const float dot_r = 5.0f, dot_gap = 6.0f, pad = 8.0f;
    return dot_r * 2.0f + dot_gap + ImGui::CalcTextSize(label).x + pad * 2.0f;
}

// How many tabs stand in one row: `row_len` (or `count`, when that is fewer)
// while the widest label fits a tab of that row, else two, else one.
inline int rui_tabs_per_row(int count, int row_len, float width, float gap,
                            float widest_label) {
    int n = count < row_len ? count : row_len;
    if (n < 1) n = 1;
    while (n > 1 && (width - gap * (float)(n - 1)) / (float)n < widest_label)
        n = n > 2 ? 2 : 1;
    return n;
}

// Draws `count` tabs in rows across `width` at the cursor and returns the
// selected tab (0-based) after this frame's input.
//
// dots[i] puts a status dot on tab i (filled: on; ring: off); the player strip
// shows there whether the player has a device, so nobody has to open every tab
// to see who plays. `step` is -1 / +1 to move the selection by one tab with
// the focus ring (rui_player_tabs_shoulder_step), or 0.
//
// Every row has tabs of one width, so a short last row stands under the first
// tabs of the row above: Player 5 under Player 1.
inline int rui_tab_rows(const char* id, int count, int selected,
                        const char* const* labels, const bool* dots,
                        float width, const RuiPlayerTabsLook& look, int row_len,
                        int step) {
    if (count < 1) return 0;
    if (count > kRuiTabsMax) count = kRuiTabsMax;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;

    if (step) selected = (selected + count + step) % count;
    int picked = selected;

    float widest = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float w = rui_tab_label_width(labels[i]);
        if (w > widest) widest = w;
    }
    const int per_row = rui_tabs_per_row(count, row_len, width, look.gap, widest);
    const float tab_w = (width - look.gap * (float)(per_row - 1)) / (float)per_row;
    const float dot_r = 5.0f, dot_gap = 6.0f;
    const float text_h = ImGui::GetTextLineHeight();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        if (i % per_row)
            ImGui::SameLine(0, look.gap);
        else if (i)   // a new row, the strip's own gap under the row above
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                                 ImGui::GetStyle().ItemSpacing.y + look.gap);
        ImGui::PushID(i);
        const ImVec2 mn = ImGui::GetCursorScreenPos();
        const ImVec2 mx(mn.x + tab_w, mn.y + look.height);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_w, look.height),
                                   ImGuiButtonFlags_EnableNav))
            picked = i;
        /* -1: the item just submitted. It only focuses a button. */
        if (step && i == selected) ImGui::SetKeyboardFocusHere(-1);
        const bool sel = i == selected;
        const bool hov = ImGui::IsItemHovered();

        dl->AddRectFilled(mn, mx, sel ? look.tab_selected
                                      : hov ? look.tab_hovered : look.tab,
                          look.rounding);
        if (sel) {
            dl->AddRect(mn, mx, look.border, look.rounding);
            dl->AddRectFilled(ImVec2(mn.x + look.rounding, mx.y - 3.0f),
                              ImVec2(mx.x - look.rounding, mx.y), look.accent, 1.5f);
        }

        const float text_w = ImGui::CalcTextSize(labels[i]).x;
        const float x = mn.x + (tab_w - (dot_r * 2.0f + dot_gap + text_w)) * 0.5f;
        const ImVec2 dot(x + dot_r, mn.y + look.height * 0.5f);
        if (dots && dots[i]) dl->AddCircleFilled(dot, dot_r, look.dot_on);
        else                 dl->AddCircle(dot, dot_r, look.dot_off, 0, 1.5f);
        dl->AddText(ImVec2(x + dot_r * 2.0f + dot_gap,
                           mn.y + (look.height - text_h) * 0.5f),
                    sel ? look.text_selected : look.text, labels[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return picked;
}

// The player strip: "<word> 1" to "<word> count", four to a row, with L1 / R1.
// assigned[p] is player p's dot.
inline int rui_player_tabs(int count, int selected, const bool* assigned,
                           float width, const RuiPlayerTabsLook& look,
                           const char* word) {
    if (count > kRuiTabsMax) count = kRuiTabsMax;
    char text[kRuiTabsMax][48];
    const char* labels[kRuiTabsMax];
    for (int p = 0; p < count; ++p) {
        std::snprintf(text[p], sizeof(text[p]), "%s %d", word, p + 1);
        labels[p] = text[p];
    }
    return rui_tab_rows("player_tabs", count, selected, labels, assigned, width,
                        look, kRuiPlayerTabsRow, rui_player_tabs_shoulder_step());
}
