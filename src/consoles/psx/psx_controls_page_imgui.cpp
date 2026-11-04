// psx_controls_page_imgui.cpp -- draws the Controls page (psx_controls_page.h)
// on a 640x480 page centred in the framebuffer, pillarboxed like the game.
// The look follows Capcom's OPTION screens at their own resolution: one PS
// pixel is 2x2 page units; row text is Lato rasterised 1:1 into the font
// atlas, thresholded and drawn pixel by pixel with the game's colour ramps
// and 1-pixel drop shadow. PS face buttons are drawn as shapes (Lato has no
// glyphs for them).
//
// A title that sets a skin (recomp_ui_psx_controls_set_skin) gets its own
// art instead: font, selector frames and backdrop sprites from its VRAM,
// drawn at native 320x240 coordinates through one nearest-filtered texture.
#include "consoles/psx/psx_controls_page.h"
#include "launcher_gl.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace rui_psx {
namespace controls {
namespace {

constexpr float kPageW = 640, kPageH = 480;
constexpr float kPx = 2;  // page units per PS pixel

// Colours sampled from the game's OPTION screen.
const ImU32 kBgBase = IM_COL32(0, 0, 72, 255);
const ImU32 kBgStreaks[] = {IM_COL32(8, 0, 88, 255), IM_COL32(8, 0, 88, 255), IM_COL32(8, 8, 96, 255),
                            IM_COL32(0, 0, 56, 255), IM_COL32(8, 16, 120, 255)};
const ImU32 kWhite[] = {IM_COL32(248, 248, 248, 255), IM_COL32(248, 248, 248, 255), IM_COL32(248, 248, 248, 255),
                        IM_COL32(248, 248, 248, 255), IM_COL32(240, 240, 240, 255), IM_COL32(224, 224, 224, 255),
                        IM_COL32(200, 200, 200, 255), IM_COL32(184, 184, 184, 255), IM_COL32(168, 168, 168, 255)};
const ImU32 kOrange[] = {IM_COL32(160, 24, 0, 255), IM_COL32(200, 56, 0, 255), IM_COL32(224, 80, 0, 255),
                         IM_COL32(240, 96, 0, 255), IM_COL32(240, 136, 0, 255), IM_COL32(240, 112, 0, 255),
                         IM_COL32(240, 96, 0, 255), IM_COL32(224, 80, 0, 255), IM_COL32(192, 64, 0, 255)};
const ImU32 kTitleRamp[] = {IM_COL32(120, 168, 240, 255), IM_COL32(160, 208, 240, 255), IM_COL32(176, 224, 240, 255),
                            IM_COL32(192, 240, 248, 255)};
const ImU32 kMutedRamp[] = {IM_COL32(150, 160, 210, 255)};
const ImU32 kCapRamp[] = {IM_COL32(0x1A, 0x1E, 0x2C, 255)};
const ImU32 kShadow = IM_COL32(8, 8, 8, 255);
const ImU32 kCapFace = IM_COL32(0xC9, 0xCC, 0xD6, 255);
const ImU32 kCapEdge = IM_COL32(0x6E, 0x73, 0x85, 255);
const ImU32 kMuted = IM_COL32(0x8E, 0x98, 0xC9, 255);
const ImU32 kWarn = IM_COL32(0xFF, 0xD2, 0x4A, 255);
const ImU32 kGood = IM_COL32(0x7C, 0xF0, 0xB0, 255);
const ImU32 kFace[] = {
    IM_COL32(0x3E, 0x6C, 0xE0, 255),  // cross
    IM_COL32(0xE0, 0x41, 0x3A, 255),  // circle
    IM_COL32(0xD9, 0x47, 0x9A, 255),  // square
    IM_COL32(0x3F, 0xAE, 0x6A, 255),  // triangle
};

struct Pen {
    ImDrawList *dl;
    const Fonts &f;
    ImVec2 origin;
    float s;
    ImVec2 at(float x, float y) const { return ImVec2(origin.x + x * s, origin.y + y * s); }
};

struct Style {           // how a pixel text run is coloured
    const ImU32 *ramp;   // top-to-bottom over the capital height
    int ramp_n;
    ImU32 shadow;        // drawn one PS pixel down-right; 0 = none
    float slant;         // PS pixels of shift per row above the baseline
    int spacing;         // extra PS pixels between letters
    int threshold;       // atlas alpha that lights a pixel
};

const Style kItem = {kWhite, 9, kShadow, 0, 2, 120};
const Style kItemSel = {kOrange, 9, kShadow, 0, 2, 120};
const Style kTitle = {kTitleRamp, 4, 0, 0.25f, 7, 120};
const Style kParen = {kMutedRamp, 1, kShadow, 0, 2, 120};
const Style kCapText = {kCapRamp, 1, 0, 0, 1, 120};

// One PS pixel at page coordinates, snapped so neighbours never leave seams.
void ps_pixel(const Pen &p, float x, float y, ImU32 col) {
    const ImVec2 a = p.at(x, y), b = p.at(x + kPx, y + kPx);
    p.dl->AddRectFilled(ImVec2(std::floor(a.x), std::floor(a.y)), ImVec2(std::floor(b.x), std::floor(b.y)), col);
}

struct Lit { int x, y; };

struct Run {
    std::vector<Lit> lit;  // relative to the run's left edge and the font's top
    int cap_top, cap_h;    // where the capitals sit, so the colour ramp spans them
    int width;             // PS pixels
};

// Lato's parentheses threshold into braces at this size: draw a clean 3-wide
// pixel paren spanning one pixel past the capitals. Returns the new pen.
int paren(Run *run, bool open, int pen) {
    const int top = run->cap_top - 1, bottom = run->cap_top + run->cap_h;
    for (int y = top; y <= bottom; ++y) {
        const int edge = std::min(y - top, bottom - y);  // 0 at the tips
        const int col = edge == 0 ? 2 : edge == 1 ? 1 : 0;
        run->lit.push_back({pen + (open ? col : 2 - col), y});
    }
    return pen + 3;
}

// Each glyph is placed by its lit columns, not its advance, so every gap is
// exactly st.spacing pixels -- the even rhythm of Capcom's fixed-cell font.
Run rasterise(ImFont *f, const std::string &t, const Style &st) {
    unsigned char *tex = nullptr;
    int tw = 0, th = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
    const ImFontGlyph *h = f->FindGlyph('H');
    Run run{{}, h ? (int)std::round(h->Y0) : 0, h ? std::max(1, (int)std::round(h->Y1 - h->Y0)) : 9, 0};
    int pen = 0;
    for (char c : t) {
        if (c == '(' || c == ')') { pen = paren(&run, c == '(', pen) + st.spacing; continue; }
        const ImFontGlyph *g = f->FindGlyph((ImWchar)(unsigned char)c);
        if (!g) continue;
        const int u0 = (int)std::round(g->U0 * tw), v0 = (int)std::round(g->V0 * th);
        const int gw = (int)std::round((g->U1 - g->U0) * tw), gh = (int)std::round((g->V1 - g->V0) * th);
        int lo = gw, hi = -1;
        const size_t first = run.lit.size();
        for (int y = 0; g->Visible && tex && y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                if (tex[((size_t)(v0 + y) * tw + u0 + x) * 4 + 3] >= st.threshold) {
                    run.lit.push_back({x, (int)std::round(g->Y0) + y});
                    lo = std::min(lo, x);
                    hi = std::max(hi, x);
                }
        if (hi < 0) { pen += (int)std::round(g->AdvanceX) + st.spacing; continue; }  // space
        for (size_t i = first; i < run.lit.size(); ++i) run.lit[i].x += pen - lo;
        pen += hi - lo + 1 + st.spacing;
    }
    run.width = std::max(0, pen - st.spacing);
    return run;
}

// Width of a pixel run in page units.
float pixel_width(ImFont *f, const std::string &t, const Style &st) { return rasterise(f, t, st).width * kPx; }

// Draws t with its capitals' top at page y; returns its width in page units.
float pixel_text(const Pen &p, ImFont *f, float x, float y, const std::string &t, const Style &st) {
    const Run run = rasterise(f, t, st);
    const int base = run.cap_top + run.cap_h;
    auto px_x = [&](const Lit &l) { return x + (l.x + std::round((base - l.y) * st.slant)) * kPx; };
    auto px_y = [&](const Lit &l) { return y + (l.y - run.cap_top) * kPx; };
    if (st.shadow)
        for (const Lit &l : run.lit) ps_pixel(p, px_x(l) + kPx, px_y(l) + kPx, st.shadow);
    for (const Lit &l : run.lit) {
        const int row = std::clamp((l.y - run.cap_top) * st.ramp_n / run.cap_h, 0, st.ramp_n - 1);
        ps_pixel(p, px_x(l), px_y(l), st.ramp[row]);
    }
    return run.width * kPx;
}

constexpr float kCapPx = 9 * kPx;  // capital height of the pixel row font

void face_button(const Pen &p, float cx, float cy, Glyph g) {
    const float r = 15 * p.s, k = 6.5f * p.s, th = 2.6f * p.s;
    const ImVec2 c = p.at(cx, cy);
    p.dl->AddCircleFilled(ImVec2(c.x + kPx * p.s, c.y + kPx * p.s), r, kShadow);
    p.dl->AddCircleFilled(c, r, IM_COL32(0x8C, 0x90, 0x9E, 255));
    p.dl->AddCircleFilled(ImVec2(c.x - r * .15f, c.y - r * .2f), r * .78f, IM_COL32(0xA6, 0xAA, 0xB6, 255));
    const ImU32 col = kFace[g];
    if (g == kCross) {
        p.dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, th);
        p.dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, th);
    } else if (g == kCircle) {
        p.dl->AddCircle(c, k, col, 0, th);
    } else if (g == kSquare) {
        p.dl->AddRect(ImVec2(c.x - k * .85f, c.y - k * .85f), ImVec2(c.x + k * .85f, c.y + k * .85f), col, 0, 0, th);
    } else {
        p.dl->AddTriangle(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y + k * .7f), ImVec2(c.x - k, c.y + k * .7f), col, th);
    }
}

