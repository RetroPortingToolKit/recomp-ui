/*
 * recomp_netplay_host.h -- the launcher's netplay backend, shared.
 *
 * RecompLauncherCNetplayCallbacks implemented once, over recomp-net's lobby
 * protocol client (recomp_net/lobby_client.h) and its LAN modules
 * (lan_lobby / lan_direct / lan_beacon). An engine fills one hook struct,
 * calls recomp_netplay_host_init, and hands recomp_netplay_host_callbacks()
 * to RecompLauncherCGameInfo.netplay. It does not copy create / join /
 * fill_launch glue into each engine: that copying is how snesrecomp,
 * psxrecomp and (almost) n64lle each grew their own lobby.
 *
 * Lifted from snesrecomp runner/src/netplay/snes_host_lobby.c (snesrecomp
 * 36d6ce5), which is now a thin adapter over this file.
 *
 * OPTIONAL: built only when the consumer provides recomp-net
 * (recomp_target_launcher_netplay() in recomp_ui.cmake). recomp-ui itself
 * builds and runs without it; every callback in the launcher is NULL-guarded.
 *
 * Contract details the launcher relies on are in docs/HOST_NETPLAY.md.
 *
 * Not thread-safe; call from the thread that pumps the launcher, and from the
 * game's thread after it (the lobby client is process-global).
 */
#ifndef RECOMP_NETPLAY_HOST_H
#define RECOMP_NETPLAY_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "recomp_launcher.h"
#include "recomp_net/lobby_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How lobby seats become session slots at launch.
 *
 * HOST_FIRST (the default, and the contract in docs/HOST_NETPLAY.md): the
 * lobby host is ALWAYS session slot 0 -- the sim authority every host-only
 * path keys on -- whatever seat it holds; the other players follow in seat
 * order; slot_port[] maps each session slot to the controller port of its
 * LOBBY seat, and occupied_mask covers exactly the seated players. The
 * engine MUST honour slot_port[] or a moved seat lands on the wrong port.
 *
 * SEAT (legacy): session slot == lobby seat == port. slot_port[] is filled
 * as the identity and occupied_mask from the seats, so the launch is still
 * complete, but whoever sits in seat 0 is the sim authority. For an engine
 * that does not consume slot_port[] yet (snesrecomp today). A launch with the
 * host in the gallery is REFUSED under this policy, since the host has no
 * seat to be the slot of. */
enum {
    RECOMP_NETPLAY_SLOTS_HOST_FIRST = 0,
    RECOMP_NETPLAY_SLOTS_SEAT = 1
};

/* RecompLauncherCNetplayLaunch.input_player to publish. The launcher reads
 * -1 as "auto" (prefer the dashboard P1 / NETPLAY card). */
#define RECOMP_NETPLAY_INPUT_AUTO (-1)

/* The engine's mod runtime, as the lobby needs it. NULL (the whole struct)
 * means this build runs vanilla only: it offers no packages, publishes an
 * empty plan as host, answers every plan row "this build has no mod
 * support", and REFUSES to launch into a room whose host requires mods
 * (last_error "mods_unsupported") rather than starting a desync.
 *
 * The text forms are the engine's canonical "effective set": one line per
 * enabled feature, "<pkg>@<ver>/<feature> <opt>=<val> ...\n", "(none)\n"
 * when empty. The lobby carries it with ';' for '\n'. */
typedef struct RecompNetplayModHooks {
    /* Packages the host RUNS (its plan) / every installed (id, version).
     * Rows fill id, ver and -- for the plan -- name and feats. Return count. */
    int  (*plan_rows)(void *ctx, RNetLobbyModPkg *out, int max);
    int  (*installed_rows)(void *ctx, RNetLobbyModPkg *out, int max);
    /* Write the effective set; return its full length (>= cap = truncated). */
    int  (*effective_set)(void *ctx, char *out, uint32_t cap);
    /* Apply an authority's cosmetic grant (NULL/"" = none). */
    void (*set_cosmetic_allow)(void *ctx, const char *allow);
    /* 0 when the local configuration already equals `want`. */
    int  (*check_set)(void *ctx, const char *want, char *reason, uint32_t cap);
    /* Adopt `want` for the next session; 0 on success. */
    int  (*adopt_set)(void *ctx, const char *want, char *reason, uint32_t cap);
    /* >0 when installed (version NULL = any); fills a display name if known. */
    int  (*have_package)(void *ctx, const char *id, const char *version,
                         char *name, uint32_t name_cap);
    /* Cosmetic claims no authority granted, one per line; return length. */
    int  (*unapproved_cosmetics)(void *ctx, char *out, uint32_t cap);
    /* Exemptions this build is taking ("id@ver#sha256" per line); length. */
    int  (*exempted_packages)(void *ctx, char *out, uint32_t cap);
    /* The transfer: pack / free / verify-then-install. See lobby_client.h. */
    RNetLobbyModExportFn  export_package;
    RNetLobbyModFreeFn    free_blob;
    RNetLobbyModInstallFn install_blob;
    void *ctx;
    /* Optional (append-only): features of installed `package_id` that cannot
     * run because a required owner-supplied file is unselected or wrong,
     * comma-separated into `out`; returns how many. Every feature, enabled or
     * not. Carried in the lobby offer (RNetLobbyModPkg.nf) so the host can
     * hold the match and say who still has to pick a file. NULL = this build
     * claims nothing is missing. */
    int  (*missing_files)(void *ctx, const char *package_id, char *out,
                          uint32_t cap);
} RecompNetplayModHooks;

