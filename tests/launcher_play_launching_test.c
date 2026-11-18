/* The PLAY press, frame by frame (launcher_model_launch_*).
 *
 * The launch work (image preparation, the host's mod commit) blocks the frame
 * it runs in, and a host's commit can read a whole disc image: 20 seconds and
 * more. Done in the frame of the click, the window stood still showing PLAY
 * for that whole time. The contract tested here:
 *
 *   frame N    the click only asks;
 *   frame N+1  the button is drawn greyed, "Launching...", and no work runs;
 *   frame N+2  the work is handed out, exactly once, while that picture is on
 *              screen;
 *   after      a refused launch returns the button to PLAY; a started launch
 *              keeps the greyed label until the window closes.
 *
 * The backend calls launcher_model_launch_due() once per frame before it
 * draws, and draws the button from launcher_model_launch_announced(). */
#include "launcher_model.h"

#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    (void)mouse_enabled;
    (void)crosshair;
}

static int fails;
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

static void fresh(LauncherModel *model) {
    static RecompLauncherCSettings settings;
    static RecompLauncherCGameInfo game;
    memset(&settings, 0, sizeof(settings));
    memset(&game, 0, sizeof(game));
    game.name = "Play launching test";
    launcher_model_init(model, &settings, &game, NULL);
}

int main(void) {
    LauncherModel m;

    /* Nothing asked: the button is PLAY and no frame starts any work. */
    fresh(&m);
    check(m.launch_pending == LNG_LAUNCH_IDLE, "a new model has no launch pending");
    check(!launcher_model_launch_announced(&m), "PLAY is not greyed before a click");
    check(!launcher_model_launch_due(&m) && !launcher_model_launch_due(&m),
          "frames without a click start no launch work");
    check(m.launch_pending == LNG_LAUNCH_IDLE, "idle frames leave the state idle");

    /* Frame N: the click. */
    launcher_model_launch_request(&m);
    check(launcher_model_launch_announced(&m),
          "the button is greyed from the click on");
    check(m.action == LNG_ACTION_NONE, "the click itself launches nothing");

    /* Frame N+1: draws "Launching...", no work. */
    check(!launcher_model_launch_due(&m),
          "the frame after the click draws the greyed button and starts no work");
    check(launcher_model_launch_announced(&m),
          "the greyed button is drawn in the frame after the click");

    /* Frame N+2: the work, once. */
    check(launcher_model_launch_due(&m),
          "the second frame after the click starts the launch work");
    check(!launcher_model_launch_due(&m),
          "the launch work is handed out once, not once per call");
    check(launcher_model_launch_announced(&m),
          "the button stays greyed while the work runs");

    /* The work refused (a mod plan that cannot be applied, an image that
     * cannot be prepared): back to PLAY, and PLAY works again. */
    launcher_model_launch_finish(&m, false);
    check(!launcher_model_launch_announced(&m) && m.launch_pending == LNG_LAUNCH_IDLE,
          "a refused launch returns the button to PLAY");
    check(!launcher_model_launch_due(&m), "a refused launch is not retried by itself");
    launcher_model_launch_request(&m);
    check(!launcher_model_launch_due(&m) && launcher_model_launch_due(&m),
          "PLAY can be pressed again after a refused launch");

    /* The work launched: the label stays for the frames the window still
     * lives, and nothing starts a second launch. */
    m.action = LNG_ACTION_LAUNCH;
    launcher_model_launch_finish(&m, true);
    check(launcher_model_launch_announced(&m),
          "a started launch keeps \"Launching...\" until the window closes");
    launcher_model_launch_request(&m);
    check(!launcher_model_launch_due(&m) && !launcher_model_launch_due(&m),
          "nothing starts a second launch after a started one");

    /* A second click before the work ran is one request, not two. */
    fresh(&m);
    launcher_model_launch_request(&m);
    launcher_model_launch_request(&m);
    check(!launcher_model_launch_due(&m), "double click: first frame draws only");
    launcher_model_launch_request(&m);
    check(launcher_model_launch_due(&m), "double click: the work starts in the second frame");
    check(!launcher_model_launch_due(&m), "double click: the work starts once");

    /* The window was closed between the click and the work: the close wins,
     * no launch work runs. */
    fresh(&m);
    launcher_model_launch_request(&m);
    check(!launcher_model_launch_due(&m), "close case: first frame draws only");
    m.action = LNG_ACTION_QUIT;
    check(!launcher_model_launch_due(&m),
          "an action taken before the work runs cancels the launch work");

    /* NULL is harmless. */
    launcher_model_launch_request(NULL);
    launcher_model_launch_finish(NULL, true);
    check(!launcher_model_launch_announced(NULL) && !launcher_model_launch_due(NULL),
          "a NULL model asks for nothing");

    if (fails) return 1;
    printf("ok: PLAY is answered two frames after the click, behind a greyed \"Launching...\"\n");
    return 0;
}
