// psx_controls_page_test: the provider's Controls page model
// (src/consoles/psx/psx_controls_page.cpp) against scratch keybinds.ini /
// input.ini in argv[1], driven by synthetic SDL3 events.
//
// Testing strategy (partition -> test):
//   chooser navigation: wraps / selects page / Start leaves  -> test_chooser
//   keyboard bind, key unused by any input                    -> test_key_unused
//   keyboard bind, key held by another action (swap)          -> test_key_swap
//   keyboard bind, key held by a host hotkey (warn, confirm)  -> test_key_hotkey
//   keys that never bind: arrows, Start's key (boundary)      -> test_key_reserved
//   pad bind, button held by another action (swap)            -> test_pad_swap
//   pad trigger: at threshold (no bind) / above (bind)        -> test_pad_trigger
//   prompts name the keyboard key of the PS input they mention -> test_prompts
//   triangle's key: cancels the chooser / binds on a page      -> test_triangle
//   cues: move, accept, bound, warn, back, in order; empty     -> test_cues
#include "consoles/psx/psx_controls_page.h"
#include "consoles/psx/psx_binds.h"
#include "consoles/psx/psx_pad_binds.h"
#include "host_keymap.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

// Host hotkeys: only plain F (the default performance display) is one here.
extern "C" int host_keymap_match_event(HostKeymapAction action, int, int scancode, int mod) {
    return action == HOST_KEYMAP_DISPLAY_PERF && scancode == SDL_SCANCODE_F && mod == 0;
}

namespace c = rui_psx::controls;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

static std::string g_dir, g_kb, g_pad;
// kPsxPadButtons order (psx_profile.h).
enum { kCircle = 5, kCross = 6, kSquare = 7, kR2 = 11, kSelect = 15 };

static const RecompUiPsxControlAction kActions[] = {
    {"Fire", "square"}, {"Jump", "cross"}, {"Giga", "r2"},
};

static bool key(SDL_Scancode sc) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    bool changed = false;
    c::event(&e, &changed);
    return changed;
}

static bool pad_button(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = (Uint8)b;
    bool changed = false;
    c::event(&e, &changed);
    return changed;
}

static bool trigger(Sint16 value) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    e.gaxis.value = value;
    bool changed = false;
    c::event(&e, &changed);
    return changed;
}

static int kb(int b) { return rui_psx_binds_get(g_kb.c_str(), 0, b); }

static std::string pad(int b) {
    char buf[64];
    char guid[64];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(0), guid, (int)sizeof guid);
    rui_psx_pad_binds_label(g_pad.c_str(), guid, b, buf, (int)sizeof buf);
    return buf;
}

static void reopen() {
    rui_psx_binds_reset(g_kb.c_str(), 0);
    c::close();
    CHECK(c::open(g_dir));
}

static void open_page(int down) {  // 0 keyboard, 1 controller
    reopen();
    for (int i = 0; i < down; ++i) key(SDL_SCANCODE_DOWN);
    key(SDL_SCANCODE_X);  // cross's default key selects
}

static void test_chooser() {
    reopen();
    CHECK(c::view().page == c::kChooser);
    CHECK(c::view().rows.size() == 3);
    key(SDL_SCANCODE_UP);
    CHECK(c::view().sel == 2);  // wraps to EXIT
    key(SDL_SCANCODE_DOWN);
    key(SDL_SCANCODE_X);
    CHECK(c::view().page == c::kKeyboard);
    CHECK(c::view().title == "KEYBOARD CONFIG.");
    CHECK(c::view().rows.size() == 4);  // three actions + EXIT
    key(SDL_SCANCODE_RETURN);           // Start's key: back to the chooser
    CHECK(c::view().page == c::kChooser);
    key(SDL_SCANCODE_RETURN);           // and out
    CHECK(!c::is_open());
}

static void test_key_unused() {
    open_page(0);
    CHECK(key(SDL_SCANCODE_C));
    CHECK(kb(kSquare) == SDL_SCANCODE_C);
    CHECK(c::view().rows[0].key == "C");
}

static void test_key_swap() {
    open_page(0);
    const int fire_old = kb(kSquare);
    CHECK(key(SDL_SCANCODE_X));  // Jump's key
    CHECK(kb(kSquare) == SDL_SCANCODE_X);
    CHECK(kb(kCross) == fire_old);
    key(SDL_SCANCODE_DOWN);
    CHECK(key(SDL_SCANCODE_RSHIFT));  // Select's key, not an action: still swaps
    CHECK(kb(kCross) == SDL_SCANCODE_RSHIFT);
    CHECK(kb(kSelect) == fire_old);
}

