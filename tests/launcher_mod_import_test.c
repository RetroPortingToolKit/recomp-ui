/* Background owner-resource imports must not replace the primary game image,
 * overlap another job, or change setup readiness. Tests the real worker/poll. */
#include "launcher_model.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void launcher_binds_set_zapper(int a,int b) {(void)a;(void)b;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int entered,released,success;
static void pause_thread(void) {
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}
static int import_resource(void *ctx,const char *pkg,const char *feature,
    const char *resource,const char *path,const char *image,
    void (*progress)(void *,float,const char *),void *user,char *error,size_t cap)
{
    (void)ctx;
    CHECK(!strcmp(pkg,"game.cd") && !strcmp(feature,"campaign"));
    CHECK(!strcmp(resource,"disc") && !strcmp(path,"owned & disc.sgcd"));
    CHECK(!strcmp(image,"s3k.bin"));
    progress(user,0.5f,"Importing music");
    prep_lock();entered++;prep_unlock();
    for(;;){prep_lock();int ready=released;prep_unlock();if(ready)break;pause_thread();}
    if(!success)snprintf(error,cap,"Owned input rejected; previous resources retained.");
    return success;
}
int main(void)
{
    RecompLauncherCModProvider provider={0};provider.feature_resource_import=import_resource;
    LauncherModel *m=calloc(1,sizeof *m);CHECK(m);m->mods=&provider;
    strcpy(m->rom_full,"s3k.bin");m->setup_prepare_satisfied=false;
    CHECK(!m->setup_wizard_supported); /* cartridge launcher, no first-run wizard */
    for(success=1;success>=0;success--) {
        prep_lock();entered=released=0;prep_unlock();
        launcher_model_import_mod_resource(m,"game.cd","campaign","disc","owned & disc.sgcd");
        CHECK(m->setup_preparing);
        launcher_model_import_mod_resource(m,"game.cd","campaign","disc","owned & disc.sgcd");
        for(int n=0;n<5000;n++){
            prep_lock();int ready=entered;prep_unlock();if(ready)break;pause_thread();
        }
        prep_lock();CHECK(entered==1);prep_unlock();
        launcher_model_poll_prepare_disc(m);
        CHECK(m->setup_preparing && m->setup_prepare_fraction==0.5f);
        CHECK(!strcmp(m->setup_status,"Importing music"));
        prep_lock();released=1;prep_unlock();
        for(int n=0;n<5000&&m->setup_preparing;n++){pause_thread();launcher_model_poll_prepare_disc(m);}
        CHECK(!m->setup_preparing && !m->setup_prepare_satisfied);
        CHECK(!strcmp(m->rom_full,"s3k.bin"));
        CHECK(strstr(m->mod_status,success?"Content imported":"Owned input rejected"));
    }
    char long_path[RECOMP_LAUNCHER_MOD_PATH_MAX+1];memset(long_path,'a',sizeof long_path-1);long_path[sizeof long_path-1]=0;
    launcher_model_import_mod_resource(m,"game.cd","campaign","disc",long_path);
    CHECK(!m->setup_preparing && strstr(m->mod_status,"too long"));
    m->rom_full[0]=0;
    launcher_model_import_mod_resource(m,"game.cd","campaign","disc","owned.sgcd");
    CHECK(!m->setup_preparing && strstr(m->mod_status,"Select your game image"));
    free(m);puts("Background resource import: success/failure, progress, exclusivity and image preservation passed");return 0;
}
