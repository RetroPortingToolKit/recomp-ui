// SPDX-License-Identifier: MIT
// Manager actions operate on drafts. Only confirmed Save / Save As write files.
#pragma once
#include "card_manager_support.hpp"
namespace card_manager_ui {
using View=cardview::View;
inline const RecompCardManager* api=nullptr;
inline void* service=nullptr;
inline bool available(const LauncherModel* m){return m->card_manager && m->card_manager->abi_version==RECOMP_CARD_MANAGER_ABI;}
inline View cards[2];
inline void sync() {
    if(!service)return;
    for(int slot=0;slot<2;++slot) {
        auto* p=api->view(service,slot);if(!p)continue;auto& v=cards[slot];
        v.snapshot.path=std::filesystem::u8path(p->path?p->path:"");v.snapshot.readonly=!p->writable;
        v.error=p->error?p->error:"";v.loaded_flag=p->loaded!=0;v.dirty_flag=p->dirty!=0;v.writable_flag=p->writable!=0;v.revision=p->revision;
        v.card={};v.card.valid_flag=p->valid!=0;v.card.editable_flag=p->editable!=0;v.card.free_blocks=p->free_blocks;
        if(p->diagnostics && *p->diagnostics)v.card.errors.push_back(p->diagnostics);
        for(int n=0;n<15;++n)v.card.occupied[n]=(p->used_blocks & (1<<n))!=0;
        for(size_t n=0;n<p->count;++n) {
            const auto& s=p->saves[n];cardview::Save out;out.name=s.name;out.title=s.title;out.region=s.region;out.game_id=s.game_id;out.raw_title=s.raw_title;out.bytes=s.bytes;out.blocks.resize(s.blocks);
            out.icons.resize(s.icon_frames);for(unsigned f=0;f<s.icon_frames;++f)std::copy(s.icons[f],s.icons[f]+256,out.icons[f].begin());v.card.saves.push_back(std::move(out));
        }
        if(v.selected>=int(v.card.saves.size()))v.selected=-1;
    }
}
inline void command(RecompCardOp op,int slot,size_t entry=0,int destination=0,const std::filesystem::path& path={},bool confirmed=false,uint64_t revision=UINT64_MAX) {
    if(!service)throw std::runtime_error("No card service");
    std::string u8=path.u8string();char error[1024]{};
    RecompCardCommand c{op,slot,destination,confirmed?1:0,entry,revision==UINT64_MAX?cards[slot].revision:revision,u8.c_str()};
    if(!api->command(service,&c,error,sizeof(error)))throw std::runtime_error(error);
    sync();
}
inline void shutdown();

inline bool open=false,details=false,exit_ready=false;
inline int details_slot=0;
inline std::string notice;
struct Picker {bool active=false;cardpicker::Request request;std::function<void(const std::filesystem::path&)> done;};
inline Picker picker;
#ifdef CARD_MANAGER_TEST
inline std::function<cardpicker::Result(const cardpicker::Request&)> picker_driver=[](const auto& r){return cardpicker::choose(r);};
#endif
inline void pick(cardpicker::Kind kind,const std::filesystem::path& start,std::function<void(const std::filesystem::path&)> done) {
    if(picker.active)throw std::runtime_error("A file selection is already pending");
    picker={true,{kind,start},std::move(done)};
}
inline void process_picker() {
    if(!picker.active)return;auto job=std::move(picker);picker=Picker{};
    try {
#ifdef CARD_MANAGER_TEST
        auto result=picker_driver(job.request);
#else
#if defined(_WIN32)
        auto result=cardpicker::choose(job.request);
#else
        cardpicker::Result result{{},"No writable card service is available on this platform."};
#endif
#endif
        if(!result.error.empty())notice=result.error;
        else if(!result.path.empty())job.done(result.path);
    }catch(const std::exception& e){notice=e.what();}
}
inline bool has_dirty(){return cards[0].dirty() || cards[1].dirty();}
inline bool selected(const View& v){return v.selected>=0 && v.selected<int(v.card.saves.size());}
inline bool editable(const View& v){return v.loaded() && v.card.editable();}
inline bool writable(const View& v){return editable(v) && v.writable_flag;}
inline bool load(int slot,const std::filesystem::path& path) {
    try {command(RECOMP_CARD_OPEN,slot,0,0,path);cards[slot].selected=-1;return true;}
    catch(const std::exception& e){notice=e.what();if(!cards[slot].loaded()){cards[slot].error=notice;cards[slot].snapshot.path=path;}return false;}
}
inline void start(LauncherModel* m) {
    if(open || !available(m) || m->in_session)return;notice.clear();details=false;exit_ready=false;
    shutdown();api=m->card_manager;service=api->create();if(!service){notice="Cannot create card service";return;}
    for(int n=0;n<2;++n)load(n,std::filesystem::u8path(m->s.memcard_path[n]));open=true;
}
enum class Next {none,open_card,refresh,close_manager,close_app};
struct Guard {Next next=Next::none;int slot=0;std::filesystem::path path;std::function<void()> after;bool shown=false;};
inline Guard guard;
inline int first_dirty(){for(int n=0;n<2;++n)if(cards[n].dirty())return n;return -1;}
inline void continue_guard() {
    if(guard.next==Next::close_manager || guard.next==Next::close_app) {
        int n=first_dirty();if(n>=0){guard.slot=n;return;}
        exit_ready=guard.next==Next::close_app;open=false;details=false;guard.next=Next::none;return;
    }
    if(guard.next==Next::open_card || guard.next==Next::refresh) {
        auto after=guard.after;int slot=guard.slot;auto path=guard.path;
        if(load(slot,path)){guard.next=Next::none;if(after)after();}
    }
}
inline void request_replace(int slot,Next next,const std::filesystem::path& path,std::function<void()> after={}) {
    guard={next,slot,path,std::move(after),false};if(!cards[slot].dirty())continue_guard();
}
inline void request_open(int slot,std::function<void()> after={}) {
    pick(cardpicker::Kind::card,cards[slot].snapshot.path,[slot,after](const auto& path){request_replace(slot,Next::open_card,path,after);});
}
inline void request_refresh(int slot){request_replace(slot,Next::refresh,cards[slot].snapshot.path);}
inline void request_close(bool application=false) {
    guard={application?Next::close_app:Next::close_manager,0,{}, {},false};open=true;continue_guard();
}
inline void discard_guard() {
    try {command(RECOMP_CARD_DISCARD,guard.slot,0,0,{},true);cards[guard.slot].selected=-1;continue_guard();}catch(const std::exception& e){notice=e.what();}
}
inline bool save_now(int slot,const std::filesystem::path& output={}) {
    try {
        command(output.empty()?RECOMP_CARD_SAVE:RECOMP_CARD_SAVE_AS,slot,0,0,output,true);
        notice="Memory Card "+std::to_string(slot+1)+" saved. Backups verified.";return true;
    }catch(const std::exception& e){notice=e.what();return false;}
}
struct SavePrompt {bool active=false,shown=false;int slot=0;uint64_t revision=0;std::filesystem::path output;};
inline SavePrompt save_prompt;
inline void request_save(int slot,bool as=false) {
    if(as)pick(cardpicker::Kind::save,cards[slot].snapshot.path,[slot](const auto& path){save_prompt={true,false,slot,cards[slot].revision,path};});
    else save_prompt={true,false,slot,cards[slot].revision,{}};
}
inline void confirm_save() {
    auto p=save_prompt;
    if(cards[p.slot].revision!=p.revision)notice="Draft changed after confirmation was opened; review and save again.";
    else save_now(p.slot,p.output);
    save_prompt.active=false;
}
inline void copy_entry(const cardview::Drag& ref,int to) {
    try {command(RECOMP_CARD_COPY,ref.source,ref.entry,to,{},false,ref.revision);notice="Save copied to the other card draft. Use Save Card to write it.";}
    catch(const std::exception& e){notice=e.what();}
}
inline void copy_selected(LauncherModel*,int from) {
    if(!selected(cards[from]))return;
    cardview::Drag ref{from,size_t(cards[from].selected),cards[from].revision};int to=1-from;
    if(!cards[to].loaded())request_open(to,[ref,to](){copy_entry(ref,to);});else copy_entry(ref,to);
}
struct DeletePrompt {bool active=false,shown=false;cardview::Drag ref;std::string name,title;};
inline DeletePrompt delete_prompt;
inline void request_delete(int slot) {
    if(!selected(cards[slot]))return;
    const auto& s=cards[slot].card.saves[cards[slot].selected];
    delete_prompt={true,false,{slot,size_t(cards[slot].selected),cards[slot].revision},s.name,s.title};
}
inline void confirm_delete() {
    try {
        auto ref=delete_prompt.ref;auto& d=cards[ref.source];
        if(d.revision!=ref.revision)throw std::runtime_error("Save selection changed; select it again");
        command(RECOMP_CARD_DELETE,ref.source,ref.entry,0,{},true,ref.revision);notice="Save removed from the draft. Use Save Card to write changes.";
    }catch(const std::exception& e){notice=e.what();}delete_prompt.active=false;
}
inline void request_import(int slot) {
    pick(cardpicker::Kind::import_mcs,cards[slot].snapshot.path.parent_path(),[slot](const auto& path){
        command(RECOMP_CARD_IMPORT,slot,0,0,path);notice="Save imported into the draft. Use Save Card to write changes.";
    });
}
inline void request_export(int slot) {
    size_t n=size_t(cards[slot].selected);auto d=cards[slot];
    auto initial=d.snapshot.path.parent_path()/std::filesystem::u8path(d.card.saves[n].name+".mcs");
    pick(cardpicker::Kind::export_mcs,initial,[slot,d,n](const auto& path){command(RECOMP_CARD_EXPORT,slot,n,0,path,false,d.revision);notice="MCS exported. No card changed and no backup created.";});
}
inline void shutdown() {
    if(service)api->destroy(service);
    service=nullptr;api=nullptr;cards[0]=View{};cards[1]=View{};
    open=false;details=false;exit_ready=false;notice.clear();picker=Picker{};
    guard=Guard{};save_prompt=SavePrompt{};delete_prompt=DeletePrompt{};
}
} // namespace card_manager_ui
