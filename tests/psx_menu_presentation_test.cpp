// Real provider SDL input, ImGui context/frame/drawer and fake host. Only GL
// submission is replaced with a CPU surface; no window, focus or audio.
#define RUI_MENU_PRESENTATION_TEST
#define main provider_model_checks
#include "psx_menu_provider_test.cpp"
#undef main
#include "../src/consoles/psx/psx_menu_provider_imgui.cpp"
#include "../src/common/backends/imgui/runtime_ui_imgui.cpp"
#include "imgui_internal.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../src/third_party/stb_image_write.h"
#include <filesystem>
#include <vector>
#include <fstream>
#include <cstdlib>

static int invalidations = 0;
bool ImGui_ImplOpenGL3_Init(const char *) { return true; }
void ImGui_ImplOpenGL3_Shutdown() {}
void ImGui_ImplOpenGL3_NewFrame() {}
void ImGui_ImplOpenGL3_DestroyFontsTexture() { ++invalidations; }
void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData *) {}

static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL presentation: %s\n", message); std::exit(1); }
}
static void key(SDL_Keycode code) {
    SDL_Event e{}; e.type = SDL_EVENT_KEY_DOWN; e.key.key = code;
    require(rui_psx::sdl_event(&e) == 1, "keyboard event consumed");
}
static void pad(Uint8 button) {
    SDL_Event e{}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = button;
    require(rui_psx::sdl_event(&e) == 1, "controller event consumed");
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; rui_psx::sdl_event(&e);
}

static void capture(const std::filesystem::path &path, int width, int height) {
    auto *data = ImGui::GetDrawData();
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    require(surface != nullptr, "surface");
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    require(renderer != nullptr, "software renderer");
    unsigned char *atlas; int w, h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &w, &h);
    SDL_Surface *font = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, atlas, w * 4);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, font);
    require(texture != nullptr, "font texture");
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    const float scale = data->FramebufferScale.x;
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderDrawColor(renderer, 26, 32, 45, 255); SDL_RenderClear(renderer);
    for (int n = 0; n < data->CmdListsCount; ++n) {
        const auto *list = data->CmdLists[n]; std::vector<SDL_Vertex> vertices;
        for (const auto &v : list->VtxBuffer) {
            const auto c = ImGui::ColorConvertU32ToFloat4(v.col);
            vertices.push_back({{v.pos.x, v.pos.y}, {c.x, c.y, c.z, c.w}, {v.uv.x, v.uv.y}});
        }
        for (const auto &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            SDL_Rect clip{int(cmd.ClipRect.x), int(cmd.ClipRect.y),
                int(cmd.ClipRect.z - cmd.ClipRect.x), int(cmd.ClipRect.w - cmd.ClipRect.y)};
            SDL_SetRenderClipRect(renderer, &clip);
            std::vector<int> indices;
            for (unsigned i = 0; i < cmd.ElemCount; ++i)
                indices.push_back(list->IdxBuffer[cmd.IdxOffset + i] + cmd.VtxOffset);
            require(SDL_RenderGeometry(renderer, texture, vertices.data(), int(vertices.size()),
                indices.data(), int(indices.size())), "draw geometry");
        }
    }
    SDL_RenderPresent(renderer);
    require(stbi_write_png(path.u8string().c_str(), width, height, 4,
        surface->pixels, surface->pitch) != 0, "screenshot");
    SDL_DestroyTexture(texture); SDL_DestroySurface(font);
    SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface);
}

