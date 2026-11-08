/* Optional PS1 card editor host service. No engine or C++ dependencies. */
#ifndef RECOMP_CARD_MANAGER_H
#define RECOMP_CARD_MANAGER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RECOMP_CARD_MANAGER_ABI 1
/* All strings are UTF-8. View pointers remain borrowed until the next view of that slot, command
 * or destruction. No call may run concurrently with another service call.
 * The host owns the service and all card drafts, the UI owns selection/pickers.
 * This is a source-level extension, not a stable binary-plugin ABI. */
typedef struct RecompCardSave {
    const char *name, *title, *region, *game_id, *raw_title;
    size_t bytes;
    unsigned blocks, icon_frames;
    uint32_t icons[3][256]; /* RGBA; low byte red */
} RecompCardSave;
typedef struct RecompCardView {
    const char *path, *error, *diagnostics;
    int loaded, valid, editable, writable, dirty, free_blocks;
    uint16_t used_blocks;
    uint64_t revision;
    size_t count;
    const RecompCardSave *saves; /* at most 15 */
} RecompCardView;
typedef enum RecompCardOp {
    RECOMP_CARD_OPEN, RECOMP_CARD_DISCARD, RECOMP_CARD_COPY, RECOMP_CARD_DELETE,
    RECOMP_CARD_IMPORT, RECOMP_CARD_EXPORT, RECOMP_CARD_SAVE, RECOMP_CARD_SAVE_AS,
    RECOMP_CARD_BACKUP_PRIMARY, RECOMP_CARD_BACKUP_SECONDARY
} RecompCardOp;
typedef struct RecompCardCommand {
    RecompCardOp op;
    int slot, destination, confirmed;
    size_t entry;
    uint64_t revision; /* expected source/draft version; OPEN/backup setters ignore */
    const char *path;
} RecompCardCommand;
typedef struct RecompCardManager {
    unsigned abi_version;
    void *(*create)(void);
    void (*destroy)(void *);
    const RecompCardView *(*view)(void *, int slot);
    /* 1 = success. 0 = refused; error is UTF-8 and always NUL terminated. */
    int (*command)(void *, const RecompCardCommand *, char *error, size_t capacity);
    const char *(*backup_path)(void *, int secondary);
} RecompCardManager;
#ifdef __cplusplus
}
#endif
#endif