static void test_key_hotkey() {
    open_page(0);
    const int before = kb(kSquare);
    CHECK(!key(SDL_SCANCODE_F));
    CHECK(kb(kSquare) == before);
    CHECK(c::view().msg_kind == c::kMsgWarn);
    CHECK(key(SDL_SCANCODE_F));  // second press confirms
    CHECK(kb(kSquare) == SDL_SCANCODE_F);
}

static void test_key_reserved() {
    open_page(0);
    const int before = kb(kSquare);
    CHECK(!key(SDL_SCANCODE_LEFT));
    CHECK(!key(SDL_SCANCODE_RIGHT));
    CHECK(kb(kSquare) == before);
    key(SDL_SCANCODE_RETURN);
    CHECK(c::view().page == c::kChooser);
    CHECK(kb(kSquare) == before);
}

static void test_pad_swap() {
    open_page(1);
    CHECK(c::view().page == c::kPad);
    CHECK(pad_button(SDL_GAMEPAD_BUTTON_SOUTH));  // Jump's button onto Fire
    CHECK(pad(kSquare) == "a");
    CHECK(pad(kCross) == "x");
    CHECK(c::view().rows[0].glyph == c::kCross);
    CHECK(!c::view().rows[0].key.empty() && c::view().rows[0].key.front() == '(');
}

static void test_pad_trigger() {
    open_page(1);  // Fire row; Giga holds R2 by default
    const std::string fire_old = pad(kSquare);
    CHECK(!trigger(16000));
    CHECK(trigger(16001));
    CHECK(pad(kSquare).rfind("righttrigger", 0) == 0);
    CHECK(pad(kR2) == fire_old);
}

static void test_prompts() {
    reopen();
    const auto &p = c::view().prompt;
    CHECK(p.size() == 8 && p[0].glyph == c::kCross && p[1].text == " (X) " && p[2].glyph == c::kEllipsis &&
          p[3].text.rfind("CHOOSE", 0) == 0 && p[4].glyph == c::kTriangle && p[5].text == " (A) " &&
          p[7].text == "CANCEL");
    key(SDL_SCANCODE_X);
    CHECK(c::view().prompt.size() == 1 && c::view().prompt[0].text == "PRESS START (ENTER) TO EXIT");
}

static void test_triangle() {
    reopen();
    key(SDL_SCANCODE_A);  // triangle's default key
    CHECK(!c::is_open());
    open_page(0);
    CHECK(key(SDL_SCANCODE_A));  // on a page it is just a key to bind
    CHECK(c::view().page == c::kKeyboard);
}

static void test_cues() {
    reopen();
    while (c::take_cue() != RECOMP_UI_PSX_CUE_NONE) {}
    key(SDL_SCANCODE_DOWN);
    key(SDL_SCANCODE_UP);
    key(SDL_SCANCODE_X);  // keyboard page
    key(SDL_SCANCODE_C);
    key(SDL_SCANCODE_DOWN);
    key(SDL_SCANCODE_F);  // hotkey: warn
    key(SDL_SCANCODE_RETURN);
    const int want[] = {RECOMP_UI_PSX_CUE_MOVE, RECOMP_UI_PSX_CUE_MOVE, RECOMP_UI_PSX_CUE_ACCEPT,
                        RECOMP_UI_PSX_CUE_BOUND, RECOMP_UI_PSX_CUE_MOVE, RECOMP_UI_PSX_CUE_WARN,
                        RECOMP_UI_PSX_CUE_BACK, RECOMP_UI_PSX_CUE_NONE};
    for (int w : want) CHECK(c::take_cue() == w);
}

int main(int argc, char **argv) {
    g_dir = std::string(argc > 1 ? argv[1] : ".") + "/";
    SDL_CreateDirectory(g_dir.c_str());
    g_kb = g_dir + "keybinds.ini";
    g_pad = g_dir + "input.ini";
    SDL_RemovePath(g_kb.c_str());
    SDL_RemovePath(g_pad.c_str());
    c::set_actions(kActions, 3);
    test_chooser();
    test_key_unused();
    test_key_swap();
    test_key_hotkey();
    test_key_reserved();
    test_pad_swap();
    test_pad_trigger();
    test_prompts();
    test_triangle();
    test_cues();
    std::printf("%s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
