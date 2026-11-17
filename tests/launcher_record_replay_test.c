/* The "Record replay" footer checkbox (Settings.record_replay,
 * GameInfo.has_record_replay).
 *
 * A host that records replays sets has_record_replay, seeds the field from
 * its own session state and reads it back after PLAY. A host that does not
 * must never get a 1 back, whatever it seeded, so an older runtime is never
 * asked to record.
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

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static RecompLauncherCGameInfo game;
static RecompLauncherCSettings defaults;

static LauncherModel* session(int offered, int seeded, RecompLauncherCSettings* io) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return NULL; }
    memset(&game, 0, sizeof(game));
    launcher_profile_apply_psx(&game);
    game.name = "Record Replay Fixture";
    game.num_players = 2;
    game.has_record_replay = offered;
    memset(&defaults, 0, sizeof(defaults));
    game.default_settings = &defaults;
    memset(io, 0, sizeof(*io));
    io->record_replay = seeded;
    launcher_model_init(m, io, &game, NULL);
    return m;
}

static void test_offered_round_trip(void) {
    RecompLauncherCSettings io;
    LauncherModel* m = session(1, 0, &io);
    if (!m) return;
    expect(m->has_record_replay, "the host's has_record_replay reaches the model");
    expect(m->s.record_replay == 0, "off by default");
    launcher_model_toggle_record_replay(m);
    expect(m->s.record_replay == 1, "the checkbox turns it on");
    launcher_model_commit(m, &io);
    expect(io.record_replay == 1, "PLAY hands the tick back to the host");
    free(m);

    /* The host seeds its session choice on the next launcher (a return
     * within the same process): the box opens ticked and can be cleared. */
    m = session(1, 1, &io);
    if (!m) return;
    expect(m->s.record_replay == 1, "a seeded tick is shown");
    launcher_model_toggle_record_replay(m);
    launcher_model_commit(m, &io);
    expect(io.record_replay == 0, "clearing it hands 0 back");
    free(m);

    /* Anything positive is a tick; anything else is off. */
    m = session(1, 7, &io);
    if (!m) return;
    expect(m->s.record_replay == 1, "a stray positive seed reads as 1");
    free(m);
    m = session(1, -3, &io);
    if (!m) return;
    expect(m->s.record_replay == 0, "a negative seed reads as 0");
    free(m);
}

static void test_not_offered(void) {
    RecompLauncherCSettings io;
    LauncherModel* m = session(0, 1, &io);
    if (!m) return;
    expect(!m->has_record_replay, "no checkbox without has_record_replay");
    expect(m->s.record_replay == 0, "a host that does not offer it cannot seed a tick");
    launcher_model_toggle_record_replay(m);
    expect(m->s.record_replay == 0, "the toggle is inert");
    launcher_model_commit(m, &io);
    expect(io.record_replay == 0, "and 0 goes back to the host");
    free(m);
}

static void test_restore_defaults_keeps_tick(void) {
    RecompLauncherCSettings io;
    LauncherModel* m = session(1, 0, &io);
    if (!m) return;
    launcher_model_toggle_record_replay(m);
    m->s.volume = 13;
    launcher_model_restore_defaults(m);
    expect(m->s.volume == defaults.volume, "Restore Defaults resets the settings");
    expect(m->s.record_replay == 1, "but keeps the per-launch record tick");
    free(m);
}

int main(void) {
    test_offered_round_trip();
    test_not_offered();
    test_restore_defaults_keeps_tick();
    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("all record-replay checks passed\n");
    return 0;
}