void key_cap(const Pen &p, float right, float cy, const std::string &key) {
    const float tw = pixel_width(p.f.pixel, key, kCapText);
    const float w = std::max(32.f, tw + 16), l = right - w, t = cy - 15;
    p.dl->AddRectFilled(p.at(l + kPx, t + kPx), p.at(right + kPx, t + 30 + kPx), kShadow, 5 * p.s);
    p.dl->AddRectFilled(p.at(l, t), p.at(right, t + 30), kCapEdge, 5 * p.s);
    p.dl->AddRectFilled(p.at(l, t), p.at(right, t + 27), kCapFace, 5 * p.s);
    pixel_text(p, p.f.pixel, l + (w - tw) / 2, t + 13.5f - kCapPx / 2, key, kCapText);
}

void binding(const Pen &p, const View &v, const Row &r, float cy) {
    const float right = kPageW - 56, ty = cy - kCapPx / 2;
    if (v.page == kKeyboard) { key_cap(p, right, cy, r.key); return; }
    const float kw = pixel_width(p.f.pixel, r.key, kParen);
    pixel_text(p, p.f.pixel, right - kw, ty, r.key, kParen);
    const float gx = right - kw - 10;
    if (r.glyph != kNoGlyph) { face_button(p, gx - 15, cy, r.glyph); return; }
    pixel_text(p, p.f.pixel, gx - pixel_width(p.f.pixel, r.pad, kItem), ty, r.pad, kItem);
}

