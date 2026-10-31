/* The disc panel's Serial row and the reason line under it (PS1B-403).
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

    /* The host says "not listed" with no serial and no list: there is nothing
     * to print about the serial, so the old note stands. */
    answer("", RECOMP_SERIAL_NOT_LISTED, "", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m), "nothing read and nothing listed: not a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!strcmp(note, TOC_NOTE) && !wrong, "nothing read and nothing listed: the old note, no sentence of its own");
    m->netplay_supported = false;
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!note[0] && !wrong, "the same in a title without netplay: no line");
    m->netplay_supported = true;

    /* A set, and an image its disc list does not know (another file name,
     * another folder). The host compares the image's serial with every serial
     * of the set and gives the whole list. Another game's disc: */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-01234, SLES-11234", 1, 1, "");
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m), "a set: a serial outside the set is not a tick");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(wrong && !strcmp(note, "This disc is SLES-03398. This build needs SLES-01234, SLES-11234."),
           "a set: the reason carries every serial of the set");
    /* A copy of one of the set's discs under another name: */
    answer("SLES-11234", RECOMP_SERIAL_LISTED, "SLES-01234, SLES-11234", 1, 1, "");
    run_verify(m);
    expect(launcher_model_disc_serial_ok(m), "a set: a serial of the set is a tick, whatever the file is called");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(!note[0] && !wrong, "a set: a disc of the set has no reason line");

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

    /* The colour of the Serial row's cross (Alex, 2026-10-02, A15): the
     * headline's red when the disc is refused and the serial is the reason;
     * every other cross keeps the amber. */
    answer("SLES-02913", RECOMP_SERIAL_NOT_LISTED, "SLES-01156", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(launcher_model_disc_serial_refused(m), "another game's disc, refused: the Serial cross is red");
    answer("", RECOMP_SERIAL_NOT_LISTED, "SLES-01156", 3, 1, "");
    run_verify(m);
    expect(launcher_model_disc_serial_refused(m), "no serial found, refused: the Serial cross is red");
    answer("SLES-01156", RECOMP_SERIAL_LISTED, "SLES-01156", 1, 1, "");
    run_verify(m);
    expect(!launcher_model_disc_serial_refused(m), "the right disc: no cross at all");
    answer("SLES-01156", RECOMP_SERIAL_LISTED, "SLES-01156", 2, 0, TOC_NOTE);
    run_verify(m);
    expect(!launcher_model_disc_serial_refused(m),
           "the right serial with the online-play warning: the Tracks cross is not the Serial row's");
    answer("", RECOMP_SERIAL_UNSAID, "", 3, 1, "");
    run_verify(m);
    expect(!launcher_model_disc_serial_ok(m) && !launcher_model_disc_serial_refused(m),
           "a file that did not open: the Serial row has a cross, and it stays amber (the header is the reason)");
    answer("SLES-02913", RECOMP_SERIAL_NOT_LISTED, "SLES-01156", 1, 1, "");
    run_verify(m);
    expect(!launcher_model_disc_serial_refused(m), "not listed but not refused by the host: amber");
    m->rom_present = false;
    answer("SLES-02913", RECOMP_SERIAL_NOT_LISTED, "SLES-01156", 3, 0, "");
    expect(!launcher_model_disc_serial_refused(m), "no disc selected: nothing is refused");
    m->rom_present = true;
    expect(!launcher_model_disc_serial_refused(NULL), "no model: nothing is refused");

    /* The wrong-disc sentence is brought into view. The disc card hugs its
     * content and the dashboard's body scrolls, so on a window smaller than
     * the launcher's own the sentence ended below the window's edge (Pegasus,
     * 1102 x 606 units). The dashboard asks once per frame it draws the card
     * and scrolls the sentence into view while the answer is yes. Each call
     * below is one drawn frame; a disc is chosen through launcher_model_set_rom,
     * as Browse and the disc list do. */
#define FRAME() launcher_model_disc_note_reveal(m)
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 1, 1, "");
    launcher_model_set_rom(m, "right.chd");
    expect(!FRAME() && !FRAME(), "the right disc: nothing to bring into view");

    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    launcher_model_set_rom(m, "wrong.chd");
    expect(FRAME(), "a new wrong-disc sentence is brought into view");
    expect(FRAME(), "and for a second frame: the card has grown by the sentence");
    expect(!FRAME() && !FRAME(), "then the scroll is the player's");

    /* Another file in between, then the same wrong disc: the same words are new again. */
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 1, 1, "");
    launcher_model_set_rom(m, "right.chd");
    expect(!FRAME(), "the sentence went away with the wrong disc");
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    launcher_model_set_rom(m, "wrong.chd");
    expect(FRAME() && FRAME() && !FRAME(), "the same sentence after another file was chosen is new again");

    /* The same wrong disc chosen again: no frame is drawn in between without
     * the sentence, and its words are the same. Choosing a disc makes it new. */
    launcher_model_set_rom(m, "wrong.chd");
    expect(FRAME() && FRAME() && !FRAME(), "the same wrong disc chosen again is brought into view again");
    /* Another wrong disc with other words, with no right disc in between. */
    answer("SLES-02913", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    launcher_model_set_rom(m, "other-game.chd");
    expect(FRAME() && FRAME() && !FRAME(), "another wrong disc is a new sentence");

    /* A sentence that goes away while it is still being brought into view. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    launcher_model_set_rom(m, "wrong.chd");
    expect(FRAME(), "first frame of a new sentence");
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 1, 1, "");
    launcher_model_set_rom(m, "right.chd");
    expect(!FRAME() && !FRAME(), "the right disc ends it at once: nothing is scrolled for a sentence that is gone");

    /* The verdict can change without a disc being chosen (the host is asked
     * again): a sentence that appears is new, the same one is not. */
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(FRAME() && FRAME() && !FRAME(), "a sentence that appears without a new disc is brought into view");
    run_verify(m);
    expect(!FRAME(), "the same sentence, the same disc, asked again: nothing moves");
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 1, 1, "");
    run_verify(m);
    expect(!FRAME(), "the sentence went away without a new disc");
    answer("SLES-03398", RECOMP_SERIAL_NOT_LISTED, "SLES-03396", 3, 0, TOC_NOTE);
    run_verify(m);
    expect(FRAME() && FRAME() && !FRAME(), "and when it comes back with the same words it is new");

    /* The online-play note alone is not the wrong-disc sentence. */
    answer("SLES-03396", RECOMP_SERIAL_LISTED, "SLES-03396", 2, 0, TOC_NOTE);
    launcher_model_set_rom(m, "other-dump.chd");
    note = launcher_model_disc_note(m, buf, sizeof(buf), &wrong);
    expect(note[0] != '\0' && !wrong, "the online-play note is shown for the right serial");
    expect(!FRAME() && !FRAME(), "and is not brought into view");
    /* No disc, no model. */
    launcher_model_set_rom(m, "");
    expect(!FRAME(), "no disc selected: nothing to bring into view");
    expect(!launcher_model_disc_note_reveal(NULL), "no model: nothing to do");
