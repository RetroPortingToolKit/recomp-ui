/* "Start from" on the dashboard: what the next PLAY starts with.
 *
 * Power on (the default), one of the title's save states, or one of its
 * replays. The model holds the list a lister gave it and the choice; this
 * file checks what is listed, what the choice does to its neighbours (Record
 * replay, the disc to boot) and what is handed to the host.
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

static void expect_str(const char* got, const char* want, const char* what) {
    if (got && want && strcmp(got, want) == 0) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %s, want %s)\n", what, got ? got : "(null)", want ? want : "(null)");
    ++fails;
}

enum { PSX = 0, SNES = 1 };

static const RecompLauncherCDisc kDiscs[3] = {
    { 1, NULL, "set_disc1.cue" }, { 2, NULL, "set_disc2.cue" }, { 3, NULL, "set_disc3.cue" },
};

static LauncherModel* session(int console, int discs, int record_replay) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    if (console == PSX) launcher_profile_apply_psx(&game);
    else launcher_profile_apply_snes(&game);
    game.name = "Start From Fixture";
    game.num_players = 2;
    game.has_record_replay = 1;
    if (discs > 1) { game.discs = kDiscs; game.num_discs = discs; }
    memset(&io, 0, sizeof(io));
    io.record_replay = record_replay;
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

static LauncherStartEntry replay(const char* path) {
    LauncherStartEntry e;
    memset(&e, 0, sizeof(e));
    e.kind = LNG_START_REPLAY;
    e.slot = -1;
    e.power_on = 1;
    snprintf(e.label, sizeof(e.label), "%s", path);
    snprintf(e.path, sizeof(e.path), "%s", path);
    return e;
}

static void test_which_console_has_it(void) {
    LauncherModel* m = session(PSX, 1, 0);
    if (!m) return;
    expect_int(launcher_model_start_from_available(m), 1, "PSX has the Start from block");
    expect_int(launcher_model_start_count(m), 0, "it starts with no entry");
    expect_int(launcher_model_start_selected(m), -1, "and on Power on");
    free(m);

    m = session(SNES, 1, 0);
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
    LauncherModel* m = session(PSX, 1, 0);
    LauncherStartEntry list[LNG_START_MAX + 4];
    LauncherStartNotes notes;
    if (!m) return;
    for (int i = 0; i < LNG_START_MAX + 4; ++i) list[i] = state(i % 12, 0, "Slot");
    list[1] = replay("saves/replays/a.psxrpl");
    memset(&notes, 0, sizeof(notes));
    notes.build_known = 1;
    notes.states_unlisted = 3;
    launcher_model_set_start_entries(m, list, 3, &notes);
    expect_int(launcher_model_start_count(m), 3, "three entries are listed");
    expect_int(launcher_model_start_entry(m, 0)->kind, LNG_START_STATE, "the first is the save state");
    expect_int(launcher_model_start_entry(m, 1)->kind, LNG_START_REPLAY, "the second is the replay");
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
    LauncherModel* m = session(PSX, 1, 0);
    LauncherStartEntry list[3];
    int slot = 99;
    const char* path = "unset";
    if (!m) return;
    list[0] = state(4, 0, "Slot 5");
    list[1] = state(0, 0, "Slot 1");
    list[2] = replay("saves/replays/Game-boot-20261007T101500Z.psxrpl");
    launcher_model_set_start_entries(m, list, 3, NULL);

    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_POWER_ON, "nothing chosen: Power on");
    expect_int(slot, -1, "with no slot");
    expect_int(path == NULL, 1, "and no file");

    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_selected(m), 0, "the first save state is chosen");
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_STATE, "it is handed over as a save state");
    expect_int(slot, 4, "with its slot, counted from 0");
    expect_int(path == NULL, 1, "and no file");

    launcher_model_select_start(m, 1);
    launcher_model_start_handover(m, &slot, NULL);
    expect_int(slot, 0, "slot 0 is a slot, not Power on");

    launcher_model_select_start(m, 2);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_REPLAY, "the replay is handed over as a replay");
    expect_str(path, "saves/replays/Game-boot-20261007T101500Z.psxrpl", "with its file");
    expect_int(slot, -1, "and no slot");

    launcher_model_select_start(m, 7);
    expect_int(launcher_model_start_selected(m), -1, "an entry that is not there is Power on");
    launcher_model_select_start(m, 0);
    launcher_model_select_start(m, -1);
    expect_int(launcher_model_start_handover(m, NULL, NULL), LNG_START_POWER_ON, "Power on can be chosen again");

    /* An entry a lister left unusable is never handed over half. */
    list[0].slot = -1;
    list[2].path[0] = '\0';
    launcher_model_set_start_entries(m, list, 3, NULL);
    launcher_model_select_start(m, 0);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_POWER_ON, "a save state without a slot is Power on");
    launcher_model_select_start(m, 2);
    expect_int(launcher_model_start_handover(m, &slot, &path), LNG_START_POWER_ON, "a replay without a file is Power on");
    list[1].kind = 77;
    launcher_model_set_start_entries(m, list, 3, NULL);
    launcher_model_select_start(m, 1);
    expect_int(launcher_model_start_selected(m), -1, "an entry of no known kind cannot be chosen");
    free(m);
}

