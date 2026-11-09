/* Render the production Settings drawer into CPU-only ImGui frames. No SDL
 * window, GL context, game, focus or operating-system input is created. */
#if defined(RUI_SETTINGS_MODULE_CASE)
#define RECOMP_LAUNCHER_V2_BUILD_MODULE 1
#endif
#include "../src/common/backends/imgui/launcher_imgui.cpp"
#include "player_settings.h"
#include "launcher_profile.h"
#include <set>
#include <cassert>
#include "../src/third_party/stb_image_write.h"
#include <filesystem>
#include <fstream>
static void probe_fail(const char* condition,int line) {
    std::fprintf(stderr,"FAIL Settings drawer line%d: %s\n",line,condition);std::exit(1);
}
#undef assert
#define assert(value) ((value)?(void)0:probe_fail(#value,__LINE__))

#if defined(RUI_SETTINGS_MODULE_CASE)
extern "C" const RecompLauncherV2Run* launcher_v2_current_run(void) { return nullptr; }
extern "C" uint32_t launcher_v2_thumbnail(uint32_t,uint8_t*,uint32_t) { return 0; }
#endif
static std::set<std::string> visited_general,visited_display,visited_packages;
static const char* packages[]={"psx.enhancement.crt-filter","psx.enhancement.image-filtering",
 "psx.enhancement.internal-resolution","psx.enhancement.pgxp","psx.enhancement.enhanced-audio","psx.feature.rewind"};
static int features(void*) { return 8; }
static int feature(void*,int index,RecompLauncherCModFeature* out) {
    if(index<0 || index>=8)return 0;
    *out={};out->enabled=1;
    if(index<2) {
        std::snprintf(out->package_id,sizeof out->package_id,"psx.runtime.settings");
        std::snprintf(out->id,sizeof out->id,"%s",index?"display":"general");
        std::snprintf(out->group,sizeof out->group,"%s",index?"Display":"General");
        std::snprintf(out->name,sizeof out->name,"%s",index?"Advanced graphics":"Startup and input preferences");
        for(const auto& spec:PSXRecompV4::kExtraPlayerSettings)out->option_count+=!strcmp(spec.page,out->group);
        out->option_count+=index?1:6;
    } else {
        std::snprintf(out->package_id,sizeof out->package_id,"%s",packages[index-2]);
        std::snprintf(out->id,sizeof out->id,"feature");
        std::snprintf(out->name,sizeof out->name,"%s",packages[index-2]);out->option_count=1;
    }
    return 1;
}
static int option(void*,const char* package,const char* group,int index,RecompLauncherCModOption* out) {
    *out={};out->type=RECOMP_MOD_OPTION_TEXT;
    if(strcmp(package,"psx.runtime.settings")) {
        if(index)return 0;
        visited_packages.insert(package);
        std::snprintf(out->id,sizeof out->id,"preserved-option");
        std::snprintf(out->label,sizeof out->label,"Preserved graphics/audio preference");
        std::snprintf(out->value,sizeof out->value,"saved-value");return 1;
    }
    const char* page=!strcmp(group,"general")?"General":"Display";
    const PSXRecompV4::PlayerSettingSpec* found=nullptr;
    for(const auto& spec:PSXRecompV4::kExtraPlayerSettings)if(!strcmp(spec.page,page) && index--==0)found=&spec;
    std::string id;
    if(found){id=PSXRecompV4::player_setting_id(*found);std::snprintf(out->label,sizeof out->label,"%s",found->label);}
    else {
        static const char* general[]={"video.fast_boot","video.bios_hle","video.low_latency_input","memcard.dir","controller.mouse_sensitivity","controller.mouse_capture"};
        // The loop decremented once for every matching spec.
        if(!strcmp(page,"General")) {const int extra=index;if(extra<0 || extra>=6)return 0;id=general[extra];}
        else {if(index!=0)return 0;id="video.fov_scale";}
        std::snprintf(out->label,sizeof out->label,"%s",id.c_str());
    }
    std::snprintf(out->id,sizeof out->id,"%s",id.c_str());
    std::snprintf(out->value,sizeof out->value,"saved-value");
    (!strcmp(page,"General")?visited_general:visited_display).insert(id);
    return 1;
}
static int enable(void*,const char*,const char*,int){return 1;}
static int set(void*,const char*,const char*,const char*,const char*){return 1;}