#undef FRAME

    /* How far the body moves. Pegasus's default window, in its pixels: the
     * body shows about 1,100 of them from y = 366, and the sentence sits
     * about 1,450 to 1,560 below the body's top. Alex's window shows it. */
    expect(launcher_model_scroll_into_view(1816.0f, 1926.0f, 366.0f, 1466.0f, 30.0f) == 490.0f,
           "a sentence below the window's edge: the body moves down until its lower edge plus the margin is in view");
    expect(launcher_model_scroll_into_view(627.0f, 665.0f, 122.0f, 772.0f, 10.0f) == 0.0f,
           "a sentence in view: nothing moves");
    expect(launcher_model_scroll_into_view(627.0f, 665.0f, 122.0f, 675.0f, 10.0f) == 0.0f,
           "a sentence that just fits with its margin: nothing moves");
    expect(launcher_model_scroll_into_view(627.0f, 665.0f, 122.0f, 674.0f, 10.0f) == 1.0f,
           "one unit short: one unit");
    expect(launcher_model_scroll_into_view(100.0f, 140.0f, 122.0f, 772.0f, 10.0f) == -32.0f,
           "a sentence above the view (the player scrolled past it): the body moves up");
    expect(launcher_model_scroll_into_view(400.0f, 900.0f, 122.0f, 422.0f, 10.0f) == 268.0f,
           "a sentence taller than the view: only as far as keeps its top in view");
    expect(launcher_model_scroll_into_view(100.0f, 900.0f, 122.0f, 422.0f, 10.0f) == 0.0f,
           "a sentence that covers the view: nothing moves");

    free(m);
    if (fails) {
        fprintf(stderr, "launcher disc serial: %d check(s) failed\n", fails);
        return 1;
    }
    printf("launcher disc serial: ok\n");
    return 0;
}
