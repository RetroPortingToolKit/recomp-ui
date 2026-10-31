#ifndef RECOMP_LAUNCHER_MODULE_H
#define RECOMP_LAUNCHER_MODULE_H

#include "recomp_launcher.h"

/* In-process v1 contract. Include this shared header in C and C++ callers.
 * Existing settings/callback records are frozen and checked before dereference.
 * Each object owns its allocations. All borrowed data and contexts survive
 * until run returns; no callback or worker may survive that return. No SDL
 * object crosses this interface. Instance IDs require the same SDL provider.
 * A compatible module replacement happens only after the host exits. */
#if defined(_WIN32)
#define RECOMP_LAUNCHER_V1_CALL __cdecl
#if defined(RECOMP_LAUNCHER_V1_BUILD_MODULE)
#define RECOMP_LAUNCHER_V1_EXPORT __declspec(dllexport)
#else
#define RECOMP_LAUNCHER_V1_EXPORT
#endif
#else
#define RECOMP_LAUNCHER_V1_CALL
#define RECOMP_LAUNCHER_V1_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_LAUNCHER_V1_MAJOR UINT32_C(1)
#define RECOMP_LAUNCHER_V1_MINOR UINT32_C(0)
#define RECOMP_LAUNCHER_V1_CDECL UINT32_C(1)
#define RECOMP_LAUNCHER_V1_LITTLE_ENDIAN UINT32_C(1)
#define RECOMP_LAUNCHER_V1_BIG_ENDIAN UINT32_C(2)
#define RECOMP_LAUNCHER_V1_X86 UINT32_C(1)
#define RECOMP_LAUNCHER_V1_X64 UINT32_C(2)
#define RECOMP_LAUNCHER_V1_ARM64 UINT32_C(3)

#define RECOMP_LAUNCHER_V1_SETTINGS (UINT64_C(1) << 0)
#define RECOMP_LAUNCHER_V1_DISC_BIOS_CARD (UINT64_C(1) << 1)
#define RECOMP_LAUNCHER_V1_INPUT (UINT64_C(1) << 2)
#define RECOMP_LAUNCHER_V1_MODS (UINT64_C(1) << 3)
#define RECOMP_LAUNCHER_V1_NETPLAY (UINT64_C(1) << 4)
#define RECOMP_LAUNCHER_V1_SETUP_PROGRESS (UINT64_C(1) << 5)
#define RECOMP_LAUNCHER_V1_START_CHOICE (UINT64_C(1) << 6)
#define RECOMP_LAUNCHER_V1_IN_SESSION (UINT64_C(1) << 7)
#define RECOMP_LAUNCHER_V1_CAPABILITIES UINT64_C(255)

/* Query/run status is separate from the unchanged 0/1/2/3 launcher action.
 * A refusal must be displayed and must not silently boot as UNAVAILABLE. */
enum {
    RECOMP_LAUNCHER_V1_OK = 0,
    RECOMP_LAUNCHER_V1_BAD_RECORD = 1,
    RECOMP_LAUNCHER_V1_BAD_VERSION = 2,
    RECOMP_LAUNCHER_V1_BAD_ARCHITECTURE = 3,
    RECOMP_LAUNCHER_V1_BAD_LAYOUT = 4,
    RECOMP_LAUNCHER_V1_MISSING_CAPABILITY = 5,
    RECOMP_LAUNCHER_V1_BAD_SDL = 6,
    RECOMP_LAUNCHER_V1_BUSY = 7,
    RECOMP_LAUNCHER_V1_START_ERROR = 8,
    RECOMP_LAUNCHER_V1_LIBRARY_ERROR = 9
};

typedef struct RecompLauncherV1Header {
    uint32_t size;
    uint32_t major;
    uint32_t minor;
    uint32_t reserved;
} RecompLauncherV1Header;

typedef struct RecompLauncherV1Layout {
    uint64_t settings;
    uint64_t callbacks;
    uint64_t records;
    uint64_t boundary;
    uint32_t settings_bytes;
    uint32_t game_bytes;
    uint32_t mods_bytes;
    uint32_t netplay_bytes;
} RecompLauncherV1Layout;

typedef struct RecompLauncherV1SDL {
    RecompLauncherV1Header header;
    uint32_t version;
    uint32_t reserved;
    uint64_t get_version_address;
    uint64_t get_joystick_address;
    uint8_t sha256[32];
    char provider_path[1024];
} RecompLauncherV1SDL;

typedef struct RecompLauncherV1Query {
    RecompLauncherV1Header header;
    uint32_t pointer_bits;
    uint32_t byte_order;
    uint32_t architecture;
    uint32_t calling_convention;
    RecompLauncherV1Layout layout;
    uint64_t offered;
    uint64_t required;
    RecompLauncherV1SDL sdl;
} RecompLauncherV1Query;

typedef struct RecompLauncherV1Settings {
    RecompLauncherV1Header header;
    RecompLauncherCSettings value;
} RecompLauncherV1Settings;

