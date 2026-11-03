/* SNES first-launch input defaults, in launcher_binds.c.
 *
 * Two reports this pins:
 *
 *  1. "when a game is first launched it shows Gamepad in the drop down,
 *     instead of the name of the gamepad it defaulted to". config.ini says
 *     EnableGamepad1, so the slot's source is Gamepad, but nothing has ever
 *     named a device -- no GUID -- and the Input source box fell back to the
 *     generic placeholder while the game was about to use a real controller.
 *     Hydration now binds the first live pad no other slot has claimed.
 *
 *  2. The deadzone that comes with it. A pad with no saved profile gets the
 *     console default (10%), not whatever the previous selection left in the
 *     slot: a deadzone belongs to the device.
 *
 * Includes the translation unit under test (as launcher_discs_test.c does) so
 * the static profile helpers are reachable, and links launcher_model.c for the
 * real set_source/visible_player_count. Every other console bridge is stubbed.
 */
#include "launcher_binds.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated stubs. launcher_binds.c reaches every console's bridge; this
 * test drives only the SNES paths, so the rest resolve to empty bodies. */

int rui_gb_binds_get(const char* path, int b) { (void)path; (void)b; return 0; }
int rui_gb_binds_get_slot(const char* path, int b, int slot) { (void)path; (void)b; (void)slot; return 0; }
void rui_gb_binds_init(const char* path) { (void)path; }
void rui_gb_binds_reset(const char* path) { (void)path; }
void rui_gb_binds_set(const char* path, int b, int scancode) { (void)path; (void)b; (void)scancode; }
void rui_gb_binds_set_slot(const char* path, int b, int slot, int scancode) { (void)path; (void)b; (void)slot; (void)scancode; }
int rui_genesis_binds_get_key(const char* path, int player, int b) { (void)path; (void)player; (void)b; return 0; }
void rui_genesis_binds_get_pad(const char* path, int player, int b, int* kind, int* code, int* axis_dir) { (void)path; (void)player; (void)b; (void)kind; (void)code; (void)axis_dir; }
void rui_genesis_binds_init(const char* path) { (void)path; }
void rui_genesis_binds_reset(const char* path, int player) { (void)path; (void)player; }
void rui_genesis_binds_set_key(const char* path, int player, int b, int scancode) { (void)path; (void)player; (void)b; (void)scancode; }
void rui_genesis_binds_set_pad(const char* path, int player, int b, int kind, int code, int axis_dir) { (void)path; (void)player; (void)b; (void)kind; (void)code; (void)axis_dir; }
void rui_n64_binds_get(const char* path, int device, int b, int slot, int* out_type, int* out_id) { (void)path; (void)device; (void)b; (void)slot; (void)out_type; (void)out_id; }
void rui_n64_binds_init(const char* path) { (void)path; }
void rui_n64_binds_label(int type, int id, char* out, size_t cap) { (void)type; (void)id; (void)out; (void)cap; }
void rui_n64_binds_reset_device(const char* path, int device) { (void)path; (void)device; }
void rui_n64_binds_set(const char* path, int device, int b, int slot, int type, int id) { (void)path; (void)device; (void)b; (void)slot; (void)type; (void)id; }
void rui_n64_pad_binds_init(const char* path) { (void)path; }
void rui_n64_pad_binds_path(const char* bind_cfg_path, char* out, int cap) { (void)bind_cfg_path; (void)out; (void)cap; }
void rui_n64_pad_binds_remember(const char* path, const char* guid, const char* name, int deadzone_pct) { (void)path; (void)guid; (void)name; (void)deadzone_pct; }
void rui_n64_pad_binds_save_profile(const char* path, const char* guid, const char* name, int name_custom, int deadzone_pct) { (void)path; (void)guid; (void)name; (void)name_custom; (void)deadzone_pct; }
void rui_n64_pad_binds_rename(const char* path, const char* guid, const char* name) { (void)path; (void)guid; (void)name; }
void rui_n64_pad_binds_delete(const char* path, const char* guid) { (void)path; (void)guid; }
int rui_n64_pad_binds_name_is_custom(const char* path, const char* guid) { (void)path; (void)guid; return 0; }
void rui_n64_pad_binds_reset(const char* path, const char* guid) { (void)path; (void)guid; }
void rui_n64_pad_binds_set(const char* path, const char* guid, int b, int kind, int code, int axis_dir) { (void)path; (void)guid; (void)b; (void)kind; (void)code; (void)axis_dir; }
void rui_n64_pad_binds_source(const char* path, const char* guid, int b, char* out, int cap) { (void)path; (void)guid; (void)b; (void)out; (void)cap; }
int rui_nes_binds_get(const char* path, int player, int b) { (void)path; (void)player; (void)b; return 0; }
void rui_nes_binds_init(const char* path) { (void)path; }
void rui_nes_binds_reset(const char* path, int player) { (void)path; (void)player; }
void rui_nes_binds_set(const char* path, int player, int b, int scancode) { (void)path; (void)player; (void)b; (void)scancode; }
int rui_nes_camera_bind_get(const char* path, int action) { (void)path; (void)action; return 0; }
void rui_nes_camera_bind_reset(const char* path) { (void)path; }
void rui_nes_camera_bind_set(const char* path, int action, int scancode) { (void)path; (void)action; (void)scancode; }
void rui_nes_zapper_get(const char* path, int* mouse_enabled, int* crosshair) { (void)path; (void)mouse_enabled; (void)crosshair; }
void rui_nes_zapper_set(const char* path, int mouse_enabled, int crosshair) { (void)path; (void)mouse_enabled; (void)crosshair; }
void rui_pad_binds_sibling_path(const char* ref, const char* file, char* out, int cap) { (void)ref; (void)file; (void)out; (void)cap; }
int rui_psx_binds_get_slot(const char* path, int player, int b, int slot) { (void)path; (void)player; (void)b; (void)slot; return 0; }
void rui_psx_binds_init(const char* path) { (void)path; }
void rui_psx_binds_reset(const char* path, int player) { (void)path; (void)player; }
void rui_psx_binds_save(const char* path) { (void)path; }
void rui_psx_binds_set_slot(const char* path, int player, int b, int slot, int scancode) { (void)path; (void)player; (void)b; (void)slot; (void)scancode; }
int rui_psx_pad_binds_deadzone(const char* path, const char* guid) { (void)path; (void)guid; return 0; }
void rui_psx_pad_binds_delete(const char* path, const char* guid) { (void)path; (void)guid; }
void rui_psx_pad_binds_init(const char* path) { (void)path; }
int rui_psx_pad_binds_known_at(const char* path, int index, char* guid, int guid_cap, char* name, int name_cap) { (void)path; (void)index; (void)guid; (void)guid_cap; (void)name; (void)name_cap; return 0; }
int rui_psx_pad_binds_known_count(const char* path) { (void)path; return 0; }
void rui_psx_pad_binds_label(const char* path, const char* guid, int b, char* out, int cap) { (void)path; (void)guid; (void)b; (void)out; (void)cap; }
void rui_psx_pad_binds_name(const char* path, const char* guid, char* out, int cap) { (void)path; (void)guid; (void)out; (void)cap; }
int rui_psx_pad_binds_name_is_custom(const char* path, const char* guid) { (void)path; (void)guid; return 0; }
void rui_psx_pad_binds_remember(const char* path, const char* guid, const char* name, int deadzone_pct) { (void)path; (void)guid; (void)name; (void)deadzone_pct; }
void rui_psx_pad_binds_rename(const char* path, const char* guid, const char* name) { (void)path; (void)guid; (void)name; }
void rui_psx_pad_binds_reset(const char* path, const char* guid) { (void)path; (void)guid; }
void rui_psx_pad_binds_save_profile(const char* path, const char* guid, const char* name, int name_custom, int deadzone_pct) { (void)path; (void)guid; (void)name; (void)name_custom; (void)deadzone_pct; }
void rui_psx_pad_binds_set(const char* path, const char* guid, int b, int kind, int code, int axis_dir) { (void)path; (void)guid; (void)b; (void)kind; (void)code; (void)axis_dir; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static SystemProfile g_snes_prof;

static void make_model(LauncherModel* m) {
    memset(m, 0, sizeof(*m));
    g_snes_prof = kSystemProfileSnes;
    m->profile = &g_snes_prof;
    m->player_count = 2;
    /* What config.ini's EnableGamepad1/2 produce: both slots are "gamepad",
     * neither names a device. */
    m->s.player_src[0] = 2;
    m->s.player_src[1] = 2;
    m->s.deadzone[0] = 30;   /* the value the former default converted to */
    m->s.deadzone[1] = 30;
}

int main(int argc, char** argv) {
    /* Keep the profile store out of any real config.ini. */
    static char cfg[1024];
    snprintf(cfg, sizeof(cfg), "%s/snes_pad_defaults_test.ini",
             argc > 1 ? argv[1] : ".");
    remove(cfg);
    g_launcher_config_path = cfg;

    LauncherPad pads[2];
    memset(pads, 0, sizeof(pads));
    snprintf(pads[0].guid, sizeof(pads[0].guid), "%s", "03000000aabbccdd0001");
    snprintf(pads[0].name, sizeof(pads[0].name), "%s", "DualSense Wireless Controller");
    pads[0].id = 7;
    snprintf(pads[1].guid, sizeof(pads[1].guid), "%s", "03000000aabbccdd0002");
    snprintf(pads[1].name, sizeof(pads[1].name), "%s", "8BitDo SN30 Pro");
    pads[1].id = 9;

    /* ---- one live pad, two gamepad slots ------------------------------- */
    LauncherModel m;
    make_model(&m);
    launcher_binds_hydrate_snes_pad_names(&m, pads, 1);

    expect(strcmp(m.s.player_gamepad_guid[0], pads[0].guid) == 0,
           "player 1 binds the live pad's GUID");
    expect(strcmp(m.player_pad_name[0], pads[0].name) == 0,
           "and shows its name, not the generic placeholder");
    expect(strcmp(launcher_model_player_src_label(&m, 0), pads[0].name) == 0,
           "so the Input source label reads the device name");
    expect(m.player_pad_id[0] == pads[0].id, "and carries the SDL id");
    expect(m.s.deadzone[0] == RUI_SNES_PAD_DEFAULT_DEADZONE_PCT,
           "an unconfigured pad takes the console default deadzone");
    expect(m.s.player_gamepad_guid[1][0] == '\0',
           "player 2 does not steal the one pad player 1 holds");

    /* ---- a second pad appears ------------------------------------------ */
    launcher_binds_hydrate_snes_pad_names(&m, pads, 2);
    expect(strcmp(m.s.player_gamepad_guid[1], pads[1].guid) == 0,
           "player 2 binds the second pad once it is live");
    expect(strcmp(m.s.player_gamepad_guid[0], pads[0].guid) == 0,
           "and player 1 keeps the pad it already had");

    /* ---- a saved profile wins over the default ------------------------- */
    make_model(&m);
    {
        char sect[96];
        snes_profile_section(pads[0].guid, sect, sizeof(sect));
        launcher_ini_kv_write(cfg, sect, "Deadzone", "22");
        launcher_ini_kv_write(cfg, sect, "Name", "Alex's pad");
    }
    launcher_binds_hydrate_snes_pad_names(&m, pads, 1);
    expect(m.s.deadzone[0] == 22, "a saved profile's deadzone wins over the default");
    expect(strcmp(m.player_pad_name[0], "Alex's pad") == 0,
           "and so does its saved name");

    /* ---- hydration never re-binds a slot that already has a GUID ------- */
    m.s.deadzone[0] = 3;                       /* as if dragged on the slider */
    launcher_binds_hydrate_snes_pad_names(&m, pads, 2);
    expect(m.s.deadzone[0] == 3,
           "a per-frame hydration does not fight a live slider edit");

    /* ---- the slider's setter clamps ------------------------------------ */
    launcher_model_set_deadzone(&m, 0, 137);
    expect(m.s.deadzone[0] == 100, "set_deadzone clamps above 100");
    launcher_model_set_deadzone(&m, 0, -4);
    expect(m.s.deadzone[0] == 0, "set_deadzone clamps below 0");
    launcher_model_set_deadzone(&m, 0, 7);
    expect(m.s.deadzone[0] == 7, "and takes any whole percent between");

    /* A title opts into four seats; legacy/two-player titles stay at two. */
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    launcher_profile_apply_snes(&game);
    for(int count=0;count<=5;++count) {
        game.num_players=count;
        launcher_model_init(&m,&io,&game,NULL);
        expect(launcher_model_visible_player_count(&m)==(count==0?2:count>4?4:count),
               "SNES controller cards follow the game capability, bounded at four");
    }
    make_model(&m);m.player_count=4;
    LauncherPad four[4]={0};
    for(int p=0;p<4;++p) {
        snprintf(four[p].guid,sizeof(four[p].guid),"four-seat-pad-%d",p);
        snprintf(four[p].name,sizeof(four[p].name),"Pad %d",p+1);four[p].id=p+20;
        m.s.player_src[p]=2;
    }
    launcher_binds_hydrate_snes_pad_names(&m,four,4);
    for(int p=0;p<4;++p)expect(!strcmp(m.s.player_gamepad_guid[p],four[p].guid),
                              "each of four seats hydrates its own available gamepad");
    char exe[1024];snprintf(exe,sizeof(exe),"%s/bind-test.exe",argc>1?argv[1]:".");
    recompui_keybinds_init(exe);
    const SDL_Scancode scans[4]={SDL_SCANCODE_F,SDL_SCANCODE_G,SDL_SCANCODE_H,SDL_SCANCODE_J};
    for(int p=0;p<4;++p) {
        launcher_binds_set_button(&m,p+1,0,scans[p]); /* D-pad Up */
        launcher_binds_set_pad_button(&m,p+1,4,LNG_PADBIND_BUTTON,p,0);
    }
    recompui_keybinds_init(exe);launcher_binds_refresh(&m);
    for(int p=0;p<4;++p) {
        uint8_t held[512]={0};held[scans[p]]=1;
        expect(recompui_keybinds_read_player(held,p+1)==0x80,"four independent saved keyboard layouts read back");
        for(int other=0;other<4;++other)if(other!=p)
            expect(!(recompui_keybinds_read_player(held,other+1)&0x80),"a seat's rebind never aliases another keyboard");
        char controls[12][48];snes_gamepad_read_controls(p,controls);
        expect(!strcmp(controls[4],(const char*[]){"A","B","X","Y"}[p]),
               "Controls, ControlsP2, ControlsP3 and ControlsP4 stay independent");
    }
    launcher_binds_save_snes_gamepad(&m,3);
    launcher_binds_rename_snes_gamepad(&m,3,"Third pad");
    expect(launcher_binds_snes_profile_exists(&m,3),"P3 profile can be saved");
    launcher_binds_reset_player(&m,3);
    launcher_binds_apply_snes_pad_profile(&m,3);
    expect(!strcmp(m.player_pad_name[2],"Third pad"),"P3 saved name and mapping restore");
    char restored[12][48];snes_gamepad_read_controls(2,restored);
    expect(!strcmp(restored[4],"X"),"P3 profile restore targets ControlsP3");
    snes_gamepad_read_controls(3,restored);
    expect(!strcmp(restored[4],"Y"),"P3 reset leaves P4 unchanged");
    expect(!recompui_keybinds_read_player((uint8_t[512]){0},5),"invalid seat is rejected");
    char keys_path[1024];snprintf(keys_path,sizeof(keys_path),"%s/keybinds.ini",argc>1?argv[1]:".");remove(keys_path);

    remove(cfg);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("snes_pad_defaults_test: ok");
    return 0;
}
