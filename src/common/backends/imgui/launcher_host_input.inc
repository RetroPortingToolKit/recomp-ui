// SPDX-License-Identifier: MIT
// Thin PSP device handoff on the shared Controller card/page. Uses the shared
// live SDL list; GUID + device path, never an ephemeral instance ID, is saved.
static const char* host_pad_path(uint32_t id) {
#if defined(LNG_SDL3)
    auto* pad=SDL_GetGamepadFromID((SDL_JoystickID)id);
    return pad ? SDL_GetGamepadPath(pad) : nullptr;
#else
    auto* pad=SDL_GameControllerFromInstanceID((SDL_JoystickID)id);
    return pad ? SDL_GameControllerPath(pad) : nullptr;
#endif
}
static bool host_input_available() { return s_host_controls && s_host_controls->get_input && s_host_controls->set_input; }
static RecompLauncherCHostInput host_input() {
    RecompLauncherCHostInput value{};
    if(host_input_available()) s_host_controls->get_input(s_host_controls->ctx,&value);
    return value;
}
static bool host_selected_pad_live() {
    const auto value=host_input();const auto* h=&value;
    if(!h || h->source!=4)return h && h->source!=3;
    for(int i=0;i<g_pad_count;++i) {
        const char* path=host_pad_path(g_pads[i].id);
        if(path && !std::strcmp(path,h->controller_path) && !std::strcmp(g_pads[i].guid,h->controller_guid))return true;
    }
    return false;
}
static const char* host_source_label() {
    static RecompLauncherCHostInput value;value=host_input();const auto* h=&value;
    if(h->source==4)return h->controller_name[0]?h->controller_name:"Selected SDL controller";
    if(h->source==1)return "Keyboard";
    if(h->source==3)return "None";
    if(h->source==2) {
        static char label[64];std::snprintf(label,sizeof(label),"XInput controller %d",h->gamepad_index+1);return label;
    }
    return "Automatic (keyboard + XInput)";
}
static void host_choose_source(LauncherModel* m,int source,int index=0) {
    auto value=host_input();auto* h=&value;h->source=source;
    if(source==2)h->gamepad_index=index;
    const int kind=source==3?0:source==1?1:2;
    if(s_host_controls->set_input(s_host_controls->ctx,h)) launcher_model_set_source(m,0,kind,0,nullptr,nullptr);
}
static void draw_host_sources(LauncherModel* m) {
    auto value=host_input();auto* h=&value;
    if(ImGui::Selectable("Automatic (keyboard + XInput)",h->source==0))host_choose_source(m,0);
    if(ImGui::Selectable("Keyboard",h->source==1))host_choose_source(m,1);
    if(ImGui::Selectable("None",h->source==3))host_choose_source(m,3);
    ImGui::Separator();ImGui::TextDisabled("Connected controllers");
    int offered=0;
    for(int i=0;i<g_pad_count;++i) {
        const char* path=host_pad_path(g_pads[i].id);
        if(!path || !path[0] || std::strlen(path)>1024 || std::strlen(g_pads[i].guid)!=32 || std::strlen(g_pads[i].name)>255)continue;
        const bool selected=h->source==4 && !std::strcmp(path,h->controller_path) && !std::strcmp(g_pads[i].guid,h->controller_guid);
        ImGui::PushID(path);
        if(ImGui::Selectable(g_pads[i].name,selected)) {
            h->source=4;
            std::snprintf(h->controller_guid,sizeof(h->controller_guid),"%s",g_pads[i].guid);
            std::snprintf(h->controller_path,sizeof(h->controller_path),"%s",path);
            std::snprintf(h->controller_name,sizeof(h->controller_name),"%s",g_pads[i].name);
            if(s_host_controls->set_input(s_host_controls->ctx,h)) launcher_model_set_source(m,0,2,g_pads[i].id,g_pads[i].name,g_pads[i].guid);
        }
        ImGui::PopID();++offered;
    }
    if(!offered)ImGui::TextDisabled("No SDL controller connected");
    if(!s_host_controls->legacy_xinput)return;
    ImGui::Separator();ImGui::TextDisabled("Existing XInput selections");
    for(int i=0;i<4;++i) {
        char label[64];std::snprintf(label,sizeof(label),"XInput controller %d",i+1);
        if(ImGui::Selectable(label,h->source==2 && h->gamepad_index==i))host_choose_source(m,2,i);
    }
}
static void draw_host_controller(LauncherModel* m,const LauncherTheme& th) {
    if(begin_panel("host_controller",0)) {
        ImGui::TextUnformatted("Gameplay controller");
        ImGui::SetNextItemWidth(px(320));
        if(ImGui::BeginCombo("##host_controller_source",host_source_label())) {draw_host_sources(m);ImGui::EndCombo();}
        if(host_input().source==4 && !host_selected_pad_live())ImGui::TextWrapped("Selected controller is disconnected. Another controller will not replace it.");
        ImGui::TextWrapped("Choose a connected controller by name. Only that device controls gameplay. Keyboard and Automatic are separate choices.");
        if(s_host_controls->input_help)ImGui::TextWrapped("%s",s_host_controls->input_help);
    }end_panel();
}