/* GameInfo's direct callback families/context remain byte-for-byte intact.
 * Its raw mods/netplay pointers MUST be null here. The adapter installs only
 * the separately validated sized provider objects below in a private copy. */
typedef struct RecompLauncherV1Game {
    RecompLauncherV1Header header;
    RecompLauncherCGameInfo value;
} RecompLauncherV1Game;

typedef struct RecompLauncherV1Mods {
    RecompLauncherV1Header header;
    RecompLauncherCModProvider value;
} RecompLauncherV1Mods;

typedef struct RecompLauncherV1Netplay {
    RecompLauncherV1Header header;
    RecompLauncherCNetplayCallbacks value;
} RecompLauncherV1Netplay;

#define RECOMP_LAUNCHER_V1_POWER_ON UINT32_C(0)
#define RECOMP_LAUNCHER_V1_SAVE_SLOT UINT32_C(1)
#define RECOMP_LAUNCHER_V1_REPLAY UINT32_C(2)
#define RECOMP_LAUNCHER_V1_THUMB_BGRA8 UINT32_C(1)

typedef struct RecompLauncherV1StartEntry {
    RecompLauncherV1Header header;
    uint32_t kind;
    int32_t slot;
    int32_t disc;
    uint32_t build;
    uint32_t power_on;
    uint32_t partial;
    uint32_t frames;
    uint32_t thumbnail_format;
    uint32_t thumbnail_width;
    uint32_t thumbnail_height;
    int64_t when;
    char label[96];
    char replay_path[512];
} RecompLauncherV1StartEntry;

typedef struct RecompLauncherV1StartNotes {
    RecompLauncherV1Header header;
    uint32_t build_known;
    uint32_t states_elsewhere;
    uint32_t states_unlisted;
    uint32_t replays_unlisted;
} RecompLauncherV1StartNotes;

/* count/get describe a stable snapshot for the blocking run. get writes only
 * the caller-sized record and returns 1 on success. thumbnail copies tightly
 * packed BGRA8 bytes to caller storage and reports required bytes on a small
 * buffer. Zero means unavailable/error; no host allocation is handed back.
 * Entries/thumbnail bytes are copied before another provider call. The host
 * validates its state/replay formats and build identity, not the module. */
typedef struct RecompLauncherV1StartProvider {
    RecompLauncherV1Header header;
    void* ctx;
    int32_t (RECOMP_LAUNCHER_V1_CALL *count)(void* ctx, RecompLauncherV1StartNotes* notes);
    int32_t (RECOMP_LAUNCHER_V1_CALL *get)(void* ctx, uint32_t index, RecompLauncherV1StartEntry* entry);
    uint32_t (RECOMP_LAUNCHER_V1_CALL *thumbnail)(void* ctx, uint32_t index, uint8_t* pixels, uint32_t capacity);
} RecompLauncherV1StartProvider;

typedef struct RecompLauncherV1Run {
    RecompLauncherV1Header header;
    const RecompLauncherV1Query* contract;
    RecompLauncherV1Settings* settings;
    const RecompLauncherV1Game* game;
    const RecompLauncherV1Mods* mods;
    const RecompLauncherV1Netplay* netplay;
    const RecompLauncherV1StartProvider* starts;
    const char* window_title;
    const char* product_root;
    const char* save_root;
    const char* assets_root;
    const char* initial_disc;
} RecompLauncherV1Run;

typedef struct RecompLauncherV1Result {
    RecompLauncherV1Header header;
    int32_t action;
    uint32_t start_kind;
    int32_t save_slot;
    uint32_t reserved;
    char* selected_disc;
    uint32_t selected_disc_capacity;
    char* relaunch_path;
    uint32_t relaunch_path_capacity;
    char* replay_path;
    uint32_t replay_path_capacity;
} RecompLauncherV1Result;

typedef uint32_t (RECOMP_LAUNCHER_V1_CALL *RecompLauncherV1RunFn)(
    const RecompLauncherV1Run*, RecompLauncherV1Result*, char* diagnostic, uint32_t diagnostic_capacity);

typedef struct RecompLauncherV1Module {
    RecompLauncherV1Header header;
    uint64_t supported;
    RecompLauncherV1Layout layout;
    RecompLauncherV1SDL sdl;
    RecompLauncherV1RunFn run;
    char build_identity[65];
} RecompLauncherV1Module;

typedef uint32_t (RECOMP_LAUNCHER_V1_CALL *RecompLauncherV1QueryFn)(
    const RecompLauncherV1Query*, RecompLauncherV1Module*, char* diagnostic, uint32_t diagnostic_capacity);

/* The only exported module symbol. Query has no callbacks, settings edits or
 * persistent writes. Caller initializes every header including module output.
 * Caller owns diagnostic storage. A successful query is repeated at run, so
 * a caller cannot bypass checks by fabricating a function-table pointer. */
RECOMP_LAUNCHER_V1_EXPORT uint32_t RECOMP_LAUNCHER_V1_CALL recomp_launcher_query_v1(
    const RecompLauncherV1Query*, RecompLauncherV1Module*, char*, uint32_t);

#ifdef __cplusplus
}
#endif
#endif
