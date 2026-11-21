#include "launcher_files.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(expr) do { if(!(expr)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#expr);exit(1); } } while(0)
int main(void) {
    char out[32]="old selection";
    const char* patterns[]={"*.bin"};
    CHECK(!launcher_native_file_picker_available());
    CHECK(launcher_try_pick_file("BIOS",patterns,1,"BIOS",out,sizeof(out))==-1);
    CHECK(out[0]=='\0');
    strcpy(out,"old selection");
    CHECK(launcher_try_pick_folder("Directory",out,sizeof(out))==-1);
    CHECK(out[0]=='\0');
    strcpy(out,"old selection");
    CHECK(launcher_try_pick_save_file("State",NULL,0,NULL,out,sizeof(out))==-1);
    CHECK(out[0]=='\0');
    CHECK(!launcher_pick_rom(out,sizeof(out)));
    CHECK(launcher_try_pick_file(NULL,NULL,0,NULL,NULL,0)==-1);
    puts("Android selects the built-in browser without desktop helper calls");
    return 0;
}
