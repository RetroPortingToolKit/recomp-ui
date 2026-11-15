/* Host-backed selection failures must preserve the active device and boot. */
#include "launcher_model.c"
#include <assert.h>
#include <time.h>
void launcher_binds_set_zapper(int a,int b) {(void)a;(void)b;}
static int calls,apply_accepted;
static int import_ok,import_notified;
#ifdef _WIN32
static DWORD ui_thread;
static void mark_ui(void) {ui_thread=GetCurrentThreadId();}
static int on_ui(void) {return ui_thread==GetCurrentThreadId();}
static unsigned long long ticks(void) {return GetTickCount64();}
static void pause_test(void) {Sleep(1);}
#else
static pthread_t ui_thread;
static void mark_ui(void) {ui_thread=pthread_self();}
static int on_ui(void) {return pthread_equal(ui_thread,pthread_self());}
static unsigned long long ticks(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void pause_test(void) {struct timespec t={0,1000000};nanosleep(&t,NULL);}
#endif
static int import_system(const char* source,char* out,size_t out_cap,char* error,size_t cap) {
    assert(!strcmp(source,"owned-system-source"));
    if(!import_ok) {snprintf(error,cap,"Missing required system content.");return 0;}
    FILE* f=fopen("peripheral-import-boot.app","wb");assert(f);
    assert(fwrite("verified fixture",1,16,f)==16);assert(!fclose(f));
    snprintf(out,out_cap,"peripheral-import-boot.app");return 1;
}
static void imported(const char* path) {
    assert(on_ui());
    assert(!strcmp(path,"peripheral-import-boot.app"));++import_notified;
}
static void wait_import(LauncherModel* m) {
    const unsigned long long deadline=ticks()+10000;
    while(m->setup_preparing && ticks()<deadline) {
        pause_test();launcher_model_poll_prepare_disc(m);
    }
    assert(!m->setup_preparing);
}
static int apply(void* ctx,int slot,const RecompLauncherCPeripheralConfig* c,char* error,size_t cap) {
    assert(ctx==&calls && slot==0);++calls;
    if(!apply_accepted)snprintf(error,cap,"Image is in use.");
    return apply_accepted;
}
int main(void) {
    LauncherModel* m=calloc(1,sizeof *m);assert(m);
    m->rom_present=true;strcpy(m->rom_size,"256 bytes");
    assert(launcher_model_can_launch(m)); /* old hosts keep their behavior */
    const RecompLauncherCPeripheralKind kinds[]={{"None",0},{"Keyboard",0},{"Image",1}};
    RecompLauncherCPeripheral p={0};p.kinds=kinds;p.num_kinds=3;p.config.kind=1;p.ready=1;
    m->peripherals=&p;m->num_peripherals=1;m->peripheral_apply_cb=apply;m->peripheral_ctx=&calls;
    RecompLauncherCPeripheralConfig next={0};next.kind=2;next.read_only=1;
    assert(!launcher_model_set_peripheral(m,0,&next) && calls==0 && p.config.kind==1);
    strcpy(next.image_path,"storage.img");
    assert(!launcher_model_set_peripheral(m,0,&next) && calls==1);
    assert(p.config.kind==1 && !p.config.image_path[0] && strstr(p.error,"in use"));
    assert(launcher_model_can_launch(m)); /* failed edit does not break old mount */
    p.ready=0;assert(!launcher_model_can_launch(m)); /* saved missing image */
    apply_accepted=1;assert(launcher_model_set_peripheral(m,0,&next));
    assert(calls==2 && p.ready && !p.error[0] && p.config.kind==2 && p.config.read_only);
    assert(!strcmp(p.config.image_path,"storage.img") && launcher_model_can_launch(m));
    next.kind=9;assert(!launcher_model_set_peripheral(m,0,&next) && calls==2);
    assert(!launcher_model_set_peripheral(m,1,&next));
    /* A failed system import must retain the installed boot, identity and
     * launchability; the completion hook runs only after a successful join. */
    m->system_media_title="System files";
    m->prepare_disc_cb=import_system;m->system_media_imported=imported;
    strcpy(m->rom_full,"working-boot.app");mark_ui();
    launcher_model_start_prepare_disc(m,"owned-system-source");
    assert(!strcmp(m->rom_full,"working-boot.app"));wait_import(m);
    assert(!strcmp(m->rom_full,"working-boot.app") && !import_notified);
    assert(strstr(m->setup_error,"Missing required") && launcher_model_can_launch(m));
    import_ok=1;launcher_model_start_prepare_disc(m,"owned-system-source");wait_import(m);
    assert(import_notified==1 && !strcmp(m->rom_full,"peripheral-import-boot.app"));
    assert(!m->setup_error[0] && launcher_model_can_launch(m));
    assert(!remove("peripheral-import-boot.app"));
    free(m);return 0;
}
