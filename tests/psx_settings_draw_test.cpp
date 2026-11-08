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

static void frame(LauncherModel& model,const LauncherTheme& theme,bool draw=true) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(1280,720));
    ImGui::Begin("settings-probe",nullptr,ImGuiWindowFlags_NoSavedSettings);
    if(draw)draw_settings(&model,theme);
    ImGui::End();ImGui::Render();
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
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    const auto theme=launcher_theme_default();g_th=&theme;
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
    ImGui::DestroyContext();
    puts("PASS: production Settings drawer General default/reentry,Display selection,35 provider controls+6 real widgets,preserved values,built-in grouping,plugin boundary;CPU-only");
}
