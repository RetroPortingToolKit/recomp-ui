#include "launcher_model.c"
#include <assert.h>
void launcher_binds_set_zapper(int a,int b) {(void)a;(void)b;}
static int calls;
static int count(void *ctx) {(void)ctx;return 1;}
static int get(void *ctx,int i,RecompLauncherCModPreset *out) {
 (void)ctx;if(i)return 0;memset(out,0,sizeof(*out));strcpy(out->id,"test");strcpy(out->name,"Test");return 1;
}
static const char *current(void *ctx,const RecompLauncherCSettings *s) {(void)ctx;return s->msu1_enabled?"test":"";}
static int apply(void *ctx,const char *id,RecompLauncherCSettings *s) {
 (void)ctx;assert(!strcmp(id,"test"));++calls;s->msu1_enabled=1;return 1;
}
int main(void) {
 static LauncherModel m;
 assert(!launcher_model_preset_count(&m));assert(!launcher_model_apply_preset(&m,"test"));
 RecompLauncherCModProvider p={0};m.mods=&p;
 assert(!launcher_model_preset_count(&m));
 p.preset_count=count;p.preset_get=get;p.preset_current=current;
 assert(!launcher_model_preset_count(&m));
 p.preset_apply=apply;
 assert(launcher_model_preset_count(&m)==1);
 m.s.volume=42;m.s.fullscreen=2;strcpy(m.s.msu1_dir,"keep this music folder");
 assert(!launcher_model_apply_preset(&m,"missing") && !calls);
 assert(launcher_model_apply_preset(&m,"test") && calls==1 && m.s.msu1_enabled);
 assert(m.s.volume==42 && m.s.fullscreen==2);
 assert(!strcmp(m.s.msu1_dir,"keep this music folder"));
 static const RecompLauncherCMsuPack packs[]={{"first","First"},{"second","Second"}};
 assert(!launcher_model_set_msu1_pack(&m,"first"));
 m.msu1_supported=true;m.msu1_packs=packs;m.num_msu1_packs=2;
 assert(launcher_model_set_msu1_pack(&m,"second"));assert(!strcmp(m.s.msu1_pack,"second"));
 assert(!launcher_model_set_msu1_pack(&m,"unknown"));assert(!strcmp(m.s.msu1_pack,"second"));
 assert(launcher_model_set_msu1_pack(&m,""));assert(!m.s.msu1_pack[0]);
 assert(!strcmp(m.s.msu1_dir,"keep this music folder"));
 puts("Optional presets and soundtrack choices passed");return 0;
}
