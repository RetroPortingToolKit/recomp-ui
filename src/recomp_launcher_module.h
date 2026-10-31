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
#define RECOMP_LAUNCHER_V2_CALL __cdecl
#if defined(RECOMP_LAUNCHER_V2_BUILD_MODULE)
#define RECOMP_LAUNCHER_V2_EXPORT __declspec(dllexport)
#else
#define RECOMP_LAUNCHER_V2_EXPORT
#endif
#else
#define RECOMP_LAUNCHER_V2_CALL
#define RECOMP_LAUNCHER_V2_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_LAUNCHER_V2_MAJOR UINT32_C(2)
#define RECOMP_LAUNCHER_V2_MINOR UINT32_C(0)
#define RECOMP_LAUNCHER_V2_CDECL UINT32_C(1)
#define RECOMP_LAUNCHER_V2_LITTLE_ENDIAN UINT32_C(1)
#define RECOMP_LAUNCHER_V2_BIG_ENDIAN UINT32_C(2)
#define RECOMP_LAUNCHER_V2_X86 UINT32_C(1)
#define RECOMP_LAUNCHER_V2_X64 UINT32_C(2)
#define RECOMP_LAUNCHER_V2_ARM64 UINT32_C(3)

#define RECOMP_LAUNCHER_V2_SETTINGS (UINT64_C(1) << 0)
#define RECOMP_LAUNCHER_V2_DISC_BIOS_CARD (UINT64_C(1) << 1)
#define RECOMP_LAUNCHER_V2_INPUT (UINT64_C(1) << 2)
#define RECOMP_LAUNCHER_V2_MODS (UINT64_C(1) << 3)
#define RECOMP_LAUNCHER_V2_NETPLAY (UINT64_C(1) << 4)
#define RECOMP_LAUNCHER_V2_SETUP_PROGRESS (UINT64_C(1) << 5)
#define RECOMP_LAUNCHER_V2_START_CHOICE (UINT64_C(1) << 6)
#define RECOMP_LAUNCHER_V2_IN_SESSION (UINT64_C(1) << 7)
#define RECOMP_LAUNCHER_V2_CAPABILITIES UINT64_C(255)

/* Query/run status is separate from the unchanged 0/1/2/3 launcher action.
 * A refusal must be displayed and must not silently boot as UNAVAILABLE. */
enum {
    RECOMP_LAUNCHER_V2_OK = 0,
    RECOMP_LAUNCHER_V2_BAD_RECORD = 1,
    RECOMP_LAUNCHER_V2_BAD_VERSION = 2,
    RECOMP_LAUNCHER_V2_BAD_ARCHITECTURE = 3,
    RECOMP_LAUNCHER_V2_BAD_LAYOUT = 4,
    RECOMP_LAUNCHER_V2_MISSING_CAPABILITY = 5,
    RECOMP_LAUNCHER_V2_BAD_SDL = 6,
    RECOMP_LAUNCHER_V2_BUSY = 7,
    RECOMP_LAUNCHER_V2_START_ERROR = 8,
    RECOMP_LAUNCHER_V2_LIBRARY_ERROR = 9
};

typedef struct RecompLauncherV2Header {
    uint32_t size;
    uint32_t major;
    uint32_t minor;
    uint32_t reserved;
} RecompLauncherV2Header;

typedef struct RecompLauncherV2Layout {
    uint64_t settings;
    uint64_t callbacks;
    uint64_t records;
    uint64_t boundary;
    uint32_t settings_bytes;
    uint32_t game_bytes;
    uint32_t mods_bytes;
    uint32_t netplay_bytes;
} RecompLauncherV2Layout;

typedef struct RecompLauncherV2SDL {
    RecompLauncherV2Header header;
    uint32_t version;
    uint32_t reserved;
    uint64_t get_version_address;
    uint64_t get_joystick_address;
    uint8_t sha256[32];
    char provider_path[1024];
} RecompLauncherV2SDL;

typedef struct RecompLauncherV2Query {
    RecompLauncherV2Header header;
    uint32_t pointer_bits;
    uint32_t byte_order;
    uint32_t architecture;
    uint32_t calling_convention;
    RecompLauncherV2Layout layout;
    uint64_t offered;
    uint64_t required;
    RecompLauncherV2SDL sdl;
} RecompLauncherV2Query;

typedef struct RecompLauncherV2Settings {
    RecompLauncherV2Header header;
    RecompLauncherCSettings value;
} RecompLauncherV2Settings;

