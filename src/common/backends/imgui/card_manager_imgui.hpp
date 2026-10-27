// SPDX-License-Identifier: MIT
// Embedded manager UI. File dialogs run after presentation via process_picker().
#pragma once
#include "card_manager_actions.hpp"
namespace card_manager_ui {
#ifdef CARD_MANAGER_TEST
inline std::vector<ImVec4> action_bounds;
inline float action_limit=0;
inline bool clipped_footer=false;
inline std::vector<ImVec4> source_bounds;
inline ImVec4 drop_bounds[2];
inline ImVec4 context_copy_bounds;
#endif
inline std::string title(const cardview::Save& s){return s.title;}
inline std::string fit(std::string text,float width) {
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;
    while(!text.empty() && ImGui::CalcTextSize((text+"...").c_str()).x>width) {
        size_t at=text.size()-1;while(at && (uint8_t(text[at])&0xc0)==0x80)--at;text.resize(at);
    }
    return text.empty()?"...":text+"...";
}
inline void icon(const cardview::Save& s,float side) {
    auto p=ImGui::GetCursorScreenPos();auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(p,ImVec2(p.x+side,p.y+side),ImGui::GetColorU32(ImGuiCol_FrameBg),4);
    if(!s.icons.empty()) {
        double dt=s.icons.size()==2?16.0/50:11.0/50;
        const auto& frame=s.icons[size_t(ImGui::GetTime()/dt)%s.icons.size()];float cell=side/16;
        for(int i=0;i<256;++i)if(frame[i]>>24) {
            auto c=frame[i];ImVec2 a(p.x+(i%16)*cell,p.y+(i/16)*cell);
            dl->AddRectFilled(a,ImVec2(a.x+cell,a.y+cell),IM_COL32(c&255,(c>>8)&255,(c>>16)&255,c>>24));
        }
    }ImGui::Dummy(ImVec2(side,side));
}
inline float button_width(const char* label){return ImGui::CalcTextSize(label).x+2*ImGui::GetStyle().FramePadding.x;}
inline int action_rows(float width,int slot) {
    const char* labels[]={"Export","Import",slot?"Copy to Card 1":"Copy to Card 2","Delete","Details"};
    int rows=1;float used=0,gap=ImGui::GetStyle().ItemSpacing.x;
    for(const char* label:labels){float w=button_width(label);if(used && used+gap+w>width){++rows;used=0;}used+=(used?gap:0)+w;}return rows;
}
inline bool action(const char* label,bool enabled,bool& first,float width) {
    float w=std::min(width,button_width(label));if(!first){ImGui::SameLine();if(ImGui::GetContentRegionAvail().x<w)ImGui::NewLine();}first=false;
    ImGui::BeginDisabled(!enabled);bool hit=ImGui::Button(label,ImVec2(w,0));ImGui::EndDisabled();
#ifdef CARD_MANAGER_TEST
    auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();action_bounds.push_back(ImVec4(a.x,a.y,b.x,b.y));
    if(b.y>action_limit+.5f)clipped_footer=true;
#endif
    return hit;
}
inline bool refresh_button() {
    bool hit=ImGui::Button("   Refresh");auto a=ImGui::GetItemRectMin();float r=ImGui::GetFontSize()*.27f;
    ImVec2 c(a.x+ImGui::GetStyle().FramePadding.x+r,a.y+ImGui::GetFrameHeight()/2);auto* dl=ImGui::GetWindowDrawList();
    dl->PathArcTo(c,r,-2.1f,2.5f,16);dl->PathStroke(ImGui::GetColorU32(ImGuiCol_Text),0,1.5f);
    dl->AddTriangleFilled(ImVec2(c.x+r,c.y-r),ImVec2(c.x+r+2,c.y+1),ImVec2(c.x-1,c.y-r),ImGui::GetColorU32(ImGuiCol_Text));
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Read the card again. No backup is created.");return hit;
}

inline void accept_drop(int target) {
    if(ImGui::BeginDragDropTarget()) {
        if(const ImGuiPayload* p=ImGui::AcceptDragDropPayload("PS1_CARD_SAVE")) {
            if(p->IsDelivery() && p->DataSize==sizeof(cardview::Drag)) {
                cardview::Drag ref;std::memcpy(&ref,p->Data,sizeof(ref));copy_entry(ref,target);
            }
        }
        ImGui::EndDragDropTarget();
    }
}
inline void toolbar(LauncherModel*,int slot) {
    auto& v=cards[slot];float width=ImGui::GetContentRegionAvail().x;bool first=true;
    // Shared flow helper keeps all four labels usable on narrow card columns.
    // Toolbar bounds are part of the scrollable card body, not the footer.
#ifdef CARD_MANAGER_TEST
    float limit=action_limit;action_limit=1e9f;
#endif
    if(action("Open Card",true,first,width))request_open(slot);
    if(!first){ImGui::SameLine();if(ImGui::GetContentRegionAvail().x<button_width("   Refresh"))ImGui::NewLine();}
    if(refresh_button())request_refresh(slot);first=false;
    if(action("Save Card",v.dirty() && writable(v),first,width))request_save(slot);
    if(action("Save Card As",editable(v),first,width))request_save(slot,true);
#ifdef CARD_MANAGER_TEST
    action_limit=limit;
#endif
}
inline void pane(LauncherModel* m,int slot,ImVec2 size) {
    ImGui::PushID(slot);ImGui::BeginChild("card_pane",size,ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar);
    auto& v=cards[slot];float width=ImGui::GetContentRegionAvail().x;
    float reserve=action_rows(width,slot)*ImGui::GetFrameHeightWithSpacing()+2*ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("card_body",ImVec2(0,std::max(1.0f,ImGui::GetContentRegionAvail().y-reserve)));
    ImGui::Text("Memory Card %d%s",slot+1,v.dirty()?"  * Unsaved changes":"");
    auto name=v.snapshot.path.filename().u8string();auto short_name=fit(name.empty()?"No card selected":name,ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(short_name.c_str());if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",v.snapshot.path.u8string().c_str());
    toolbar(m,slot);
    if(!v.error.empty())ImGui::TextWrapped("Unavailable: %s",v.error.c_str());
    else if(v.loaded()) {
        ImGui::TextColored(v.card.valid()?ImVec4(.35f,.85f,.55f,1):ImVec4(1,.4f,.35f,1),"%s | %d / 15 blocks free",v.card.valid()?"Valid card":"Damaged card",v.card.free_blocks);
        if(v.snapshot.readonly)ImGui::TextDisabled("Disk file is read-only: use Save Card As.");
        for(const auto& e:v.card.errors)ImGui::TextWrapped("%s",e.c_str());
        for(const auto& e:v.card.warnings)ImGui::TextWrapped("%s",e.c_str());
        float gap=3,cell=std::max(2.0f,std::min(22.0f,(ImGui::GetContentRegionAvail().x-14*gap)/15));
        for(int b=0;b<15;++b) {
            if(b)ImGui::SameLine(0,gap);auto a=ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(a,ImVec2(a.x+cell,a.y+cell),v.card.occupied[b]?IM_COL32(60,160,130,255):ImGui::GetColorU32(ImGuiCol_FrameBg),3);
            ImGui::Dummy(ImVec2(cell,cell));if(ImGui::IsItemHovered())ImGui::SetTooltip("Block %d: %s",b+1,v.card.occupied[b]?"Used":"Free / deleted");
        }
        float table_h=std::max(90.0f,ImGui::GetContentRegionAvail().y-ImGui::GetTextLineHeightWithSpacing());
        if(ImGui::BeginTable("saves",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,ImVec2(0,table_h))) {
            ImGui::TableSetupColumn("Icon",ImGuiTableColumnFlags_WidthFixed,38);
            ImGui::TableSetupColumn("Save / PS1 file",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Blocks",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize("Blocks").x);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            for(size_t n=0;n<v.card.saves.size();++n) {
                const auto& save=v.card.saves[n];ImGui::PushID(int(n));ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);icon(save,32);
                ImGui::TableSetColumnIndex(1);auto pos=ImGui::GetCursorScreenPos();float cw=ImGui::GetContentRegionAvail().x;
                float row_h=std::max(36.0f,2*ImGui::GetTextLineHeightWithSpacing());
                if(ImGui::Selectable("##save",v.selected==int(n),ImGuiSelectableFlags_SpanAllColumns,ImVec2(0,row_h)))v.selected=int(n);
#ifdef CARD_MANAGER_TEST
                if(slot==0 && n==0){auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();source_bounds.push_back(ImVec4(a.x,a.y,b.x,b.y));}
#endif
                if(editable(v) && ImGui::BeginDragDropSource()) {
                    cardview::Drag ref{slot,n,v.revision};ImGui::SetDragDropPayload("PS1_CARD_SAVE",&ref,sizeof(ref));
                    ImGui::TextWrapped("%s",title(save).c_str());ImGui::Text("Copy %zu block(s)",save.blocks.size());ImGui::EndDragDropSource();
                }
                if(ImGui::BeginPopupContextItem("save_actions")) {
                    v.selected=int(n);
                    bool copy_hit=ImGui::MenuItem(slot?"Copy to Card 1":"Copy to Card 2",nullptr,false,editable(v));
#ifdef CARD_MANAGER_TEST
                    auto ca=ImGui::GetItemRectMin(),cb=ImGui::GetItemRectMax();context_copy_bounds=ImVec4(ca.x,ca.y,cb.x,cb.y);
#endif
                    if(copy_hit)copy_selected(m,slot);
                    if(ImGui::MenuItem("Export",nullptr,false,editable(v)))request_export(slot);
                    if(ImGui::MenuItem("Delete from draft",nullptr,false,editable(v)))request_delete(slot);
                    if(ImGui::MenuItem("Details")){details_slot=slot;details=true;}
                    ImGui::EndPopup();
                }
                auto label=fit(title(save),cw),id=fit(save.name,cw);
                ImVec4 clip(pos.x,pos.y,pos.x+cw,pos.y+row_h);auto* dl=ImGui::GetWindowDrawList();
                dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),pos,ImGui::GetColorU32(ImGuiCol_Text),label.c_str(),nullptr,0,&clip);
                ImVec2 second(pos.x,pos.y+ImGui::GetTextLineHeightWithSpacing());
                dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),second,ImGui::GetColorU32(ImGuiCol_TextDisabled),id.c_str(),nullptr,0,&clip);
                ImGui::TableSetColumnIndex(2);ImGui::Text("%d",int(save.blocks.size()));ImGui::PopID();
            }ImGui::EndTable();
        }
        if(v.card.saves.empty() && v.card.valid())ImGui::TextDisabled("No saves. Drop a save from the other card here.");
    }
    ImGui::EndChild();