// Capcom's faceted silver diamond, lit from the top left.
void selector(const Pen &p, float cy) {
    const ImVec2 c = p.at(56 + 14, cy);
    const float k = 14 * p.s;
    const ImVec2 t(c.x, c.y - k), r(c.x + k, c.y), b(c.x, c.y + k), l(c.x - k, c.y);
    const ImVec2 o(kPx * p.s, kPx * p.s);
    p.dl->AddQuadFilled(ImVec2(t.x + o.x, t.y + o.y), ImVec2(r.x + o.x, r.y + o.y), ImVec2(b.x + o.x, b.y + o.y),
                        ImVec2(l.x + o.x, l.y + o.y), kShadow);
    p.dl->AddTriangleFilled(c, l, t, IM_COL32(216, 212, 228, 255));
    p.dl->AddTriangleFilled(c, t, r, IM_COL32(160, 152, 168, 255));
    p.dl->AddTriangleFilled(c, l, b, IM_COL32(136, 128, 144, 255));
    p.dl->AddTriangleFilled(c, b, r, IM_COL32(120, 104, 128, 255));
}

void rows(const Pen &p, const View &v) {
    float y = 88;
    for (size_t i = 0; i < v.rows.size(); ++i) {
        const Row &r = v.rows[i];
        const size_t nl = r.label.find('\n');
        const int lines = nl == std::string::npos ? 1 : 2;
        const float h = std::max(36.f, lines * 22.f + 4), cy = y + h / 2;
        const bool sel = (int)i == v.sel;
        const Style &st = sel ? kItemSel : kItem;
        const float ty = cy - (lines * 22.f - (22.f - kCapPx)) / 2;
        pixel_text(p, p.f.pixel, 96, ty, r.label.substr(0, nl), st);
        if (lines == 2) pixel_text(p, p.f.pixel, 96, ty + 22, r.label.substr(nl + 1), st);
        if (sel) selector(p, cy);
        if (v.page != kChooser && i + 1 < v.rows.size()) binding(p, v, r, cy);
        y += h;
    }
}