typedef struct RecompNetplaySessionVariant {
    int value;
    const char *label;
} RecompNetplaySessionVariant;

typedef struct RecompNetplayHostHooks {
    /* ---- identity -------------------------------------------------------- */
    const char *game_name;          /* the server's scoping key; required */
    const char *game_version;       /* exact release pin (SHIPPING.md §2) */
    /* Lower-case hex SHA-256 of the guest image, 64 chars; NULL/"" = unknown
     * (joins still work, automatch refuses to queue). */
    const char *content_fingerprint;
    const char *default_lobby_name; /* NULL -> "Netplay Lobby" */
    const char *lan_registry_path;  /* NULL -> "netplay_lan_lobby.txt" */
    const char *platform;           /* moderation metadata: "snes", "n64" ... */
    /* Older environment spelling, e.g. "SNES_NET_" (RNetLobbyConfig). */
    const char *legacy_env_prefix;

    /* ---- seats ----------------------------------------------------------- */
    /* PLAYER seats online, 2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS. 0 -> 2.
     * LAN / Direct IP rooms are TWO seats whatever this says: recomp-net's
     * lan_lobby / lan_direct carry one joiner. A larger title hosting on LAN
     * gets a two-seat room and a loud log line, never a fake bigger one. */
    int max_players;
    int slot_policy;                /* RECOMP_NETPLAY_SLOTS_* */
    int input_player;               /* launch.input_player; 0 or ..._AUTO */

    /* ---- host environment ------------------------------------------------ */
    /* Resolve `leaf` beside the executable (the LAN registry and the account
     * secret are anchored there, not at the cwd). Return 1 on success. NULL
     * leaves both cwd-relative. */
    int  (*exe_dir_path)(void *ctx, const char *leaf, char *out, size_t cap);
    /* Persist / recall the player's display name across runs. Optional. */
    int  (*name_store)(void *ctx, const char *name);
    int  (*name_load)(void *ctx, char *out, size_t cap);

    /* ---- match caps ------------------------------------------------------ */
    /* The engine's own match_caps keys, carried in RNetLobbyMatchCaps.ext. */
    const RNetLobbyCapsCodec *caps_codec;
    /* Host: finish the caps this room publishes. Called with valid=1,
     * rollback=1, the waiting room's input_delay / input_prediction already
     * set; may change any field (input_delay is re-clamped after). The mod
     * plan and the relay/TURN toggles are filled AFTER this returns, by this
     * module, and overwrite whatever the hook put there. `settings` may be
     * NULL (a republish outside a launcher frame). */
    void (*fill_match_caps)(void *ctx, const RecompLauncherCSettings *settings,
                            RNetLobbyMatchCaps *caps);
    /* Every peer: the launch is filled from `caps`; adjust it or read the
     * engine's own keys out of caps->ext here. Optional. */
    void (*apply_match_caps)(void *ctx, const RNetLobbyMatchCaps *caps,
                             RecompLauncherCNetplayLaunch *launch);

    /* ---- mods ------------------------------------------------------------ */
    const RecompNetplayModHooks *mods;   /* NULL = vanilla only (see above) */
    /* Automatch: is a SIM-AFFECTING feature on beyond what `ruleset_id`
     * imposes? 1 refuses the queue locally with `why`. NULL = never. */
    int  (*mods_enabled)(void *ctx, const char *ruleset_id, char *why,
                         size_t why_cap);
    /* This build's cosmetic grant when it hosts (';'-separated
     * id@version[#sha256]); NULL = grants nothing. */
    const char *cosmetic_allow;

    /* ---- desync reporting ------------------------------------------------ */
    /* The session's first simulation fork, if any: 1 and the fields filled,
     * else 0. Reported to the server once per connection. NULL = the engine
     * has no fork detector, nothing is reported. */
    int  (*last_fork)(void *ctx, uint32_t *tick, const char **partition,
                      uint32_t *mine, uint32_t *theirs);

    /* ---- rematch policy -------------------------------------------------- */
    int auto_ready_guests;  /* 1: guests set_ready(1) from the pump */
    int rematch_set_ready;  /* 1: prepare_rematch re-arms ready */

    void *ctx;
    /* Optional (append-only): host-selected hardware/rules choice. Stable wire
     * IDs; zero is the legacy default. Array and strings must outlive this
     * backend. The engine validates it in its simulation startup identity. */
    const RecompNetplaySessionVariant *session_variants;
    int session_variant_count;
    int default_session_variant;
    /* Optional (append-only): this TITLE's online player seats, for a game
     * whose netplay seats differ from its local controller cards
     * (GameInfo.num_players) -- a GBA link-cable title draws one card and
     * links up to four consoles. 2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS.
     *
     * When set it is the whole story: the lobby server's ceiling, create's
     * clamp, and (through create_max_slots) the Host panel's Max Players
     * offer 2..this online. LAN / Direct IP rooms stay two seats.
     *
     * 0 = unset: max_players above stays the engine-wide ceiling it always
     * was (SNES passes 4 for every title) and the Max Players combo keeps
     * following num_players, exactly as before this field existed. */
    int netplay_max_players;
    /* Opt-in: new rooms with this many seats or more default to delay-sync.
     * Zero leaves the historical rollback default for every room size. */
    int netplay_delay_sync_from_players;
} RecompNetplayHostHooks;

