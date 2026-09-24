#include "launcher_model.c"
#include <assert.h>

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }
static int calls, preset_count = 1;
static int count(void *ctx) { (void)ctx; return preset_count; }
static int get(void *ctx, int i, RecompLauncherCModPreset *out) {
    (void)ctx;
    if (i) return 0;
    memset(out, 0, sizeof(*out));
    strcpy(out->id, "test");
    strcpy(out->name, "Test");
    return 1;
}
static const char *current(void *ctx, const RecompLauncherCSettings *settings) {
    (void)ctx;
    return settings->msu1_enabled ? "test" : "";
}
static int apply(void *ctx, const char *id, RecompLauncherCSettings *settings) {
    (void)ctx;
    assert(!strcmp(id, "test"));
    ++calls;
    settings->msu1_enabled = 1;
    return 1;
}

int main(void) {
    static LauncherModel model;
    assert(!launcher_model_preset_count(&model));
    assert(!launcher_model_apply_preset(&model, "test"));
    RecompLauncherCModProvider provider = {0};
    model.mods = &provider;
    assert(!launcher_model_preset_count(&model));
    provider.preset_count = count;
    provider.preset_get = get;
    provider.preset_current = current;
    assert(!launcher_model_preset_count(&model));
    provider.preset_apply = apply;
    assert(launcher_model_preset_count(&model) == 1);
    preset_count = 0;
    assert(!launcher_model_preset_count(&model));
    assert(!launcher_model_apply_preset(&model, "test") && !calls);
    preset_count = 65;
    assert(!launcher_model_preset_count(&model));
    preset_count = 1;

    model.s.volume = 42;
    model.s.fullscreen = 2;
    strcpy(model.s.msu1_dir, "keep this music folder");
    assert(!launcher_model_apply_preset(&model, "missing") && !calls);
    assert(launcher_model_apply_preset(&model, "test") && calls == 1 && model.s.msu1_enabled);
    assert(model.s.volume == 42 && model.s.fullscreen == 2);
    assert(!strcmp(model.s.msu1_dir, "keep this music folder"));

    static const RecompLauncherCMsuPack packs[] = {{"first", "First"}, {"second", "Second"}};
    assert(!launcher_model_set_msu1_pack(&model, "first"));
    model.msu1_supported = true;
    model.msu1_packs = packs;
    model.num_msu1_packs = 2;
    assert(launcher_model_set_msu1_pack(&model, "second"));
    assert(!strcmp(model.s.msu1_pack, "second"));
    assert(!launcher_model_set_msu1_pack(&model, "unknown"));
    assert(!strcmp(model.s.msu1_pack, "second"));
    assert(launcher_model_set_msu1_pack(&model, ""));
    assert(!model.s.msu1_pack[0]);
    assert(!strcmp(model.s.msu1_dir, "keep this music folder"));
    puts("Optional presets and soundtrack choices passed");
    return 0;
}
