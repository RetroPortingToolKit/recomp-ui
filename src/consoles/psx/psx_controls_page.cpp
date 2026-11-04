// psx_controls_page.cpp -- model and input of the provider's Controls page
// (recomp_ui_psx_menu.h). Keyboard binds go through the PSX keybinds bridge
// (keybinds.ini, player 1), gamepad binds through the PSX pad bridge
// (input.ini [mapping.<guid>]); both are the files the runtime reloads.
#include "consoles/psx/psx_controls_page.h"

#include "consoles/psx/psx_binds.h"
#include "consoles/psx/psx_pad_binds.h"

#if defined(__has_include)
#if __has_include("host_keymap.h")
#include "host_keymap.h"
#define RUI_PSX_HAS_HOST_KEYMAP 1
#endif
#endif

#if defined(LNG_SDL3)
#include <SDL3/SDL.h>
#endif

#include <cctype>
#include <cstring>
#include <deque>

namespace rui_psx {
namespace controls {
namespace {

// kPsxPadButtons (psx_profile.h) order: the index both bind bridges take.
const char *const kButtonNames[] = {
    "up", "down", "left", "right", "triangle", "circle", "cross", "square",
    "l1", "l2", "r1", "r2", "l3", "r3", "start", "select",
};
constexpr int kButtonCount = (int)(sizeof kButtonNames / sizeof kButtonNames[0]);
constexpr int kTriangleIndex = 4;
constexpr int kCrossIndex = 6;
constexpr int kStartIndex = 14;
constexpr int kMaxActions = 12;

struct Action {
    std::string label;
    int button;
};

std::vector<Action> g_actions;
Page g_page = kClosed;
int g_sel = 0;
int g_pending_key = -1;  // a hotkey's scancode waiting for its second press
std::string g_kb_path;
std::string g_pad_path;
std::string g_guid;
std::string g_msg;
MsgKind g_msg_kind = kMsgInfo;
bool g_trigger_held[2] = {false, false};
View g_view;
std::deque<int> g_cues;

void cue(int c) {
    if (g_cues.size() < 16) g_cues.push_back(c);
}

int button_index(const char *name) {
    for (int i = 0; name && i < kButtonCount; ++i)
        if (!std::strcmp(kButtonNames[i], name)) return i;
    return -1;
}

std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

int row_count() { return g_page == kChooser ? 3 : (int)g_actions.size() + 1; }
bool on_action_row() { return g_page != kChooser && g_sel < (int)g_actions.size(); }
void say(const std::string &text, MsgKind kind) { g_msg = text; g_msg_kind = kind; }

int key_of(int button) { return rui_psx_binds_get(g_kb_path.c_str(), 0, button); }

std::string key_name(int scancode) {
#if defined(LNG_SDL3)
    const char *n = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (!n || !n[0]) return "None";
    if (!std::strcmp(n, "Return")) return "Enter";
    return n;
#else
    (void)scancode;
    return "?";
#endif
}

std::string pad_source(int button) {
    char buf[64];
    rui_psx_pad_binds_label(g_pad_path.c_str(), g_guid.c_str(), button, buf, (int)sizeof buf);
    std::string s = buf;
    if (!s.empty() && s.back() == '+') s.pop_back();  // "righttrigger+" == "righttrigger"
    return s;
}

void pad_face(const std::string &src, Row *row) {
    static const struct { const char *src; Glyph glyph; const char *text; } kFaces[] = {
        {"a", kCross, ""}, {"b", kCircle, ""}, {"x", kSquare, ""}, {"y", kTriangle, ""},
        {"leftshoulder", kNoGlyph, "L1"}, {"rightshoulder", kNoGlyph, "R1"},
        {"lefttrigger", kNoGlyph, "L2"}, {"righttrigger", kNoGlyph, "R2"},
        {"leftstick", kNoGlyph, "L3"}, {"rightstick", kNoGlyph, "R3"},
        {"back", kNoGlyph, "SELECT"}, {"start", kNoGlyph, "START"},
    };
    for (const auto &f : kFaces) {
        if (src == f.src) { row->glyph = f.glyph; row->pad = f.text; return; }
    }
    row->glyph = kNoGlyph;
    row->pad = upper(src);
}

void rebuild() {
    View &v = g_view;
    v.page = g_page;
    v.sel = g_sel;
    v.rows.clear();
    v.prompt.clear();
    v.msg = g_msg;
    v.msg_kind = g_msg_kind;
    const std::string start = upper(key_name(key_of(kStartIndex)));
    if (g_page == kChooser) {
        v.title = "BUTTON CONFIG.";
        for (const char *l : {"KEYBOARD", "CONTROLLER", "EXIT"}) v.rows.push_back({l, "", "", kNoGlyph});
        v.prompt = {{"", kCross}, {" (" + upper(key_name(key_of(kCrossIndex))) + ") ", kNoGlyph},
                    {"", kEllipsis}, {"CHOOSE      ", kNoGlyph},
                    {"", kTriangle}, {" (" + upper(key_name(key_of(kTriangleIndex))) + ") ", kNoGlyph},
                    {"", kEllipsis}, {"CANCEL", kNoGlyph}};
        return;
    }
    v.title = g_page == kKeyboard ? "KEYBOARD CONFIG." : "CONTROLLER CONFIG.";
    for (const Action &a : g_actions) {
        Row r{upper(a.label), upper(key_name(key_of(a.button))), "", kNoGlyph};
        if (g_page == kPad) {
            pad_face(pad_source(a.button), &r);
            r.key = "(" + r.key + ")";
        }
        v.rows.push_back(r);
    }
    v.rows.push_back({"EXIT", "", "", kNoGlyph});
    v.prompt = {{"PRESS START (" + start + ") TO EXIT", kNoGlyph}};
}

void enter(Page page) {
    g_page = page;
    g_sel = 0;
    g_pending_key = -1;
    if (page == kKeyboard) say("Highlight a row, then press the key you want for it.", kMsgInfo);
    else if (page == kPad && g_guid.empty()) say("Connect a controller to change these buttons.", kMsgWarn);
    else if (page == kPad) say("Highlight a row, then press the controller button you want for it.", kMsgInfo);
    else say("", kMsgInfo);
}

// Label used in messages for a PS input: the action that reads it, if any.
std::string input_label(int button) {
    for (const Action &a : g_actions) {
        if (a.button != button) continue;
        std::string l = a.label;
        for (char &c : l) if (c == '\n') c = ' ';
        return l;
    }
    return upper(kButtonNames[button]);
}

#if defined(LNG_SDL3)
const char *hotkey_name(int keycode, int scancode, int mod) {
#if defined(RUI_PSX_HAS_HOST_KEYMAP)
    static const char *const kNames[HOST_KEYMAP_ACTION_COUNT] = {
        "fullscreen", "fast-forward", "volume up", "volume down", "performance display",
        "rewind", "save-state menu", "scanlines", "fast-forward toggle", "capture mark",
        "game menu",
    };
    for (int a = 0; a < HOST_KEYMAP_ACTION_COUNT; ++a)
        if (host_keymap_match_event((HostKeymapAction)a, keycode, scancode, mod)) return kNames[a];
#else
    (void)keycode; (void)scancode; (void)mod;
#endif
    return nullptr;
}

bool bind_key(const SDL_KeyboardEvent &k) {
    const int sc = (int)k.scancode;
    const char *hotkey = hotkey_name((int)k.key, sc, (int)(k.mod & SDL_KMOD_CTRL));
    if (hotkey && g_pending_key != sc) {
        g_pending_key = sc;
        say(key_name(sc) + " is the " + hotkey + " hotkey. Press " + key_name(sc) +
            " again to use it anyway.", kMsgWarn);
        cue(RECOMP_UI_PSX_CUE_WARN);
        return false;
    }
    g_pending_key = -1;
    const int b = g_actions[(size_t)g_sel].button;
    const int old = key_of(b);
    std::string text = input_label(b) + " now on " + key_name(sc) + ".";
    for (int o = 0; o < kButtonCount && old != sc; ++o) {
        if (o == b || key_of(o) != sc) continue;
        rui_psx_binds_set(g_kb_path.c_str(), 0, o, old);
        text += " " + input_label(o) + " now on " + key_name(old) + ".";
    }
    if (old != sc) rui_psx_binds_set(g_kb_path.c_str(), 0, b, sc);
    say(text, kMsgGood);
    cue(RECOMP_UI_PSX_CUE_BOUND);
    return old != sc;
}

// source: an input.ini name ("x", "righttrigger"); kind/code/dir: how to write it.
bool bind_pad(const std::string &source, int kind, int code, int dir) {
    if (g_guid.empty()) return false;
    const int b = g_actions[(size_t)g_sel].button;
    const std::string old = pad_source(b);
    if (old == source) { say(input_label(b) + " already uses that button.", kMsgInfo); return false; }
    Row shown{};
    pad_face(source, &shown);
    std::string text = input_label(b) + " now on " + (shown.pad.empty() ? upper(source) : shown.pad) + ".";
    for (int o = 0; o < kButtonCount; ++o) {
        if (o == b || pad_source(o) != source) continue;
        const SDL_GamepadButton ob = SDL_GetGamepadButtonFromString(old.c_str());
        const SDL_GamepadAxis oa = SDL_GetGamepadAxisFromString(old.c_str());
        if (ob != SDL_GAMEPAD_BUTTON_INVALID) rui_psx_pad_binds_set(g_pad_path.c_str(), g_guid.c_str(), o, 1, (int)ob, 0);
        else if (oa != SDL_GAMEPAD_AXIS_INVALID) rui_psx_pad_binds_set(g_pad_path.c_str(), g_guid.c_str(), o, 2, (int)oa, 1);
        text += " " + input_label(o) + " takes the old button.";
    }
    rui_psx_pad_binds_set(g_pad_path.c_str(), g_guid.c_str(), b, kind, code, dir);
    say(text, kMsgGood);
    cue(RECOMP_UI_PSX_CUE_BOUND);
    return true;
}

void use_pad(SDL_JoystickID which) {
    char buf[64];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(which), buf, (int)sizeof buf);
    g_guid = buf;
}

void move(int delta) {
    const int n = row_count();
    g_sel = (g_sel + delta + n) % n;
    g_pending_key = -1;
    cue(RECOMP_UI_PSX_CUE_MOVE);
}

void back() {
    if (g_page == kChooser) g_page = kClosed;
    else enter(kChooser);
    cue(RECOMP_UI_PSX_CUE_BACK);
}

void activate() {
    if (g_page != kChooser || g_sel == 2) { back(); return; }  // the EXIT row
    enter(g_sel == 0 ? kKeyboard : kPad);
    cue(RECOMP_UI_PSX_CUE_ACCEPT);
}

bool on_key(const SDL_KeyboardEvent &k) {
    const int sc = (int)k.scancode;
    if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) { move(sc == SDL_SCANCODE_UP ? -1 : 1); return false; }
    if (k.repeat) return false;
    if (sc == key_of(kStartIndex) || sc == SDL_SCANCODE_ESCAPE ||
        (g_page == kChooser && sc == key_of(kTriangleIndex))) {
        back();
        return false;
    }
    if (g_page == kKeyboard && on_action_row()) {
        return sc != SDL_SCANCODE_LEFT && sc != SDL_SCANCODE_RIGHT && bind_key(k);
    }
    if (sc == key_of(kCrossIndex) || sc == SDL_SCANCODE_SPACE) activate();
    return false;
}

bool on_pad_button(const SDL_GamepadButtonEvent &b) {
    const auto button = (SDL_GamepadButton)b.button;
    if (button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
        move(button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : 1);
        return false;
    }
    if (button == SDL_GAMEPAD_BUTTON_START || (g_page == kChooser && button == SDL_GAMEPAD_BUTTON_NORTH)) {
        back();
        return false;
    }
    if (g_page == kPad && on_action_row()) {
        if (button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) return false;
        use_pad(b.which);
        return bind_pad(SDL_GetGamepadStringForButton(button), 1, (int)button, 0);
    }
    if (button == SDL_GAMEPAD_BUTTON_SOUTH) activate();
    return false;
}

bool on_pad_axis(const SDL_GamepadAxisEvent &a) {
    const int t = a.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : a.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? 1 : -1;
    if (t < 0) return false;
    const bool held = a.value > 16000;
    const bool pressed = held && !g_trigger_held[t];
    g_trigger_held[t] = held;
    if (!pressed || g_page != kPad || !on_action_row()) return false;
    use_pad(a.which);
    const auto axis = (SDL_GamepadAxis)a.axis;
    return bind_pad(SDL_GetGamepadStringForAxis(axis), 2, (int)axis, 1);
}

std::string first_pad_guid() {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    std::string guid;
    if (ids && n > 0) {
        char buf[64];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[0]), buf, (int)sizeof buf);
        guid = buf;
    }
    SDL_free(ids);
    return guid;
}
#endif

}  // namespace

