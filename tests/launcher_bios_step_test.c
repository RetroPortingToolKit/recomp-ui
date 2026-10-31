/* The BIOS step of the setup window in a build that holds no BIOS of its own
 * (PS1B-420).
 *
 * Every PlayStation kit's setup window said "1. PlayStation BIOS (optional)",
 * "OpenBIOS is used unless you browse for a retail dump", and offered "Use
 * OpenBIOS". The pin H kits hold no OpenBIOS: the button cleared the BIOS the
 * player had chosen and switched to nothing, and Generate could be pressed
 * with no BIOS and ended in a command-line hint.
 *
 * The launcher learns which kind of build it is from an answer it already
 * asks for: the host's verdict on an EMPTY BIOS path. Tested here, at the
 * model:
 *
 *   - launcher_model_bundled_bios_offered: yes for a build that accepts the
 *     empty path, no for one that refuses it, whatever BIOS is chosen;
 *   - launcher_model_setup_bios_blocks_generate: Generate waits for a usable
 *     BIOS in a build that holds none, and only there.
 *
 * Includes the model translation unit (as the other model tests do).
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to the BIOS. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

/* ---- stand-in hosts ------------------------------------------------------ */

static int empty_calls;
static char last_verified_path[512];

/* A build that holds OpenBIOS: the empty path is fine. */
static int host_with_openbios(const char* path, RecompLauncherCBiosVerify* out) {
    safe_copy(last_verified_path, sizeof(last_verified_path), path);
    memset(out, 0, sizeof(*out));
    if (!path[0]) {
        ++empty_calls;
        out->ok = 1;
        snprintf(out->detail, sizeof(out->detail), "Using bundled OpenBIOS.");
        return 1;
    }
    if (strstr(path, "good")) { out->ok = 1; snprintf(out->detail, sizeof(out->detail), "SCPH1001.BIN (CRC OK)."); }
    else if (strstr(path, "other")) { out->needs_regen = 1; snprintf(out->detail, sizeof(out->detail), "not compiled into this build"); }
    else snprintf(out->detail, sizeof(out->detail), "BIOS must be exactly 524288 bytes (got 12).");
    return 1;
}

/* A pin H kit: no OpenBIOS, the empty path is refused. */
static int host_without_openbios(const char* path, RecompLauncherCBiosVerify* out) {
    safe_copy(last_verified_path, sizeof(last_verified_path), path);
    memset(out, 0, sizeof(*out));
    if (!path[0]) {
        ++empty_calls;
        snprintf(out->detail, sizeof(out->detail), "PlayStation BIOS required (SCPH1001.BIN).");
        return 1;
    }
    return host_with_openbios(path, out);
}

/* A host that does not answer. */
static int host_silent(const char* path, RecompLauncherCBiosVerify* out) {
    (void)path; (void)out;
    return 0;
}

/* A console whose BIOS is not needed for the selected game. */
static int host_not_needed(const char* path, RecompLauncherCBiosVerify* out) {
    (void)path;
    memset(out, 0, sizeof(*out));
    out->not_needed = 1;
    return 1;
}

static LauncherModel* make_model(int (*host)(const char*, RecompLauncherCBiosVerify*)) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) return NULL;
    m->has_bios = true;
    m->setup_wizard_supported = true;
    m->bios_verify_cb = host;
    launcher_model_refresh_bios_status(m);
    return m;
}

static void choose(LauncherModel* m, const char* path) {
    safe_copy(m->s.bios_path, sizeof(m->s.bios_path), path);
    launcher_model_refresh_bios_status(m);
}

