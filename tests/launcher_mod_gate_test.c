/* Model policy only; no window, input simulation or gameplay state. */
#include "launcher_model.c"
#include "launcher_mod_feature_actions.h"
#include "launcher_quick_mods.h"
#include <assert.h>
#include <stdio.h>
void launcher_binds_set_zapper(int a,int b) { (void)a;(void)b; }
static int feature_enabled,enable_calls,deny_change;
static int enable_feature(void* ctx,const char* package,const char* id,int enabled) {
    (void)ctx;assert(!strcmp(package,"fixture.package"));
    assert(!strcmp(id,"fixture-feature"));++enable_calls;
    if(deny_change)return 0;feature_enabled=enabled;return 1;
}
int main(void) {
    LauncherModel m = {0};
    SystemProfile profile = {0};
    profile.verify.mode = 1; m.profile = &profile;
    assert(!launcher_model_can_open_mods(NULL));
    assert(!launcher_model_can_open_mods(&m));
    m.rom_present = true; strcpy(m.rom_full,"verified-disc.cue");
    for(int v=0;v<4;v++) { m.verify.verdict=v;assert(launcher_model_can_open_mods(&m)==(v==1)); }
    m.verify.verdict=1;assert(launcher_model_can_open_mods(&m));
    m.profile_handoff=true;assert(!launcher_model_can_open_mods(&m));
    m.profile_handoff=false;m.setup_preparing=true;assert(!launcher_model_can_open_mods(&m));
    m.setup_preparing=false;m.rom_present=false;assert(!launcher_model_can_open_mods(&m));
    profile.verify.mode=0;assert(launcher_model_can_open_mods(&m));
    m.disc_selection_locked=true;strcpy(m.rom_full,"verified-disc.cue");m.rom_present=true;
    launcher_model_set_rom(&m,"foreign-disc.cue");assert(!strcmp(m.rom_full,"verified-disc.cue"));
    launcher_model_set_rom(&m,NULL);assert(m.rom_present && !strcmp(m.rom_full,"verified-disc.cue"));
    assert(!m.profile_handoff);
    RecompLauncherCModProvider provider={0};provider.feature_enable=enable_feature;
    m.mods=&provider;profile.verify.mode=1;m.verify.verdict=1;
    RecompLauncherCModFeature feature={0};
    strcpy(feature.package_id,"fixture.package");strcpy(feature.id,"fixture-feature");
    assert(launcher_mod_enable_feature(&m,&feature,1) && feature_enabled && enable_calls==1);
    assert(launcher_mod_enable_feature(&m,&feature,0) && !feature_enabled);
    deny_change=1;assert(!launcher_mod_enable_feature(&m,&feature,1) && !feature_enabled);deny_change=0;
    m.verify.verdict=0;int calls=enable_calls;
    assert(!launcher_mod_enable_feature(&m,&feature,1) && enable_calls==calls);
    m.verify.verdict=1;provider.hide_hidden_features=1;feature.hidden=1;
    assert(!launcher_mod_enable_feature(&m,&feature,1) && enable_calls==calls);
    static const char* labels[]={"Native","1080p","4K"};static const int values[]={1,1080,2160};
    m.has_supersampling=true;m.internal_resolution_labels=labels;m.internal_resolution_values=values;m.num_internal_resolutions=3;
    launcher_model_set_internal_resolution(&m,2);assert(m.s.internal_resolution==2160 && launcher_model_internal_resolution_index(&m)==2);
    m.has_vsync=true;launcher_model_set_vsync(&m,RECOMP_LAUNCHER_VSYNC_ON);assert(m.s.vsync==RECOMP_LAUNCHER_VSYNC_ON);
    launcher_model_set_vsync(&m,RECOMP_LAUNCHER_VSYNC_OFF);assert(m.s.vsync==RECOMP_LAUNCHER_VSYNC_OFF);
    launcher_model_set_fullscreen(&m,2);assert(!strcmp(launcher_model_fullscreen_label(&m),"Exclusive"));
    launcher_model_toggle_fullscreen(&m);assert(!m.s.fullscreen);
    launcher_model_set_fullscreen(&m,1);assert(!strcmp(launcher_model_fullscreen_label(&m),"Borderless"));
    m.has_screen_kind=true;assert(launcher_model_screen_kind_count(&m)==4);
    for(int i=0;i<4;++i){launcher_model_set_screen_kind(&m,i);assert(!strcmp(launcher_model_screen_kind_label(&m),launcher_model_screen_kind_name(&m,i)));}
    static const char* custom_screens[]={"Panel A","Panel B","Panel C","Panel D","Panel E"};
    profile.screen_kind_names=custom_screens;profile.screen_kind_count=5;
    assert(launcher_model_screen_kind_count(&m)==5);launcher_model_set_screen_kind(&m,4);assert(!strcmp(launcher_model_screen_kind_label(&m),"Panel E"));
    launcher_model_cycle_screen_kind(&m);assert(m.s.screen_kind==0);
    profile.screen_kind_names=NULL;profile.screen_kind_count=0;
    feature.hidden=0;provider.hide_hidden_features=0;
    /* Quick Mods lists exactly the provider's quick features that the Mods
     * page lists; no feature name or id is special. */
    feature.quick=0;assert(!launcher_quick_listed(&provider,&feature));
    feature.quick=1;assert(launcher_quick_listed(&provider,&feature));
    strcpy(feature.id,"widescreen");strcpy(feature.name,"Seamless Loading");
    feature.quick=0;assert(!launcher_quick_listed(&provider,&feature));
    feature.quick=1;feature.hidden=1;provider.hide_hidden_features=1;
    assert(!launcher_quick_listed(&provider,&feature));
    assert(!launcher_quick_listed(NULL,&feature) && !launcher_quick_listed(&provider,NULL));
    puts("PASS Quick controls: one provider/state, rejected/hidden/invalid-disc changes blocked, existing setting vocabularies and model values shared");
    puts("PASS locked disc rejects foreign paths/removal without verification or handoff");
    puts("PASS Mods button policy: empty/bad/warn/handoff/preparing locked; verified enabled; non-disc unchanged");
    return 0;
}
