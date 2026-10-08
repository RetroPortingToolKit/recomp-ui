/* The shared launcher queues host work and reads snapshots. These checks cover
 * the boundaries that matter across games: opt-in, exact title/source passing,
 * single-job gating, launch readiness, and honest failure/success results. */
#include "launcher_model.c"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

typedef struct FakeContent {
    RecompLauncherCCustomContentStatus status;
    int starts;
    int folders;
    int reject;
    int installed;
    char source[1024];
    char image[1024];
    char title[128];
    char type[96];
    char folder_id[96];
} FakeContent;

static int entries(void* ctx) { return ((FakeContent*)ctx)->installed; }
static int entry(void* ctx, int index, RecompLauncherCCustomContentEntry* out) {
    FakeContent* f = (FakeContent*)ctx;
    if (index != 0 || !f->installed) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", "safe-generated-id");
    snprintf(out->name, sizeof(out->name), "%s", f->title);
    return 1;
}
static int start(void* ctx, const char* type, const char* source,
                  const char* image, const char* title) {
    FakeContent* f = (FakeContent*)ctx;
    ++f->starts;
    if (f->reject) return 0;
    snprintf(f->source, sizeof(f->source), "%s", source);
    snprintf(f->image, sizeof(f->image), "%s", image);
    snprintf(f->title, sizeof(f->title), "%s", title);
    snprintf(f->type, sizeof(f->type), "%s", type);
    f->status.state = RECOMP_CONTENT_BUSY;
    f->status.progress = -1;
    return 1;
}
static int status(void* ctx, RecompLauncherCCustomContentStatus* out) {
    *out = ((FakeContent*)ctx)->status;
    return 1;
}
static int open_folder(void* ctx, const char* id) {
    FakeContent* f = (FakeContent*)ctx;
    ++f->folders;
    snprintf(f->folder_id, sizeof(f->folder_id), "%s", id);
    return 1;
}
static const char* error(void* ctx) {
    (void)ctx;
    return "Convert this patch to a supported data pack first.";
}

int main(void) {
    static LauncherModel m;
    RecompLauncherCGameInfo game = {0};
    RecompLauncherCSettings settings = {0};
    FakeContent f = {0};
    RecompLauncherCCustomContentProvider p = {0};
    p.ctx = &f;
    p.entry_count = entries;
    p.entry_get = entry;
    p.import_start = start;
    p.import_status = status;
    p.open_folder = open_folder;
    p.last_error = error;

    launcher_model_init(&m, &settings, &game, "");
    assert(!launcher_model_custom_content_available(&m));
    launcher_model_set_view(&m, LNG_VIEW_CUSTOM_CONTENT);
    assert(m.view == LNG_VIEW_DASHBOARD);
    assert(!launcher_model_custom_content_import(&m, "file", "source.zip", "Title"));
    assert(f.starts == 0);

    game.custom_content = &p;
    launcher_model_init(&m, &settings, &game, "");
    assert(launcher_model_custom_content_available(&m));
    /* This suite compiles with Mods disabled: content is its own host opt-in. */
    assert(m.mods == NULL);
    launcher_model_set_view(&m, LNG_VIEW_CUSTOM_CONTENT);
    assert(m.view == LNG_VIEW_CUSTOM_CONTENT);
    assert(!strcmp(launcher_view_name(m.view), "Custom Content"));
    m.rom_present = true;
    snprintf(m.rom_size, sizeof(m.rom_size), "%s", "512 KB");
    snprintf(m.rom_full, sizeof(m.rom_full), "%s", "owner-selected.sfc");
    assert(launcher_model_can_launch(&m));

    assert(!launcher_model_custom_content_import(&m, "file", "", "Title"));
    assert(f.starts == 0);
    assert(launcher_model_custom_content_import(&m, "file", "source.zip", "My / custom title"));
    assert(f.starts == 1);
    assert(!strcmp(f.source, "source.zip"));
    assert(!strcmp(f.image, "owner-selected.sfc"));
    assert(!strcmp(f.title, "My / custom title"));
    assert(!strcmp(f.type, "file"));
    assert(launcher_model_custom_content_busy(&m));
    assert(!launcher_model_can_launch(&m));
    assert(!launcher_model_custom_content_import(&m, "folder", "editor/vsfiles", "Second"));
    assert(f.starts == 1);

    f.status.progress = 45;
    snprintf(f.status.message, sizeof(f.status.message), "%s", "Checking source...");
    launcher_model_custom_content_poll(&m);
    assert(m.content_status.progress == 45);
    assert(!strcmp(m.content_status.message, "Checking source..."));
    f.status.message[0] = '\0';
    assert(!strcmp(m.content_status.message, "Checking source..."));

    f.status.state = RECOMP_CONTENT_FAILED;
    snprintf(f.status.detail, sizeof(f.status.detail), "%s", "Unknown patches need conversion; no files installed.");
    launcher_model_custom_content_poll(&m);
    assert(!launcher_model_custom_content_busy(&m));
    assert(launcher_model_can_launch(&m));
    assert(strstr(m.content_status.detail, "need conversion"));
    assert(entries(&f) == 0);

    f.reject = 1;
    assert(!launcher_model_custom_content_import(&m, "file", "unsupported.bps", "Unsupported"));
    assert(strstr(m.content_error, "supported data pack"));
    assert(!launcher_model_custom_content_busy(&m));
    f.reject = 0;
    assert(launcher_model_custom_content_import(&m, "folder", "editor/vsfiles", "New course"));
    assert(m.content_error[0] == '\0');
    assert(!strcmp(f.type, "folder"));
    f.installed = 1;
    f.status.state = RECOMP_CONTENT_SUCCEEDED;
    f.status.progress = 100;
    snprintf(f.status.message, sizeof(f.status.message), "%s", "Installed New course.");
    launcher_model_custom_content_poll(&m);
    assert(m.content_status.state == RECOMP_CONTENT_SUCCEEDED);
    assert(launcher_model_can_launch(&m));
    RecompLauncherCCustomContentEntry row = {0};
    assert(entry(&f, 0, &row) && !strcmp(row.name, "New course"));
    assert(launcher_model_custom_content_open_folder(&m, ""));
    assert(f.folders == 1 && !strcmp(f.folder_id, ""));
    assert(launcher_model_custom_content_open_folder(&m, row.id));
    assert(!strcmp(f.folder_id, "safe-generated-id"));
    p.open_folder = NULL;
    assert(!launcher_model_custom_content_open_folder(&m, row.id));

    /* A catalog can be read-only; missing importer hooks must never execute. */
    p.import_status = NULL;
    assert(launcher_model_custom_content_available(&m));
    const int previous_starts = f.starts;
    assert(!launcher_model_custom_content_import(&m, "file", "source.zip", "Title"));
    assert(f.starts == previous_starts);
    puts("custom content opt-in and asynchronous provider checks passed");
    return 0;
}
