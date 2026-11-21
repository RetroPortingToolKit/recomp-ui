/* Renderer and VSync as LISTS, not cycles.
 *
 * These two rows were click-to-cycle buttons. With a host-supplied renderer
 * vocabulary of five entries, reaching the last one meant four clicks and
 * counting, so they became dropdowns -- which needs enumerate-and-set rather
 * than next(). The risk in that change is the three-way precedence the label
 * getter has always had (game labels, then console profile pair, then the
 * legacy pair): a dropdown that enumerates one vocabulary while the label
 * shows another would offer entries that do not match what is selected.
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
    static const char* const kLabels[] = {
        "Auto", "Direct3D 11", "Direct3D 9", "OpenGL", "Software",
    };

    /* ---- game-supplied vocabulary (the Gundam Wing case) ---------------- */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    m.renderer_labels = kLabels;
    m.num_renderers = 5;

    ok(launcher_model_renderer_count(&m) == 5, "count follows the game's vocabulary");
    ok(strcmp(launcher_model_renderer_label_at(&m, 0), "Auto") == 0,
       "index 0 is Auto");
    ok(strcmp(launcher_model_renderer_label_at(&m, 4), "Software") == 0,
       "the last entry is reachable directly, without cycling to it");

    launcher_model_set_renderer(&m, 4);
    ok(m.s.renderer == 4, "set lands on the requested index");
    ok(strcmp(launcher_model_renderer_label(&m), "Software") == 0,
       "and the label the combo shows agrees with what is selected");

    launcher_model_set_renderer(&m, 99);
    ok(m.s.renderer == 4, "an out-of-range index clamps rather than wrapping to Auto");
    launcher_model_set_renderer(&m, -1);
    ok(m.s.renderer == 0, "and clamps at the bottom too");

    /* ---- legacy pair: no game vocabulary, no profile -------------------- */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    ok(launcher_model_renderer_count(&m) == 2, "legacy surface enumerates two");
    ok(strcmp(launcher_model_renderer_label_at(&m, 0), "Software") == 0 &&
       strcmp(launcher_model_renderer_label_at(&m, 1), "OpenGL") == 0,
       "with the legacy pair, in the order the label getter uses");
    launcher_model_set_renderer(&m, 1);
    ok(strcmp(launcher_model_renderer_label(&m), "OpenGL") == 0,
       "label still agrees on the legacy surface");

    /* ---- vsync takes VALUES, not indices -------------------------------- */
    memset(&m, 0, sizeof(m));
    m.has_vsync = true;
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_ADAPTIVE);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_ADAPTIVE &&
       strcmp(launcher_model_vsync_label(&m), "Adaptive") == 0,
       "adaptive is selectable directly");
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_OFF);
    ok(strcmp(launcher_model_vsync_label(&m), "Off") == 0, "and Off");
    launcher_model_set_vsync(&m, 12345);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_OFF,
       "a value outside the vocabulary is refused, not stored");

    /* ---- host-supplied IDS: what the ENGINE is told, not an index ------
     * The n64lle case. The port declares {id,label} pairs; the committed
     * choice round-trips by NAME so a settings file survives the host
     * reordering or adding a backend. */
    static const char* const kIds[]  = { "software", "opengl" };
    static const char* const kIdLbls[] = { "Software (reference)",
                                           "OpenGL (experimental)" };
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    m.renderer_ids = kIds; m.renderer_labels = kIdLbls; m.num_renderers = 2;
    m.renderer_note = "Applies when the game starts.";

    ok(launcher_model_renderer_offered(&m), "a declared vocabulary composes");
    ok(launcher_model_renderer_count(&m) == 2, "and enumerates what was declared");
    launcher_model_set_renderer(&m, 1);
    ok(strcmp(launcher_model_renderer_id(&m), "opengl") == 0,
       "picking by index commits the host's ID, which is what the engine reads");
    ok(strcmp(launcher_model_renderer_label(&m), "OpenGL (experimental)") == 0,
       "while the label stays the thing a human reads");
    launcher_model_set_renderer_id(&m, "software");
    ok(m.s.renderer == 0 && strcmp(launcher_model_renderer_id(&m), "software") == 0,
       "and selecting by ID is the inverse");
    launcher_model_set_renderer_id(&m, "vulkan");
    ok(m.s.renderer == 0, "an ID this host never declared selects nothing");
    ok(strcmp(launcher_model_renderer_note(&m), "Applies when the game starts.") == 0,
       "the note is the host's, passed through verbatim");

    /* An OLDER settings file has no renderer_id at all: it must load and
     * default, not fail. That is a memset-clean s.renderer_id. */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    m.renderer_ids = kIds; m.renderer_labels = kIdLbls; m.num_renderers = 2;
    launcher_model_apply_renderer_settings(&m);
    ok(m.s.renderer == 0 && strcmp(m.s.renderer_id, "software") == 0,
       "a settings file predating the key defaults to entry 0 and gains the ID");

    /* A saved ID the host has since REORDERED still lands on the same
     * renderer, which is the whole reason the ID is persisted, not the
     * index. Same file, list reversed. */
    {
        static const char* const kRev[] = { "opengl", "software" };
        memset(&m, 0, sizeof(m));
        m.has_renderer = true;
        m.renderer_ids = kRev; m.num_renderers = 2;
        strcpy(m.s.renderer_id, "software");
        m.s.renderer = 0;                       /* the OLD index for software */
        launcher_model_apply_renderer_settings(&m);
        ok(m.s.renderer == 1, "a reordered list is followed by name, not by index");
    }

    /* ---- ZERO renderers: absent, never an empty dropdown ---------------- */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    m.renderer_ids = kIds; m.num_renderers = 0;
    ok(launcher_model_renderer_count(&m) == 0, "zero declared is zero, not the legacy pair");
    ok(!launcher_model_renderer_offered(&m), "and the control does not compose at all");
    launcher_model_set_renderer(&m, 0);
    ok(m.s.renderer_id[0] == 0, "with nothing offered, nothing is committed");

    /* A host that does not offer the control must not be edited by it. */
    memset(&m, 0, sizeof(m));
    launcher_model_set_renderer(&m, 3);
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_ON);
    ok(m.s.renderer == 0 && m.s.vsync == 0,
       "neither setter writes when the console does not have the control");

    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails;
}