/* Present since RecompNetplayHostHooks.netplay_max_players was appended. */
#define RECOMP_NETPLAY_HOST_HAS_NETPLAY_MAX_PLAYERS 1
#define RECOMP_NETPLAY_HOST_HAS_DELAY_SYNC_FROM_PLAYERS 1

/* Init once before the launcher first opens. `hooks` is copied (the strings
 * and hook tables it points to must outlive the process's netplay use).
 * Returns 0, or -1 without a game_name. */
int  recomp_netplay_host_init(const RecompNetplayHostHooks *hooks);
void recomp_netplay_host_shutdown(void);

/* Stable callback table for RecompLauncherCGameInfo.netplay (NULL before
 * init). */
const RecompLauncherCNetplayCallbacks *recomp_netplay_host_callbacks(void);

/* Soft-return: un-start the LAN room, clear the launch, apply the rematch
 * ready policy -- and rebuild the Direct IP channel the launch closed: the
 * HOST re-opens its listener with the joiner seat freed, the GUEST starts a
 * non-blocking re-join that the pump drives until it is re-seated, refused,
 * or 30 s pass (docs/HOST_NETPLAY.md, "LAN / Direct IP rematch"). */
void recomp_netplay_host_prepare_rematch(void);
/* prepare_rematch + reopen the launcher on the waiting room (sets
 * resume_netplay_room / resume_netplay_endpoint on `gi`). */
void recomp_netplay_host_begin_soft_return(RecompLauncherCGameInfo *gi,
                                           int set_resume_room);

/* Leave LAN + lobby-server seats (keeps the WebSocket). */
int  recomp_netplay_host_leave(void);
/* leave + disconnect the lobby client. */
void recomp_netplay_host_disconnect(void);
/* LAN endpoint for gi.resume_netplay_endpoint ("" when none). */
const char *recomp_netplay_host_resume_endpoint(void);
int  recomp_netplay_host_in_lan(void);
/* Surface a game-session failure through last_error when the waiting room
 * reopens (one error channel, not one per engine). */
void recomp_netplay_host_set_runtime_error(const char *error_code);

/*
 * Headless self-test over the callback table (no ImGui): connect, create
 * ("host") or find-and-join ("guest") `lobby_name`, ready, start, and fill
 * *out from the launch. Leaves the WebSocket open so ICE signaling can
 * continue. Returns 0 on a filled launch, a negative stage code otherwise.
 */
int  recomp_netplay_host_auto_launch(const char *role, const char *player_name,
                                     const char *lobby_name, unsigned timeout_ms,
                                     RecompLauncherCNetplayLaunch *out);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NETPLAY_HOST_H */
