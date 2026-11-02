/* Controller-page writes must reach their selected Genesis seat and file. */
#include "launcher_model.h"
#include "launcher_binds.h"
#include "launcher_profile.h"
#include "launcher_sdlcompat.h"
#include "consoles/genesis/genesis_binds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main(int argc,char **argv) {
    char path[1024]; snprintf(path,sizeof path,"%s/settings.ini",argc>1?argv[1]:".");
    remove(path);
    RecompLauncherCSettings io={0}; RecompLauncherCGameInfo gi={0};
    launcher_profile_apply("genesis",&gi); gi.name="Genesis bindings fixture"; gi.num_players=6;
    LauncherModel *m=calloc(1,sizeof *m); CHECK(m);
    launcher_model_init(m,&io,&gi,""); launcher_binds_load(m,NULL,path);
    for (int p=2;p<6;++p) {
        launcher_model_set_pad_mode(m,p,1);
        CHECK(m->s.pad_mode[p]==1 && m->s.pad_mode[1]==0);
        CHECK(launcher_model_active_button_count(m,p)==12);
        CHECK(launcher_model_active_button_count(m,1)==8);
        launcher_binds_set_button(m,p+1,4,40+p);
        CHECK(rui_genesis_binds_get_key(path,p,4)==40+p);
        launcher_binds_set_pad_button(m,p+1,4,RUI_GEN_BIND_BUTTON,5,0);
        int kind,code,dir; rui_genesis_binds_get_pad(path,p,4,&kind,&code,&dir);
        CHECK(kind==RUI_GEN_BIND_BUTTON && code==5 && dir==0);
    }
    /* Reload from disk; the store's in-memory echo alone is insufficient. */
    launcher_binds_load(m,NULL,path);
    for(int p=2;p<6;++p) {
        int kind,code,dir; rui_genesis_binds_get_pad(path,p,4,&kind,&code,&dir);
        CHECK(rui_genesis_binds_get_key(path,p,4)==40+p && kind==RUI_GEN_BIND_BUTTON && code==5);
    }
    launcher_binds_reset_player(m,6); launcher_binds_load(m,NULL,path);
    CHECK(rui_genesis_binds_get_key(path,5,4)==0);
    CHECK(rui_genesis_binds_get_key(path,2,4)==42);
    CHECK(rui_genesis_binds_get_key(path,0,4)==SDL_SCANCODE_Z);
    int kind,code,dir; rui_genesis_binds_get_pad(path,5,4,&kind,&code,&dir);
    CHECK(kind==RUI_GEN_BIND_BUTTON && code==1);
    free(m); remove(path); puts("PASS Genesis P3-P6 bindings, reset, mode isolation and persistence"); return 0;
}