#ifdef CARD_MANAGER_TEST
    auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();drop_bounds[slot]=ImVec4(lo.x,lo.y,hi.x,hi.y);
    action_limit=ImGui::GetWindowPos().y+ImGui::GetWindowSize().y-ImGui::GetStyle().WindowPadding.y;
#endif
    if(editable(v))accept_drop(slot);
    bool first=true,has=selected(v);
    if(action("Export",has && editable(v),first,width))request_export(slot);
    if(action("Import",editable(v),first,width))request_import(slot);
    if(action(slot?"Copy to Card 1":"Copy to Card 2",has && editable(v),first,width))copy_selected(m,slot);
    if(action("Delete",has && editable(v),first,width))request_delete(slot);
    if(action("Details",has,first,width)){details_slot=slot;details=true;}
    ImGui::EndChild();ImGui::PopID();
}
inline void draw_details() {
    if(!details)return;ImGui::OpenPopup("Save details");auto d=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(700.0f,d.x-40),std::min(570.0f,d.y-40)),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("Save details",&details)) {
        const auto& v=cards[details_slot];
        if(selected(v)) {
            const auto& s=v.card.saves[v.selected];ImGui::TextWrapped("%s",title(s).c_str());ImGui::Separator();
            ImGui::Text("Memory Card: %d",details_slot+1);ImGui::TextWrapped("Path: %s",v.snapshot.path.u8string().c_str());
            ImGui::Text("PS1 file: %s",s.name.c_str());ImGui::Text("Game ID: %s",s.game_id.c_str());ImGui::Text("Region: %s",s.region.c_str());
            ImGui::Text("Size: %zu bytes | %zu blocks | %zu icon frames",s.bytes,s.blocks.size(),s.icons.size());
            ImGui::Text("Structure: %s",v.card.editable()?"Valid chain and directory":"Card has errors / unsupported features");
            ImGui::TextDisabled("PS1 cards do not store per-save modification dates.");
            if(ImGui::CollapsingHeader("Raw Shift-JIS title (unchanged)"))ImGui::TextWrapped("%s",s.raw_title.c_str());
        }
        if(ImGui::Button("Close"))details=false;ImGui::EndPopup();
    }
}

