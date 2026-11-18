/* The setup wizard's error text holds a host's whole sentence.
 *
 * A setup host tells the player why a disc is refused: which disc image the
 * build was made from, which file was selected, and what it may be. With the
 * host's prefix that sentence runs to about 480 bytes. The wizard's text and
 * the job buffer the host writes into held 255, so the sentence was cut in
 * the middle of the selected file's name or size, and its last sentence was
 * never shown.
 *
 * Includes the model translation unit to run the prepare job's own code: the
 * thread function that calls the host, and the poll that moves the host's
 * reason into the wizard.
 */
/* usleep, realpath and readlink are not declared under a strict -std=c11
 * unless this is set before the first system header. The product build uses
 * the compiler's default (GNU) mode, where they are. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define chdir _chdir
static void nap(void) { Sleep(2); }
#else
#include <unistd.h>
static void nap(void) { usleep(2000); }
#endif

/* Pulled in by the model TU; unrelated to setup. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

/* A long refusal, as a setup host hands it over. */
static const char kSentence[] =
    "Disc verification failed: This is not the disc image this build was made from. "
    "The build needs Example Game, The - Long Subtitle - Special Edition (USA) "
    "(data track 264,858,720 bytes). The selected file Example Game, The - Long "
    "Subtitle - Special Edition (USA) (Track 1).bin is 264,858,720 bytes, SHA-1 "
    "0123456789abcdef0123456789abcdef01234567. It may be another pressing, revision "
    "or region of the game (a different disc), a damaged copy, or the right disc in "
    "a form setup cannot read.";

static size_t given_cap;
static int host_prepare(const char* source, char* out_path, size_t out_cap,
                        char* err_msg, size_t err_cap,
                        RecompLauncherCPrepareProgressFn on_progress, void* ctx) {
    (void)source; (void)out_path; (void)out_cap; (void)on_progress; (void)ctx;
    given_cap = err_cap;
    snprintf(err_msg, err_cap, "%s", kSentence);
    return 0;   /* refused */
}

int main(int argc, char** argv) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
    /* Generate writes the setup's small record files beside the launcher: keep
     * them in the folder the caller names. */
    if (argc > 1 && chdir(argv[1]) != 0) {
        fprintf(stderr, "FAIL: cannot chdir to %s\n", argv[1]);
        return 1;
    }

    expect(strlen(kSentence) > 255, "the sentence is longer than the old 255 bytes");
    expect(strlen(kSentence) < LNG_SETUP_ERROR_CAP, "the sentence fits the wizard's text");
    expect(sizeof(m->setup_error) == LNG_SETUP_ERROR_CAP, "the wizard's text has the named size");
    expect(sizeof(g_prep_job.err) >= sizeof(m->setup_error),
           "the job buffer the host writes into is no smaller than the wizard's text");

    /* The prepare job, as launcher_model_begin_prepare_disc sets it up, run
     * here on this thread: the host refuses with its sentence. */
    m->prepare_with_progress_cb = host_prepare;
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_PREPARE;
    safe_copy(g_prep_job.source, sizeof(g_prep_job.source), "wrong.cue");
    m->setup_preparing = true;
    (void)prep_thread_main(&g_prep_job);
    expect(given_cap == LNG_SETUP_ERROR_CAP, "the host is given the whole room");
    expect(g_prep_job.done && !g_prep_job.result, "the job ended as refused");

    /* The poll of the next frame moves the reason into the wizard. */
    launcher_model_poll_prepare_disc(m);
    expect(!m->setup_preparing, "the wizard is no longer busy");
    expect(strcmp(m->setup_error, kSentence) == 0, "the wizard holds the host's sentence whole");
    expect(strstr(m->setup_error, "a damaged copy, or the right disc in a form setup cannot read.") != NULL,
           "the last sentence is there");

    /* The wizard is taller than its window at the launcher's own size, and
     * the error is drawn under its last step: the player had to scroll to
     * read it. A new error asks the wizard to scroll its end into view, for
     * the two frames a reopened window needs, and then leaves the scroll to
     * the player. */
    expect(launcher_model_setup_error_reveal(m), "a new error is brought into view");
    expect(launcher_model_setup_error_reveal(m), "and once more: the reopened window's second frame");
    expect(!launcher_model_setup_error_reveal(m), "then the scroll is the player's again");
    expect(!launcher_model_setup_error_reveal(m), "and stays so while the error is the same");
    safe_copy(m->setup_error, sizeof(m->setup_error), "Rebuild failed (exit 1): another reason");
    expect(launcher_model_setup_error_reveal(m) && launcher_model_setup_error_reveal(m) &&
           !launcher_model_setup_error_reveal(m), "another error text is new again");
    m->setup_error[0] = '\0';
    expect(!launcher_model_setup_error_reveal(m), "no error: nothing to bring into view");
    safe_copy(m->setup_error, sizeof(m->setup_error), "Rebuild failed (exit 1): another reason");
    expect(launcher_model_setup_error_reveal(m), "the same error after it was cleared is new again");
    expect(!launcher_model_setup_error_reveal(NULL), "no model: nothing to do");
    safe_copy(m->setup_error, sizeof(m->setup_error), kSentence);

    /* The same refusal twice in a row, as the wizard meets it. The player
     * reads the error, scrolls back up, and presses Generate again on the same
     * disc. While the job runs the wizard draws only the progress window and
     * returns before it asks for the reveal, so it never draws a frame with
     * the error cleared. The second refusal has the same words as the first
     * and must be brought into view again. */
    while (launcher_model_setup_error_reveal(m)) { }
    expect(!launcher_model_setup_error_reveal(m), "the first refusal has been shown; the scroll is the player's");
    safe_copy(m->rom_full, sizeof(m->rom_full), "wrong.cue");
    m->rom_present = true;
    launcher_model_start_prepare_disc(m, m->rom_full);   /* the Generate button */
    expect(m->setup_preparing, "Generate started a job");
    expect(m->setup_error[0] == '\0', "the job's start clears the error text");
    {
        int frames = 0;
        while (m->setup_preparing && frames < 5000) {
            /* A frame of the busy wizard: the poll, the progress window, no reveal. */
            launcher_model_poll_prepare_disc(m);
            if (m->setup_preparing) nap();
            ++frames;
        }
    }
    expect(!m->setup_preparing, "the job ended");
    expect(strcmp(m->setup_error, kSentence) == 0, "with the same sentence as before");
    expect(launcher_model_setup_error_reveal(m), "the same refusal a second time is brought into view again");
    expect(launcher_model_setup_error_reveal(m), "for the second frame too");
    expect(!launcher_model_setup_error_reveal(m), "and then the scroll is the player's again");

    /* A host that writes more than the room is cut at the room, and ended. */
    {
        static char big[2 * LNG_SETUP_ERROR_CAP];
        memset(big, 'x', sizeof(big) - 1);
        safe_copy(m->setup_error, sizeof(m->setup_error), big);
        expect(strlen(m->setup_error) == LNG_SETUP_ERROR_CAP - 1, "a longer text is cut at the room");
    }

    free(m);
    if (fails) {
        fprintf(stderr, "launcher setup error room: %d check(s) failed\n", fails);
        return 1;
    }
    printf("launcher setup error room: ok\n");
    return 0;
}
