/* Run the production script parser; only the model/platform seams are stubs. */
#include "launcher_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mutations, failures;
static int assist_action = -1;
static bool assist_gamepad;
void launcher_model_import_mod_resource(LauncherModel* m, const char* pkg,
    const char* feature, const char* resource, const char* path) {
    (void)m; (void)pkg; (void)feature; (void)resource; (void)path; ++mutations;
}

void launcher_model_set_view(LauncherModel* m, LngView view) { ++mutations; m->view = view; }
void launcher_model_begin_capture(LauncherModel* m, int button) { ++mutations; m->capture_btn = button; }
void launcher_model_begin_pad_capture(LauncherModel* m, int button) { ++mutations; m->capture_btn = button; m->capture_pad = true; }
void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey hotkey) { (void)m; (void)hotkey; ++mutations; }
void launcher_model_begin_assist_capture(LauncherModel* m, int action, bool gamepad) {
    (void)m; ++mutations; assist_action = action; assist_gamepad = gamepad;
}
void launcher_model_set_player_tab(LauncherModel* m, int tab) { (void)m; (void)tab; ++mutations; }
void launcher_model_set_card_tab(LauncherModel* m, int tab) { (void)m; (void)tab; ++mutations; }
void launcher_model_select_start(LauncherModel* m, int idx) { (void)m; (void)idx; ++mutations; }
void launcher_model_select_disc(LauncherModel* m, int idx) { (void)m; (void)idx; ++mutations; }
void launcher_platform_refresh_metrics(LauncherPlatform* p) { (void)p; }
static void check(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static int netplay_calls, netplay_result;
static int netplay_handler(LauncherModel* model, const char* command) {
    check(model != NULL, "semantic command receives the live model");
    check(strcmp(command, "play") == 0, "netplay prefix is stripped exactly");
    ++netplay_calls;
    if (netplay_result == 2) { model->action = LNG_ACTION_LAUNCH; return 1; }
    return netplay_result;
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
    if (strcmp(argv[1], "netplay-sync-launch") == 0) {
        script("netplay:play;netplay:wait-start;quit");
        netplay_result = 2;
        launcher_debug_set_netplay_handler(netplay_handler);
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_LAUNCH && netplay_calls == 1,
              "synchronous Play keeps its real launch and consumes its awaited token");
        model.action = LNG_ACTION_NONE;
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_QUIT && netplay_calls == 1,
              "the next action waits until the launcher returns");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "netplay-launched") == 0) {
        script("netplay:wait-start;quit");
        model.action = LNG_ACTION_LAUNCH;
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_LAUNCH && netplay_calls == 0,
              "actual launch completes wait-start without clobbering the action");
        model.action = LNG_ACTION_NONE; /* a normal return to the launcher */
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_QUIT,
              "completed wait-start cannot hang a returned waiting room");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "netplay") == 0) {
        script("netplay:play;view:settings;quit");
        launcher_debug_set_netplay_handler(netplay_handler);
        check(!launcher_debug_step_pending(&platform) && netplay_calls == 0,
              "provider-owned model defers semantic netplay commands");
        launcher_debug_step(&platform, &model);
        check(netplay_calls == 1 && model.action == LNG_ACTION_NONE,
              "real room pending retains its command");
        netplay_result = 1;
        launcher_debug_step(&platform, &model);
        check(netplay_calls == 2 && mutations == 0,
              "completed command advances exactly once");
        launcher_debug_step(&platform, &model);
        check(mutations == 1 && model.view == LNG_VIEW_SETTINGS,
              "next command follows successful completion");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "netplay-refused") == 0 || strcmp(argv[1], "netplay-unavailable") == 0) {
        script("netplay:play;view:settings");
        if (strcmp(argv[1], "netplay-refused") == 0) {
            netplay_result = -1;
            launcher_debug_set_netplay_handler(netplay_handler);
        }
        launcher_debug_step(&platform, &model);
        check(model.action == LNG_ACTION_QUIT && mutations == 0,
              "refused/unavailable semantic action cannot continue as success");
        return failures ? 1 : 0;
    }
    if (strcmp(argv[1], "model") != 0) return 2;

    script("view:settings;player:2;capbtn:3;cappad:5;caphk:1;capassist:2,1;quit");
    for (int i = 0; i < 8; ++i)
        check(!launcher_debug_step_pending(&platform), "model command deferred while busy");
    check(mutations == 0 && model.cfg_player == 0, "pending path cannot mutate model");
    launcher_debug_step(&platform, &model);
    check(mutations == 1 && model.view == LNG_VIEW_SETTINGS, "deferred view resumes after join");
    check(!launcher_debug_step_pending(&platform), "direct player edit remains deferred");
    launcher_debug_step(&platform, &model);
    check(model.cfg_player == 2, "deferred player command is retained");
    launcher_debug_step(&platform, &model);
    check(mutations == 2 && model.capture_btn == 3, "keyboard capture runs on normal path");
    check(!launcher_debug_step_pending(&platform) && mutations == 2,
          "gamepad capture remains deferred while busy");
    launcher_debug_step(&platform, &model);
    check(mutations == 3 && model.capture_btn == 5 && model.capture_pad,
          "deferred gamepad capture resumes on normal path");
    launcher_debug_step(&platform, &model);
    check(mutations == 4, "hotkey capture runs only on normal path");
    check(!launcher_debug_step_pending(&platform) && mutations == 4,
          "assist capture remains deferred while busy");
    launcher_debug_step(&platform, &model);
    check(mutations == 5 && assist_action == 2 && assist_gamepad,
          "deferred assist capture retains action and gamepad selection");
    launcher_debug_step(&platform, &model);
    check(model.action == LNG_ACTION_QUIT, "normal quit behavior retained");
    return failures ? 1 : 0;
}
