// SPDX-License-Identifier: MIT
static void host_field_changed(LauncherModel* m,int i) {
    if (s_host_controls->field_changed) {
        const char* path=s_host_controls->field_changed(s_host_controls->ctx,i);
        if(path && path[0]) launcher_model_set_rom(m,path);
    }
}
static void draw_host_system(LauncherModel* m) {
    const auto* h=s_host_controls;
    if(h->system_title) ImGui::TextUnformatted(h->system_title);
    if(h->system_help) ImGui::TextWrapped("%s",h->system_help);
    for(int i=0;i<h->field_count && i<16;++i){
        auto* f=&h->fields[i];
        if(!f->label || !f->value || f->capacity<2 || f->capacity>65536)continue;
        ImGui::PushID(i);
        if(f->help) ImGui::TextWrapped("%s",f->help);
        ImGui::BeginDisabled(f->locked!=0);
        if(ImGui::InputText(f->label,f->value,f->capacity,ImGuiInputTextFlags_EnterReturnsTrue)) host_field_changed(m,i);
        if(ImGui::Button("Choose folder")){
            PickRequest req;req.mode=PickMode::Folder;req.title=f->label;req.start_path=f->value;
            req.on_pick=[f,m,i](const char* p){if(p && std::strlen(p)<f->capacity){std::snprintf(f->value,f->capacity,"%s",p);host_field_changed(m,i);}};
            ui_pick(m,std::move(req));
        }
        ImGui::EndDisabled();ImGui::PopID();
    }
}
