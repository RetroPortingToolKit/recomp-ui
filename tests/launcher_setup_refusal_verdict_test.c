/* The disc panel follows a file that setup refused (PS1B-415).
 *
 * In a kit's setup window two checks judge the selected disc. The disc panel
 * asks the game program (header and serial). "Generate & rebuild" asks setup,
 * which compares the disc with the kit and can refuse it. Nothing connected
 * them: a first pressing with the kit's serial showed "Disc verified" in the
 * panel and "Disc verification failed: ..." under it.
 *
 * The setup host now answers "bad" for the file it refused. The model's half,
 * tested here:
 *
 *   - after a failed prepare the disc check runs again, so the panel takes the
 *     host's new answer without the player touching the file;
 *   - a failure that is not a refusal of the file leaves "Disc verified";
 *   - another file, and a prepare that succeeds, end it;
 *   - Generate can be started again for the refused file (the player replaced
 *     it in place);
 *   - launcher_model_disc_refused_by_setup() is true only while the "bad" is
 *     setup's, for a panel row that says so.
 *
 * The host below stands in for the setup host of the framework
 * (host/psxrecomp_codegen_host.c, host/psx_setup_refusal.h): it remembers one
 * refused file, forgets it on a success and on a check of another file.
 *
 * Includes the model translation unit to reach the prepare job.
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

/* ---- the stand-in setup host ------------------------------------------- */

enum { GEN_REFUSES = 0, GEN_BUILD_FAILS, GEN_NO_TOOLS, GEN_ACCEPTS };
static int  gen_does;
static int  gen_calls;
static char refused[512];       /* the file setup refused last; "" when none */
static char program_bad[512];   /* a file the game program itself calls bad */
static int  verify_calls;

#define REFUSAL "Disc verification failed: This is not the disc image this kit was made from."

static int host_generate(const char* source, char* out, size_t out_cap, char* err, size_t err_cap,
                         RecompLauncherCPrepareProgressFn on_progress, void* ctx) {
    (void)on_progress; (void)ctx;
    ++gen_calls;
    switch (gen_does) {
    case GEN_REFUSES:
        snprintf(refused, sizeof(refused), "%s", source);
        snprintf(err, err_cap, "%s", REFUSAL);
        return 0;
    case GEN_BUILD_FAILS:
        snprintf(err, err_cap, "psxrecomp generate failed (exit 1): ninja: build stopped");
        return 0;
    case GEN_NO_TOOLS:
        snprintf(err, err_cap, "Local codegen tools are not available.");
        return 0;
    default:
        refused[0] = '\0';
        snprintf(out, out_cap, "%s", source);
        return 1;
    }
}

static int host_verify(const char* path, RecompLauncherCDiscVerify* out) {
    ++verify_calls;
    memset(out, 0, sizeof(*out));
    snprintf(out->serial, sizeof(out->serial), "SLUS-00292");
    snprintf(out->region, sizeof(out->region), "NTSC-U");
    out->iso_ok = 1;
    out->netplay_ok = 1;
    out->verdict = 1;
    if (program_bad[0] && !strcmp(path, program_bad)) out->verdict = 3;
    if (refused[0]) {
        if (!strcmp(path, refused)) out->verdict = 3;
        else refused[0] = '\0';
    }
    return 1;
}

/* ---- the model, as the setup window has it ------------------------------ */

static SystemProfile profile;

static LauncherModel* make_model(void) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) return NULL;
    memset(&profile, 0, sizeof(profile));
    profile.verify.mode = 1;
    m->profile = &profile;
    m->setup_wizard_supported = true;
    m->setup_wizard_open = true;
    m->setup_page = 1;
    m->prepare_required_before_continue = true;
    m->prepare_use_selected_rom = true;
    m->prepare_with_progress_cb = host_generate;
    m->disc_verify_cb = host_verify;
    snprintf(m->rom_size, sizeof(m->rom_size), "--");
    refused[0] = '\0';
    program_bad[0] = '\0';
    gen_calls = 0;
    return m;
}

static void make_file(const char* path) {
    FILE* f = fopen(path, "wb");
    if (f) { fputs("disc", f); fclose(f); }
}

