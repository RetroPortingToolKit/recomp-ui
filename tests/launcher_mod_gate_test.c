/* Model policy only; no window, input simulation or gameplay state. */
#include <stdio.h>
static int native_save_read_fault, native_save_read_calls;
static FILE* native_save_failed_stream;
static size_t native_save_test_fread(void*,size_t,size_t,FILE*);
static int native_save_test_ferror(FILE*);
#define fread native_save_test_fread
#define ferror native_save_test_ferror
#include "launcher_model.c"
#undef fread
#undef ferror
static size_t native_save_test_fread(void* p,size_t width,size_t count,FILE* stream) {
    if (native_save_read_fault && ++native_save_read_calls == 2) {
        native_save_failed_stream=stream;return 0;
    }
    return fread(p,width,count,stream);
}
static int native_save_test_ferror(FILE* stream) {
    return native_save_read_fault && native_save_failed_stream==stream ? 1 : ferror(stream);
}
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
static int selected_profile;
static const char* profile_save(void* ctx,const char* rom) {
    assert(ctx==&selected_profile); assert(!strcmp(rom,"verified-disc.cue"));
    if(selected_profile==2)return NULL;
    if(selected_profile==3)return "rui-save-route-\xE8\xB3\x87\xE7\x94\xA3.fixture";
    return selected_profile ? "rui-save-route-de.fixture" : "rui-save-route-stock.fixture";
}
static void write_fixture(const char* path,const char* text) {
    FILE* file=fopen(path,"rb"); assert(!file); // never overwrite an unknown file
    file=fopen(path,"wb"); assert(file); assert(fwrite(text,1,strlen(text),file)==strlen(text));fclose(file);
}
static void expect_fixture(const char* path,const char* text) {
    char bytes[64]={0};FILE* file=fopen(path,"rb");assert(file);
    assert(fread(bytes,1,sizeof(bytes)-1,file)==strlen(text));fclose(file);assert(!strcmp(bytes,text));
}
static void test_profile_save_routing(void) {
    LauncherModel m={0};strcpy(m.rom_full,"verified-disc.cue");m.rom_present=true;
    m.sram_path="rui-save-route-stock.fixture";
    m.native_save_path_cb=profile_save;m.native_save_path_ctx=&selected_profile;
    write_fixture(m.sram_path,"stock progress");
    write_fixture("rui-save-route-de.fixture","DE progress");
    write_fixture("rui-save-route-import.fixture","imported progress");
    selected_profile=1;
    assert(!strcmp(launcher_model_native_save_path(&m),"rui-save-route-de.fixture"));
    launcher_model_import_sram(&m,"rui-save-route-import.fixture");
    expect_fixture(m.sram_path,"stock progress");
    expect_fixture("rui-save-route-de.fixture","imported progress");
    expect_fixture("rui-save-route-de.fixture.bak","DE progress");
    launcher_model_clear_sram(&m);
    expect_fixture(m.sram_path,"stock progress");
    expect_fixture("rui-save-route-de.fixture.bak","imported progress");
    selected_profile=2;assert(!launcher_model_native_save_path(&m));
    launcher_model_import_sram(&m,"rui-save-route-import.fixture");launcher_model_clear_sram(&m);
    expect_fixture(m.sram_path,"stock progress");
    selected_profile=0;assert(!strcmp(launcher_model_native_save_path(&m),m.sram_path));
    m.native_save_path_cb=NULL;assert(launcher_model_native_save_path(&m)==m.sram_path);
    assert(!remove(m.sram_path));assert(!remove("rui-save-route-de.fixture.bak"));assert(!remove("rui-save-route-import.fixture"));
}
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
static void test_failed_backup_and_unicode(void) {
    LauncherModel m={0};strcpy(m.rom_full,"verified-disc.cue");m.rom_present=true;
    m.sram_path="rui-save-route-stock.fixture";m.native_save_path_cb=profile_save;m.native_save_path_ctx=&selected_profile;
    write_fixture(m.sram_path,"stock kept");write_fixture("rui-save-route-de.fixture","DE kept");
    write_fixture("rui-save-route-import.fixture","replacement");
#if defined(_WIN32)
    assert(CreateDirectoryA("rui-save-route-de.fixture.bak",NULL));
#else
    assert(!mkdir("rui-save-route-de.fixture.bak",0700));
#endif
    selected_profile=1;
    launcher_model_import_sram(&m,"rui-save-route-import.fixture");launcher_model_clear_sram(&m);
    expect_fixture("rui-save-route-de.fixture","DE kept");expect_fixture(m.sram_path,"stock kept");
#if defined(_WIN32)
    assert(RemoveDirectoryA("rui-save-route-de.fixture.bak"));
    assert(CreateDirectoryA("rui-save-route-stock.fixture.bak",NULL));
#else
    assert(!rmdir("rui-save-route-de.fixture.bak"));assert(!mkdir("rui-save-route-stock.fixture.bak",0700));
#endif
    selected_profile=0;
    launcher_model_import_sram(&m,"rui-save-route-import.fixture");launcher_model_clear_sram(&m);
    expect_fixture(m.sram_path,"stock kept");expect_fixture("rui-save-route-de.fixture","DE kept");
#if defined(_WIN32)
    assert(RemoveDirectoryA("rui-save-route-stock.fixture.bak"));
    const wchar_t* wide=L"rui-save-route-\x8CC7\x7523.fixture";
    const wchar_t* wide_backup=L"rui-save-route-\x8CC7\x7523.fixture.bak";
    assert(GetFileAttributesW(wide)==INVALID_FILE_ATTRIBUTES);
    assert(GetFileAttributesW(wide_backup)==INVALID_FILE_ATTRIBUTES);
    FILE* f=_wfopen(wide,L"wb");assert(f);assert(fwrite("unicode kept",1,12,f)==12);assert(!fclose(f));
    selected_profile=3;
    const char* target=launcher_model_native_save_path(&m);
    assert(launcher_native_save_size(target)==12);
    launcher_model_import_sram(&m,"rui-save-route-import.fixture");
    char content[32]={0};f=_wfopen(wide,L"rb");assert(f);assert(fread(content,1,31,f)==11);fclose(f);assert(!strcmp(content,"replacement"));
    memset(content,0,sizeof(content));f=_wfopen(wide_backup,L"rb");assert(f);assert(fread(content,1,31,f)==12);fclose(f);assert(!strcmp(content,"unicode kept"));
    launcher_model_clear_sram(&m);assert(GetFileAttributesW(wide)==INVALID_FILE_ATTRIBUTES);
    memset(content,0,sizeof(content));f=_wfopen(wide_backup,L"rb");assert(f);assert(fread(content,1,31,f)==11);fclose(f);assert(!strcmp(content,"replacement"));
    assert(!_wremove(wide_backup));
#else
    assert(!rmdir("rui-save-route-stock.fixture.bak"));
#endif
    expect_fixture(m.sram_path,"stock kept");expect_fixture("rui-save-route-de.fixture","DE kept");
    selected_profile=1;
    assert(!remove("rui-save-route-de.fixture"));
    write_fixture("rui-save-route-de.fixture","current A");
    write_fixture("rui-save-route-de.fixture.bak","backup B");
    launcher_model_import_sram(&m,"rui-save-route-de.fixture.bak");
    expect_fixture("rui-save-route-de.fixture","backup B");
    expect_fixture("rui-save-route-de.fixture.bak","current A");
    assert(!remove("rui-save-route-de.fixture"));
    assert(!remove("rui-save-route-de.fixture.bak"));
    write_fixture("rui-save-route-de.fixture","DE kept");
    write_fixture("rui-save-route-de.fixture.bak","old backup kept");
    char large_source[9001];memset(large_source,'N',9000);large_source[9000]=0;
    write_fixture("rui-save-route-read-error.fixture",large_source);
    selected_profile=1;native_save_read_fault=1;native_save_read_calls=0;native_save_failed_stream=NULL;
    launcher_model_import_sram(&m,"rui-save-route-read-error.fixture");
    assert(native_save_failed_stream && native_save_read_calls==2);
    native_save_read_fault=0;
    expect_fixture("rui-save-route-de.fixture","DE kept");
    expect_fixture("rui-save-route-de.fixture.bak","old backup kept");
    expect_fixture(m.sram_path,"stock kept");
    FILE* staging=fopen("rui-save-route-de.fixture.import.tmp","rb");assert(!staging);
    assert(!remove("rui-save-route-read-error.fixture"));assert(!remove("rui-save-route-de.fixture.bak"));
    assert(!remove(m.sram_path));assert(!remove("rui-save-route-de.fixture"));assert(!remove("rui-save-route-import.fixture"));
}
int main(void) {
    test_profile_save_routing();
    test_failed_backup_and_unicode();

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