void footer(const Pen &p, const View &v) {
    const ImU32 msg_col = v.msg_kind == kMsgWarn ? kWarn : v.msg_kind == kMsgGood ? kGood : kMuted;
    ImFont *f = p.f.smooth;
    const float mx = (kPageW - f->CalcTextSizeA(14, FLT_MAX, 0, v.msg.c_str()).x) / 2;
    p.dl->AddText(f, 14 * p.s, p.at(mx + 1, 405), kShadow, v.msg.c_str());
    p.dl->AddText(f, 14 * p.s, p.at(mx, 404), msg_col, v.msg.c_str());
    float w = 0;
    auto text_of = [](const Segment &s) { return s.glyph == kEllipsis ? std::string("...") : s.text; };
    auto is_face = [](const Segment &s) { return s.glyph != kNoGlyph && s.glyph != kEllipsis; };
    for (const Segment &s : v.prompt) w += is_face(s) ? 30 : pixel_width(p.f.pixel, text_of(s), kItem);
    float x = (kPageW - w) / 2;
    for (const Segment &s : v.prompt) {
        if (is_face(s)) { face_button(p, x + 15, 440 + kCapPx / 2, s.glyph); x += 30; continue; }
        x += pixel_text(p, p.f.pixel, x, 440, text_of(s), kItem);
    }
}

// Dark navy with horizontal brushed streaks, one PS pixel tall, fixed seed.
void background(const Pen &p) {
    p.dl->AddRectFilled(p.at(0, 0), p.at(kPageW, kPageH), kBgBase);
    uint32_t seed = 0x4D4D5834u;
    auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (float y = 0; y < kPageH; y += kPx) {
        for (float x = 0; x < kPageW;) {
            const float len = (float)(4 + next() % 40) * kPx;
            const uint32_t pick = next() % 16;
            if (pick < 9) {
                const ImU32 col = kBgStreaks[pick < 7 ? 0 : pick < 8 ? 3 : next() % 6 == 0 ? 4 : 2];
                const ImVec2 a = p.at(x, y), b = p.at(std::min(x + len, kPageW), y + kPx);
                p.dl->AddRectFilled(ImVec2(std::floor(a.x), std::floor(a.y)), ImVec2(std::floor(b.x), std::floor(b.y)), col);
            }
            x += len;
        }
    }
}


// ---- title skin ------------------------------------------------------------

struct SkinState {
    bool has = false;
    RecompUiPsxControlsSkin skin{};
    std::vector<uint32_t> rgba;
    LauncherTexture tex{};
    bool uploaded = false;  // tex holds rgba
};
SkinState g_skin;

// The skin's texture, uploaded on first use after set_skin (GL thread).
bool skin_ready() {
    if (!g_skin.has) return false;
    if (!g_skin.uploaded) {
        launcher_texture_free(&g_skin.tex);
        g_skin.tex = launcher_texture_from_rgba_nearest(reinterpret_cast<const unsigned char *>(g_skin.rgba.data()),
                                                        g_skin.skin.width, g_skin.skin.height);
        g_skin.uploaded = true;
    }
    return g_skin.tex.id != 0;
}

constexpr int kNativeW = 320;
const ImU32 kTintNone = IM_COL32(255, 255, 255, 255);
const ImU32 kTintInfo = IM_COL32(176, 184, 232, 255);
const ImU32 kTintWarn = IM_COL32(255, 216, 80, 255);
const ImU32 kTintGood = IM_COL32(128, 240, 176, 255);