void set_actions(const RecompUiPsxControlAction *actions, int count) {
    g_actions.clear();
    for (int i = 0; actions && i < count && i < kMaxActions; ++i) {
        const int b = button_index(actions[i].button);
        if (b >= 0 && actions[i].label) g_actions.push_back({actions[i].label, b});
    }
}

bool open(const std::string &exe_dir) {
#if defined(LNG_SDL3)
    if (g_actions.empty()) return false;
    g_kb_path = exe_dir + "keybinds.ini";
    g_pad_path = exe_dir + "input.ini";
    g_guid = first_pad_guid();
    g_trigger_held[0] = g_trigger_held[1] = false;
    g_cues.clear();
    enter(kChooser);
    rebuild();
    return true;
#else
    (void)exe_dir;
    return false;  // input capture needs the SDL3 event adapter
#endif
}

void close() { g_page = kClosed; }
int take_cue() {
    if (g_cues.empty()) return RECOMP_UI_PSX_CUE_NONE;
    const int c = g_cues.front();
    g_cues.pop_front();
    return c;
}
bool is_open() { return g_page != kClosed; }
const View &view() { return g_view; }

int event(const void *p, bool *bindings_changed) {
    *bindings_changed = false;
#if defined(LNG_SDL3)
    if (!p || g_page == kClosed) return 0;
    const SDL_Event &e = *static_cast<const SDL_Event *>(p);
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: *bindings_changed = on_key(e.key); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: *bindings_changed = on_pad_button(e.gbutton); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: *bindings_changed = on_pad_axis(e.gaxis); break;
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        return 1;
    default:
        return 0;
    }
    if (g_page != kClosed) rebuild();
    return 1;
#else
    (void)p;
    return 0;
#endif
}

}  // namespace controls
}  // namespace rui_psx
