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
    int field_count;
    int field_gets;
    int submits;
    int cancels;
    int reject_review;
    char review_error[256];
    RecompLauncherCCustomContentField fields[RECOMP_LAUNCHER_CONTENT_REVIEW_MAX_FIELDS];
    RecompLauncherCCustomContentValue submitted[RECOMP_LAUNCHER_CONTENT_REVIEW_MAX_FIELDS];
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
    FakeContent* f = (FakeContent*)ctx;
    if (f->review_error[0]) return f->review_error;
    return "Convert this patch to a supported data pack first.";
}

static int review_count(void* ctx) { return ((FakeContent*)ctx)->field_count; }
static int review_field(void* ctx, int index, RecompLauncherCCustomContentField* out) {
    FakeContent* f = (FakeContent*)ctx;
    ++f->field_gets;
    if (index < 0 || index >= f->field_count || index >= RECOMP_LAUNCHER_CONTENT_REVIEW_MAX_FIELDS) return 0;
    *out = f->fields[index];
    return 1;
}
static int review_option(void* ctx, const char* id, int index,
                         RecompLauncherCCustomContentOption* out) {
    (void)ctx;
    if (strcmp(id, "course_1_cup") || index < 0 || index > 1) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->value, sizeof(out->value), "%d", index);
    snprintf(out->label, sizeof(out->label), "%s", index ? "King Cup" : "Knight Cup");
    return 1;
}
static int review_submit(void* ctx, const RecompLauncherCCustomContentValue* values, int count) {
    FakeContent* f = (FakeContent*)ctx;
    ++f->submits;
    assert(count == f->field_count);
    memcpy(f->submitted, values, (size_t)count * sizeof(*values));
    if (f->reject_review) return 0;
    f->status.state = RECOMP_CONTENT_BUSY;
    return 1;
}
static int review_cancel(void* ctx) {
    FakeContent* f = (FakeContent*)ctx;
    ++f->cancels;
    if (f->reject_review) return 0;
    f->status.state = RECOMP_CONTENT_IDLE;
    return 1;
}

