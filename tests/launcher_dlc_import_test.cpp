// SPDX-License-Identifier: MIT
#include "launcher_model.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
extern "C" void launcher_binds_set_zapper(int,int) {}

struct Importer {
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false},release{false};
    std::string path;
};
static int failures;
static void check(bool ok,const char* what) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);++failures;}
}
static int import_pack(void* context,const char* path,char* error,size_t capacity) {
    auto& importer=*static_cast<Importer*>(context);
    ++importer.calls;importer.path=path;importer.entered=true;
    while(!importer.release)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(importer.path=="unsupported.wad") {
        std::snprintf(error,capacity,"Unsupported DLC title");return 0;
    }
    return 1;
}
static void finish(LauncherModel& model) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(model.dlc_importing && std::chrono::steady_clock::now()<deadline) {
        launcher_model_poll_prepare_disc(&model);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!model.dlc_importing && !model.setup_preparing,"import worker completes");
}
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    RecompLauncherCSettings settings{};
    RecompLauncherCGameInfo game{};
    LauncherModel model;
    launcher_model_init(&model,&settings,&game,argv[1]);
    launcher_model_import_dlc(&model,"ignored.wad");
    check(!model.import_dlc_cb && !model.dlc_importing && !model.setup_preparing,
          "zero-initialized hosts have no DLC capability or import side effects");

    Importer importer;
    const char* filters[]={"*.wad"};
    game.import_dlc=import_pack;game.dlc_ctx=&importer;
    game.dlc_path="prior.wad";game.dlc_status="Previously installed";
    game.dlc_patterns=filters;game.num_dlc_patterns=1;
    const RecompLauncherCDisc discs[]={{1,"Disc 1",argv[1]},{2,"Disc 2",argv[2]}};
    game.discs=discs;game.num_discs=2;
    launcher_model_init(&model,&settings,&game,argv[1]);
    launcher_model_select_disc(&model,1);
    check(launcher_model_disc_selected(&model)==1,"fixture selects a real second disc");
    check(std::strcmp(model.dlc_path,"prior.wad")==0 &&
          std::strcmp(model.dlc_status,"Previously installed")==0,
          "host persisted pack and installed status seed the row");
    check(model.dlc_patterns==filters && model.num_dlc_patterns==1,"host controls file filter");
    const std::string rom=model.rom_full;
    const auto selected=model.disc_selected;
    const auto disc_index=model.s.disc_index;
    const bool could_launch=launcher_model_can_launch(&model);
    launcher_model_import_dlc(&model,nullptr);
    launcher_model_import_dlc(&model,"");
    check(importer.calls==0 && !model.dlc_importing,"picker cancellation does not invoke host");
    launcher_model_import_dlc(&model,"pack with spaces.wad");
    check(model.dlc_importing && model.setup_preparing,"pack import is asynchronous");
    check(!launcher_model_can_launch(&model),"Play waits for pack installation");
    launcher_model_import_dlc(&model,"overlapping.wad");
    importer.release=true;finish(model);
    check(importer.calls==1 && importer.path=="pack with spaces.wad","one import receives the complete selected path");
    check(!model.dlc_failed && std::strcmp(model.dlc_path,"pack with spaces.wad")==0,
          "successful import adopts only the DLC pack path");
    check(rom==model.rom_full && selected==model.disc_selected && disc_index==model.s.disc_index,
          "DLC import preserves the selected boot image and disc");
    check(launcher_model_can_launch(&model)==could_launch,"optional DLC does not become a launch prerequisite");
    launcher_model_import_dlc(&model,"unsupported.wad");finish(model);
    check(importer.calls==2 && model.dlc_failed &&
          std::strcmp(model.dlc_status,"Unsupported DLC title")==0,
          "host validation error remains visible");
    check(std::strcmp(model.dlc_path,"pack with spaces.wad")==0 && rom==model.rom_full,
          "failed import preserves the installed pack and boot image");
    const std::string too_long(600,'x');launcher_model_import_dlc(&model,too_long.c_str());
    check(importer.calls==2 && model.dlc_failed && !model.dlc_importing,
          "oversized paths reject before host invocation");
    model.in_session=true;launcher_model_import_dlc(&model,"another.wad");finish(model);
    check(!model.dlc_failed && std::strstr(model.dlc_status,"Restart")!=nullptr,
          "in-session installation states its restart requirement");
    return failures?1:0;
}
