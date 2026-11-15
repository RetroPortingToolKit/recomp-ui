/* Run the production script parser; only the model/platform seams are stubs. */
#include "launcher_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mutations, failures;
void launcher_model_set_view(LauncherModel* m, LngView view) { ++mutations; m->view = view; }
void launcher_model_begin_capture(LauncherModel* m, int button) { ++mutations; m->capture_btn = button; }
void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey hotkey) { (void)m; (void)hotkey; ++mutations; }
void launcher_model_import_dlc(LauncherModel* m, const char* path) {
    ++mutations;snprintf(m->dlc_path,sizeof(m->dlc_path),"%s",path);
}
void launcher_model_start_prepare_disc(LauncherModel* m,const char* path) {
    ++mutations;snprintf(m->rom_full,sizeof m->rom_full,"%s",path);
}
bool launcher_model_set_peripheral(LauncherModel* m,int slot,const RecompLauncherCPeripheralConfig* config) {
    ++mutations;m->peripherals[slot].config=*config;return true;
}
void launcher_platform_refresh_metrics(LauncherPlatform* p) { (void)p; }
static void check(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static void script(const char* text) {
#ifdef _WIN32
    _putenv_s("LNG_SCRIPT", text);
#else
    setenv("LNG_SCRIPT", text, 1);
#endif
    launcher_debug_init();
}
int main(int argc, char** argv) {
    LauncherPlatform platform = {0};
    LauncherModel model;
    memset(&model, 0, sizeof(model));
    model.action = LNG_ACTION_NONE;
    if (argc != 2) return 2;
    /* The interpreter initializes once per launcher process. Each case gets
     * its own process instead of inventing reset semantics for its globals. */
    if (strcmp(argv[1], "wait") == 0) {
        script("wait:2;quit");
        check(!launcher_debug_step_pending(&platform), "pending accepts wait");
        check(!launcher_debug_step_pending(&platform), "first wait frame stays responsive");
        check(!launcher_debug_step_pending(&platform), "second wait frame stays responsive");
        check(launcher_debug_step_pending(&platform), "quit becomes a close request");
        check(model.action == LNG_ACTION_NONE && !platform.should_quit,
              "pending quit does not tear down borrowed state");
        check(launcher_debug_step_pending(&platform), "script exhaustion also requests close");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "launch") == 0) {
        script("quit");
        model.action = LNG_ACTION_LAUNCH;
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_LAUNCH, "script cannot overwrite successful launch");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "dlc") == 0) {
        script("dlc:pack with spaces.wad;quit");
        check(!launcher_debug_step_pending(&platform), "DLC import deferred while provider is busy");
        check(mutations == 0, "pending path cannot start an import");
        launcher_debug_step(&platform, &model);
        check(mutations == 1 && strcmp(model.dlc_path,"pack with spaces.wad") == 0,
              "prefilled DLC path reaches model without a dialog");
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_QUIT, "script quit follows DLC request");
        return failures ? 1 : 0;
    }
    if(strcmp(argv[1],"media")==0) {
        model.system_media_title="System files";
        script("media:C:/owned folder/nand.bin;quit");
        check(!launcher_debug_step_pending(&platform) && mutations==0,"system import waits for provider");
        launcher_debug_step(&platform,&model);
        check(mutations==1 && !strcmp(model.rom_full,"C:/owned folder/nand.bin"),"prefilled NAND source has no dialog");
        return failures?1:0;
    }
    if(strcmp(argv[1],"peripheral")==0) {
        RecompLauncherCPeripheral peripheral={0};model.peripherals=&peripheral;model.num_peripherals=1;
        script("peripheral:0,2,1,C:/owned folder/storage.img;quit");
        check(!launcher_debug_step_pending(&platform) && mutations==0,"mount waits for provider");
        launcher_debug_step(&platform,&model);
        check(mutations==1 && peripheral.config.kind==2 && peripheral.config.read_only &&
              !strcmp(peripheral.config.image_path,"C:/owned folder/storage.img"),"prefilled mount reaches model");
        return failures?1:0;
    }
    if (strcmp(argv[1], "model") != 0) return 2;

    script("view:settings;player:2;capbtn:3;caphk:1;quit");
    for (int i = 0; i < 8; ++i)
        check(!launcher_debug_step_pending(&platform), "model command deferred while busy");
    check(mutations == 0 && model.cfg_player == 0, "pending path cannot mutate model");
    launcher_debug_step(&platform, &model);
    check(mutations == 1 && model.view == LNG_VIEW_SETTINGS, "deferred view resumes after join");
    check(!launcher_debug_step_pending(&platform), "direct player edit remains deferred");
    launcher_debug_step(&platform, &model);
    check(model.cfg_player == 2, "deferred player command is retained");
    launcher_debug_step(&platform, &model);
    launcher_debug_step(&platform, &model);
    check(mutations == 3 && model.capture_btn == 3, "capture mutations run only on normal path");
    launcher_debug_step(&platform, &model);
    check(model.action == LNG_ACTION_QUIT, "normal quit behavior retained");
    return failures ? 1 : 0;
}
