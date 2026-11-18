/* The disc panel's Serial row and the reason line under it.
 *
 * A player gave a build made for SLES-03396 a disc of another release,
 * SLES-03398. The panel showed "Serial  SLES-03398" with a tick, and the only
 * sentence on it was the online-play note about the TOC fingerprint. He could
 * not tell that he had the wrong release.
 *
 * The contract tested here, at the model:
 *
 *   - the Serial row is a tick only for a serial that was read and that the
 *     host did not call unlisted;
 *   - for an unlisted serial the line under the checklist names the disc's
 *     serial and what the build needs, and the online-play note is not shown;
 *   - a host that does not say whether the serial is listed gets the old rows.
 *
 * Includes the model translation unit to reach the static run_verify(), which
 * is what copies the host's answer into the model.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to discs. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

/* What the host answers for the selected disc. */
static RecompLauncherCDiscVerify host;
static int host_verify(const char* path, RecompLauncherCDiscVerify* out) {
    (void)path;
    *out = host;
    return 1;
}

#define TOC_NOTE "Disc TOC fingerprint does not match this build's required dump."

static void answer(const char* serial, int status, const char* expected, int verdict,
                   int netplay_ok, const char* netplay_detail) {
    memset(&host, 0, sizeof(host));
    safe_copy(host.serial, sizeof(host.serial), serial);
    safe_copy(host.region, sizeof(host.region), "PAL");
    host.iso_ok = 1;
    host.verdict = verdict;
    host.track_count = 1;
    host.netplay_ok = netplay_ok;
    safe_copy(host.netplay_detail, sizeof(host.netplay_detail), netplay_detail);
    host.serial_status = status;
    safe_copy(host.expected_serials, sizeof(host.expected_serials), expected);
}

int main(void) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    SystemProfile profile = {0};
    char buf[256];
    bool wrong = false;
    const char* note;

    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
    profile.verify.mode = 1;
    m->profile = &profile;
    m->disc_verify_cb = host_verify;
    m->rom_present = true;
    m->netplay_supported = true;
    safe_copy(m->rom_size, sizeof(m->rom_size), "256.6 MB");

    /* The reported case: a disc of another release, in a build with netplay. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(!strcmp(m->verify.serial, "SLES-03398"), "the row shows the serial that was read");
    expect(!launcher_model_disc_serial_ok(m), "a serial the build does not list is not a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!strcmp(note, "This disc is SLES-03398. This build needs SLES-03396."),
           "the reason names the disc's serial and the serial the build needs");
    expect(wrong, "the reason is marked as the wrong-disc reason");
    expect(!strstr(note, "TOC"), "the online-play note is not shown for a wrong disc");
    expect(!launcher_model_can_launch(m), "PLAY stays blocked for a wrong disc");

    /* The same disc in a build without netplay: the same row and reason. */
    m->netplay_supported = false;
    expect(!launcher_model_disc_serial_ok(m), "offline title: not a tick either");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(wrong && !strcmp(note, "This disc is SLES-03398. This build needs SLES-03396."),
           "offline title: the same reason");
    m->netplay_supported = true;

    /* The right disc. */
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 1, 1, "");
    run_verify(m);
    expect(launcher_model_disc_serial_ok(m), "a listed serial is a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!note[0] && !wrong, "the right disc has no reason line");
    expect(launcher_model_can_launch(m), "PLAY is open for the right disc");

    /* The right serial, another dump: the online-play note, as before. */
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 2, 0, TOC_NOTE);
    run_verify(m);
    expect(launcher_model_disc_serial_ok(m), "a listed serial on another dump is still a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!strcmp(note, TOC_NOTE) && !wrong,
           "a listed serial keeps the online-play note, as a warning");
    m->netplay_supported = false;
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!note[0], "a title without netplay never shows the online-play note");
    m->netplay_supported = true;

    /* Nothing was read from the disc. */
    answer("", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m), "no serial read is not a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(wrong && !strcmp(note, "No serial was found on this disc. This build needs SLES-03396."),
           "the reason says no serial was found, and what the build needs");

    /* The host did not say what the build lists. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "", 3, 1, "");
    run_verify(m);
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(wrong && !strcmp(note, "This disc is SLES-03398. This build is made for another disc."),
           "without the build's list the reason still names the disc");

    /* A set: the build lists more than one serial. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-01234, SLES-11234", 3, 1, "");
    run_verify(m);
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!strcmp(note, "This disc is SLES-03398. This build needs SLES-01234, SLES-11234."),
           "the reason carries every serial the host lists");

    /* A host built before these fields existed says nothing: the old rows. */
    answer("SLES-03398", RECOMP_SERIAL_UNSAID, "", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(launcher_model_disc_serial_ok(m), "a host that does not say keeps the old tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!strcmp(note, TOC_NOTE) && !wrong, "a host that does not say keeps the old note");
    answer("", RECOMP_SERIAL_UNSAID, "", 3, 1, "");
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m), "no serial read was never a tick");

    /* A new answer replaces the last one whole. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    answer("SLES-03396", RECOMP_SERIAL_UNSAID, "", 1, 1, "");
    run_verify(m);
    expect(m->verify.serial_status == RECOMP_SERIAL_UNSAID && !m->verify.expected_serials[0],
           "the next disc does not inherit the last disc's answer");

    /* No disc selected: dashes, no mark, no line. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    m->rom_present = false;
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!note[0] && !wrong, "no disc selected: no reason line");
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m), "no disc selected: no tick");
    m->rom_present = true;

    /* The line fits its buffer, and a missing buffer is not a crash. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    {
        char small[16];
        note = launcher_model_disc_note(m, small, sizeof(small), &wrong);
        expect(strlen(note) == sizeof(small) - 1 && wrong, "a small buffer cuts the line and ends it");
    }
    expect(!launcher_model_disc_note(m, NULL, 0, &wrong)[0] && !wrong, "no buffer: an empty line");
    expect(launcher_model_disc_note(m, buf, sizeof(buf), NULL)[0] != '\0', "the flag is optional");
    expect(!launcher_model_disc_serial_ok(NULL), "no model: no tick");

    free(m);
    if (fails) {
        fprintf(stderr, "launcher disc serial: %d check(s) failed\n", fails);
        return 1;
    }
    printf("launcher disc serial: ok\n");
    return 0;
}