inline void dialogs() {
    auto display=ImGui::GetIO().DisplaySize;
    if(save_prompt.active && !save_prompt.shown){ImGui::OpenPopup("Save Card confirmation");save_prompt.shown=true;}
    if(save_prompt.shown) {
        ImGui::SetNextWindowSize(ImVec2(std::min(600.0f,display.x-32),0),ImGuiCond_Appearing);
        if(ImGui::BeginPopupModal("Save Card confirmation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            auto& p=save_prompt;const auto& d=cards[p.slot];
            ImGui::Text("Save Memory Card %d?",p.slot+1);
            ImGui::TextWrapped("%s",(p.output.empty()?d.snapshot.path:p.output).u8string().c_str());
            ImGui::TextWrapped("Write this complete card draft after validation and verified backups? Existing Save As targets are never overwritten.");
            if(ImGui::Button("Cancel"))p.active=false;
            ImGui::SameLine();if(ImGui::Button("Save"))confirm_save();
            if(!p.active){p.shown=false;ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
    }
    if(delete_prompt.active && !delete_prompt.shown){ImGui::OpenPopup("Delete save from draft");delete_prompt.shown=true;}
    if(delete_prompt.shown && ImGui::BeginPopupModal("Delete save from draft",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        auto& p=delete_prompt;ImGui::Text("Delete from Memory Card %d draft?",p.ref.source+1);
        ImGui::TextWrapped("%s\n%s",p.title.c_str(),p.name.c_str());
        ImGui::TextWrapped("The disk file stays unchanged until Save Card.");
        if(ImGui::Button("Cancel"))p.active=false;
        ImGui::SameLine();if(ImGui::Button("Delete"))confirm_delete();
        if(!p.active){p.shown=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    if(guard.next!=Next::none && !guard.shown){ImGui::OpenPopup("Unsaved card changes");guard.shown=true;}
    if(guard.shown) {
        ImGui::SetNextWindowSize(ImVec2(std::min(650.0f,display.x-32),0),ImGuiCond_Appearing);
        if(ImGui::BeginPopupModal("Unsaved card changes",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            if(guard.next==Next::none){guard.shown=false;ImGui::CloseCurrentPopup();}
            else {
                int slot=guard.slot;ImGui::Text("Memory Card %d has unsaved changes.",slot+1);
                ImGui::TextWrapped("%s",cards[slot].snapshot.path.u8string().c_str());
                ImGui::TextWrapped("Save the draft before closing or opening another card?");
                if(ImGui::Button("Keep Editing")){guard.next=Next::none;guard.shown=false;ImGui::CloseCurrentPopup();}
                ImGui::SameLine();if(ImGui::Button("Discard Changes"))discard_guard();
                ImGui::SameLine();ImGui::BeginDisabled(!writable(cards[slot]));
                if(ImGui::Button("Save and Continue")){if(save_now(slot))continue_guard();}
                ImGui::EndDisabled();
                if(ImGui::Button("Save As and Continue"))pick(cardpicker::Kind::save,cards[slot].snapshot.path,[slot](const auto& path){if(save_now(slot,path))continue_guard();});
            }
            ImGui::EndPopup();
        }
    }
}
inline int columns(float,float){return 2;}
inline void draw(LauncherModel* m) {
    if(!open)return;
#ifdef CARD_MANAGER_TEST
    action_bounds.clear();source_bounds.clear();context_copy_bounds=ImVec4();clipped_footer=false;
#endif
    ImGui::OpenPopup("Memory Card Manager");auto display=ImGui::GetIO().DisplaySize;float scale=ImGui::GetFontSize()/16;
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(620.0f*scale,display.x-24),std::min(330.0f*scale,display.y-24)),ImVec2(display.x-24,display.y-24));
    ImGui::SetNextWindowSize(ImVec2(std::min(1280.0f*scale,display.x-24),std::min(840.0f*scale,display.y-24)),ImGuiCond_Appearing);
    bool visible=true;
    if(!ImGui::BeginPopupModal("Memory Card Manager",&visible,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar)) {
        if(!visible)request_close();return;
    }
    ImGui::TextWrapped("Drag or copy saves between card drafts. Disk files change only when you choose Save Card.");
    if(ImGui::CollapsingHeader("Backup locations (required before Save)")) {
        ImGui::TextWrapped("Choose two folders on distinct local NTFS volumes. Opening cards creates no backups. Existing backup versions are verified and reused.");
        for(int n=0;n<2;++n) {
            ImGui::PushID(n);const char* path=api->backup_path(service,n);
            ImGui::TextWrapped("%s: %s",n?"Secondary":"Primary",path && *path?path:"Not configured");
            if(ImGui::Button("Choose backup folder"))ui_pick_folder(m,"Memory card backup folder",[n](const char* p){try{command(n?RECOMP_CARD_BACKUP_SECONDARY:RECOMP_CARD_BACKUP_PRIMARY,0,0,0,std::filesystem::u8path(p));}catch(const std::exception& e){notice=e.what();}});
            ImGui::PopID();
        }
    }
    float h=ImGui::GetContentRegionAvail().y-ImGui::GetTextLineHeightWithSpacing()-ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("cards_canvas",ImVec2(0,std::max(70.0f,h)),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    float gap=ImGui::GetStyle().ItemSpacing.x,avail=ImGui::GetContentRegionAvail().x;
    float width=std::max(avail,2*280.0f*scale+gap);
    float height=ImGui::GetContentRegionAvail().y-(width>avail?ImGui::GetStyle().ScrollbarSize:0);
    pane(m,0,ImVec2((width-gap)/2,std::max(60.0f,height)));ImGui::SameLine(0,gap);
    pane(m,1,ImVec2((width-gap)/2,std::max(60.0f,height)));ImGui::EndChild();
    if(!notice.empty()){auto line=fit(notice,ImGui::GetContentRegionAvail().x);ImGui::TextUnformatted(line.c_str());if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",notice.c_str());}
    draw_details();dialogs();
    if(!open)ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
} // namespace card_manager_ui
