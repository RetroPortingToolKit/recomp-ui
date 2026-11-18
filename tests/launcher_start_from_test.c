/* "Start from" on the dashboard: what the next PLAY starts with.
 *
 * Power on (the default), one of the title's save states or one of its
 * replays. The model holds
 * the list a lister gave it and the choice; this file checks what is listed,
 * what the choice does to the disc to boot and what is handed to the host.
 *
 * Includes the model translation unit, as launcher_pad_mode_test.c does.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect_int(int got, int want, const char* what) {
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
    ++fails;
}

enum { PSX = 0, SNES = 1 };

static const RecompLauncherCDisc kDiscs[3] = {
    { 1, NULL, "set_disc1.cue" }, { 2, NULL, "set_disc2.cue" }, { 3, NULL, "set_disc3.cue" },
};

static LauncherModel* session(int console, int discs) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    if (console == PSX) launcher_profile_apply_psx(&game);
    else launcher_profile_apply_snes(&game);
    game.name = "Start From Fixture";
    game.num_players = 2;
    if (discs > 1) { game.discs = kDiscs; game.num_discs = discs; }
#ifdef RECOMP_LAUNCHER_HAS_RECORD_REPLAY
    game.has_record_replay = console == PSX;
#endif
    memset(&io, 0, sizeof(io));
    launcher_model_init(m, &io, &game, discs > 1 ? kDiscs[0].path : NULL);
    return m;
}

static LauncherStartEntry state(int slot, int disc, const char* label) {
    LauncherStartEntry e;
    memset(&e, 0, sizeof(e));
    e.kind = LNG_START_STATE;
    e.slot = slot;
    e.disc = disc;
    snprintf(e.label, sizeof(e.label), "%s", label);
    snprintf(e.path, sizeof(e.path), "saves/scph1001/state_80010000_slot%02d.pst", slot);
    return e;
}

static LauncherStartEntry replay(const char* path, const char* label) {
    LauncherStartEntry e;
    memset(&e, 0, sizeof(e));
    e.kind = LNG_START_REPLAY;
    e.slot = -1;
    e.power_on = 1;
    e.frames = 600;
    snprintf(e.label, sizeof(e.label), "%s", label);
    snprintf(e.path, sizeof(e.path), "%s", path);
    return e;
}

static void test_which_console_has_it(void) {
    LauncherModel* m = session(PSX, 1);
    if (!m) return;
    expect_int(launcher_model_start_from_available(m), 1, "PSX has the Start from block");
    expect_int(launcher_model_start_count(m), 0, "it starts with no entry");
    expect_int(launcher_model_start_selected(m), -1, "and on Power on");
    free(m);

    m = session(SNES, 1);
    if (!m) return;
    LauncherStartEntry e = state(0, 0, "Slot 1");
    expect_int(launcher_model_start_from_available(m), 0, "a console that did not opt in has no block");
    launcher_model_set_start_entries(m, &e, 1, NULL);
    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_count(m), 0, "and lists nothing, whatever it is given");
    expect_int(launcher_model_start_handover(m, NULL, NULL), LNG_START_POWER_ON, "and hands over Power on");
    free(m);
    expect_int(launcher_model_start_from_available(NULL), 0, "no model: no block");
    expect_int(launcher_model_start_handover(NULL, NULL, NULL), LNG_START_POWER_ON, "no model: Power on");
}

static void test_list(void) {
    LauncherModel* m = session(PSX, 1);
    LauncherStartEntry list[LNG_START_MAX + 4];
    LauncherStartNotes notes;
    if (!m) return;
    for (int i = 0; i < LNG_START_MAX + 4; ++i) list[i] = state(i % 12, 0, "Slot");
    memset(&notes, 0, sizeof(notes));
    notes.build_known = 1;
    notes.states_unlisted = 3;
    launcher_model_set_start_entries(m, list, 3, &notes);
    expect_int(launcher_model_start_count(m), 3, "three entries are listed");
    expect_int(launcher_model_start_entry(m, 1)->slot, 1, "in the order they were given");
    expect_int(launcher_model_start_entry(m, 3) == NULL, 1, "there is no entry past the list");
    expect_int(launcher_model_start_entry(m, -1) == NULL, 1, "and none before it");
    expect_int(m->start_notes.states_unlisted, 3, "the lister's notes are kept");

    launcher_model_select_start(m, 2);
    launcher_model_set_start_entries(m, list, LNG_START_MAX + 4, NULL);
    expect_int(launcher_model_start_count(m), LNG_START_MAX, "a longer list is cut at the limit");
    expect_int(launcher_model_start_selected(m), -1, "a new list goes back to Power on");
    expect_int(m->start_notes.build_known, 0, "a list without notes has none");
    launcher_model_set_start_entries(m, NULL, 5, NULL);
    expect_int(launcher_model_start_count(m), 0, "no entries: an empty list");
    free(m);
}

static void test_choice_and_handover(void) {
    LauncherModel* m = session(PSX, 1);
    LauncherStartEntry list[3];
    RecompLauncherCSettings before;
    int slot = 99;
    if (!m) return;
    list[0] = state(4, 0, "Slot 5");
    list[1] = state(0, 0, "Slot 1");
    list[2] = state(11, 0, "Slot 12");
    launcher_model_set_start_entries(m, list, 3, NULL);
    before = m->s;

    expect_int(launcher_model_start_handover(m, &slot, NULL), LNG_START_POWER_ON, "nothing chosen: Power on");
    expect_int(slot, -1, "with no slot");

    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_selected(m), 0, "the first save state is chosen");
    expect_int(launcher_model_start_handover(m, &slot, NULL), LNG_START_STATE, "it is handed over as a save state");
    expect_int(slot, 4, "with its slot, counted from 0");

    launcher_model_select_start(m, 1);
    launcher_model_start_handover(m, &slot, NULL);
    expect_int(slot, 0, "slot 0 is a slot, not Power on");
    launcher_model_select_start(m, 2);
    launcher_model_start_handover(m, &slot, NULL);
    expect_int(slot, 11, "the last slot is handed over too");
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "the choice changes no setting");

    launcher_model_select_start(m, 7);
    expect_int(launcher_model_start_selected(m), -1, "an entry that is not there is Power on");
    launcher_model_select_start(m, 0);
    launcher_model_select_start(m, -1);
    expect_int(launcher_model_start_handover(m, NULL, NULL), LNG_START_POWER_ON, "Power on can be chosen again");

    /* An entry a lister left unusable is never handed over. */
    list[0].slot = -1;
    launcher_model_set_start_entries(m, list, 3, NULL);
    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_handover(m, &slot, NULL), LNG_START_POWER_ON, "a save state without a slot is Power on");
    list[1].kind = 77;
    launcher_model_set_start_entries(m, list, 3, NULL);
    launcher_model_select_start(m, 1);
    expect_int(launcher_model_start_selected(m), -1, "an entry of no known kind cannot be chosen");
    free(m);
}

