#ifndef RECOMP_MODERATION_H
#define RECOMP_MODERATION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-player moderation the LOCAL player controls: who they would rather not
 * hear from, and who they would rather not meet.
 *
 * WHAT THIS IS KEYED ON, AND WHY IT MATTERS
 * -----------------------------------------
 * An opaque account id published by the lobby server -- never a display name.
 * A name is not an identity here: the server's own schema calls its handle
 * "never an identity, is not unique" and player-editable. A list keyed on one
 * blocks whoever renames INTO that name and frees whoever renames out of it,
 * which is worse than having no list at all.
 *
 * A guest has no account and therefore no key. Ignoring one works for as long
 * as their connection lasts and is not written down -- an honest nothing
 * rather than a promise the wire cannot keep.
 *
 * IGNORE vs BLOCK
 * ---------------
 *   ignored  their chat lines are not shown. They may still share a lobby.
 *   blocked  everything ignore does, and they are hidden from the players
 *            list and their rooms from the browser, and the queue is asked
 *            not to pair you.
 *
 * Block implies ignore; the store keeps that true so no caller has to.
 *
 * ONLINE ONLY. A LAN / Direct IP room has no accounts and no server to have
 * published one, so the surface is not offered there at all.
 */

#define RECOMP_MOD_KEY_CAP  40
#define RECOMP_MOD_NAME_CAP 64

typedef enum RecompModLevel {
    RECOMP_MOD_NONE = 0,
    RECOMP_MOD_IGNORED = 1,
    RECOMP_MOD_BLOCKED = 2
} RecompModLevel;

/*
 * Load from `moderation.ini` beside the executable, creating nothing if it is
 * absent. Safe to call more than once; a second call reloads.
 */
void recomp_moderation_load(const char *exe_dir);

/* Written immediately on every change rather than at exit: a launcher that is
 * killed, or a game that takes the process over, must not lose the fact that
 * somebody was blocked. */
int  recomp_moderation_save(void);

RecompModLevel recomp_moderation_level(const char *account);
int  recomp_moderation_is_ignored(const char *account);  /* ignored OR blocked */
int  recomp_moderation_is_blocked(const char *account);

/*
 * `last_seen_name` is stored as a LABEL so the review list can show who an id
 * belongs to. It is never matched on. NULL/"" keeps whatever label is there.
 */
void recomp_moderation_set(const char *account, const char *last_seen_name,
                           RecompModLevel level);

/* The review list, so a block can be undone without editing a file by hand. */
int  recomp_moderation_count(void);
int  recomp_moderation_get(int index, char *account_out, size_t account_cap,
                           char *name_out, size_t name_cap,
                           RecompModLevel *level_out);

/*
 * The blocked ids as one ';'-separated string, for handing to a backend that
 * can ask the matchmaker not to pair you with them. Returns the length that
 * WOULD be written, so truncation is detectable. Empty when nothing is
 * blocked.
 */
int  recomp_moderation_blocked_list(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_MODERATION_H */