/* The choice is no setting: it must not change what the host is handed as
 * settings, except the Record replay tick, which it clears. */
static void test_record_replay(void) {
    LauncherModel* m = session(PSX, 1, 1);
    LauncherStartEntry list[2];
    RecompLauncherCSettings before;
    if (!m) return;
    list[0] = state(2, 0, "Slot 3");
    list[1] = replay("saves/replays/b.psxrpl");
    launcher_model_set_start_entries(m, list, 2, NULL);
    expect_int(m->s.record_replay, 1, "Record replay is ticked at the start");
    before = m->s;
    launcher_model_select_start(m, -1);
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "choosing Power on changes no setting");

    launcher_model_select_start(m, 0);
    expect_int(m->s.record_replay, 0, "choosing a save state unticks Record replay");
    before.record_replay = 0;
    expect_int(memcmp(&before, &m->s, sizeof(before)), 0, "and changes nothing else");

    launcher_model_toggle_record_replay(m);
    expect_int(m->s.record_replay, 1, "Record replay can be ticked again");
    expect_int(launcher_model_start_selected(m), -1, "and that goes back to Power on");

    launcher_model_select_start(m, 1);
    expect_int(m->s.record_replay, 0, "choosing a replay unticks Record replay");
    launcher_model_toggle_record_replay(m);
    expect_int(launcher_model_start_selected(m), -1, "ticking it again goes back to Power on");
    launcher_model_toggle_record_replay(m);
    expect_int(m->s.record_replay, 0, "unticking it");
    expect_int(launcher_model_start_selected(m), -1, "leaves Power on");
    free(m);
}

/* A save state belongs to the disc it was made on, and the host loads the
 * slot of the disc it boots. */
static void test_disc_of_a_state(void) {
    LauncherModel* m = session(PSX, 3, 0);
    LauncherStartEntry list[3];
    if (!m) return;
    list[0] = state(1, 2, "Slot 2");
    list[1] = state(1, 1, "Slot 2");
    list[2] = replay("saves/replays/c.psxrpl");
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
    expect_int(launcher_model_start_selected(m), 2, "a replay stays chosen when the disc changes");
    expect_int(launcher_model_disc_selected(m), 1, "and it does not move the disc itself");
    free(m);

    /* A state that names a disc the title does not have moves nothing. */
    m = session(PSX, 3, 0);
    if (!m) return;
    list[0] = state(1, 9, "Slot 2");
    launcher_model_set_start_entries(m, list, 1, NULL);
    launcher_model_select_start(m, 0);
    expect_int(launcher_model_disc_selected(m), 0, "a save state of a disc outside the set leaves the disc");
    expect_int(launcher_model_start_selected(m), 0, "and is still chosen");
    free(m);
}

int main(void) {
    test_which_console_has_it();
    test_list();
    test_choice_and_handover();
    test_record_replay();
    test_disc_of_a_state();
    if (fails) { fprintf(stderr, "launcher_start_from_test: %d failure(s)\n", fails); return 1; }
    printf("launcher_start_from_test: all checks passed\n");
    return 0;
}
