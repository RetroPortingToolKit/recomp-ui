#include "launcher_module_contract.h"
#include <stdio.h>
#include <string.h>

extern const uint64_t* launcher_v2_cpp_witness(uint32_t* count);
static unsigned checks;
static unsigned callbacks;
#define CHECK(condition) do { ++checks; if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static int disc_verify(const char* path, RecompLauncherCDiscVerify* out) {
    (void)path; (void)out; ++callbacks; return 1;
}

static int refused_unchanged(RecompLauncherV2Run* run, RecompLauncherV2Result* result,
                             RecompLauncherV2SDL* sdl, uint32_t expected) {
    const RecompLauncherV2Settings before = *run->settings;
    const RecompLauncherV2Result result_before = *result;
    char selected[512], relaunch[512], replay[512], diagnostic[512] = {0};
    memcpy(selected, result->selected_disc, sizeof(selected));
    memcpy(relaunch, result->relaunch_path, sizeof(relaunch));
    memcpy(replay, result->replay_path, sizeof(replay));
    const uint32_t status = launcher_v2_validate_run(run, result, sdl,
                                                    RECOMP_LAUNCHER_V2_CAPABILITIES, diagnostic, sizeof(diagnostic));
    CHECK(status == expected);
    CHECK(diagnostic[0]);
    CHECK(memcmp(&before, run->settings, sizeof(before)) == 0);
    CHECK(memcmp(&result_before, result, sizeof(result_before)) == 0);
    CHECK(memcmp(selected, result->selected_disc, sizeof(selected)) == 0);
    CHECK(memcmp(relaunch, result->relaunch_path, sizeof(relaunch)) == 0);
    CHECK(memcmp(replay, result->replay_path, sizeof(replay)) == 0);
    CHECK(callbacks == 0);
    return 0;
}

