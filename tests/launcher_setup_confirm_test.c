/* First-run wizard: confirming the disc / ROM records it immediately.
 *
 * Regression it exists for: Confirm disc / Continue to launcher only closed
 * the modal. The sidecars (rom.cfg / disc.cfg / bios.cfg) and the host's
 * persist_setup were written on a BIOS change, before Generate and after a
 * rebuild -- every path except the one where the player confirms a disc that
 * needs none of those. Quit from the dashboard, or let the host relaunch, and
 * the next start opened the same wizard again asking to confirm the pick it
 * had already been given.
 *
 * Includes the model translation unit (as launcher_setup_bios_test.c does) so
 * the static sidecar writer is reachable.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
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

/* Stand-in for the host's persist_setup: records what it was handed. */
static int  persist_calls;
static char persist_rom[512];
static char persist_bios[512];
static int fake_persist(void* ctx, const char* rom, const char* bios) {
    ++*(int*)ctx;
    snprintf(persist_rom, sizeof(persist_rom), "%s", rom ? rom : "(null)");
    snprintf(persist_bios, sizeof(persist_bios), "%s", bios ? bios : "(null)");
    return 0;
}

static int read_first_line(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    out[0] = '\0';
    if (!fgets(out, (int)cap, f)) { fclose(f); return 0; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

/* A cart title (no BIOS) whose build is ready: the wizard opened because
 * nothing was remembered. Same fields launcher_model_init would set. */
static LauncherModel* make_model(void) {
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) return NULL;
    m->setup_wizard_supported = true;
    m->has_bios = false;
    m->setup_wizard_open = true;
    m->setup_page = 1;
    m->persist_setup_cb = fake_persist;
    m->persist_setup_ctx = &persist_calls;
    snprintf(m->rom_size, sizeof(m->rom_size), "--");
    return m;
}

static void test_nothing_to_confirm_persists_nothing(void) {
    LauncherModel* m = make_model();
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    persist_calls = 0;
    remove("rom.cfg");

    launcher_model_finish_setup(m);
    expect(m->setup_wizard_open, "no pick: the wizard stays open");
    expect(persist_calls == 0, "no pick: the host is not asked to persist");
    expect(!read_first_line("rom.cfg", persist_rom, sizeof(persist_rom)),
           "no pick: no rom.cfg is written");
    free(m);
}

static void test_confirm_records_the_pick(const char* dir) {
    char rom[512];
    char line[512];
    LauncherModel* m = make_model();
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(rom, sizeof(rom), "%s/setupconfirm_game.sfc", dir);
    persist_calls = 0;
    persist_rom[0] = '\0';
    remove("rom.cfg");

    /* The pick, as the wizard's Browse leaves it on the model. */
    snprintf(m->rom_full, sizeof(m->rom_full), "%s", rom);
    m->rom_present = true;
    snprintf(m->rom_size, sizeof(m->rom_size), "4.00 MB (32 Mbit)");
    expect(launcher_model_can_finish_setup(m), "a picked ROM can be confirmed");

    launcher_model_finish_setup(m);   /* the Confirm disc / Continue button */

    expect(!m->setup_wizard_open, "Confirm closes the wizard");
    expect(persist_calls == 1,
           "Confirm asks the host to persist, once, right away");
    expect(!strcmp(persist_rom, rom), "the host is handed the confirmed path");
    expect(!strcmp(persist_bios, ""), "a cart title passes no BIOS");
    expect(read_first_line("rom.cfg", line, sizeof(line)) && !strcmp(line, rom),
           "rom.cfg beside the launcher names the confirmed ROM");
    expect(read_first_line("disc.cfg", line, sizeof(line)) && !strcmp(line, rom),
           "disc.cfg (the disc-host spelling) names it too");
    free(m);
}

/* A roster title flushes every located image through persist_setup_discs,
 * never the single-path callback -- the same rule the other flush points use. */
static int   discs_calls;
static int   discs_count;
static char  discs_first[512];
static int fake_persist_discs(void* ctx, const char* const* paths, int n,
                              const char* bios) {
    (void)ctx; (void)bios;
    ++discs_calls;
    discs_count = n;
    snprintf(discs_first, sizeof(discs_first), "%s",
             (n > 0 && paths[0]) ? paths[0] : "");
    return 0;
}

static void test_confirm_flushes_a_roster_through_discs_cb(const char* dir) {
    static char p1[512], p2[512];
    static RecompLauncherCDisc roster[2];
    LauncherModel* m = make_model();
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return; }
    snprintf(p1, sizeof(p1), "%s/setupconfirm_d1.cue", dir);
    snprintf(p2, sizeof(p2), "%s/setupconfirm_d2.cue", dir);
    /* "Located" means the file exists: the roster check is on disk. */
    { FILE* f = fopen(p1, "wb"); if (f) fclose(f); }
    { FILE* f = fopen(p2, "wb"); if (f) fclose(f); }
    roster[0].number = 1; roster[0].label = NULL; roster[0].path = p1;
    roster[1].number = 2; roster[1].label = NULL; roster[1].path = p2;
    m->discs = roster;
    m->num_discs = 2;
    m->persist_setup_discs_cb = fake_persist_discs;
    persist_calls = 0;
    discs_calls = 0;

    snprintf(m->rom_full, sizeof(m->rom_full), "%s", p1);
    m->rom_present = true;
    snprintf(m->rom_size, sizeof(m->rom_size), "512 MiB");
    /* Both slots located: the roster's paths exist on the model. */
    expect(launcher_model_discs_ready_count(m) == 2, "both discs located");
    expect(launcher_model_can_finish_setup(m), "a complete set can be confirmed");

    launcher_model_finish_setup(m);

    expect(!m->setup_wizard_open, "Confirm closes the wizard");
    expect(discs_calls == 1 && persist_calls == 0,
           "a set goes through persist_setup_discs, not the single-path flush");
    expect(discs_count == 2 && !strcmp(discs_first, p1),
           "every located image is handed over, in order");
    free(m);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    if (chdir(dir) != 0) {
        fprintf(stderr, "FAIL: cannot chdir to %s\n", dir);
        return 1;
    }
    test_nothing_to_confirm_persists_nothing();
    test_confirm_records_the_pick(dir);
    test_confirm_flushes_a_roster_through_discs_cb(dir);
    if (fails) { fprintf(stderr, "\n%d FAILED\n", fails); return 1; }
    printf("\nall passed\n");
    return 0;
}