// One atlas rectangle at native (x, y), corners snapped to whole pixels.
void sprite(const Pen &p, const RecompUiPsxRect &r, int x, int y, ImU32 tint = kTintNone) {
    if (!r.w || !r.h) return;
    const float aw = (float)g_skin.skin.width, ah = (float)g_skin.skin.height;
    const ImVec2 a = p.at(x * kPx, y * kPx), b = p.at((x + r.w) * kPx, (y + r.h) * kPx);
    p.dl->AddImage((ImTextureID)(intptr_t)g_skin.tex.id, ImVec2(std::floor(a.x), std::floor(a.y)),
                   ImVec2(std::floor(b.x), std::floor(b.y)), ImVec2(r.x / aw, r.y / ah),
                   ImVec2((r.x + r.w) / aw, (r.y + r.h) / ah), tint);
}

const RecompUiPsxRect *glyph_of(const RecompUiPsxFont &f, char c) {
    const unsigned char u = (unsigned char)c;
    if (u < 32 || u > 127) return nullptr;
    const RecompUiPsxRect *g = &f.glyph[u - 32];
    if (!g->w && std::islower(u)) g = &f.glyph[std::toupper(u) - 32];
    return g->w ? g : nullptr;
}

int advance_of(const RecompUiPsxFont &f, char c) {
    if (f.advance) return f.advance;
    const RecompUiPsxRect *g = glyph_of(f, c);
    return g ? g->w + 1 : 4;  // proportional: one pixel between glyphs
}

int text_width(const RecompUiPsxFont &f, const std::string &t) {
    int w = 0;
    for (char c : t) w += advance_of(f, c);
    return w;
}

// Returns the native width drawn.
int skin_text(const Pen &p, const RecompUiPsxFont &f, int x, int y, const std::string &t, ImU32 tint = kTintNone) {
    const int x0 = x;
    for (char c : t) {
        if (const RecompUiPsxRect *g = glyph_of(f, c)) sprite(p, *g, x, y, tint);
        x += advance_of(f, c);
    }
    return x - x0;
}

// Lines of at most max_w native pixels, broken at spaces.
std::vector<std::string> wrap(const RecompUiPsxFont &f, const std::string &t, int max_w) {
    std::vector<std::string> lines(1);
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find(' ', at);
        if (end == std::string::npos) end = t.size();
        const std::string word = t.substr(at, end - at);
        std::string &line = lines.back();
        const std::string joined = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_width(f, joined) > max_w) lines.push_back(word);
        else line = joined;
        at = end + 1;
    }
    return lines;
}

const RecompUiPsxRect *face_sprite(Glyph g) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    switch (g) {
    case kCross: return &k.face[0];
    case kCircle: return &k.face[1];
    case kTriangle: return &k.face[2];
    case kSquare: return &k.face[3];
    case kEllipsis: return &k.ellipsis;
    default: return nullptr;
    }
}

const RecompUiPsxRect *shoulder_sprite(const std::string &pad) {
    static const char *const kNames[] = {"L1", "L2", "R1", "R2"};
    for (int i = 0; i < 4; ++i)
        if (pad == kNames[i] && g_skin.skin.shoulder[i].w) return &g_skin.skin.shoulder[i];
    return nullptr;
}

// Capcom's selector: the diamond spins through its frames on the vblank clock.
void skin_cursor(const Pen &p, int x, int y) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    if (k.cursor_frames < 1) return;
    const long vbl = (long)(ImGui::GetTime() * 60.0);
    const int f = (int)((vbl / std::max(1, k.cursor_vblanks)) % std::min(k.cursor_frames, 4));
    sprite(p, k.cursor[f], x + k.cursor_dx[f], y);
}

constexpr int kSkinCursorX = 23, kSkinLabelX = 40, kSkinBindRight = 304, kSkinLine = 12;

void skin_binding(const Pen &p, const View &v, const Row &r, int y) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    if (v.page == kKeyboard) {
        skin_text(p, k.item, kSkinBindRight - text_width(k.item, r.key), y, r.key);
        return;
    }
    int x = kSkinBindRight - text_width(k.item, r.key);
    skin_text(p, k.item, x, y, r.key);
    const RecompUiPsxRect *icon = r.glyph != kNoGlyph ? face_sprite(r.glyph) : shoulder_sprite(r.pad);
    if (icon) { sprite(p, *icon, x - 2 - icon->w, y + (k.item.line_height - icon->h) / 2); return; }
    skin_text(p, k.item, x - 4 - text_width(k.item, r.pad), y, r.pad);
}

