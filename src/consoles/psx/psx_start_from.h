// psx_start_from.h - the PlayStation side of the dashboard's "Start from"
// block: which save states and replays a title has, and how the host is told
// which one starts the game.
//
// The launcher reads the files itself, by the names and headers psxrecomp
// writes (its runtime/include/savestate.h, runtime/include/boot_state.h,
// docs/INPUT_ROUTES.md and docs/PLAYER_REPLAYS.md):
//
//   <save root>/<bios>/state_<entry pc>[_disc<N>]_slot<NN>.pst    NN 00..11
//   <save root>/<bios>/state_<entry pc>[_disc<N>]_slot<NN>.thumb  its picture
//   <save root>/<bios>/replay_<entry pc>[_disc<N>]_slot<NN>.psxrpl  NN 01..12
//   <save root>/replays/<name>.psxrpl        power-on and exported replays
//
// <bios> is "scph1001" or "openbios"; the save root is "saves" beside the exe.
//
// The host is told through the two start-up variables it reads after the
// launcher has returned: PSX_LOAD_SLOT=<slot> and PSX_REPLAY_FILE=<file>. The
// launcher runs in the game's own process, so setting them there is enough.
//
// Limits, which a host that supplied the list itself would not have:
//   - only "saves" beside the exe is read, not a --memcard-dir folder;
//   - the BIOS folder and the program (entry pc) are those of the newest save
//     state; states of the other BIOS or of another program of a set are
//     counted, not listed;
//   - "other build" compares the overlay codegen hash, which the product
//     ships as overlay_codegen_hash.h beside the exe. A build with the same
//     hash and another exe is not told apart.
#ifndef RUI_CONSOLE_PSX_START_FROM_H
#define RUI_CONSOLE_PSX_START_FROM_H

#include "launcher_model.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Most save states and most replays one list holds (the newest of each).
#define PSX_START_MAX_STATES  24
#define PSX_START_MAX_REPLAYS 24

// The codegen hash of the product in `product_dir`, from its
// overlay_codegen_hash.h. Returns 1 and the hash, or 0 when the file is not
// there or holds no hash.
int psx_start_codegen_hash(const char* product_dir, uint32_t* hash);

// Lists the title's save states, then its replays, each newest first, from
// `save_root`. `product_dir` (may be NULL) gives the running build's identity
// for the build mark. Returns the number of entries written to out[0..cap-1].
int psx_start_scan(const char* save_root, const char* product_dir,
                   LauncherStartEntry* out, int cap, LauncherStartNotes* notes);

// Scans and hands the list to the model. product_dir is the folder of the
// exe. save_root NULL: "saves" in product_dir.
void psx_start_from_load(LauncherModel* m, const char* product_dir, const char* save_root);

// One start-up variable of the hand-over.
typedef struct {
    const char* name;        // "PSX_LOAD_SLOT" or "PSX_REPLAY_FILE"
    char        value[512];  // "" = the variable is not set by this choice
} PsxStartVar;

// The two variables for the model's choice. netplay_launch != 0 (a netplay
// room starts the game) gives both empty: the host ignores a slot there, and
// a replay must not play into a match.
void psx_start_from_vars(const LauncherModel* m, int netplay_launch, PsxStartVar out[2]);

// Puts the choice into the process environment, for the host to read after
// the launcher has returned. A variable is removed only when an earlier call
// of this function set it, so one that a script set from outside stays.
void psx_start_from_apply(const LauncherModel* m, int netplay_launch);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_PSX_START_FROM_H