/* A save state belongs to the disc it was made on, and the host loads the
 * slot of the disc it boots. */
static void test_disc_of_a_state(void) {
    LauncherModel* m = session(PSX, 3);
    LauncherStartEntry list[3];
    if (!m) return;
    list[0] = state(1, 2, "Slot 2");
    list[1] = state(1, 1, "Slot 2");
    list[2] = state(3, 0, "Slot 4");
    launcher_model_set_start_entries(m, list, 3, NULL);
    expect_int(launcher_model_disc_selected(m), 0, "the title starts on disc 1");

    launcher_model_select_start(m, 0);
    expect_int(launcher_model_disc_selected(m), 1, "a save state of disc 2 selects disc 2");
    expect_int(launcher_model_start_selected(m), 0, "and stays chosen");
    expect_int(m->s.disc_index, 2, "the disc setting follows");

    launcher_model_select_disc(m, 2);
    expect_int(launcher_model_start_selected(m), -1, "another disc to boot: Power on again");

    launcher_model_select_start(m, 1);
    expect_int(launcher_model_disc_selected(m), 0, "a save state of disc 1 selects disc 1");
    launcher_model_select_disc(m, 0);
    expect_int(launcher_model_start_selected(m), 1, "choosing its own disc again leaves it chosen");

    launcher_model_select_start(m, 2);
    launcher_model_select_disc(m, 1);
    expect_int(launcher_model_start_selected(m), 2, "a save state that names no disc stays chosen");
    expect_int(launcher_model_disc_selected(m), 1, "and it does not move the disc itself");
    free(m);

    /* A state that names a disc the title does not have moves nothing. */
    m = session(PSX, 3);
    if (!m) return;
    list[0] = state(1, 9, "Slot 2");
    launcher_model_set_start_entries(m, list, 1, NULL);
    launcher_model_select_start(m, 0);
    expect_int(launcher_model_disc_selected(m), 0, "a save state of a disc outside the set leaves the disc");
    expect_int(launcher_model_start_selected(m), 0, "and is still chosen");
    free(m);
}

