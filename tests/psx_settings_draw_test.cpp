/* Render the production Settings and Configure drawers into CPU-only ImGui
 * frames. No SDL window, GL context, game, focus or operating-system input is
 * created. */
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

static float probe_width=1280,probe_height=800;
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
        if (name.find("settings-probe/")!=0) continue;
        ++cards;
        out<<page<<','<<width<<','<<dpi<<','<<name<<','<<w->Pos.x<<','<<w->Pos.y<<','<<w->Size.x<<','<<w->Size.y<<','<<w->ContentSize.x<<','<<w->ContentSize.y<<'\n';
        // A card never overhangs the page it sits in.
        assert(w->Pos.x>=-1 && w->Pos.x+w->Size.x<=width+2);
    }
    // A page with no card at all would make the checks above vacuous.
    assert(cards>0);
}
/* Select a tab in `bar` the same way a click queues it, so the page below is
 * rendered through the real tab-item path without synthesising OS input. */
static void click_bar(LauncherModel& model,const LauncherTheme& theme,ImGuiTabBar* bar,int index) {
    assert(bar && index<(int)bar->Tabs.Size);
    const ImGuiID id=bar->Tabs[index].ID;
    bar->NextSelectedTabId=id;
    for (int i=0;i<3;++i) frame(model,theme);
    assert(bar->SelectedTabId==id);
}
static void click_tab(LauncherModel& model,const LauncherTheme& theme,const char* bar_key,int index) {
    auto* root=ImGui::FindWindowByName("settings-probe"); assert(root);
    click_bar(model,theme,
              ImGui::GetCurrentContext()->TabBars.GetByKey(root->GetID(bar_key)),index);
}
/* The Display page's sub-tabs live inside the Display tab item's ID scope, so
 * their bar is keyed by that tab's ID rather than by the window's. */