int main() {
    require(provider_model_checks() == 0, "existing provider semantics");
    auto *p = recomp_ui_psx_menu_provider();
    g_exit = -1; g_netplay = 0; g_discs = 3;
    PsxMenuHost host = make_host(0);
    require(p->attach(p->ctx, nullptr, nullptr, &host) != 0, "attach");
    key(SDLK_F10); require(p->is_open(p->ctx), "toggle opens");
    // Use the production SDL input path, not a second ImGui navigation cursor.
    key(SDLK_DOWN); key(SDLK_RETURN);
    auto *ui = rui_psx::model();
    require(ui->in_section != 0, "keyboard enters category");
    const size_t before_row = ui->row_index;
    pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    require(ui->row_index != before_row, "controller moves row");

    const char *folder = std::getenv("RUI_MENU_CAPTURE_DIR");
    std::ofstream geometry;
    if (folder) {
        std::filesystem::create_directories(folder);
        geometry.open(std::filesystem::path(folder) / "geometry.csv");
        geometry << "mode,width,height,dpi,scale,font_pixels,panel_w,panel_h,atlas_w,atlas_h\n";
    }
    // A single context survives all window/fullscreen/resolution/DPI changes.
    struct Size { int w, h; };
    for (const char *mode : {"windowed", "fullscreen"}) {
        for (const Size size : {Size{640,480}, {960,540}, {1280,720},
                               {1920,1080}, {2560,1440}, {3840,2160}, {1280,720}}) {
            for (const float dpi : {1.0f, 1.5f, 2.0f}) {
                rui_psx::g_display_scale.store(dpi);
                for (int frame = 0; frame < 3; ++frame) p->draw(p->ctx, size.w, size.h);
                ImGui::SetCurrentContext(rui_psx::g_ctx);
                auto &io = ImGui::GetIO();
                auto *menu = ImGui::FindWindowByName("##recomp-runtime-ui");
                const float scale = io.DisplayFramebufferScale.x;
                require(menu && menu->Active, "real menu drawn");
                require(std::fabs(io.DisplaySize.x * scale - size.w) < 1,
                        "framebuffer scale applied exactly once");
                require(menu->Pos.x >= 0 && menu->Pos.y >= 0 &&
                    (menu->Pos.x + menu->Size.x) * scale <= size.w + 1 &&
                    (menu->Pos.y + menu->Size.y) * scale <= size.h + 1, "panel fits");
                require(menu->ScrollMax.x < 1 && menu->ScrollMax.y < 1, "outer panel fits footer");
                require(io.Fonts->Fonts.Size == 1, "no stale accumulated fonts");
                require(std::fabs(io.Fonts->Fonts[0]->FontSize - 18) < .1f, "logical font stable");
                const int before = invalidations;
                p->draw(p->ctx, size.w, size.h);
                require(invalidations == before, "unchanged density reuses atlas");
                ImGui::SetCurrentContext(rui_psx::g_ctx);
                if (size.h >= 1080) require(18 * scale >= 27, "fullscreen font readable");
                for (auto *child : GImGui->Windows) {
                    if (!child->Active || child->ParentWindow != menu) continue;
                    require(child->ScrollMax.x < 1, "no horizontal child clipping");
                    if (std::strstr(child->Name, "##footer"))
                        require(child->Pos.y + child->Size.y <= menu->InnerRect.Max.y + 1 &&
                                child->ScrollMax.y < 1, "footer reachable");
                }
                if (folder) {
                    char name[160]; std::snprintf(name, sizeof name, "%s-%dx%d-dpi%.0f.png",
                        mode, size.w, size.h, dpi * 100);
                    capture(std::filesystem::path(folder) / name, size.w, size.h);
                    geometry << mode << ',' << size.w << ',' << size.h << ',' << dpi << ',' << scale
                        << ',' << 18 * scale << ',' << menu->Size.x * scale << ',' << menu->Size.y * scale
                        << ',' << io.Fonts->TexWidth << ',' << io.Fonts->TexHeight << '\n';
                }
            }
        }
    }
    // Actual SDL controller activation of Disc/insert, then keyboard Resume.
    require(go("Disc", "media.disc"), "disc category retained");
    pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    require(go("Disc", "media.insert"), "disc insertion row retained");
    g_inserted = 0; pad(SDL_GAMEPAD_BUTTON_SOUTH);
    require(g_inserted == 2 && !p->is_open(p->ctx), "controller inserts disc through host");
    key(SDLK_F10); require(go("Graphics", "graphics.render_thread"), "graphics setting retained");
    key(SDLK_RIGHT); require(g_applies > 0, "keyboard setting provider write");
    require(go("System", RECOMP_RUNTIME_UI_KEY_RESUME), "Resume retained");
    key(SDLK_RETURN); require(!p->is_open(p->ctx) && !g_paused, "Resume releases pause");
    p->detach(p->ctx);
    require(ImGui::GetCurrentContext() == nullptr, "provider context released");
    std::puts("PASS: real provider/input/drawer,42 same-context viewport/DPI transitions; CPU-only renders, categories/settings/disc/Resume; no physical GUI coverage");
}