/* The Generate button, then the frames until the job has ended. */
static int generate(LauncherModel* m, int does) {
    int frames = 0;
    gen_does = does;
    launcher_model_start_prepare_disc(m, m->rom_full);
    if (!m->setup_preparing) return 0;
    while (m->setup_preparing && frames < 5000) {
        launcher_model_poll_prepare_disc(m);
        if (m->setup_preparing) nap();
        ++frames;
    }
    return !m->setup_preparing;
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    char first[512], rev1[512];
    LauncherModel* m;
    int calls;

    if (chdir(dir) != 0) { fprintf(stderr, "FAIL: cannot chdir to %s\n", dir); return 1; }
    snprintf(first, sizeof(first), "%s/Suikoden (USA).cue", dir);
    snprintf(rev1, sizeof(rev1), "%s/Suikoden (USA) (Rev 1).cue", dir);
    make_file(first);
    make_file(rev1);

    /* 1. A first pressing with the kit's serial: verified, then refused. */
    m = make_model();
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
    launcher_model_set_rom(m, first);
    expect(m->verify.verdict == 1, "before Generate the panel says verified (serial and header are right)");
    expect(!launcher_model_disc_refused_by_setup(m), "before Generate nothing is refused");
    calls = verify_calls;
    expect(generate(m, GEN_REFUSES), "Generate ran and ended");
    expect(!strcmp(m->setup_error, REFUSAL), "the refusal is shown under step 3, whole");
    expect(verify_calls == calls + 1, "the disc check ran again after the failed prepare, once");
    expect(m->verify.verdict == 3, "the panel now says verification failed: it no longer contradicts the refusal");
    expect(launcher_model_disc_refused_by_setup(m), "the model knows the 'bad' is setup's");
    expect(!strcmp(m->verify.serial, "SLUS-00292") && m->verify.iso_ok,
           "the rows keep what was read: the serial and the header are still true");
    expect(!launcher_model_can_finish_setup(m), "the window cannot be confirmed with the refused file");
    expect(!launcher_model_can_launch(m), "PLAY is not offered for it");
    expect(!strcmp(m->rom_full, first) && m->rom_present, "the file stays selected");

    /* 2. Selecting the same file again changes nothing. */
    launcher_model_set_rom(m, first);
    expect(m->verify.verdict == 3 && launcher_model_disc_refused_by_setup(m),
           "the same file selected again stays refused");

    /* 3. Generate again for the same file, refused again: still refused. */
    expect(generate(m, GEN_REFUSES) && gen_calls == 2, "Generate can be started again for the refused file");
    expect(m->verify.verdict == 3 && launcher_model_disc_refused_by_setup(m), "refused twice is refused");

    /* 4. A later failure that is not a refusal leaves the refusal standing. */
    expect(generate(m, GEN_BUILD_FAILS), "Generate ran and failed for another reason");
    expect(strstr(m->setup_error, "exit 1") != NULL, "the new reason is shown");
    expect(m->verify.verdict == 3 && launcher_model_disc_refused_by_setup(m),
           "the file is still the one setup refused");

    /* 5. The player replaces the file in place and presses Generate: accepted. */
    expect(generate(m, GEN_ACCEPTS), "Generate ran and accepted the disc");
    expect(m->setup_error[0] == '\0', "no error is shown");
    expect(m->verify.verdict == 1, "the panel says verified again");
    expect(!launcher_model_disc_refused_by_setup(m), "nothing is refused any more");
    expect(m->setup_prepare_satisfied && launcher_model_can_finish_setup(m), "the window can be confirmed");
    free(m);

    /* 6. Another file ends the refusal; going back does not bring it back. */
    m = make_model();
    launcher_model_set_rom(m, first);
    generate(m, GEN_REFUSES);
    expect(m->verify.verdict == 3, "refused");
    launcher_model_set_rom(m, rev1);
    expect(m->verify.verdict == 1 && !launcher_model_disc_refused_by_setup(m),
           "another file is judged on its own: verified, not refused");
    expect(m->setup_error[0] == '\0', "the refusal's sentence is gone with the file it was about");
    launcher_model_set_rom(m, first);
    expect(m->verify.verdict == 1 && !launcher_model_disc_refused_by_setup(m),
           "the first file selected again is verified until setup refuses it again");
    free(m);

    /* 6b. Another file that the game program itself calls bad (another
     * game's disc): "bad", but not setup's. */
    m = make_model();
    launcher_model_set_rom(m, first);
    generate(m, GEN_REFUSES);
    expect(launcher_model_disc_refused_by_setup(m), "refused");
    snprintf(program_bad, sizeof(program_bad), "%s", rev1);
    launcher_model_set_rom(m, rev1);
    expect(m->verify.verdict == 3 && !launcher_model_disc_refused_by_setup(m),
           "setup's refusal does not pass to another file that is bad for its own reason");
    free(m);

    /* 7. A build failure, and missing tools: "Disc verified" stays. */
    m = make_model();
    launcher_model_set_rom(m, first);
    expect(generate(m, GEN_BUILD_FAILS), "Generate ran and the build failed");
    expect(m->setup_error[0] != '\0', "the build failure is shown");
    expect(m->verify.verdict == 1, "a build failure does not turn the panel to failed");
    expect(!launcher_model_disc_refused_by_setup(m), "a build failure is not a refusal of the disc");
    expect(generate(m, GEN_NO_TOOLS), "Generate ended at once: no tools");
    expect(m->verify.verdict == 1 && !launcher_model_disc_refused_by_setup(m),
           "missing tools do not turn the panel to failed");
    free(m);

    /* 8. A file the game program itself calls bad is not "refused by setup". */
    m = make_model();
    snprintf(program_bad, sizeof(program_bad), "%s", first);
    launcher_model_set_rom(m, first);
    expect(m->verify.verdict == 3 && !launcher_model_disc_refused_by_setup(m),
           "a wrong serial is the game program's 'bad'");
    expect(generate(m, GEN_BUILD_FAILS), "Generate ran and failed");
    expect(m->verify.verdict == 3 && !launcher_model_disc_refused_by_setup(m),
           "a failed prepare does not relabel the game program's 'bad' as setup's");
    free(m);

    /* 9. A host that answers no disc check, and a cartridge system: untouched. */
    m = make_model();
    m->disc_verify_cb = NULL;
    launcher_model_set_rom(m, first);
    generate(m, GEN_REFUSES);
    expect(m->verify.verdict == 1 && !launcher_model_disc_refused_by_setup(m),
           "a host with no disc check keeps the placeholder verdict");
    free(m);
    m = make_model();
    profile.verify.mode = 0;
    launcher_model_set_rom(m, first);
    calls = verify_calls;
    generate(m, GEN_REFUSES);
    expect(verify_calls == calls && m->verify.verdict == 0 && !launcher_model_disc_refused_by_setup(m),
           "a system without a disc verdict is not asked");
    free(m);
    expect(!launcher_model_disc_refused_by_setup(NULL), "no model: not refused");

    remove(first);
    remove(rev1);
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
