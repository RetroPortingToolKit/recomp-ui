#include "launcher_mod_text_path.h"
#include <stdlib.h>
#include <string.h>

typedef struct TextPath {
    const RecompLauncherCModProvider* provider;
    char package_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char feature_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char option_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    RecompLauncherCModTextPathV1 info;
    struct TextPath* next;
} TextPath;
static TextPath* paths;

static int valid_id(const char* id, int empty) {
    return id && (empty || id[0]) && strlen(id) < RECOMP_LAUNCHER_MOD_ID_MAX;
}
static TextPath** find_path(const RecompLauncherCModProvider* provider,
        const char* package, const char* feature, const char* option) {
    TextPath** entry = &paths;
    while (*entry && ((*entry)->provider != provider ||
           strcmp((*entry)->package_id, package) ||
           strcmp((*entry)->feature_id, feature) ||
           strcmp((*entry)->option_id, option))) entry = &(*entry)->next;
    return entry;
}
int recomp_launcher_register_mod_text_path_v1(
        const RecompLauncherCModProvider* provider, const char* package,
        const char* feature, const char* option,
        const RecompLauncherCModTextPathV1* info) {
    if (!feature) feature = "";
    if (!provider || !valid_id(package, 0) || !valid_id(feature, 1) ||
        !valid_id(option, 0)) return 0;
    if (info) {
        if (info->pattern_count < 0 || info->pattern_count > RECOMP_LAUNCHER_TEXT_PATH_PATTERNS ||
            !memchr(info->filter_description, 0, sizeof(info->filter_description))) return 0;
        for (int i=0; i<info->pattern_count; ++i)
            if (!info->patterns[i][0] || !memchr(info->patterns[i], 0, sizeof(info->patterns[i]))) return 0;
    }
    TextPath** slot = find_path(provider, package, feature, option);
    if (!info) {
        if (*slot) { TextPath* old=*slot; *slot=old->next; free(old); }
        return 1;
    }
    if (!*slot) {
        unsigned count=0; for (TextPath* p=paths; p; p=p->next) ++count;
        if (count >= 128) return 0;
        *slot=(TextPath*)calloc(1,sizeof(TextPath));
        if (!*slot) return 0;
        (*slot)->provider=provider;
        strcpy((*slot)->package_id,package); strcpy((*slot)->feature_id,feature);
        strcpy((*slot)->option_id,option);
    }
    memset(&(*slot)->info,0,sizeof((*slot)->info));
    strcpy((*slot)->info.filter_description,info->filter_description);
    (*slot)->info.pattern_count=info->pattern_count;
    for (int i=0;i<info->pattern_count;++i) strcpy((*slot)->info.patterns[i],info->patterns[i]);
    return 1;
}
int recomp_launcher_get_mod_text_path_v1(
        const RecompLauncherCModProvider* provider, const char* package,
        const char* feature, const char* option, RecompLauncherCModTextPathV1* out) {
    if (out) memset(out,0,sizeof(*out));
    if (!feature) feature="";
    if (!provider || !valid_id(package,0) || !valid_id(feature,1) || !valid_id(option,0)) return 0;
    TextPath* entry=*find_path(provider,package,feature,option);
    if (!entry) return 0;
    if (out) *out=entry->info;
    return 1;
}
void recomp_launcher_unregister_mod_text_paths(const RecompLauncherCModProvider* provider) {
    TextPath** slot=&paths;
    while (*slot) {
        if ((*slot)->provider==provider) {TextPath* old=*slot;*slot=old->next;free(old);}
        else slot=&(*slot)->next;
    }
}
int launcher_mod_text_path_set(const RecompLauncherCModProvider* provider,
        const char* package, const char* feature, const char* option, const char* value) {
    if (!provider || !package || !option || !value ||
        strlen(value)>=RECOMP_LAUNCHER_MOD_VALUE_MAX) return 0;
    if (feature && feature[0]) return provider->feature_set_option &&
        provider->feature_set_option(provider->ctx,package,feature,option,value);
    return provider->set_option && provider->set_option(provider->ctx,package,option,value);
}
