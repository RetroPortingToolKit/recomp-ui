/* Multi-disc roster: sibling autofill and per-slot binding.
 *
 * Includes the model translation unit so the static path-rewriting helper is
 * reachable. That helper is where the risk lives -- it builds candidate paths
 * that are then existence-checked, so a subtly wrong candidate does not fail
 * loudly, it just silently finds nothing.
 *
 * Regression it exists for: the rewriter first emitted a normalised lowercase
 * "disc N" instead of preserving the source's own "Disc N", which found every
 * sibling on Windows and none on Linux.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in by the model TU; unrelated to disc paths. */
void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect(int cond, const char* what) {
    if (cond) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static void touch(const char* path) {
    FILE* f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

static void test_token_rewriting(void) {
    char out[512];

    expect(lm_swap_disc_token("/x/Foo (Disc 1)/Foo (Disc 1).cue", "disc", 1,
                              1, 2, out, sizeof(out)) == 2 &&
           !strcmp(out, "/x/Foo (Disc 2)/Foo (Disc 2).cue"),
           "rewrites every occurrence, preserving capitalisation");

    expect(lm_swap_disc_token("/x/game cd1.cue", "cd", 0, 1, 3,
                              out, sizeof(out)) == 1 &&
           !strcmp(out, "/x/game cd3.cue"),
           "cd1 -> cd3 with no separator");

    expect(lm_swap_disc_token("/x/Final Fantasy 11 (Disc 1).cue", "disc", 1,
                              1, 2, out, sizeof(out)) == 1 &&
           !strcmp(out, "/x/Final Fantasy 11 (Disc 2).cue"),
           "an unrelated number in the title is left alone");

    expect(lm_swap_disc_token("/x/Foo (Disc 12).cue", "disc", 1, 1, 2,
                              out, sizeof(out)) == 0,
           "Disc 1 does not match inside Disc 12");
}

static void test_autofill(const char* dir) {
    static RecompLauncherCDisc roster[3];
    char p[3][512];
    LauncherModel* m;
    int i, filled;

    for (i = 0; i < 3; ++i) {
        snprintf(p[i], sizeof(p[i]), "%s/Game (Disc %d).cue", dir, i + 1);
        roster[i].number = i + 1;
        roster[i].label = NULL;
        roster[i].path = "";
    }
    /* Only discs 1 and 3 exist: disc 2 must stay empty rather than be bound to
     * a path that is merely plausible. */
    touch(p[0]);
    touch(p[2]);

    m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    m->discs = roster;
    m->num_discs = 3;
    m->disc_selected = 0;
    safe_copy(m->disc_path_override[0], sizeof(m->disc_path_override[0]), p[0]);

    expect(launcher_model_discs_ready_count(m) == 1, "seeded with 1 of 3 located");

    filled = launcher_model_autofill_sibling_discs(m);
    expect(filled == 1, "fills only the sibling that exists");
    expect(launcher_model_disc_ready(m, 2), "disc 3 located");
    expect(!launcher_model_disc_ready(m, 1),
           "disc 2 left empty -- a missing file is not guessed at");
    expect(!strcmp(launcher_model_disc_path(m, 0), p[0]), "disc 1 untouched");
    expect(m->disc_selected == 0, "autofill never moves the mount");
    expect(m->rom_full[0] == '\0', "autofill never binds the ROM");

    /* Re-running must be idempotent, not additive. */
    expect(launcher_model_autofill_sibling_discs(m) == 0,
           "a second pass fills nothing new");

    /* Binding a non-selected slot is bookkeeping only. */
    launcher_model_set_disc_path(m, 1, p[1]);
    expect(m->disc_selected == 0 && m->rom_full[0] == '\0',
           "binding a non-selected slot leaves the mount alone");

    free(m);
    remove(p[0]);
    remove(p[2]);
}

static int fixture_sbi_status;
static int fixture_verify(const char* path, RecompLauncherCDiscVerify* out) {
    (void)path;
    out->sbi_status = fixture_sbi_status;
    out->iso_ok = 1;
    out->verdict = fixture_sbi_status == RECOMP_SBI_MISSING ? 3 : 1;
    return 1;
}

static int fixture_import_ok;
static int fixture_import(const char* disc, const char* sbi, char* out, size_t cap, char* error, size_t error_cap) {
    expect(!strcmp(disc, "selected.cue"), "SBI callback receives the selected disc");
    expect(!strcmp(sbi, "matching.SBI"), "SBI suffix accepts uppercase");
    if (fixture_import_ok == 2) {
        if (cap <= 600) {
            safe_copy(error, error_cap, "Imported disc path is too long");
            return 0;
        }
        memset(out, 'x', 600);
        out[600] = '\0';
        return 1;
    }
    if (!fixture_import_ok) {
        safe_copy(error, error_cap, "Wrong companion");
        return 0;
    }
    safe_copy(out, cap, "imported.cue");
    return 1;
}

static void test_sbi_verification_refresh(void) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    SystemProfile profile = {0};
    if (!m) { ++fails; return; }
    profile.verify.mode = 1;
    m->profile = &profile;
    m->rom_present = true;
    m->disc_verify_cb = fixture_verify;
    for (int status = RECOMP_SBI_MISSING; status <= RECOMP_SBI_OK; ++status) {
        fixture_sbi_status = status;
        run_verify(m);
        expect(m->verify.sbi_status == status, "selected disc carries current SBI status");
        expect(m->verify.verdict == (status == RECOMP_SBI_MISSING ? 3 : 1), "missing SBI fails disc verification");
    }
    fixture_sbi_status = RECOMP_SBI_NA;
    run_verify(m);
    expect(m->verify.sbi_status == RECOMP_SBI_NA, "unlisted disc clears previous SBI state");
    m->rom_present = false;
    run_verify(m);
    expect(m->verify.sbi_status == RECOMP_SBI_NA, "empty selection clears SBI state");
    launcher_model_set_rom(m, "matching.SBI");
    expect(!m->rom_present && m->setup_error[0], "SBI without disc shows an error and keeps empty selection");
    launcher_model_set_rom(m, "selected.cue");
    m->import_sbi_cb = fixture_import;
    launcher_model_set_rom(m, "matching.SBI");
    expect(!strcmp(m->rom_full, "selected.cue") && m->setup_error[0], "rejected SBI preserves the selected disc");
    fixture_import_ok = 1;
    fixture_import_ok = 2;
    launcher_model_set_rom(m, "matching.SBI");
    expect(!strcmp(m->rom_full, "selected.cue") && m->setup_error[0], "oversized imported path is rejected instead of truncated");
    fixture_import_ok = 1;
    launcher_model_set_rom(m, "matching.SBI");
    expect(!strcmp(m->rom_full, "imported.cue") && !m->setup_error[0], "accepted SBI selects its mounted CUE and clears error");
    RecompLauncherCDisc roster[2] = {{0}};
    roster[0].path = "selected.cue";
    roster[1].path = "selected.cue";
    m->discs = roster;
    m->num_discs = 2;
    m->disc_selected = 0;
    fixture_import_ok = 0;
    launcher_model_set_disc_path(m, 1, "matching.SBI");
    expect(!strcmp(launcher_model_disc_path(m, 1), "selected.cue"), "rejected slot SBI preserves roster path");
    fixture_import_ok = 1;
    launcher_model_set_disc_path(m, 1, "matching.SBI");
    expect(!strcmp(launcher_model_disc_path(m, 1), "imported.cue"), "slot SBI stores imported CUE, never the SBI");
    expect(m->disc_selected == 0, "importing for another slot does not move the mount");
    free(m);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    test_token_rewriting();
    test_autofill(dir);
    test_sbi_verification_refresh();
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
