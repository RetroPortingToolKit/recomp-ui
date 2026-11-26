/* Header-only resolver fixture; no SDL window or retail cartridge needed. */
#undef NDEBUG
#include "launcher_system.h"
#include "launcher_profile.h"
#include <assert.h>
#include <string.h>
int main(void){
 RecompLauncherCGameInfo sms={0},gg={0};
 assert(launcher_profile_apply("sms",&sms));assert(launcher_profile_apply("gg",&gg));
 assert(!strcmp(sms.platform,"MASTER SYSTEM"));assert(!strcmp(gg.platform,"GAME GEAR"));
 const SystemProfile *a=launcher_system_infer(&sms),*b=launcher_system_infer(&gg);
 assert(a==b&&a->controller.button_count==7);
 assert(a->controller.buttons[4].code==4&&!strcmp(a->controller.buttons[4].label,"Button 1"));
 assert(a->controller.buttons[5].code==5&&!strcmp(a->controller.buttons[5].label,"Button 2"));
 assert(a->controller.buttons[6].code==6);
 assert(a->hotkeys_mask==0&&a->rom_filter.pattern_count==2);
 return 0;
}