int main(void) {
    LauncherModel* m;

    /* A kit that holds OpenBIOS: today's words, and Generate never waits. */
    m = make_model(host_with_openbios);
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
    expect(launcher_model_bundled_bios_offered(m), "a build that accepts no BIOS offers its own");
    expect(m->setup_bios_ok, "nothing chosen is OK there");
    expect(!launcher_model_setup_bios_blocks_generate(m), "Generate does not wait: OpenBIOS will do");
    choose(m, "D:/bios/good.bin");
    expect(launcher_model_bundled_bios_offered(m), "still offered with a BIOS chosen");
    expect(!launcher_model_setup_bios_blocks_generate(m), "Generate does not wait with a BIOS chosen");
    choose(m, "D:/bios/bad.bin");
    expect(!m->setup_bios_ok && launcher_model_bundled_bios_offered(m), "an unusable file does not change what the build holds");
    expect(!launcher_model_setup_bios_blocks_generate(m), "and Generate still does not wait in such a build");
    free(m);

    /* A pin H kit: the BIOS is the player's own. */
    m = make_model(host_without_openbios);
    expect(!launcher_model_bundled_bios_offered(m), "a build that refuses no BIOS offers none of its own");
    expect(!m->setup_bios_ok, "nothing chosen: needed");
    expect(strstr(m->setup_bios_detail, "PlayStation BIOS required") != NULL, "the host's sentence says so");
    expect(launcher_model_setup_bios_blocks_generate(m), "Generate waits while no BIOS is chosen");
    expect(!launcher_model_can_finish_setup(m), "as Confirm does");
    choose(m, "D:/bios/good.bin");
    expect(m->setup_bios_ok, "the kit's BIOS chosen: OK");
    expect(!launcher_model_bundled_bios_offered(m), "the answer does not depend on the BIOS that is chosen");
    expect(!launcher_model_setup_bios_blocks_generate(m), "Generate may be pressed");
    {
        const int before = empty_calls;
        for (int frame = 0; frame < 120; ++frame) {
            (void)launcher_model_bundled_bios_offered(m);
            (void)launcher_model_setup_bios_blocks_generate(m);
        }
        expect(empty_calls == before, "drawing does not recheck the empty BIOS path");
        expect(strcmp(last_verified_path, "D:/bios/good.bin") == 0,
               "refresh and drawing leave the host report on the chosen BIOS");
        expect(strstr(m->setup_bios_detail, "CRC OK") != NULL,
               "the chosen BIOS detail survives the empty-path offer check");
    }
    choose(m, "D:/bios/other.bin");
    expect(!m->setup_bios_ok && m->setup_bios_needs_regen, "another real BIOS: it has to be built in");
    expect(!launcher_model_setup_bios_blocks_generate(m), "Generate may be pressed: it is what builds it in");
    choose(m, "D:/bios/bad.bin");
    expect(!m->setup_bios_ok && !m->setup_bios_needs_regen, "a file that is no BIOS: not usable");
    expect(launcher_model_setup_bios_blocks_generate(m), "Generate waits");
    choose(m, "");
    expect(launcher_model_setup_bios_blocks_generate(m), "the choice cleared: Generate waits again");
    {
        const int before = empty_calls;
        (void)launcher_model_bundled_bios_offered(m);
        expect(empty_calls == before, "the offer query uses the refreshed empty-path answer");
    }
    free(m);

    /* No host to ask, a host that does not answer, no BIOS in this console. */
    m = make_model(NULL);
    expect(launcher_model_bundled_bios_offered(m), "no host to ask: the words stay as they were");
    expect(!launcher_model_setup_bios_blocks_generate(m), "and Generate does not wait");
    free(m);
    m = make_model(host_silent);
    expect(launcher_model_bundled_bios_offered(m), "a host that does not answer: the words stay as they were");
    expect(!launcher_model_setup_bios_blocks_generate(m), "and Generate does not wait");
    free(m);
    m = make_model(host_not_needed);
    expect(launcher_model_bundled_bios_offered(m), "a BIOS that is not needed counts as none required");
    free(m);
    m = make_model(host_without_openbios);
    m->has_bios = false;
    expect(!launcher_model_setup_bios_blocks_generate(m), "a console with no BIOS step never waits for one");
    free(m);
    expect(!launcher_model_bundled_bios_offered(NULL), "no model: nothing offered");
    expect(!launcher_model_setup_bios_blocks_generate(NULL), "no model: nothing waits");

    if (fails) { fprintf(stderr, "launcher bios step: %d check(s) failed\n", fails); return 1; }
    printf("launcher bios step: ok\n");
    return 0;
}
