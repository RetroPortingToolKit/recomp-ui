#include "launcher_profile.h"
#include "launcher_system.h"
#include "launcher_rom_body.h"
#include <assert.h>
#include <string.h>
int main(void) {
    RecompLauncherCGameInfo game;
    memset(&game,0,sizeof(game));
    assert(launcher_profile_apply("3DS",&game));
    assert(strcmp(game.platform,"NINTENDO 3DS")==0);
    assert(game.num_players==1 && game.has_bios==0);
    assert(!game.has_renderer && !game.has_supersampling && !game.has_virtual_stylus);
    assert(launcher_system_infer(&game)==&kSystemProfile3ds);
    assert(launcher_system_by_id("ctr")==&kSystemProfile3ds);
    assert(launcher_system_by_id("Nintendo3DS")==&kSystemProfile3ds);
    assert(launcher_system_by_id("3ds-extra")==NULL);
    assert(kSystemProfile3ds.controller.max_players==1 && kSystemProfile3ds.controller.button_count==16);
    assert(strcmp(kSystemProfile3ds.controller.buttons[12].label,"Circle Pad Up")==0);
    assert(kSystemProfile3ds.video.bios==0 && kSystemProfile3ds.video.renderer==0);
    assert(kSystemProfile3ds.video.window_scale && kSystemProfile3ds.video.fullscreen);
    assert(kSystemProfile3ds.hide_audio_freq && kSystemProfile3ds.rom_filter.pattern_count==2);
    {
        uint8_t profile[1536]={0};
        assert(launcher_rom_header_bytes("3ds",profile,sizeof(profile))==0);
        assert(launcher_rom_header_bytes("snes",profile,sizeof(profile))==512);
        memcpy(profile,"NES\032",4);
        assert(launcher_rom_header_bytes("3ds",profile,sizeof(profile))==0);
        assert(launcher_rom_header_bytes("nes",profile,sizeof(profile))==16);
    }
    memset(&game,0,sizeof(game));
    assert(launcher_profile_apply("nds",&game));
    assert(game.has_bios && launcher_system_infer(&game)==&kSystemProfileNds);
    memset(&game,0,sizeof(game));
    assert(launcher_profile_apply("psx",&game));
    assert(launcher_system_infer(&game)==&kSystemProfilePsx);
    return 0;
}
