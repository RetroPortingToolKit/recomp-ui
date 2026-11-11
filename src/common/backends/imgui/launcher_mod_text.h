#pragma once
#include "launcher_mod_text_path.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

struct RuiTextEdit { bool changed=false, browse=false; };
/* Shared by production and the headless real-ImGui fixture. Plain TEXT takes
 * the original input-only path; buttons never open an OS picker themselves. */
inline RuiTextEdit rui_mod_text_control(const char* value, char* next,
        size_t next_size, bool path) {
    static char edit[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    static ImGuiID owner=0;
    RuiTextEdit result;
    const ImGuiID id=ImGui::GetID("##text");
    if (owner!=id) std::snprintf(edit,sizeof(edit),"%s",value);
    const float spacing=ImGui::GetStyle().ItemSpacing.x;
    const float buttons=ImGui::CalcTextSize("Browse").x+ImGui::CalcTextSize("Clear").x+
        4*ImGui::GetStyle().FramePadding.x+spacing;
    const float available=ImGui::GetContentRegionAvail().x;
    const bool inline_buttons=path && available>=buttons+96+spacing;
    if (path) ImGui::SetNextItemWidth(std::max(1.0f,
        std::min(ImGui::CalcItemWidth(),available-(inline_buttons?buttons+spacing:0))));
    ImGui::InputText("##text",edit,sizeof(edit));
    if (ImGui::IsItemActivated()) owner=id;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::snprintf(next,next_size,"%s",edit); result.changed=true;
    }
    if (ImGui::IsItemDeactivated() && owner==id) owner=0;
    if (path) {
        if (inline_buttons) ImGui::SameLine();
        result.browse=ImGui::Button("Browse");
        if (available>=buttons) ImGui::SameLine();
        if (ImGui::Button("Clear")) {if(next_size) next[0]=0;result.changed=true;}
    }
    return result;
}
template<class Picker, class Completed>
bool rui_mod_text_browse(const RecompLauncherCModProvider* provider,
        const char* package,const char* feature,const char* option,
        const char* label,Picker pick,Completed completed,
        const char* pending_edit=nullptr) {
    RecompLauncherCModTextPathV1 info{};
    if (!recomp_launcher_get_mod_text_path_v1(provider,package,feature,option,&info)) return false;
    std::vector<std::string> patterns;
    for(int i=0;i<info.pattern_count;++i) patterns.emplace_back(info.patterns[i]);
    const std::string pkg=package,feat=feature?feature:"",opt=option;
    // Clicking Browse can also commit an edited text field. Apply that value
    // first: a synchronous picker selection must remain the final value, and
    // cancelling an asynchronous picker must still retain the typed edit.
    if (pending_edit) completed(launcher_mod_text_path_set(provider,package,
        feature,option,pending_edit)!=0);
    pick(label,std::move(patterns),info.filter_description,
        [provider,pkg,feat,opt,completed](const char* selected) {
            if(selected && selected[0]) completed(launcher_mod_text_path_set(provider,pkg.c_str(),
                feat.c_str(),opt.c_str(),selected)!=0);
        });
    return true;
}
inline bool rui_game_version_text() {
    const char* version=recomp_launcher_get_game_version();
    if (!version[0]) return false;
    ImGui::TextWrapped("Version %s",version);
    return true;
}