int main(void) {
    static LauncherModel m;
    RecompLauncherCGameInfo game = {0};
    RecompLauncherCSettings settings = {0};
    static FakeContent f;
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

    /* Review is an additional opt-in. Null callbacks cannot submit or cancel. */
    p.import_status = status;
    f.status.state = RECOMP_CONTENT_NEEDS_INPUT;
    launcher_model_custom_content_poll(&m);
    assert(!launcher_model_custom_content_review_available(&m));
    assert(!launcher_model_custom_content_review_submit(&m));
    assert(!launcher_model_custom_content_review_cancel(&m));
    assert(launcher_model_custom_content_busy(&m));
    launcher_model_set_view(&m, LNG_VIEW_DASHBOARD);
    assert(!launcher_model_can_launch(&m));

    p.review_field_count = review_count;
    p.review_field_get = review_field;
    p.review_option_get = review_option;
    p.review_submit = review_submit;
    p.review_cancel = review_cancel;
    f.field_count = 2;
    snprintf(f.fields[0].id, sizeof(f.fields[0].id), "%s", "pack_name");
    snprintf(f.fields[0].label, sizeof(f.fields[0].label), "%s", "Pack name");
    snprintf(f.fields[0].value, sizeof(f.fields[0].value), "%s", "Source title");
    f.fields[0].type = RECOMP_CONTENT_FIELD_TEXT;
    snprintf(f.fields[1].id, sizeof(f.fields[1].id), "%s", "course_1_cup");
    snprintf(f.fields[1].label, sizeof(f.fields[1].label), "%s", "Course 1 cup");
    snprintf(f.fields[1].value, sizeof(f.fields[1].value), "%s", "0");
    f.fields[1].type = RECOMP_CONTENT_FIELD_CHOICE;
    f.fields[1].option_count = 2;
    f.status.state = RECOMP_CONTENT_BUSY;
    launcher_model_custom_content_poll(&m);
    f.status.state = RECOMP_CONTENT_NEEDS_INPUT;
    launcher_model_custom_content_poll(&m);
    assert(launcher_model_custom_content_review_available(&m));
    assert(m.content_review_initialized && m.content_review_count == 2);
    assert(!strcmp(m.content_review_values[0].value, "Source title"));
    assert(!strcmp(m.content_review_values[1].value, "0"));
    assert(f.field_gets == 2);
    snprintf(m.content_review_values[0].value, sizeof(m.content_review_values[0].value), "%s", "My edited title");
    snprintf(m.content_review_values[1].value, sizeof(m.content_review_values[1].value), "%s", "1");
    snprintf(f.fields[0].value, sizeof(f.fields[0].value), "%s", "Updated host default");
    for (int i = 0; i < 5; ++i) launcher_model_custom_content_poll(&m);
    assert(f.field_gets == 2);
    assert(!strcmp(m.content_review_values[0].value, "My edited title"));
    assert(!launcher_model_custom_content_import(&m, "file", "second.zip", "Second"));
    assert(f.starts == previous_starts);
    f.reject_review = 1;
    snprintf(f.review_error, sizeof(f.review_error), "%s", "Enter a pack name.");
    assert(!launcher_model_custom_content_review_submit(&m));
    assert(m.content_status.state == RECOMP_CONTENT_NEEDS_INPUT);
    assert(m.content_review_initialized && m.content_review_count == 2);
    assert(!strcmp(m.content_error, "Enter a pack name."));
    assert(!strcmp(f.submitted[0].id, "pack_name"));
    assert(!strcmp(f.submitted[0].value, "My edited title"));
    assert(!strcmp(f.submitted[1].value, "1"));
    launcher_model_custom_content_poll(&m);
    assert(!strcmp(m.content_review_values[0].value, "My edited title"));
    f.reject_review = 0;
    assert(launcher_model_custom_content_review_submit(&m));
    assert(m.content_status.state == RECOMP_CONTENT_BUSY);
    assert(!m.content_review_initialized && m.content_review_count == 0);
    assert(m.content_error[0] == '\0');
    assert(!launcher_model_can_launch(&m));

    /* Another review stage starts from new defaults, and cancel restores launch. */
    f.status.state = RECOMP_CONTENT_NEEDS_INPUT;
    launcher_model_custom_content_poll(&m);
    assert(!strcmp(m.content_review_values[0].value, "Updated host default"));
    f.reject_review = 1;
    assert(!launcher_model_custom_content_review_cancel(&m));
    assert(m.content_review_initialized);
    f.reject_review = 0;
    assert(launcher_model_custom_content_review_cancel(&m));
    assert(f.cancels == 2);
    assert(m.content_review_count == 0 && !m.content_review_initialized);
    assert(launcher_model_can_launch(&m));

    /* Ordinary larger packs are supported; schema overflow has a safe cancel. */
    f.field_count = RECOMP_LAUNCHER_CONTENT_REVIEW_MAX_FIELDS;
    for (int i = 0; i < f.field_count; ++i) {
        memset(&f.fields[i], 0, sizeof(f.fields[i]));
        snprintf(f.fields[i].id, sizeof(f.fields[i].id), "field_%d", i);
        snprintf(f.fields[i].value, sizeof(f.fields[i].value), "Value %d", i);
    }
    f.status.state = RECOMP_CONTENT_NEEDS_INPUT;
    launcher_model_custom_content_poll(&m);
    assert(m.content_review_count == 256 && m.content_review_initialized);
    assert(!strcmp(m.content_review_values[255].value, "Value 255"));
    assert(launcher_model_custom_content_review_cancel(&m));
    f.field_count = RECOMP_LAUNCHER_CONTENT_REVIEW_MAX_FIELDS + 1;
    f.status.state = RECOMP_CONTENT_NEEDS_INPUT;
    launcher_model_custom_content_poll(&m);
    assert(!m.content_review_initialized && m.content_review_count == 0);
    assert(!launcher_model_custom_content_review_submit(&m));
    assert(launcher_model_custom_content_review_cancel(&m));
    puts("custom content opt-in, async import, and review-state checks passed");
    return 0;
}