void skin_rows(const Pen &p, const View &v) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    const bool chooser = v.page == kChooser;
    int y = chooser ? 64 : 44;
    for (size_t i = 0; i < v.rows.size(); ++i) {
        const Row &r = v.rows[i];
        const size_t nl = r.label.find('\n');
        const int lines = nl == std::string::npos ? 1 : 2;
        const bool sel = (int)i == v.sel;
        const RecompUiPsxFont &f = sel ? k.item_selected : k.item;
        skin_text(p, f, kSkinLabelX, y, r.label.substr(0, nl));
        if (lines == 2) skin_text(p, f, kSkinLabelX, y + kSkinLine, r.label.substr(nl + 1));
        const int mid = y + (lines - 1) * kSkinLine / 2;
        if (sel) skin_cursor(p, kSkinCursorX, mid);
        if (!chooser && i + 1 < v.rows.size()) skin_binding(p, v, r, mid);
        y += chooser ? 24 : 14 + (lines - 1) * kSkinLine;
    }
}

void skin_footer(const Pen &p, const View &v) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    const ImU32 tint = v.msg_kind == kMsgWarn ? kTintWarn : v.msg_kind == kMsgGood ? kTintGood : kTintInfo;
    const std::vector<std::string> lines = wrap(k.small, v.msg, kNativeW - 16);
    for (size_t i = 0; i < lines.size() && i < 2; ++i)
        skin_text(p, k.small, (kNativeW - text_width(k.small, lines[i])) / 2, 180 + (int)i * kSkinLine, lines[i],
                  tint);
    int w = 0;
    for (const Segment &s : v.prompt) {
        const RecompUiPsxRect *icon = face_sprite(s.glyph);
        w += icon ? icon->w : text_width(k.item, s.text);
    }
    int x = (kNativeW - w) / 2;
    for (const Segment &s : v.prompt) {
        if (const RecompUiPsxRect *icon = face_sprite(s.glyph)) { sprite(p, *icon, x, 208); x += icon->w; continue; }
        x += skin_text(p, k.item, x, 208, s.text);
    }
}

void draw_skinned(const Pen &p, const View &v) {
    const RecompUiPsxControlsSkin &k = g_skin.skin;
    p.dl->AddRectFilled(p.at(0, 0), p.at(kPageW, kPageH), IM_COL32(0, 0, 0, 255));
    for (int i = 0; i < std::min(k.backdrop_count, 32); ++i)
        sprite(p, k.backdrop[i].src, k.backdrop[i].x, k.backdrop[i].y);
    skin_text(p, k.title, (kNativeW - text_width(k.title, v.title)) / 2, 16, v.title);
    skin_rows(p, v);
    skin_footer(p, v);
}

}  // namespace

void set_skin(const RecompUiPsxControlsSkin *skin) {
    g_skin.uploaded = false;
    g_skin.has = skin && skin->rgba && skin->width > 0 && skin->height > 0;
    if (!g_skin.has) { g_skin.rgba.clear(); return; }
    g_skin.skin = *skin;
    g_skin.rgba.assign(skin->rgba, skin->rgba + (size_t)skin->width * skin->height);
    g_skin.skin.rgba = nullptr;  // the copy lives in g_skin.rgba
}

void release_gl() {
    launcher_texture_free(&g_skin.tex);
    g_skin.uploaded = false;
}

void draw(const Fonts &fonts, float fb_w, float fb_h) {
    const View &v = view();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float s = std::min(fb_w / kPageW, fb_h / kPageH);
    const Pen p{dl, fonts, ImVec2((fb_w - kPageW * s) / 2, (fb_h - kPageH * s) / 2), s};
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(fb_w, fb_h), IM_COL32(0, 0, 0, 255));  // pillarbox
    if (skin_ready()) { draw_skinned(p, v); return; }
    background(p);
    pixel_text(p, fonts.pixel_title, (kPageW - pixel_width(fonts.pixel_title, v.title, kTitle)) / 2, 36,
               v.title, kTitle);
    rows(p, v);
    footer(p, v);
}

}  // namespace controls
}  // namespace rui_psx
