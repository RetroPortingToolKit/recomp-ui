#ifndef RUI_LAUNCHER_NAV_H
#define RUI_LAUNCHER_NAV_H

/* One predicate, in a header, because it is the answer to a bug report rather
 * than a detail of the footer that raised it.
 *
 * The launcher re-homes its focus ring to PLAY on Back/Start from a pad and on
 * Backspace from the keyboard: directional nav through the card child windows
 * is easy to wander out of with no way back. But Backspace is also the delete
 * key in all two dozen of the launcher's text fields. On the Rename Controller
 * dialog it deleted a character AND snapped the focus ring to PLAY behind the
 * modal, so the field went dead after one keystroke and the name could not be
 * edited at all.
 *
 * WantTextInput is exactly "an InputText is active". The popup test is the
 * same bug with a controller in hand: Back or Start over a modal reached past
 * it to the page underneath.
 */

#include "imgui.h"

static inline bool rui_nav_rehome_to_play(void) {
    /* WantTextInput is computed at EndFrame, so mid-frame it is the PREVIOUS
     * frame's answer: on the frame an InputText first becomes active it still
     * reads false, and a Backspace arriving in that one frame would steal the
     * focus anyway. IsAnyItemActive() is already true by then. Both, so the
     * window is closed. */
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return false;
    if (ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId |
                                 ImGuiPopupFlags_AnyPopupLevel))
        return false;
    return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
}

#endif /* RUI_LAUNCHER_NAV_H */
