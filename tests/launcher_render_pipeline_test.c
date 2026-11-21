/* Rendering pipeline rows (Render thread / Present thread / Smooth motion).
 * Smooth motion is stored under the settings key frame_generation (kept for
 * compatibility).
 *
 * Host-gated: a title or runtime that does not set
 * GameInfo.has_render_pipeline never shows them, and they are OpenGL only.
 * Present thread and Smooth motion only make sense with Render thread on,
 * so the model reports them as not editable while it is off. The values are
 * carried through launcher_model_init from the host's seed settings.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;
static void ok(int cond, const char* what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(void) {
    static LauncherModel m;

    memset(&m, 0, sizeof(m));
    m.s.renderer = 1;
    m.s.render_thread = 1;
    ok(!launcher_model_render_pipeline_offered(&m), "host without has_render_pipeline => rows hidden");
    ok(!launcher_model_render_pipeline_children_enabled(&m), "hidden => children not editable");

    m.has_render_pipeline = true;
    m.s.renderer = 0;
    ok(!launcher_model_render_pipeline_offered(&m), "software renderer => rows hidden");
    m.s.renderer = 2;
    ok(!launcher_model_render_pipeline_offered(&m), "vulkan renderer => rows hidden");

    m.s.renderer = 1;
    ok(launcher_model_render_pipeline_offered(&m), "OpenGL + host flag => rows shown");
    ok(launcher_model_render_pipeline_children_enabled(&m), "render thread on => children editable");
    m.s.render_thread = 0;
    m.s.present_thread = 1;
    m.s.frame_generation = 1;
    ok(!launcher_model_render_pipeline_children_enabled(&m),
       "render thread off => present thread / smooth motion disabled");
    ok(m.s.present_thread == 1 && m.s.frame_generation == 1,
       "disabling keeps the child choices for when render thread returns");
    ok(!launcher_model_render_pipeline_offered(NULL), "NULL model is safe");

    /* Host seed -> model -> commit round-trip, and the GameInfo flag. */
    {
        static RecompLauncherCSettings io, out;
        static RecompLauncherCGameInfo gi;
        static LauncherModel lm;
        memset(&io, 0, sizeof(io));
        memset(&gi, 0, sizeof(gi));
        io.renderer = 1;
        io.render_thread = 1;
        io.present_thread = 0;
        io.frame_generation = 1;
        gi.has_render_pipeline = 1;
        launcher_model_init(&lm, &io, &gi, NULL);
        ok(lm.has_render_pipeline, "GameInfo.has_render_pipeline reaches the model");
        ok(lm.s.render_thread == 1 && lm.s.present_thread == 0 && lm.s.frame_generation == 1,
           "seed values reach the model");
        lm.s.render_thread = 0;
        lm.s.present_thread = 1;
        memset(&out, 0, sizeof(out));
        launcher_model_commit(&lm, &out);
        ok(out.render_thread == 0 && out.present_thread == 1 && out.frame_generation == 1,
           "edits commit back to the host settings");
        gi.has_render_pipeline = 0;
        launcher_model_init(&lm, &io, &gi, NULL);
        ok(!launcher_model_render_pipeline_offered(&lm), "older host (flag 0) => rows hidden");
    }

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
