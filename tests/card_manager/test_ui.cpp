// Pointer events target only this headless ImGui context, never the OS desktop.
#define CARD_MANAGER_TEST
#include "card_manager_support.hpp"
#include "imgui.h"
#include <iostream>
#include <fstream>
struct LauncherModel {struct {char memcard_path[2][4096]{};}s;const RecompCardManager* card_manager=nullptr;bool in_session=false;};
static void ui_pick_folder(LauncherModel*,const char*,std::function<void(const char*)>) {}
#include "card_manager_imgui.hpp"
namespace fixture {
struct Service {RecompCardView v[2]{};RecompCardSave saves[2][2]{};bool exists[2]={true,false};unsigned copies=0;bool confirmed_save=false;};
static Service* running;
static const RecompCardManager api={RECOMP_CARD_MANAGER_ABI,
 []()->void*{auto* p=new Service;running=p;for(int n=0;n<2;++n){p->v[n].path=n?"target.mcd":"source.mcd";p->v[n].error="";p->v[n].diagnostics="";p->v[n].saves=p->saves[n];p->saves[n][0].name="SYNTHETIC-SAVE";p->saves[n][0].title="Synthetic Save A";p->saves[n][0].game_id="TEST-00000";p->saves[n][0].region="Synthetic fixture";p->saves[n][0].raw_title="41";p->saves[n][0].blocks=3;p->saves[n][0].bytes=24576;p->saves[n][0].icon_frames=1;for(int y=0;y<16;++y)for(int x=0;x<16;++x)p->saves[n][0].icons[0][y*16+x]=(x+y)%2?0xffdfa13f:0xff467cba;}return p;},
 [](void* p){delete static_cast<Service*>(p);running=nullptr;},
 [](void* p,int n)->const RecompCardView*{auto& s=*static_cast<Service*>(p);auto& v=s.v[n];v.count=s.exists[n]?1:0;v.used_blocks=s.exists[n]?7:0;v.free_blocks=s.exists[n]?12:15;return &v;},
 [](void* p,const RecompCardCommand* c,char* err,size_t cap)->int{auto& s=*static_cast<Service*>(p);auto& v=s.v[c->slot];if(c->op==RECOMP_CARD_OPEN){v.loaded=v.valid=v.editable=v.writable=1;v.dirty=0;++v.revision;return 1;}
 if(c->revision!=v.revision){std::snprintf(err,cap,"Stale draft");return 0;}
 if(c->op==RECOMP_CARD_COPY){if(c->destination!=1 || c->entry!=0)return 0;auto& dst=s.v[1];dst.dirty=1;++dst.revision;s.exists[1]=true;++s.copies;return 1;}
 if(c->op==RECOMP_CARD_DELETE){if(!c->confirmed)return 0;v.dirty=1;++v.revision;s.exists[c->slot]=false;return 1;}
 if(c->op==RECOMP_CARD_DISCARD){v.dirty=0;++v.revision;return 1;}
 if(c->op==RECOMP_CARD_SAVE || c->op==RECOMP_CARD_SAVE_AS){if(!c->confirmed)return 0;s.confirmed_save=true;v.dirty=0;++v.revision;return 1;}return 1;},
 [](void*,int)->const char*{return "Not configured";}
};
}
// Optional reproducible software-rendered illustration of actual ImGui draw data.
// The font atlas comes from ImGui's bundled MIT font; all save content is synthetic.
static void image(const char* path) {
 auto* data=ImGui::GetDrawData();int w=int(data->DisplaySize.x),h=int(data->DisplaySize.y);
 unsigned char* tex;int tw,th;ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex,&tw,&th);
 std::vector<unsigned char> pixels(size_t(w)*h*3,25);
 auto edge=[](ImVec2 a,ImVec2 b,ImVec2 p){return (p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x);};
 for(int l=0;l<data->CmdListsCount;++l){auto* list=data->CmdLists[l];for(const auto& cmd:list->CmdBuffer){if(cmd.UserCallback)continue;for(unsigned i=0;i<cmd.ElemCount;i+=3){
  const auto& a=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+i]+cmd.VtxOffset];const auto& b=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+i+1]+cmd.VtxOffset];const auto& c=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+i+2]+cmd.VtxOffset];float area=edge(a.pos,b.pos,c.pos);if(area==0)continue;
  int x0=std::max(0,int(std::max(cmd.ClipRect.x,std::floor(std::min({a.pos.x,b.pos.x,c.pos.x}))))),x1=std::min(w,int(std::min(cmd.ClipRect.z,std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x})))));
  int y0=std::max(0,int(std::max(cmd.ClipRect.y,std::floor(std::min({a.pos.y,b.pos.y,c.pos.y}))))),y1=std::min(h,int(std::min(cmd.ClipRect.w,std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y})))));
  for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){ImVec2 p(float(x)+.5f,float(y)+.5f);float u=edge(b.pos,c.pos,p)/area,v=edge(c.pos,a.pos,p)/area,z=1-u-v;if(u<0 || v<0 || z<0)continue;
   float tx=u*a.uv.x+v*b.uv.x+z*c.uv.x,ty=u*a.uv.y+v*b.uv.y+z*c.uv.y;int ti=(std::clamp(int(ty*th),0,th-1)*tw+std::clamp(int(tx*tw),0,tw-1))*4;
   float alpha=(u*((a.col>>24)&255)+v*((b.col>>24)&255)+z*((c.col>>24)&255))/255.0f*tex[ti+3]/255.0f;
   for(int k=0;k<3;++k){float col=(u*((a.col>>(k*8))&255)+v*((b.col>>(k*8))&255)+z*((c.col>>(k*8))&255))*tex[ti+k]/255.0f;auto& dest=pixels[(size_t(y)*w+x)*3+k];dest=uint8_t(col*alpha+dest*(1-alpha));}
  }
 }}}
 std::ofstream out(path,std::ios::binary);out<<"P6\n"<<w<<" "<<h<<"\n255\n";out.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
}
int main(int argc,char** argv)try {
 LauncherModel model;model.card_manager=&fixture::api;std::strcpy(model.s.memcard_path[0],"source.mcd");std::strcpy(model.s.memcard_path[1],"target.mcd");
 model.in_session=true;card_manager_ui::start(&model);if(card_manager_ui::open)throw std::runtime_error("In-session editing was allowed");model.in_session=false;card_manager_ui::start(&model);
 ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigInputTrickleEventQueue=false;io.DisplaySize={1280,900};io.DeltaTime=1.0f/60;
 io.Fonts->AddFontDefault();unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
 auto frame=[&](){ImGui::NewFrame();card_manager_ui::draw(&model);ImGui::Render();};for(int i=0;i<4;++i)frame();
 auto source=card_manager_ui::source_bounds.at(0),target=card_manager_ui::drop_bounds[1];ImVec2 s(source.x+90,(source.y+source.w)/2),t((target.x+target.z)/2,(target.y+target.w)/2);
 io.AddMousePosEvent(s.x,s.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMousePosEvent(s.x+20,s.y);frame();io.AddMousePosEvent(t.x,t.y);frame();frame();io.AddMouseButtonEvent(0,false);frame();frame();
 if(fixture::running->copies!=1 || !card_manager_ui::cards[1].dirty() || fixture::running->confirmed_save)throw std::runtime_error("Drag did not copy to unsaved draft");
 auto button=card_manager_ui::action_bounds.at(9);io.AddMousePosEvent((button.x+button.z)/2,(button.y+button.w)/2);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
 if(!card_manager_ui::save_prompt.active || fixture::running->confirmed_save)throw std::runtime_error("Save did not wait for confirmation");card_manager_ui::save_prompt.active=false;frame();
 card_manager_ui::picker_driver=[](const auto&){return cardpicker::Result{};};card_manager_ui::request_open(1);card_manager_ui::process_picker();if(!card_manager_ui::cards[1].dirty())throw std::runtime_error("Cancelled picker lost draft");
 card_manager_ui::request_refresh(1);if(card_manager_ui::guard.next!=card_manager_ui::Next::refresh)throw std::runtime_error("Refresh lost dirty guard");card_manager_ui::guard.next=card_manager_ui::Next::none;
 card_manager_ui::request_close(true);if(card_manager_ui::exit_ready)throw std::runtime_error("Exit discarded dirty draft");card_manager_ui::guard.next=card_manager_ui::Next::none;

 // Click the real context-menu Copy item through the same service adapter.
 fixture::running->exists[1]=false;fixture::running->v[1].dirty=0;card_manager_ui::sync();frame();
 auto row=card_manager_ui::source_bounds.at(0);io.AddMousePosEvent(row.x+90,(row.y+row.w)/2);frame();io.AddMouseButtonEvent(1,true);frame();io.AddMouseButtonEvent(1,false);frame();frame();
 auto item=card_manager_ui::context_copy_bounds;if(item.z<=item.x)throw std::runtime_error("Context copy menu missing");
 io.AddMousePosEvent((item.x+item.z)/2,(item.y+item.w)/2);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
 if(fixture::running->copies!=2 || !card_manager_ui::cards[1].dirty())throw std::runtime_error("Context copy did not reach host");
 card_manager_ui::request_save(1);card_manager_ui::confirm_save();if(!fixture::running->confirmed_save || card_manager_ui::cards[1].dirty())throw std::runtime_error("Confirmed Save did not synchronize view");
 // A fresh copy is left unsaved for the screenshot and exit-guard coverage.
 fixture::running->exists[1]=false;card_manager_ui::sync();card_manager_ui::copy_entry({0,0,card_manager_ui::cards[0].revision},1);
 for(float scale:{1.f,1.5f,2.f})for(ImVec2 size:{ImVec2(640,360),ImVec2(960,540),ImVec2(1280,720)}){io.DisplaySize=size;io.FontGlobalScale=scale;ImGui::SetWindowSize("Memory Card Manager",{size.x-24,size.y-24});ImGui::SetWindowPos("Memory Card Manager",{12,12});for(int i=0;i<3;++i)frame();if(card_manager_ui::action_bounds.size()!=16 || card_manager_ui::clipped_footer)throw std::runtime_error("Two-card toolbar clipped");}
 io.FontGlobalScale=1;io.DisplaySize={1280,900};ImGui::SetWindowSize("Memory Card Manager",{1200,780});ImGui::SetWindowPos("Memory Card Manager",{40,40});io.AddMousePosEvent(-100,-100);for(int i=0;i<3;++i)frame();if(argc>1)image(argv[1]);
 card_manager_ui::shutdown();ImGui::DestroyContext();std::cout<<"PASS shared UI: service gating, drag delivery, Save confirmation, context copy, picker cancellation, dirty Refresh/exit, two columns at 9 size/scales\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
