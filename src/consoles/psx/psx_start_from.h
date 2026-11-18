// psx_start_from.h - the PlayStation side of the dashboard's "Start from"
// block: which save states a title has, and how the host is told which one
// starts the game.
//
// The launcher reads the files itself, by the names and header psxrecomp
// writes (its runtime/src/savestate.c and runtime/include/boot_state.h):
//
//   <save root>/<bios>/state_<entry pc>[_disc<N>]_slot<NN>.pst    NN 00..11
//   <save root>/<bios>/state_<entry pc>[_disc<N>]_slot<NN>.thumb  its picture
//
// <bios> is "scph1001" or "openbios"; the save root is "saves" beside the exe.
//
// The host is told through the start-up variable it reads after the launcher
// has returned: PSX_LOAD_SLOT=<slot>. The launcher runs in the game's own
// process, so setting it there is enough.
//
// Limits, which a host that supplied the list itself would not have:
//   - only "saves" beside the exe is read, not a --memcard-dir folder;
//   - the BIOS folder and the program (entry pc) are those of the newest save
//     state; states of the other BIOS or of another program of a set are
//     counted, not listed;
//   - "other build" compares the overlay codegen hash, read from the
//     product's overlay_toolchain/include/overlay_codegen_hash.h (or the same
//     file beside the exe). A build with the same hash and another exe is not
//     told apart, and without the file no entry carries a mark.
#ifndef RUI_CONSOLE_PSX_START_FROM_H
#define RUI_CONSOLE_PSX_START_FROM_H

#include "launcher_model.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Most save states one list holds (the newest).
#define PSX_START_MAX_STATES  24

// The codegen hash of the product in `product_dir`, from its
// overlay_codegen_hash.h. Returns 1 and the hash, or 0 when the file is not
// there or holds no hash.
int psx_start_codegen_hash(const char* product_dir, uint32_t* hash);

// Lists the title's save states, newest first, from `save_root`.
// `product_dir` (may be NULL) gives the running build's identity for the
// build mark. Returns the number of entries written to out[0..cap-1].
int psx_start_scan(const char* save_root, const char* product_dir,
                   LauncherStartEntry* out, int cap, LauncherStartNotes* notes);

// Scans and hands the list to the model. product_dir is the folder of the
// exe. save_root NULL: "saves" in product_dir.
void psx_start_from_load(LauncherModel* m, const char* product_dir, const char* save_root);

// The start-up variable of the hand-over.
typedef struct {
    const char* name;        // "PSX_LOAD_SLOT"
    char        value[16];   // "" = the variable is not set by this choice
} PsxStartVar;

// The variable for the model's choice. netplay_launch != 0 (a netplay room
// starts the game) gives it empty: the host ignores a slot there.
void psx_start_from_vars(const LauncherModel* m, int netplay_launch, PsxStartVar* out);

// Puts the choice into the process environment, for the host to read after
// the launcher has returned. The variable is removed only when an earlier
// call of this function set it, so one that a script set from outside stays.
void psx_start_from_apply(const LauncherModel* m, int netplay_launch);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_PSX_START_FROM_H