int main(void) {
    /* These are authored contract fixtures, not a real SDL/provider/device
     * or title/setup run. Native DLL/SDL/callback/platform gates stay open. */
    const uint64_t c_values[] = {
#define RL_WITNESS_STRUCT(type) sizeof(type), _Alignof(type),
#define RL_WITNESS_FIELD(type, member, signature) offsetof(type, member), sizeof(((type*)0)->member),
#define RL_WITNESS_SIGNATURE(signature)
#include "launcher_module_v2_records_witness.inc"
#include "launcher_module_v2_boundary_witness.inc"
#undef RL_WITNESS_STRUCT
#undef RL_WITNESS_FIELD
#undef RL_WITNESS_SIGNATURE
    };
    uint32_t cpp_count = 0;
    const uint64_t* cpp_values = launcher_v2_cpp_witness(&cpp_count);
    CHECK(cpp_count == sizeof(c_values) / sizeof(c_values[0]));
    CHECK(memcmp(c_values, cpp_values, sizeof(c_values)) == 0);
    RecompLauncherV2SDL sdl = {0};
    sdl.header = launcher_v2_header(sizeof(sdl));
    sdl.version = 3002000;
    sdl.get_version_address = 16;
    sdl.get_joystick_address = 32;
    sdl.sha256[0] = 1;
    strcpy(sdl.provider_path, "authored-fixture/SDL3.dll");
    RecompLauncherV2Query good = {0};
    good.header = launcher_v2_header(sizeof(good));
    good.pointer_bits = sizeof(void*) * 8;
    good.byte_order = launcher_v2_byte_order();
    good.architecture = launcher_v2_architecture();
    good.calling_convention = RECOMP_LAUNCHER_V2_CDECL;
    good.layout = launcher_v2_layout();
    good.offered = good.required = RECOMP_LAUNCHER_V2_CAPABILITIES;
    good.sdl = sdl;
    RecompLauncherV2Settings settings;
    memset(&settings, 0xa5, sizeof(settings));
    settings.header = launcher_v2_header(sizeof(settings));
    RecompLauncherV2Game game = {0};
    game.header = launcher_v2_header(sizeof(game));
    game.value.disc_verify = disc_verify;
    char selected[512], relaunch[512], replay[512], diagnostic[512];
    memset(selected, 's', sizeof(selected));
    memset(relaunch, 'x', sizeof(relaunch));
    memset(replay, 'r', sizeof(replay));
    RecompLauncherV2Result result = {0};
    result.header = launcher_v2_header(sizeof(result));
    result.selected_disc = selected; result.selected_disc_capacity = sizeof(selected);
    result.relaunch_path = relaunch; result.relaunch_path_capacity = sizeof(relaunch);
    result.replay_path = replay; result.replay_path_capacity = sizeof(replay);
    RecompLauncherV2Run run = {0};
    run.header = launcher_v2_header(sizeof(run));
    run.settings = &settings; run.game = &game;
#ifdef _WIN32
    run.product_root = "C:/authored-fixture/product";
    run.assets_root = "C:/authored-fixture/assets";
    run.save_root = "C:/authored-fixture/saves";
#else
    run.product_root = "/authored-fixture/product";
    run.assets_root = "/authored-fixture/assets";
    run.save_root = "/authored-fixture/saves";
#endif
    run.contract = &good;
    CHECK(launcher_v2_validate_run(&run, &result, &sdl, good.offered, diagnostic, sizeof(diagnostic)) == 0);

#define BAD_QUERY(edit, expected) do { RecompLauncherV2Query bad = good; edit; run.contract = &bad; CHECK(refused_unchanged(&run, &result, &sdl, expected) == 0); } while (0)
    BAD_QUERY(bad.header.major = 1, RECOMP_LAUNCHER_V2_BAD_VERSION);
    BAD_QUERY(bad.header.major = 3, RECOMP_LAUNCHER_V2_BAD_VERSION);
    BAD_QUERY(bad.header.minor = 1, RECOMP_LAUNCHER_V2_BAD_VERSION);
    BAD_QUERY(bad.header.size -= 1, RECOMP_LAUNCHER_V2_BAD_RECORD);
    BAD_QUERY(bad.header.reserved = 1, RECOMP_LAUNCHER_V2_BAD_RECORD);
    BAD_QUERY(bad.pointer_bits = 16, RECOMP_LAUNCHER_V2_BAD_ARCHITECTURE);
    BAD_QUERY(bad.architecture = 99, RECOMP_LAUNCHER_V2_BAD_ARCHITECTURE);
    BAD_QUERY(bad.byte_order = 99, RECOMP_LAUNCHER_V2_BAD_ARCHITECTURE);
    BAD_QUERY(bad.calling_convention = 99, RECOMP_LAUNCHER_V2_BAD_ARCHITECTURE);
    BAD_QUERY(bad.layout.settings ^= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.callbacks ^= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.records ^= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.boundary ^= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.settings_bytes -= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.game_bytes -= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.mods_bytes -= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.layout.netplay_bytes -= 1, RECOMP_LAUNCHER_V2_BAD_LAYOUT);
    BAD_QUERY(bad.offered &= ~RECOMP_LAUNCHER_V2_NETPLAY, RECOMP_LAUNCHER_V2_MISSING_CAPABILITY);
    BAD_QUERY(bad.required |= UINT64_C(1) << 63, RECOMP_LAUNCHER_V2_MISSING_CAPABILITY);
    BAD_QUERY(bad.sdl.version += 1, RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(bad.sdl.get_version_address += 1, RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(bad.sdl.get_joystick_address += 1, RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(bad.sdl.sha256[31] ^= 1, RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(bad.sdl.provider_path[0] = 'X', RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(memset(bad.sdl.provider_path, 'X', sizeof(bad.sdl.provider_path)), RECOMP_LAUNCHER_V2_BAD_SDL);
    BAD_QUERY(bad.sdl.header.size -= 1, RECOMP_LAUNCHER_V2_BAD_SDL);
#undef BAD_QUERY
    run.contract = &good;
    RecompLauncherV2Mods mods = {0};
    mods.header = launcher_v2_header(sizeof(mods));
    run.mods = &mods;
    mods.header.size -= 1;
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_BAD_RECORD) == 0);
    mods.header = launcher_v2_header(sizeof(mods));
    game.value.mods = &mods.value;
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_BAD_RECORD) == 0);
    game.value.mods = NULL; run.mods = NULL;
    RecompLauncherV2Netplay netplay = {0};
    netplay.header = launcher_v2_header(sizeof(netplay));
    run.netplay = &netplay;
    netplay.header.major = 1;
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_BAD_RECORD) == 0);
    run.netplay = NULL;
    result.replay_path_capacity = 511;
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_BAD_RECORD) == 0);
    result.replay_path_capacity = sizeof(replay);
    const char* absolute_product = run.product_root;
    run.product_root = "relative-fixture/product";
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_BAD_RECORD) == 0);
    run.product_root = absolute_product;
    good.offered = good.required = RECOMP_LAUNCHER_V2_SETTINGS;
    CHECK(refused_unchanged(&run, &result, &sdl, RECOMP_LAUNCHER_V2_MISSING_CAPABILITY) == 0);
    CHECK(callbacks == 0);
    printf("contract-fixture checks=%u callbacks=%u C/C++-witness-values=%u settings-bytes=%u game-bytes=%u mods-bytes=%u netplay-bytes=%u\n",
        checks, callbacks, cpp_count, good.layout.settings_bytes, good.layout.game_bytes, good.layout.mods_bytes, good.layout.netplay_bytes);
    puts("This is a contract fixture; full V1-REFUSAL/SDL/title/setup/platform/update measurements remain UNRUN.");
    return 0;
}

