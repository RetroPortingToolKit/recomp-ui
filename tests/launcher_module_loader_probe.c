/* A real DLL/provider query and refused-Run fixture; never opens a UI. */
#include "launcher_module_contract.h"
#include "launcher_module_sdl.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static unsigned callbacks, checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int callback(const char* path, RecompLauncherCDiscVerify* result) {
    (void)path; (void)result; ++callbacks; return 0;
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    HMODULE library = LoadLibraryA(argv[1]);
    CHECK(library != NULL);
    RecompLauncherV2QueryFn query = (RecompLauncherV2QueryFn)(void*)GetProcAddress(library,"recomp_launcher_query_v2");
    CHECK(query != NULL);
    CHECK(GetProcAddress(library,"recomp_launcher_query_v1") == NULL);
    RecompLauncherV2SDL sdl = {0};
    char error[512] = {0};
    CHECK(launcher_v2_sdl_identity(&sdl,error,sizeof(error)) == 0);
    RecompLauncherV2Query request = {0};
    request.header = launcher_v2_header(sizeof(request));
    request.architecture = launcher_v2_architecture();
    request.pointer_bits = sizeof(void*) * 8;
    request.byte_order = launcher_v2_byte_order();
    request.calling_convention = RECOMP_LAUNCHER_V2_CDECL;
    request.layout = launcher_v2_layout();
    request.offered = request.required = RECOMP_LAUNCHER_V2_CAPABILITIES;
    request.sdl = sdl;
    RecompLauncherV2Module answer = {0};
    answer.header = launcher_v2_header(sizeof(answer));
    CHECK(query(&request,&answer,error,sizeof(error)) == 0);
    CHECK(answer.run != NULL);
    CHECK(strlen(answer.build_identity) == 64);
    CHECK(memcmp(&answer.sdl,&sdl,sizeof(sdl)) == 0);
    RecompLauncherV2Query bad = request;
    RecompLauncherV2Module before = answer;
    bad.header.major = 1;
    CHECK(query(&bad,&answer,error,sizeof(error)) == RECOMP_LAUNCHER_V2_BAD_VERSION);
    CHECK(memcmp(&before,&answer,sizeof(answer)) == 0);
    bad = request;
    bad.layout.records ^= 1;
    CHECK(query(&bad,&answer,error,sizeof(error)) == RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    CHECK(memcmp(&before,&answer,sizeof(answer)) == 0);
    HMODULE second = LoadLibraryA(argv[2]);
    CHECK(second != NULL);
    uintptr_t second_version = (uintptr_t)GetProcAddress(second,"SDL_GetVersion");
    uintptr_t second_joystick = (uintptr_t)GetProcAddress(second,"SDL_GetJoystickFromID");
    CHECK(second_version && second_joystick);
    CHECK(second_version != sdl.get_version_address);
    bad = request;
    bad.sdl.get_version_address = second_version;
    bad.sdl.get_joystick_address = second_joystick;
    snprintf(bad.sdl.provider_path,sizeof(bad.sdl.provider_path),"%s",argv[2]);
    CHECK(query(&bad,&answer,error,sizeof(error)) == RECOMP_LAUNCHER_V2_BAD_SDL);
    CHECK(memcmp(&before,&answer,sizeof(answer)) == 0);
    RecompLauncherV2Settings settings;
    memset(&settings,0x5a,sizeof(settings));
    settings.header = launcher_v2_header(sizeof(settings));
    RecompLauncherV2Settings settings_before = settings;
    RecompLauncherV2Game game = {0};
    game.header = launcher_v2_header(sizeof(game));
    game.value.disc_verify = callback;
    RecompLauncherV2Run run = {0};
    run.header = launcher_v2_header(sizeof(run));
    run.header.major = 1;
    run.contract = &request;
    run.settings = &settings;
    run.game = &game;
    RecompLauncherV2Result result = {0};
    result.header = launcher_v2_header(sizeof(result));
    RecompLauncherV2Result result_before = result;
    CHECK(answer.run(&run,&result,error,sizeof(error)) == RECOMP_LAUNCHER_V2_BAD_VERSION);
    CHECK(callbacks == 0);
    CHECK(memcmp(&settings_before,&settings,sizeof(settings)) == 0);
    CHECK(memcmp(&result_before,&result,sizeof(result)) == 0);
    printf("real-module-query checks=%u callbacks=%u secondary-provider-distinct=1 sdl-version=%u module-build=%s provider=%s\n",
           checks,callbacks,sdl.version,answer.build_identity,sdl.provider_path);
    CHECK(FreeLibrary(second));
    CHECK(FreeLibrary(library));
    return 0;
}