static float probe_width=1280,probe_height=720;
static void frame(LauncherModel& model,const LauncherTheme& theme,bool draw=true) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
    ImGui::Begin("settings-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
    if(draw)draw_settings(&model,theme);
    ImGui::End();ImGui::Render();
}
// Rasterize the actual ImGui draw lists into an SDL software surface. No SDL
// window, OS input, OpenGL context or desktop capture exists in this path.
static void capture(const std::filesystem::path& path,float dpi) {
    auto* data=ImGui::GetDrawData();
    SDL_Surface* surface=SDL_CreateSurface((int)(probe_width*dpi),(int)(probe_height*dpi),SDL_PIXELFORMAT_RGBA32);
    assert(surface); SDL_Renderer* renderer=SDL_CreateSoftwareRenderer(surface); assert(renderer);
    unsigned char* atlas; int w,h; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas,&w,&h);
    SDL_Surface* font_surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_RGBA32,atlas,w*4);
    SDL_Texture* texture=SDL_CreateTextureFromSurface(renderer,font_surface); assert(texture);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(renderer,dpi,dpi);
    SDL_SetRenderDrawColor(renderer,10,13,20,255); SDL_RenderClear(renderer);
    for (int n=0;n<data->CmdListsCount;++n) {
        const auto* list=data->CmdLists[n]; std::vector<SDL_Vertex> vertices;
        for (const auto& v:list->VtxBuffer) {
            const auto c=ImGui::ColorConvertU32ToFloat4(v.col);
            vertices.push_back({{v.pos.x,v.pos.y},{c.x,c.y,c.z,c.w},{v.uv.x,v.uv.y}});
        }
        for (const auto& cmd:list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            SDL_Rect clip{(int)cmd.ClipRect.x,(int)cmd.ClipRect.y,(int)(cmd.ClipRect.z-cmd.ClipRect.x),(int)(cmd.ClipRect.w-cmd.ClipRect.y)};
            SDL_SetRenderClipRect(renderer,&clip);
            std::vector<int> indices;
            for (unsigned i=0;i<cmd.ElemCount;++i) indices.push_back(list->IdxBuffer[cmd.IdxOffset+i]+cmd.VtxOffset);
            assert(SDL_RenderGeometry(renderer,texture,vertices.data(),(int)vertices.size(),indices.data(),(int)indices.size()));
        }
    }
    SDL_RenderPresent(renderer);
    assert(stbi_write_png(path.u8string().c_str(),surface->w,surface->h,4,surface->pixels,surface->pitch));
    SDL_DestroyTexture(texture); SDL_DestroySurface(font_surface); SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface);
}
static void geometry(std::ofstream& out,const char* page,int width,float dpi) {
    int cards=0;
    for (auto* w:ImGui::GetCurrentContext()->Windows) {
        if (!w->Active || !(w->Flags&ImGuiWindowFlags_ChildWindow)) continue;
        const std::string name=w->Name;
        if (name.find("settings-matrix/")!=0) continue;
        if (name.find("hotkeys")!=std::string::npos) {
            assert(w->ScrollMax.x==0);
            assert(w->ContentSize.x<=w->Size.x-2*w->WindowPadding.x+1);
            continue;
        }
        ++cards;
        out<<page<<','<<width<<','<<dpi<<','<<name<<','<<w->Pos.x<<','<<w->Pos.y<<','<<w->Size.x<<','<<w->Size.y<<','<<w->ContentSize.x<<','<<w->ContentSize.y<<'\n';
        assert(w->ContentSize.x<=w->Size.x-2*w->WindowPadding.x+1);
        assert(w->ScrollMax.x==0);
        assert(w->Pos.x>=0 && w->Pos.x+w->Size.x<=width+1);
        if (name.find("Audio_")!=std::string::npos || name.find("/Audio")!=std::string::npos)
            assert(w->Size.y<=w->ContentSize.y+2*w->WindowPadding.y+3);
    }
    assert(cards==3); // A wrong window-name filter must not silently test zero cards.
}
static void render_matrix(LauncherModel& model,const LauncherTheme& theme,const char* folder) {
    std::filesystem::create_directories(folder);
    std::ofstream out(std::filesystem::path(folder)/"geometry.csv");
    out<<"page,width,dpi,window,x,y,w,h,content_w,content_h\n";
    std::snprintf(model.s.bios_path,sizeof(model.s.bios_path),"I:/Projects/Recompilations/PS1/Work/eagle/PS1B-529-pin-delivery-20261108/products/0.1.7/medievil/windows-x64/inputs/a-very-long-bios-folder/another-long-folder/SCPH1001.BIN");
    for (int width:{640,960,1280,1920}) for (float dpi:{1.f,1.5f,2.f}) {
        probe_width=(float)width;probe_height=800;
        auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(probe_width,probe_height);io.DisplayFramebufferScale=ImVec2(dpi,dpi);
        for (int page=0;page<2;++page) {
            for (int i=0;i<4;++i) {
                ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
                ImGui::Begin("settings-matrix",nullptr,ImGuiWindowFlags_NoSavedSettings);
                draw_psx_settings_page(&model,theme,page==1);
                ImGui::End(); ImGui::Render();
            }
            geometry(out,page?"Display":"General",width,dpi);
            char file[96];std::snprintf(file,sizeof file,"%s-%d-%.0f.png",page?"display":"general",width,dpi*100);
            capture(std::filesystem::path(folder)/file,dpi);
            auto* root=ImGui::FindWindowByName("settings-matrix");assert(root);
            // Scroll the actual owning window and prove the bottom remains reachable.
            for (int i=0;i<3;++i) {
                ImGui::NewFrame();ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
                ImGui::Begin("settings-matrix",nullptr,ImGuiWindowFlags_NoSavedSettings);
                ImGui::SetScrollY(root->ScrollMax.y);draw_psx_settings_page(&model,theme,page==1);
                ImGui::End();ImGui::Render();
            }
            assert(root->ScrollMax.y==0 || root->Scroll.y>0);
            std::snprintf(file,sizeof file,"%s-%d-%.0f-bottom.png",page?"display":"general",width,dpi*100);
            capture(std::filesystem::path(folder)/file,dpi);
            ImGui::NewFrame();ImGui::Begin("settings-matrix");ImGui::SetScrollY(0);ImGui::End();ImGui::Render();
        }
    }
}
int main() {
    RecompLauncherCModProvider provider{};
    provider.feature_count=features;provider.feature_get=feature;provider.feature_option_get=option;
    provider.feature_enable=enable;provider.feature_set_option=set;
    RecompLauncherCGameInfo game{};launcher_profile_apply("psx",&game);
    game.name="settings fixture";game.mods=&provider;game.has_bios=1;
    game.has_frame_interp=1;game.aspect_mask=7;game.has_render_pipeline=1;
    RecompLauncherCSettings settings{};settings.renderer=1;settings.frame_interp=1;settings.frame_interp_fps=120;
    settings.render_thread=1;settings.present_thread=1;settings.frame_generation=1;settings.vsync=RECOMP_LAUNCHER_VSYNC_VRR;
    LauncherModel model{};launcher_model_init(&model,&settings,&game,nullptr);
    const auto before=model.s;
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize=ImVec2(1280,720);io.DeltaTime=1.0f/60.0f;
    const auto theme=launcher_theme_psx();g_th=&theme;apply_theme_style(theme);
    ImFontConfig font; font.SizePixels=theme.font_body;
    if (const char* path=std::getenv("RUI_LAYOUT_FONT")) io.Fonts->AddFontFromFileTTF(path,theme.font_body,&font);
    else io.Fonts->AddFontDefault(&font);
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    frame(model,theme);frame(model,theme);
    assert(!visited_general.empty() && visited_display.empty());
    auto* window=ImGui::FindWindowByName("settings-probe");assert(window);
    ImGuiTabBar* bar=ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Settings tabs"));assert(bar && bar->Tabs.Size==2);
    const auto& display=bar->Tabs[1];const ImVec2 click(bar->BarRect.Min.x+display.Offset+display.Width/2,bar->BarRect.Min.y+bar->BarRect.GetHeight()/2);
    io.AddMousePosEvent(click.x,click.y);frame(model,theme);
    std::fprintf(stderr,"hover=%08x target=%08x window=%s mouse=%.1f,%.1f\n",
        ImGui::GetCurrentContext()->HoveredId,display.ID,
        ImGui::GetCurrentContext()->HoveredWindow?ImGui::GetCurrentContext()->HoveredWindow->Name:"none",io.MousePos.x,io.MousePos.y);
    io.AddMouseButtonEvent(0,true);frame(model,theme);
    io.AddMouseButtonEvent(0,false);frame(model,theme);frame(model,theme);
    std::fprintf(stderr,"Settings drawer general=%zu display=%zu selected=%08x target=%08x click=%.1f,%.1f scroll=%.1f\n",
        visited_general.size(),visited_display.size(),bar->SelectedTabId,display.ID,click.x,click.y,window->Scroll.y);
    assert(visited_general.size()+visited_display.size()==35);
    assert(visited_packages.size()==6);
    assert(visited_general.count("video.fast_boot") && visited_general.count("audio.buffer_ms"));
    assert(visited_display.count("video.pgxp_tolerance") && visited_display.count("video.fov_scale"));
    // The real Display drawer also emits all six previously gated controls.
    ImGui::NewFrame();ImGui::Begin("display-rows");ImGui::LogToBuffer();draw_display_controls(&model,theme);
    const std::string log=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();ImGui::End();ImGui::Render();
    for(const char* label:{"Render thread","Present thread","Smooth motion","Frame interpolation","Presentation target","Aspect ratio"})
        assert(log.find(label)!=std::string::npos);
    for(const char* package:packages)assert(launcher_settings_group(package));
    assert(!launcher_settings_group("third.party.game-plugin"));
    assert(!memcmp(&before,&model.s,sizeof before));
    frame(model,theme,false);frame(model,theme); // ImGui queues programmatic tab selection for the next frame.
    visited_general.clear();visited_display.clear();frame(model,theme);
    assert(!visited_general.empty() && visited_display.empty());
    if (const char* folder=std::getenv("RUI_LAYOUT_CAPTURE_DIR")) render_matrix(model,theme,folder);
    ImGui::DestroyContext();
    puts("PASS: production Settings drawer General default/reentry,Display selection,35 provider controls+6 real widgets,preserved values,built-in grouping,plugin boundary;CPU-only");
}
