// launcher_pad_nav.h - pad buttons that are already held when gamepad
// navigation turns on must be released before they count.
//
// The ImGui SDL backends poll pad STATE, and only while
// ImGuiConfigFlags_NavEnableGamepad is set. So on the frame navigation turns
// on (the launcher arms it on the first real button press, and suspends it
// during bind captures), every button that happens to be down at that moment
// reads to ImGui as a fresh press. A button that reads as held for good -- a
// stuck switch, a remapper holding it, a virtual device -- then drives the
// menu indefinitely. Observed: an Xbox One pad whose X
// (WEST) read as held opened ImGui's gamepad window switcher (the
// "(Untitled)" list, NavGamepadMenu = FaceLeft) and it never closed.
//
// Call launcher_pad_nav_lock_held_keys() before ImGui::NewFrame() on the frame
// gamepad navigation turns on. Every gamepad key gets LockUntilRelease: ImGui
// clears the lock at once for keys that are up, and keeps it for keys that
// are down until they are released. Side effect: the press that arms pad
// navigation only arms it; the next press acts.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

inline void launcher_pad_nav_lock_held_keys() {
    const ImGuiID owner = ImHashStr("##launcher-pad-held-at-nav-start");
    for (int k = ImGuiKey_Gamepad_BEGIN; k < ImGuiKey_Gamepad_END; ++k)
        ImGui::SetKeyOwner((ImGuiKey)k, owner, ImGuiInputFlags_LockUntilRelease);
}