static ImGuiTabBar* find_display_sub_tabs() {
    auto* root=ImGui::FindWindowByName("settings-probe"); assert(root);
    auto& g=*ImGui::GetCurrentContext();
    ImGuiTabBar* outer=g.TabBars.GetByKey(root->GetID("Settings tabs"));
    if (!outer || outer->Tabs.Size<2) return nullptr;
    return g.TabBars.GetByKey(ImHashStr("PSX display tabs",0,outer->Tabs[1].ID));
}
static void render_matrix(LauncherModel& model,const LauncherTheme& theme,const char* folder) {
    std::filesystem::create_directories(folder);
    std::ofstream out(std::filesystem::path(folder)/"geometry.csv");
    out<<"page,width,dpi,window,x,y,w,h,content_w,content_h\n";
    std::snprintf(model.s.bios_path,sizeof(model.s.bios_path),"D:/Games/PlayStation/Recompilations/a-title-with-a-long-name/releases/0.1.7/medievil/windows-x64/inputs/a-very-long-bios-folder/another-long-folder/SCPH1001.BIN");
    for (int width:{640,960,1280,1920}) for (float dpi:{1.f,1.5f,2.f}) {
        probe_width=(float)width;probe_height=800;
        auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(probe_width,probe_height);io.DisplayFramebufferScale=ImVec2(dpi,dpi);
        static const char* names[5]={"general","display","pgxp","display-presentation","hotkeys"};
        // The full-screen launcher is the layout these pages are designed for.
        model.s.fullscreen=1;
        for (int page=0;page<5;++page) {
            for (int i=0;i<3;++i) {      // warm-up frames settle tab-bar layout
                ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
                ImGui::Begin("settings-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
                draw_settings(&model,theme);
                ImGui::End(); ImGui::Render();
            }
            frame(model,theme);
            click_tab(model,theme,"Settings tabs",page==0?0:(page==4?2:1));
            if (page>=2 && page<=3) {
                ImGuiTabBar* sub=find_display_sub_tabs(); assert(sub);
                click_bar(model,theme,sub,page==2?1:2);
            }
            geometry(out,names[page],width,dpi);
            char file[96];std::snprintf(file,sizeof file,"%s-%d-%.0f.png",names[page],width,dpi*100);
            capture(std::filesystem::path(folder)/file,dpi);
            auto* root=ImGui::FindWindowByName("settings-probe");assert(root);
            for (int i=0;i<3;++i) {
                ImGui::NewFrame();ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
                ImGui::Begin("settings-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
                ImGui::SetScrollY(root->ScrollMax.y);draw_settings(&model,theme);
                ImGui::End();ImGui::Render();
            }
            assert(root->ScrollMax.y==0 || root->Scroll.y>0);
            std::snprintf(file,sizeof file,"%s-%d-%.0f-bottom.png",names[page],width,dpi*100);
            capture(std::filesystem::path(folder)/file,dpi);
            ImGui::NewFrame();ImGui::Begin("settings-probe");ImGui::SetScrollY(0);ImGui::End();ImGui::Render();
        }
        // One windowed capture of the Display page, where the two columns stack.
        if (width==1280) {
            model.s.fullscreen=0;
            for (int i=0;i<3;++i) {
                ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
                ImGui::Begin("settings-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
                draw_settings(&model,theme);
                ImGui::End(); ImGui::Render();
            }
            frame(model,theme);
            click_tab(model,theme,"Settings tabs",1);
            capture(std::filesystem::path(folder)/"display-window-1280.png",1.f);
            model.s.fullscreen=1;
        }
    }
}

/* The Configure page, rendered the same way, so the two-column layout and the
 * player tab header can be checked without a running game. */
static void render_config_matrix(LauncherModel& model,const LauncherTheme& theme,const char* folder) {
    std::filesystem::create_directories(folder);
    for (int fullscreen:{1,0}) for (int seat:{0,1}) {
        model.s.fullscreen=fullscreen;
        model.cfg_player=seat;
        model.view=LNG_VIEW_CONTROLLER;
        probe_width=1280;probe_height=1150;
        auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(probe_width,probe_height);io.DisplayFramebufferScale=ImVec2(1,1);
        for (int i=0;i<3;++i) {
            ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(probe_width,probe_height));
            ImGui::Begin("config-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
            draw_controller_config_view(&model,theme);
            ImGui::End(); ImGui::Render();
        }
        char file[96];
        std::snprintf(file,sizeof file,"configure-%s-player%d.png",fullscreen?"fullscreen":"window",seat+1);
        capture(std::filesystem::path(folder)/file,1.f);
    }
    model.s.fullscreen=0; model.cfg_player=0;
}
int main() {
    RecompLauncherCModProvider provider{};
    provider.feature_count=features;provider.feature_get=feature;provider.feature_option_get=option;
    provider.feature_enable=enable;provider.feature_set_option=set;
    RecompLauncherCGameInfo game{};launcher_profile_apply("psx",&game);
    game.name="settings fixture";game.mods=&provider;game.has_bios=1;
    game.has_frame_interp=1;game.aspect_mask=7;game.has_render_pipeline=1;
    game.num_players=2;
    // Host shortcuts: the Configure page's right column needs them to exist.
    static const char* const kAssist[]={"Rewind","Fast forward","Quick save"};
    game.settings_bindings=1;game.assist_binding_labels=kAssist;game.assist_binding_count=3;
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
    // Three tabs: General, Display, Hotkeys.
    ImGuiTabBar* bar=ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Settings tabs"));
    assert(bar && bar->Tabs.Size==3);
    // Row labels are what the player reads: the host label for fast boot is
    // relabelled to "Skip BIOS" here, and the two HLE rows stay behind the
    // collapsed Advanced subsection (so their rows are not drawn at all).
    ImGui::NewFrame();ImGui::Begin("general-log");ImGui::LogToBuffer();
    draw_settings(&model,theme);
    const std::string glog=ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();ImGui::End();ImGui::Render();
    assert(glog.find("Skip BIOS")!=std::string::npos);
    assert(glog.find("video.fast_boot")==std::string::npos);
    assert(glog.find("video.bios_hle")==std::string::npos);
    assert(glog.find("video.low_latency_input")==std::string::npos);
    click_tab(model,theme,"Settings tabs",1);
    std::fprintf(stderr,"general=%zu display=%zu\n",visited_general.size(),visited_display.size());
    // The Display page carries its own three sub-tabs: Display, PGXP,
    // Presentation. Each one is a row of two half-width cards, so every page
    // lists exactly its own packages and nothing else.
    ImGuiTabBar* sub=find_display_sub_tabs();
    assert(sub && sub->Tabs.Size==3);
    click_bar(model,theme,sub,0);
    visited_packages.clear();frame(model,theme);
    assert(visited_packages.count("psx.enhancement.image-filtering"));
    assert(visited_packages.count("psx.enhancement.crt-filter")==0);
    assert(visited_packages.count("psx.enhancement.pgxp")==0);
    click_bar(model,theme,sub,1);
    visited_packages.clear();visited_display.clear();frame(model,theme);
    assert(visited_packages.count("psx.enhancement.pgxp"));
    assert(visited_packages.count("psx.enhancement.image-filtering")==0);
    assert(visited_display.count("video.pgxp_tolerance"));
    click_bar(model,theme,sub,2);
    visited_packages.clear();frame(model,theme);
    assert(visited_packages.count("psx.enhancement.crt-filter"));
    assert(visited_packages.count("psx.enhancement.image-filtering")==0);
    assert(visited_packages.count("psx.enhancement.pgxp")==0);
    click_bar(model,theme,sub,0);
    click_tab(model,theme,"Settings tabs",2);
    // The Hotkeys page is text, not options: prove the page drew its catalog.
    ImGui::NewFrame();ImGui::Begin("hotkeys-log");ImGui::LogToBuffer();
    draw_hotkeys_controls(&model,theme);
    const std::string log=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();ImGui::End();ImGui::Render();
    assert(log.find(launcher_hotkey_name(LNG_HK_REWIND))!=std::string::npos);
    for(const char* package:packages)assert(launcher_settings_group(package));
    assert(!launcher_settings_group("third.party.game-plugin"));
    assert(!memcmp(&before,&model.s,sizeof before));
    frame(model,theme,false);frame(model,theme); // ImGui queues programmatic tab selection for the next frame.
    visited_general.clear();visited_display.clear();frame(model,theme);
    assert(!visited_general.empty() && visited_display.empty());
    if (const char* folder=std::getenv("RUI_LAYOUT_CAPTURE_DIR")) {
        render_matrix(model,theme,folder);
        render_config_matrix(model,theme,folder);
    }
    ImGui::DestroyContext();
    puts("PASS: production Settings drawer (General/Display+3 sub-tabs/Hotkeys), collapsed Advanced rows, Configure two-column and window layouts;CPU-only");
}
