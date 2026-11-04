/* psx_menu_provider_internal.h -- shared between the provider's model logic
 * (psx_menu_provider.cpp, no SDL/ImGui) and its SDL+ImGui presentation
 * (psx_menu_provider_imgui.cpp). */
#pragma once

#include "recomp_runtime_ui.h"

#include <mutex>
#include <string>

struct PsxMenuHost;

namespace rui_psx {

std::mutex &lock();
RecompRuntimeUi *model();           // NULL when not attached
void input(int recomp_runtime_ui_input);  // caller holds lock()
const char *assets_dir();           // the title's assets folder ("" when not attached)
// The Controls page wrote bindings; caller holds lock().
void controls_bindings_changed();
std::string exe_dir();  // folder of keybinds.ini / input.ini, with trailing slash

// Presentation side (psx_menu_provider_imgui.cpp; tests stub these).
int  sdl_event(const void *sdl_event);      // main thread
void draw(int framebuffer_w, int framebuffer_h);  // GL thread
void release_gl();                          // detach, GL current

}  // namespace rui_psx