/* A replay is handed over by its file, never with a slot, and the choice is
 * one entry: a save state and a replay never go together. */
static void test_replay_handover(void) {
    LauncherModel* m = session(PSX, 3);
    LauncherStartEntry list[3];
    RecompLauncherCSettings before;
    const char* path = "unset";
    int slot = 99;
    if (!m) return;
    list[0] = state(2, 2, "Slot 3");
    list[1] = replay("C:/product/saves/replays/TEST_00001-boot-20261118-101500Z.psxrpl", "Replay from power-on");
    list[2] = replay("", "Replay");
    launcher_model_set_start_entries(m, list, 3, NULL);
    before = m->s;

    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_POWER_ON, "nothing chosen: Power on");
    expect_int(path == NULL, 1, "with no replay");

    launcher_model_select_start(m, 1);
    expect_int(launcher_model_start_selected(m), 1, "a replay can be chosen");
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_REPLAY, "it is handed over as a replay");
    expect_int(path != NULL && strcmp(path, list[1].path) == 0, 1, "by its file");
    expect_int(slot, -1, "and with no slot");
    expect_int(launcher_model_disc_selected(m), 0, "a replay names no disc and moves none");
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "choosing a replay changes no setting");
    launcher_model_select_disc(m, 2);
    expect_int(launcher_model_start_selected(m), 1, "another disc to boot leaves a replay chosen");
    launcher_model_select_disc(m, 0);

    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_STATE, "a save state after it replaces it");
    expect_int(path == NULL && slot == 2, 1, "with its slot and no replay");
    launcher_model_select_start(m, 1);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_REPLAY, "and a replay replaces a save state");
    expect_int(slot, -1, "leaving no slot");

    launcher_model_select_start(m, 2);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_POWER_ON, "a replay without a file is Power on");
    expect_int(path == NULL, 1, "and hands over nothing");
    launcher_model_select_start(m, 1);
    launcher_model_start_sync(m);
    expect_int(launcher_model_start_selected(m), 1, "without Record replay ticked, the choice stays");
    free(m);
}

#ifdef RECOMP_LAUNCHER_HAS_RECORD_REPLAY
/* A recording starts at power-on: Record replay and a save state or a replay
 * do not go together, and the later choice wins. */
static void test_record_replay_coupling(void) {
    LauncherModel* m = session(PSX, 1);
    LauncherStartEntry list[2];
    if (!m) return;
    list[0] = state(0, 0, "Slot 1");
    list[1] = replay("C:/product/saves/replays/TEST_00001-boot-20261118-101500Z.psxrpl", "Replay from power-on");
    launcher_model_set_start_entries(m, list, 2, NULL);

    launcher_model_toggle_record_replay(m);
    expect_int(m->s.record_replay, 1, "Record replay is ticked");
    launcher_model_select_start(m, 0);
    expect_int(m->s.record_replay, 0, "choosing a save state unticks it");
    expect_int(launcher_model_start_selected(m), 0, "and the save state is chosen");

    launcher_model_toggle_record_replay(m);
    launcher_model_start_sync(m);
    expect_int(launcher_model_start_selected(m), -1, "ticking Record replay returns to Power on");
    launcher_model_toggle_record_replay(m);
    launcher_model_start_sync(m);
    expect_int(launcher_model_start_selected(m), -1, "and unticking it does not bring the save state back");

    launcher_model_toggle_record_replay(m);
    launcher_model_select_start(m, 1);
    expect_int(m->s.record_replay, 0, "choosing a replay unticks it");
    expect_int(launcher_model_start_handover(m, NULL, NULL), LNG_START_REPLAY, "and the replay is handed over");
    launcher_model_toggle_record_replay(m);
    launcher_model_start_sync(m);
    expect_int(launcher_model_start_handover(m, NULL, NULL), LNG_START_POWER_ON, "ticking it again hands over Power on");

    launcher_model_select_start(m, -1);
    expect_int(m->s.record_replay, 1, "choosing Power on leaves the tick alone");
    free(m);
}
#endif

int main(void) {
    test_which_console_has_it();
    test_list();
    test_choice_and_handover();
    test_disc_of_a_state();
    test_replay_handover();
#ifdef RECOMP_LAUNCHER_HAS_RECORD_REPLAY
    test_record_replay_coupling();
#else
    printf("skip: Record replay coupling (this tree has no Record replay checkbox)\n");
#endif
    if (fails) { fprintf(stderr, "launcher_start_from_test: %d failure(s)\n", fails); return 1; }
    printf("launcher_start_from_test: all checks passed\n");
    return 0;
}
