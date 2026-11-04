/* psx_controls_page.h -- the provider's Controls page (recomp_ui_psx_menu.h):
 * model and input in psx_controls_page.cpp, ImGui drawing in
 * psx_controls_page_imgui.cpp. Every call is made with rui_psx::lock() held. */
#pragma once

#include "recomp_ui_psx_menu.h"

#include <string>
#include <vector>

struct ImFont;

namespace rui_psx {
namespace controls {

enum Page { kClosed, kChooser, kKeyboard, kPad };
enum Glyph { kNoGlyph = -1, kCross, kCircle, kSquare, kTriangle, kEllipsis };
enum MsgKind { kMsgInfo, kMsgWarn, kMsgGood };

struct Segment {         // one run of prompt text, or one PS face-button glyph
    std::string text;
    Glyph glyph;
};

struct Row {
    std::string label;   // upper case; '\n' splits lines
    std::string key;     // keyboard page: key cap; controller page: "(key)"
    std::string pad;     // controller page: source shown when glyph is kNoGlyph
    Glyph glyph;
};

struct View {
    Page page;
    int sel;
    std::string title;
    std::vector<Row> rows;  // last row is EXIT
    std::string msg;
    MsgKind msg_kind;
    std::vector<Segment> prompt;
};

void set_actions(const RecompUiPsxControlAction *actions, int count);
bool open(const std::string &exe_dir);
void close();
bool is_open();
/* An SDL event while open; 1 = consumed. Sets *bindings_changed when a key
 * or button was written to disk. */
int event(const void *sdl_event, bool *bindings_changed);
const View &view();
int take_cue();  // RECOMP_UI_PSX_CUE_*, oldest first
// Player 1's keyboard key for a keybinds.ini button name, upper case; "" if unknown.
std::string key_label(const std::string &exe_dir, const char *button);
// Presentation side (psx_controls_page_imgui.cpp).
void set_skin(const RecompUiPsxControlsSkin *skin);  // copied
void release_gl();  // GL context going away
struct Fonts {
    ImFont *smooth;       // help line
    ImFont *pixel;        // rows, prompt, key caps: rasterised 1:1, drawn as PS pixels
    ImFont *pixel_title;
};
/* Draw the open page over the whole framebuffer (ImGui frame in progress). */
void draw(const Fonts &fonts, float framebuffer_w, float framebuffer_h);

}  // namespace controls
}  // namespace rui_psx
