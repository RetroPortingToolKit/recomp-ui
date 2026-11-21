/* recomp_moderation.c — see recomp_moderation.h. */

#include "recomp_moderation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_MAX_ENTRIES 512

typedef struct {
    char key[RECOMP_MOD_KEY_CAP];
    char name[RECOMP_MOD_NAME_CAP];
    RecompModLevel level;
} ModEntry;

static ModEntry g_entries[MOD_MAX_ENTRIES];
static int      g_count;
static char     g_path[1024];
static int      g_loaded;

static void trim(char *s)
{
    size_t n;
    char *p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static ModEntry *find(const char *account)
{
    int i;
    if (!account || !account[0]) return NULL;
    for (i = 0; i < g_count; ++i)
        if (strcmp(g_entries[i].key, account) == 0) return &g_entries[i];
    return NULL;
}

void recomp_moderation_load(const char *exe_dir)
{
    FILE *f;
    char line[512];
    RecompModLevel section = RECOMP_MOD_NONE;

    g_count = 0;
    g_loaded = 1;
    snprintf(g_path, sizeof(g_path), "%s/moderation.ini",
             (exe_dir && exe_dir[0]) ? exe_dir : ".");
    f = fopen(g_path, "rb");
    if (!f) return;   /* nothing blocked yet is the overwhelmingly common case */

    while (fgets(line, sizeof(line), f)) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            if (!strcmp(line, "[ignored]"))      section = RECOMP_MOD_IGNORED;
            else if (!strcmp(line, "[blocked]")) section = RECOMP_MOD_BLOCKED;
            else                                 section = RECOMP_MOD_NONE;
            continue;
        }
        if (section == RECOMP_MOD_NONE) continue;
        if (g_count >= MOD_MAX_ENTRIES) break;
        eq = strchr(line, '=');
        if (eq) { *eq = '\0'; ++eq; } else { eq = (char *)""; }
        trim(line);
        trim(eq);
        /* A key longer than the field is not one of ours -- a hand-edited
         * file, or a different format. Skipped rather than truncated: a
         * truncated key would silently match nobody, or worse, somebody. */
        if (!line[0] || strlen(line) >= sizeof(g_entries[g_count].key)) continue;
        memcpy(g_entries[g_count].key, line, strlen(line) + 1);
        snprintf(g_entries[g_count].name, sizeof(g_entries[g_count].name), "%s", eq);
        g_entries[g_count].level = section;
        ++g_count;
    }
    fclose(f);
}

int recomp_moderation_save(void)
{
    FILE *f;
    int i;
    if (!g_loaded) return -1;
    f = fopen(g_path, "wb");
    if (!f) return -1;
    fprintf(f,
        "# Players you have chosen not to hear from, or not to meet.\n"
        "#\n"
        "# The key on the left is an opaque account id from the lobby server.\n"
        "# The name on the right is only a LABEL, so this file is readable --\n"
        "# it is never matched on, and it is whatever the player was last\n"
        "# called. Renaming does not escape this list, and nobody inherits an\n"
        "# entry by taking a name.\n"
        "#\n"
        "#   [ignored]  their chat is hidden\n"
        "#   [blocked]  hidden everywhere, and the matchmaker is asked not to\n"
        "#              pair you\n"
        "#\n"
        "# Written by the launcher; safe to edit by hand while it is closed.\n");
    for (i = 0; i < 2; ++i) {
        const RecompModLevel want = i == 0 ? RECOMP_MOD_IGNORED : RECOMP_MOD_BLOCKED;
        int j, any = 0;
        for (j = 0; j < g_count; ++j) {
            if (g_entries[j].level != want) continue;
            if (!any) {
                fprintf(f, "\n[%s]\n", want == RECOMP_MOD_IGNORED ? "ignored" : "blocked");
                any = 1;
            }
            fprintf(f, "%s = %s\n", g_entries[j].key, g_entries[j].name);
        }
    }
    fclose(f);
    return 0;
}

RecompModLevel recomp_moderation_level(const char *account)
{
    const ModEntry *e = find(account);
    return e ? e->level : RECOMP_MOD_NONE;
}

/* Block implies ignore, so every "should I hide this line?" caller asks one
 * question instead of remembering to ask two. */
int recomp_moderation_is_ignored(const char *account)
{
    return recomp_moderation_level(account) != RECOMP_MOD_NONE;
}

int recomp_moderation_is_blocked(const char *account)
{
    return recomp_moderation_level(account) == RECOMP_MOD_BLOCKED;
}

void recomp_moderation_set(const char *account, const char *last_seen_name,
                           RecompModLevel level)
{
    ModEntry *e;
    if (!account || !account[0]) return;   /* a guest has no durable identity */
    e = find(account);
    if (level == RECOMP_MOD_NONE) {
        if (!e) return;
        *e = g_entries[--g_count];         /* order is not meaningful here */
        recomp_moderation_save();
        return;
    }
    if (!e) {
        if (g_count >= MOD_MAX_ENTRIES) return;
        e = &g_entries[g_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", account);
    }
    if (last_seen_name && last_seen_name[0])
        snprintf(e->name, sizeof(e->name), "%s", last_seen_name);
    e->level = level;
    /* Saved on every change, not at exit: the launcher hands the process to
     * the game, and a block that only reached memory would be lost there. */
    recomp_moderation_save();
}

int recomp_moderation_count(void) { return g_count; }

int recomp_moderation_get(int index, char *account_out, size_t account_cap,
                          char *name_out, size_t name_cap,
                          RecompModLevel *level_out)
{
    if (index < 0 || index >= g_count) return 0;
    if (account_out && account_cap)
        snprintf(account_out, account_cap, "%s", g_entries[index].key);
    if (name_out && name_cap)
        snprintf(name_out, name_cap, "%s", g_entries[index].name);
    if (level_out) *level_out = g_entries[index].level;
    return 1;
}

int recomp_moderation_blocked_list(char *out, size_t cap)
{
    int i;
    size_t n = 0;
    if (out && cap) out[0] = '\0';
    for (i = 0; i < g_count; ++i) {
        size_t need;
        if (g_entries[i].level != RECOMP_MOD_BLOCKED) continue;
        need = strlen(g_entries[i].key) + (n ? 1u : 0u);
        if (out && n + need + 1 <= cap) {
            if (n) out[n++] = ';';
            memcpy(out + n, g_entries[i].key, strlen(g_entries[i].key));
            n += strlen(g_entries[i].key);
            out[n] = '\0';
        } else {
            n += need;
        }
    }
    return (int)n;
}
