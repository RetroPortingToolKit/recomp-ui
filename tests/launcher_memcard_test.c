/* Memory-card slot facts the Save panel paints.
 *
 * Includes the model translation unit (like launcher_discs_test.c) to reach
 * lm_inspect_memcard, the static helper that decides whether a slot has REAL
 * block data from the host's memcard_inspect callback.
 *
 * Regression it exists for: a host reopened the launcher after a netplay
 * match with empty memcard paths. Nothing was inspected, so the panel fell
 * back to a representative placeholder pattern (3 blocks on card 1, 2 on
 * card 2) that read as foreign save data — on cards the player knew were
 * blank. A host that inspects real cards must never be shown a made-up grid.
 * The enabled flag had the sibling defect: 0 meant "unset -> on", so a slot
 * the user switched off could not be shown (or kept) off.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to memory cards. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static int  inspect_calls;
static char inspect_last_path[512];
static int  inspect_result;      /* what the fake host returns */
static int  inspect_used_blocks; /* blocks 0..n-1 reported occupied */
static int  inspect_valid;

static int fake_inspect(const char* card_path, RecompLauncherCMemcard* out) {
    ++inspect_calls;
    snprintf(inspect_last_path, sizeof(inspect_last_path), "%s", card_path);
    if (!inspect_result) return 0;
    memset(out, 0, sizeof(*out));
    out->valid = inspect_valid;
    out->used_blocks = inspect_used_blocks;
    for (int i = 0; i < inspect_used_blocks && i < 15; ++i) out->block_used[i] = 1;
    return 1;
}

static LauncherModel g_m;

static void reset_model(void) {
    memset(&g_m, 0, sizeof(g_m));
    inspect_calls = 0;
    inspect_last_path[0] = '\0';
    inspect_result = 1;
    inspect_used_blocks = 0;
    inspect_valid = 1;
}

static void test_blocks_used_inspected_wins(void) {
    reset_model();
    g_m.memcard_inspect_cb = fake_inspect;
    inspect_used_blocks = 4;
    safe_copy(g_m.s.memcard_path[0], sizeof(g_m.s.memcard_path[0]), "/saves/card1.mcd");
    lm_inspect_memcard(&g_m, 0);
    expect(inspect_calls == 1 && !strcmp(inspect_last_path, "/saves/card1.mcd"),
           "inspect: callback runs against the slot's bound path");
    expect(g_m.memcard_inspected[0] && g_m.memcard_valid[0],
           "inspect: a successful callback marks the slot inspected + valid");
    expect(launcher_model_memcard_blocks_used(&g_m, 0) == 0x000Fu,
           "blocks_used: inspected slot paints exactly the reported blocks");
}

static void test_blocks_used_empty_path_with_inspector_is_blank(void) {
    reset_model();
    g_m.memcard_inspect_cb = fake_inspect;
    /* No path bound (the post-match launcher seed bug): nothing to inspect. */
    lm_inspect_memcard(&g_m, 0);
    lm_inspect_memcard(&g_m, 1);
    expect(inspect_calls == 0,
           "inspect: an empty path never reaches the host callback");
    expect(!g_m.memcard_inspected[0] && !g_m.memcard_inspected[1],
           "inspect: an empty path leaves the slot un-inspected");
    expect(launcher_model_memcard_blocks_used(&g_m, 0) == 0 &&
           launcher_model_memcard_blocks_used(&g_m, 1) == 0,
           "blocks_used: a wired inspector with nothing inspected paints BLANK, "
           "never the placeholder pattern");
}

static void test_blocks_used_declined_inspect_is_blank(void) {
    reset_model();
    g_m.memcard_inspect_cb = fake_inspect;
    inspect_result = 0; /* host could not read the file */
    safe_copy(g_m.s.memcard_path[1], sizeof(g_m.s.memcard_path[1]), "/saves/card2.mcd");
    lm_inspect_memcard(&g_m, 1);
    expect(inspect_calls == 1 && !g_m.memcard_inspected[1],
           "inspect: a declining callback leaves the slot un-inspected");
    expect(launcher_model_memcard_blocks_used(&g_m, 1) == 0,
           "blocks_used: a declined inspect paints blank on an inspecting host");
}

static void test_blocks_used_freshly_formatted_is_blank(void) {
    reset_model();
    /* No inspector at all (proto launcher), but the card was just formatted. */
    g_m.memcard_freshly_formatted[0] = true;
    g_m.memcard_blocks_used[0] = 0x7FFFu; /* stale; must be ignored */
    expect(launcher_model_memcard_blocks_used(&g_m, 0) == 0,
           "blocks_used: a freshly formatted card paints blank");
}

static void test_blocks_used_placeholder_only_without_inspector(void) {
    reset_model();
    /* Proto launcher: no inspector, no profile probe -> representative pattern. */
    expect(launcher_model_memcard_blocks_used(&g_m, 0) == 0x0025u &&
           launcher_model_memcard_blocks_used(&g_m, 1) == 0x0009u,
           "blocks_used: the placeholder pattern is reserved for hosts with no inspector");
    expect(launcher_model_memcard_blocks_used(&g_m, 2) == 0 &&
           launcher_model_memcard_blocks_used(NULL, 0) == 0,
           "blocks_used: out-of-range slot / NULL model paint nothing");
}

static void test_enabled_tristate(void) {
    expect(lm_memcard_enabled_from_host(0) == 1,
           "enabled: 0 (host predates the field) defaults to on");
    expect(lm_memcard_enabled_from_host(1) == 1, "enabled: 1 stays on");
    expect(lm_memcard_enabled_from_host(-1) == 0,
           "enabled: -1 lands as OFF instead of re-arming as on");
    /* The model's own toggle keeps working on the normalized 0/1. */
    reset_model();
    g_m.s.memcard_enabled[0] = 0;
    launcher_model_toggle_memcard(&g_m, 0);
    expect(g_m.s.memcard_enabled[0] == 1, "enabled: toggle off -> on");
    launcher_model_toggle_memcard(&g_m, 0);
    expect(g_m.s.memcard_enabled[0] == 0, "enabled: toggle on -> off");
}

/* A game that only ever talks to port 1 hides slot 2; the profile still caps it. */
static void test_slot_count_game_override(void) {
    reset_model();
    g_m.memcard_slots = 1;
    expect(launcher_model_memcard_slot_count(&g_m, 2) == 1, "game asks for 1 slot");
}

static void test_slot_count_unset_uses_profile(void) {
    reset_model();
    expect(launcher_model_memcard_slot_count(&g_m, 2) == 2, "unset keeps the profile's 2 slots");
}

static void test_slot_count_capped_by_profile(void) {
    reset_model();
    g_m.memcard_slots = 5;
    expect(launcher_model_memcard_slot_count(&g_m, 2) == 2, "game cannot exceed the profile");
}

int main(void) {
    test_slot_count_game_override();
    test_slot_count_unset_uses_profile();
    test_slot_count_capped_by_profile();
    test_blocks_used_inspected_wins();
    test_blocks_used_empty_path_with_inspector_is_blank();
    test_blocks_used_declined_inspect_is_blank();
    test_blocks_used_freshly_formatted_is_blank();
    test_blocks_used_placeholder_only_without_inspector();
    test_enabled_tristate();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("all memcard slot checks passed\n");
    return 0;
}
