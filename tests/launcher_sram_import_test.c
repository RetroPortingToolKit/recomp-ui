#include "launcher_model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void launcher_binds_set_zapper(int mouse_enabled,int crosshair) {
    (void)mouse_enabled;(void)crosshair;
}

static void write_bytes(const char* path,size_t size,int value) {
    FILE* file=fopen(path,"wb");assert(file);
    for(size_t i=0;i<size;++i)assert(fputc(value,file)!=EOF);
    assert(fclose(file)==0);
}
static void check_bytes(const char* path,size_t size,int value) {
    FILE* file=fopen(path,"rb");assert(file);
    for(size_t i=0;i<size;++i)assert(fgetc(file)==value);
    assert(fgetc(file)==EOF);fclose(file);
}
int main(int argc,char** argv) {
    assert(argc==2);char src[1024],dst[1024],bak[1040],shorter[1024],large[1024];
    snprintf(src,sizeof(src),"%s/source.sav",argv[1]);snprintf(dst,sizeof(dst),"%s/native.sav",argv[1]);
    snprintf(bak,sizeof(bak),"%s.bak",dst);snprintf(shorter,sizeof(shorter),"%s/truncated.sav",argv[1]);
    snprintf(large,sizeof(large),"%s/container.sav",argv[1]);
    write_bytes(src,131072,0x41);write_bytes(dst,131072,0x52);
    write_bytes(shorter,31,0x71);write_bytes(large,131073,0x81);
    LauncherModel* model=(LauncherModel*)calloc(1,sizeof(*model));assert(model);
    model->sram_path=dst;model->native_save_bytes=131072;
    launcher_model_import_sram(model,src);
    assert(strstr(model->sram_status,"Imported"));check_bytes(dst,131072,0x41);check_bytes(bak,131072,0x52);
    launcher_model_import_sram(model,shorter);
    assert(strstr(model->sram_status,"rejected"));check_bytes(dst,131072,0x41);check_bytes(bak,131072,0x52);
    launcher_model_import_sram(model,large);
    assert(strstr(model->sram_status,"rejected"));check_bytes(dst,131072,0x41);
    /* Selecting the current save must never truncate it while it is read. */
    launcher_model_import_sram(model,dst);
    assert(strstr(model->sram_status,"Imported"));check_bytes(dst,131072,0x41);check_bytes(bak,131072,0x41);
    write_bytes(src,131072,0xaa);launcher_model_import_sram(model,src);
    assert(strstr(model->sram_status,"Imported"));check_bytes(dst,131072,0xaa);
    free(model);return 0;
}