/* GameInfo's direct callback families/context remain byte-for-byte intact.
 * Its raw mods/netplay pointers MUST be null here. The adapter installs only
 * the separately validated sized provider objects below in a private copy. */
typedef struct RecompLauncherV2Game {
    RecompLauncherV2Header header;
    RecompLauncherCGameInfo value;
} RecompLauncherV2Game;

typedef struct RecompLauncherV2Mods {
    RecompLauncherV2Header header;
    RecompLauncherCModProvider value;
} RecompLauncherV2Mods;

typedef struct RecompLauncherV2Netplay {
    RecompLauncherV2Header header;
    RecompLauncherCNetplayCallbacks value;
} RecompLauncherV2Netplay;

#define RECOMP_LAUNCHER_V2_POWER_ON UINT32_C(0)
#define RECOMP_LAUNCHER_V2_SAVE_SLOT UINT32_C(1)
#define RECOMP_LAUNCHER_V2_REPLAY UINT32_C(2)
#define RECOMP_LAUNCHER_V2_THUMB_BGRA8 UINT32_C(1)

typedef struct RecompLauncherV2StartEntry {
    RecompLauncherV2Header header;
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
} RecompLauncherV2StartEntry;

typedef struct RecompLauncherV2StartNotes {
    RecompLauncherV2Header header;
    uint32_t build_known;
    uint32_t states_elsewhere;
    uint32_t states_unlisted;
    uint32_t replays_unlisted;
} RecompLauncherV2StartNotes;

/* count/get describe a stable snapshot for the blocking run. get writes only
 * the caller-sized record and returns 1 on success. thumbnail copies tightly
 * packed BGRA8 bytes to caller storage and reports required bytes on a small
 * buffer. Zero means unavailable/error; no host allocation is handed back.
 * Entries/thumbnail bytes are copied before another provider call. The host
 * validates its state/replay formats and build identity, not the module. */
typedef struct RecompLauncherV2StartProvider {
    RecompLauncherV2Header header;
    void* ctx;
    int32_t (RECOMP_LAUNCHER_V2_CALL *count)(void* ctx, RecompLauncherV2StartNotes* notes);
    int32_t (RECOMP_LAUNCHER_V2_CALL *get)(void* ctx, uint32_t index, RecompLauncherV2StartEntry* entry);
    uint32_t (RECOMP_LAUNCHER_V2_CALL *thumbnail)(void* ctx, uint32_t index, uint8_t* pixels, uint32_t capacity);
} RecompLauncherV2StartProvider;

typedef struct RecompLauncherV2Run {
    RecompLauncherV2Header header;
    const RecompLauncherV2Query* contract;
    RecompLauncherV2Settings* settings;
    const RecompLauncherV2Game* game;
    const RecompLauncherV2Mods* mods;
    const RecompLauncherV2Netplay* netplay;
    const RecompLauncherV2StartProvider* starts;
    const char* window_title;
    const char* product_root;
    const char* save_root;
    const char* assets_root;
    const char* initial_disc;
} RecompLauncherV2Run;

typedef struct RecompLauncherV2Result {
    RecompLauncherV2Header header;
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
} RecompLauncherV2Result;

typedef uint32_t (RECOMP_LAUNCHER_V2_CALL *RecompLauncherV2RunFn)(
    const RecompLauncherV2Run*, RecompLauncherV2Result*, char* diagnostic, uint32_t diagnostic_capacity);

typedef struct RecompLauncherV2Module {
    RecompLauncherV2Header header;
    uint64_t supported;
    RecompLauncherV2Layout layout;
    RecompLauncherV2SDL sdl;
    RecompLauncherV2RunFn run;
    char build_identity[65];
} RecompLauncherV2Module;

typedef uint32_t (RECOMP_LAUNCHER_V2_CALL *RecompLauncherV2QueryFn)(
    const RecompLauncherV2Query*, RecompLauncherV2Module*, char* diagnostic, uint32_t diagnostic_capacity);

/* The only exported module symbol. Query has no callbacks, settings edits or
 * persistent writes. Caller initializes every header including module output.
 * Caller owns diagnostic storage. A successful query is repeated at run, so
 * a caller cannot bypass checks by fabricating a function-table pointer. */
RECOMP_LAUNCHER_V2_EXPORT uint32_t RECOMP_LAUNCHER_V2_CALL recomp_launcher_query_v2(
    const RecompLauncherV2Query*, RecompLauncherV2Module*, char*, uint32_t);

#ifdef __cplusplus
}
#endif
#endif


