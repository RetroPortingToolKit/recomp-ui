#include "launcher_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void) {
    const SystemProfile* systems[] = {&kSystemProfileGb, &kSystemProfileGbc};
    for (int i = 0; i < 2; ++i) {
        RecompLauncherCGameInfo game = {0};
        launcher_profile_apply(systems[i]->id, &game);
        CHECK(game.has_bios == 0); /* Existing hosts retain their availability choice. */
        CHECK(systems[i]->video.bios == 1);
        int found = 0;
        for (const char* const* panel = systems[i]->panels_settings; *panel; ++panel)
            found |= !strcmp(*panel, "system");
        CHECK(found); /* A real-BIOS host can expose the shared System picker. */
    }
    return 0;
}
