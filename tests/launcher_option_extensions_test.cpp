/* Real widgets/API with an injected picker; no platform backend or dialogs. */
#include "backends/imgui/launcher_mod_text.h"
#include "imgui_internal.h"
#include <cassert>
#include <fstream>

static std::string store_file;
struct Store { std::string value;int package_calls=0,feature_calls=0;bool reject=false; };
static void persist(const Store& s) { std::ofstream file(store_file);file<<s.value;assert(file.good()); }
static int set_package(void* p,const char*,const char*,const char* value) {
    auto& s=*static_cast<Store*>(p);++s.package_calls;if(s.reject)return 0;s.value=value;persist(s);return 1;
}
static int set_feature(void* p,const char*,const char*,const char*,const char* value) {
    auto& s=*static_cast<Store*>(p);++s.feature_calls;if(s.reject)return 0;s.value=value;persist(s);return 1;
}
static void cli() {
    char* args[]={const_cast<char*>("app"),const_cast<char*>("--rom"),const_cast<char*>("file"),
        const_cast<char*>("--no-launcher"),const_cast<char*>("--launcher"),
        const_cast<char*>("--skip-launcher"),const_cast<char*>("--"),
        const_cast<char*>("--launcher"),nullptr};
    int argc=8;int saved=0;
    assert(recomp_launcher_parse_cli_args(&argc,args)==RECOMP_LAUNCHER_CLI_SKIP);
    assert(argc==5 && !strcmp(args[1],"--rom") && !strcmp(args[2],"file") &&
        !strcmp(args[3],"--") && !strcmp(args[4],"--launcher") && !args[5]);
    assert(recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_SKIP,saved) && saved==0);
    assert(!recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_SHOW,1));
    assert(recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_DEFAULT,1));
    assert(!recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_DEFAULT,0));
    char* show[]={const_cast<char*>("app"),const_cast<char*>("--launcher"),nullptr};argc=2;
    assert(recomp_launcher_parse_cli_args(&argc,show)==RECOMP_LAUNCHER_CLI_SHOW && argc==1);
    assert(recomp_launcher_parse_cli_args(nullptr,nullptr)==RECOMP_LAUNCHER_CLI_DEFAULT);
    char* malformed[]={const_cast<char*>("app"),const_cast<char*>("--no-launcher"),nullptr};argc=3;
    assert(recomp_launcher_parse_cli_args(&argc,malformed)==RECOMP_LAUNCHER_CLI_DEFAULT &&
        argc==3 && !strcmp(malformed[1],"--no-launcher"));
}
static void metadata() {
    Store store;RecompLauncherCModProvider provider{},other{};
    provider.ctx=&store;provider.set_option=set_package;provider.feature_set_option=set_feature;
    RecompLauncherCModTextPathV1 info{},out{};strcpy(info.filter_description,"Seed patch");
    info.pattern_count=1;strcpy(info.patterns[0],"*.apsm");
    char pkg[]="example.mod",feature[]="randomizer",option[]="seed";
    assert(recomp_launcher_register_mod_text_path_v1(&provider,pkg,feature,option,&info));
    pkg[0]='X';feature[0]='X';option[0]='X';info.patterns[0][0]='X';
    assert(recomp_launcher_get_mod_text_path_v1(&provider,"example.mod","randomizer","seed",&out));
    assert(!strcmp(out.patterns[0],"*.apsm") && !strcmp(out.filter_description,"Seed patch"));
    assert(!recomp_launcher_get_mod_text_path_v1(&other,"example.mod","randomizer","seed",&info));
    assert(!recomp_launcher_get_mod_text_path_v1(&provider,"example.mod","other","seed",&info));
    std::function<void(const char*)> pending;bool completed=false,stored=false;
    auto picker=[&](const char* title,std::vector<std::string> patterns,const char* desc,
                   std::function<void(const char*)> cb) {
        assert(!strcmp(title,"Seed") && patterns==std::vector<std::string>{"*.apsm"} &&
            !strcmp(desc,"Seed patch"));pending=std::move(cb);
    };
    assert(rui_mod_text_browse(&provider,"example.mod","randomizer","seed","Seed",picker,
        [&](bool result){completed=true;stored=result;}));
    pending(nullptr);assert(!completed && store.feature_calls==0); // cancel
    pending("");assert(!completed && store.feature_calls==0);
    pending("test dir/seed.apsm");assert(completed && stored && store.feature_calls==1 && store.value=="test dir/seed.apsm");
    Store reloaded;{ std::ifstream file(store_file);std::getline(file,reloaded.value); }
    assert(reloaded.value=="test dir/seed.apsm"); // real provider-owned disk roundtrip
    // A text edit deactivated by Browse must commit before a synchronous
    // native selection, not overwrite the selected filename afterwards.
    int edited_calls=store.feature_calls;
    auto synchronous_picker=[&](const char*,std::vector<std::string>,const char*,
                                std::function<void(const char*)> cb) {
        assert(store.value=="typed-before-browse.apsm" && store.feature_calls==edited_calls+1);
        cb("selected-after-edit.apsm");
    };
    assert(rui_mod_text_browse(&provider,"example.mod","randomizer","seed","Seed",
        synchronous_picker,[&](bool result){assert(result);},"typed-before-browse.apsm"));
    assert(store.value=="selected-after-edit.apsm" && store.feature_calls==edited_calls+2);
    { std::ifstream file(store_file);std::getline(file,reloaded.value); }
    assert(reloaded.value=="selected-after-edit.apsm");
    // The fallback picker is deferred; cancellation retains the typed edit.
    assert(rui_mod_text_browse(&provider,"example.mod","randomizer","seed","Seed",picker,
        [&](bool result){assert(result);},"typed-before-cancel.apsm"));
    edited_calls=store.feature_calls;pending(nullptr);
    assert(store.value=="typed-before-cancel.apsm" && store.feature_calls==edited_calls);
    assert(launcher_mod_text_path_set(&provider,"example.mod","randomizer","seed","") && store.value.empty());
    assert(launcher_mod_text_path_set(&provider,"example.mod",nullptr,"seed","typed.apsm") && store.package_calls==1);
    store.reject=true;assert(!launcher_mod_text_path_set(&provider,"example.mod",nullptr,"seed","rejected.apsm") && store.value=="typed.apsm");
    const int calls=store.package_calls;
    assert(!launcher_mod_text_path_set(&provider,"example.mod",nullptr,"seed",std::string(RECOMP_LAUNCHER_MOD_VALUE_MAX,'x').c_str()));
    assert(store.package_calls==calls); // no truncated filename submitted
    out.pattern_count=9;assert(!recomp_launcher_register_mod_text_path_v1(&provider,"example.mod","randomizer","seed",&out));
    assert(recomp_launcher_get_mod_text_path_v1(&provider,"example.mod","randomizer","seed",&out) && out.pattern_count==1);
    assert(recomp_launcher_register_mod_text_path_v1(&provider,"example.mod",nullptr,"seed",&out));
    assert(recomp_launcher_register_mod_text_path_v1(&provider,"example.mod",nullptr,"seed",nullptr));
    assert(!recomp_launcher_get_mod_text_path_v1(&provider,"example.mod",nullptr,"seed",&out));
    recomp_launcher_unregister_mod_text_paths(&provider);
    assert(!recomp_launcher_get_mod_text_path_v1(&provider,"example.mod","randomizer","seed",&out));
}
static ImVec2 clear_at,browse_at;static float width=600;static bool path=true,disabled=false;
static RuiTextEdit frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({width,240});
    ImGui::Begin("options",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize);
    ImGui::SetNextItemWidth(230);char next[RECOMP_LAUNCHER_MOD_VALUE_MAX]="unchanged";
    ImGui::BeginDisabled(disabled);auto result=rui_mod_text_control("test dir/seed.apsm",next,sizeof(next),path);
    if(path) {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();clear_at={(a.x+b.x)/2,(a.y+b.y)/2};
        const float browse_w=ImGui::CalcTextSize("Browse").x+2*ImGui::GetStyle().FramePadding.x;
        browse_at={a.x-ImGui::GetStyle().ItemSpacing.x-browse_w/2,clear_at.y};
        assert(b.x<=ImGui::GetWindowContentRegionMax().x+ImGui::GetWindowPos().x+1);
    }
    ImGui::EndDisabled();if(result.changed)assert(!next[0]);
    ImGui::End();ImGui::Render();return result;
}
static RuiTextEdit click(ImVec2 p) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(p.x,p.y);frame();
    io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);return frame();
}
int main(int argc,char** argv) {
    assert(argc==2);store_file=std::string(argv[1])+"/saved-path.txt";
    cli();metadata();char version[]="v1.2.3";recomp_launcher_set_game_version(version);version[0]='X';
    assert(!strcmp(recomp_launcher_get_game_version(),"v1.2.3"));
    recomp_launcher_set_game_version(recomp_launcher_get_game_version());
    assert(!strcmp(recomp_launcher_get_game_version(),"v1.2.3"));
    recomp_launcher_set_game_version(std::string(256,'v').c_str());
    assert(strlen(recomp_launcher_get_game_version())==127);
    recomp_launcher_set_game_version("v1.2.3");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1000,700};io.DeltaTime=1.0f/60;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    ImGui::NewFrame();ImGui::Begin("version");ImGui::LogToBuffer();assert(rui_game_version_text());
    assert(strstr(GImGui->LogBuffer.c_str(),"Version v1.2.3"));ImGui::LogFinish();
    recomp_launcher_set_game_version(nullptr);assert(!rui_game_version_text());ImGui::End();ImGui::Render();
    frame();frame();assert(click(browse_at).browse);assert(click(clear_at).changed);
    disabled=true;frame();assert(!click(browse_at).browse && !click(clear_at).changed);disabled=false;
    width=200;frame();frame();assert(click(clear_at).changed);
    path=false;ImGui::NewFrame();ImGui::Begin("plain");char next[128]="same";ImGui::LogToBuffer();
    auto result=rui_mod_text_control("ordinary text",next,sizeof(next),false);
    assert(!result.browse && !result.changed && !strcmp(next,"same") && !strstr(GImGui->LogBuffer.c_str(),"Browse"));
    ImGui::LogFinish();ImGui::End();ImGui::Render();ImGui::DestroyContext();
    puts("PASS one-shot launcher arguments, copied version, opt-in path metadata/setters and real widgets; no dialogs");
}
