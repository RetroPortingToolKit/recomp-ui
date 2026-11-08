// SPDX-License-Identifier: MIT
// PS2 host profile: shared DualShock vocabulary; native PS2 saves remain host-owned.
#ifndef RUI_CONSOLE_PS2_PROFILE_H
#define RUI_CONSOLE_PS2_PROFILE_H
#include "../psx/psx_profile.h"
static const char* const kPanelsDashboardPs2[]={"game","controller",NULL};
static const char* const kPanelsSettingsPs2[]={"video","audio",NULL};
static const SystemProfile kSystemProfilePs2 = {
    /* id       */ "ps2",
    /* platform */ "PLAYSTATION 2",
    /* theme    */ "ps2",
    /* rom_noun */ "Disc",
    /* controller */ {
        kPsxPadButtons, LNG_PSX_PAD_BUTTON_COUNT,     // shared DualShock button vocabulary (Triangle/Circle/Cross/Square/L1-2/R1-2/L3/R3)
        "pad.tga", "pad_analog.tga", "pad_digital.tga",
        /* max_players */ 2,
        /* has_pad_mode */ 1,
        /* binds_per_input */ 1,  // primary + alternate; either asserts the
                                  // input, and either may be a mouse button
    },
    // PS2 native 8MiB cards remain host-owned: no PS1 formatter/inspector.
    /* save */    { SAVE_NONE, 0, NULL },
    /* video */   {
        /*window_scale*/0, /*fullscreen*/1, /*linear_filter*/0, /*widescreen*/0,
        /*renderer*/0, /*supersampling*/0, /*screen_kind*/0, /*frame_interp*/0, /*aspect*/0,
        /*texture_filter*/0, /*antialiasing*/0, /*spu_hq*/0, /*skip_fmv*/0, /*turbo_loads*/0,
        /*bios*/0, /*deadzone*/1,
        /*widescreen_cells*/0, /*fmv_filter*/0,
    },
    /* verify */  { 0, NULL },    // no PS1 synthetic disc verdict for a linked PS2 title
    // Only controls consumed by the host are exposed.
    /* hotkeys_mask */ 0,
    /* panels_dashboard  */ kPanelsDashboardPs2,
    /* panels_settings   */ kPanelsSettingsPs2,
    /* panels_controller */ kPanelsControllerCommon,
    /* screen_kind_names */ NULL,   /* legacy Raw/CRT/Composite/Trinitron set */
    /* screen_kind_count */ 0,
    /* rom_filter        */ { kPsxDiscPatterns, LNG_PSX_DISC_PATTERN_COUNT,
                              "PlayStation 2 disc (.iso/.bin)" },
    /* renderer_labels   */ NULL,
    /* hide_audio_freq   */ 1,
    /* brand             */ NULL,
    /* wordmark_image    */ NULL,
};
static inline int launcher_console_is_ps2(const char* name) {
 return lps_streq_ci(name,"ps2")||lps_streq_ci(name,"playstation2");
}
static inline void launcher_profile_apply_ps2(RecompLauncherCGameInfo* gi) {
 gi->theme="ps2";gi->platform="PLAYSTATION 2";gi->rom_noun="Disc";
 gi->pad_mode_supported=1;gi->pad_mode_selectable=0;gi->locked_pad_mode=1;
 gi->widescreen_supported=0;gi->aspect_mask=0;
 gi->has_window_size=0;gi->has_renderer=0;gi->has_supersampling=0;
 gi->has_antialiasing=0;gi->has_texture_filter=0;gi->has_screen_kind=0;
 gi->has_fmv_filter=0;gi->has_frame_interp=0;gi->has_spu_hq=0;gi->has_skip_fmv=0;
 gi->has_turbo_loads=0;gi->has_bios=0;gi->has_rewind_depth=0;
 gi->has_deadzone_pct=1;gi->num_players=2;gi->hide_rebind=0;
}
#endif
