// recomp_launcher.h — C-callable entry point for the recomp-ui launcher.
//
// A host app's C main() can't speak the C++ Dear ImGui launcher internals
// directly, so this shim wraps it: it creates its own SDL/GL window, runs the
// launcher, maps a plain-C settings struct in/out, and tears the window down —
// leaving the host to just seed/read the struct and pick up the chosen ROM
// path.

#ifndef RECOMP_LAUNCHER_H
#define RECOMP_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

/* Mod support is framework-available but developer opt-in. The integration
 * helper defines this from its default-OFF RECOMP_UI_ENABLE_MODS CMake option.
 * Keep a source-level default for hosts that compile the launcher sources
 * without recomp_ui.cmake. The public provider ABI remains visible either way
 * so enabling the option never changes C struct layouts. */
#ifndef RECOMP_UI_ENABLE_MODS
#define RECOMP_UI_ENABLE_MODS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Storage ceiling, not a default: each SystemProfile supplies its console
// ceiling and each game supplies num_players (for example SMW Co-op remains
// 2). Consoles/games with fewer players never touch the upper slots. Hosts
// that predate the widening only ever wrote [0]/[1], and their memset(0)
// leaves new slots in the same "none" state they had implicitly before.
// Every consumer compiles this header from source (submodule pin), so the
// layout change is absorbed by the consumer's normal rebuild on a pin bump.
#define RECOMP_LAUNCHER_MAX_PLAYERS 8
/* Host may #ifdef this when reading player_gamepad_guid[] from settings. */
#define RECOMP_LAUNCHER_HAS_PLAYER_GAMEPAD_GUID 1
/* Host may #ifdef this when reading multitap_enabled from settings. */
#define RECOMP_LAUNCHER_HAS_MULTITAP_ENABLED 1
/* Host may #ifdef this when reading multitap_analog (DualShock-on-tap hack). */
#define RECOMP_LAUNCHER_HAS_MULTITAP_ANALOG 1
/* Host may #ifdef this when reading Settings.rewind_depth. */
#define RECOMP_LAUNCHER_HAS_REWIND_DEPTH 1
/* Host may #ifdef this when reading Settings.rewind_interval. */
#define RECOMP_LAUNCHER_HAS_REWIND_INTERVAL 1
/* Host may #ifdef this when reading Settings.rewind_enabled. */
#define RECOMP_LAUNCHER_HAS_REWIND_ENABLED 1
/* Host may #ifdef this when reading Settings.run_ahead. */
#define RECOMP_LAUNCHER_HAS_RUN_AHEAD 1
/* Host may #ifdef this when reading Settings.vsync. */
#define RECOMP_LAUNCHER_HAS_VSYNC 1
/* Host may #ifdef this when reading Settings.virtual_stylus. */
#define RECOMP_LAUNCHER_HAS_VIRTUAL_STYLUS 1
#define RECOMP_LAUNCHER_MAX_BINDINGS 24
#define RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS 8

/* Portable encoding used by host-owned controller bindings. Button and axis
 * numbers are SDL's standard game-controller indices; zero remains unbound. */
#define RECOMP_LAUNCHER_PAD_BUTTON(code) (1 + (code))
#define RECOMP_LAUNCHER_PAD_AXIS(code, positive) \
    (100 + ((code) * 2) + ((positive) ? 1 : 0))
#define RECOMP_LAUNCHER_PAD_BUTTON_COMBO(mask) (1000 + (mask))
#define RECOMP_LAUNCHER_PAD_IS_BUTTON(value) ((value) > 0 && (value) < 100)
#define RECOMP_LAUNCHER_PAD_IS_AXIS(value) ((value) >= 100 && (value) < 1000)
#define RECOMP_LAUNCHER_PAD_IS_BUTTON_COMBO(value) ((value) >= 1000)
#define RECOMP_LAUNCHER_PAD_BUTTON_CODE(value) ((value) - 1)
#define RECOMP_LAUNCHER_PAD_AXIS_CODE(value) (((value) - 100) / 2)
#define RECOMP_LAUNCHER_PAD_AXIS_POSITIVE(value) (((value) - 100) & 1)
#define RECOMP_LAUNCHER_PAD_BUTTON_COMBO_MASK(value) ((value) - 1000)

// N64 Transfer Pak slots — one per controller port.
#define RECOMP_LAUNCHER_MAX_TPAKS 4

/* Netplay lobby membership ceiling (party games up to 8). Players only. */
#define RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS 8
/* Spectator seats a host may open, a separate pool on top of the players --
 * so a full room can still be watched. */
#define RECOMP_LAUNCHER_NETPLAY_MAX_SPECTATORS 4

typedef struct RecompLauncherCSettings RecompLauncherCSettings;

typedef struct RecompLauncherCNetplayLobby {
    char lobby_id[40];
    char name[64];
    char game_name[64];
    char game_version[32];
    int  player_count;
    int  max_slots;
    int  has_password;
    /* Round-trip ms to the lobby host; -1 when unknown / timed out. */
    int  latency_ms;
    /* 0 standard, 1 PSX-Link (browser badge; 0 when the server predates it). */
    int  lobby_kind;
    /* Host's country, ISO 3166-1 alpha-2 (e.g. "JP"), from the server's GeoIP
     * on the host's address. Empty when unknown, private, or LAN. */
    char host_country[4];
    /* Gallery, for the browser's Spectators column: allow_spectators 0 draws
     * "No"; otherwise "<spectator_count>/<max_spectators>". A backend that
     * predates the field leaves all three 0, which also reads as "No". */
    int  allow_spectators;
    int  max_spectators;
    int  spectator_count;
} RecompLauncherCNetplayLobby;

/* One player connected to the lobby server, seated or just browsing -- the
 * browser's "players online" panel. */
typedef struct RecompLauncherCNetplayOnlinePlayer {
    char display_name[64];
    /* Opaque, stable id for the ACCOUNT behind this player; "" for a guest.
     *
     * Not a name and not a Discord identifier -- the server's own row key,
     * published precisely so a client can keep a list that survives the other
     * player reconnecting or renaming. Never render it; it is a key, not a
     * label. A guest has none, so a guest can only be muted for as long as
     * their connection lasts. */
    char account[40];

    char country[4];     /* alpha-2 from the server's GeoIP; "" unknown */
    char lobby_name[64]; /* room they are in; "" while browsing */
    int  in_lobby;
    int  hosting;
    int  is_local;       /* this client */
} RecompLauncherCNetplayOnlinePlayer;

typedef struct RecompLauncherCNetplayMember {
    int  slot;
    char display_name[64];
    /* Opaque, stable id for the ACCOUNT behind this player; "" for a guest.
     *
     * Not a name and not a Discord identifier -- the server's own row key,
     * published precisely so a client can keep a list that survives the other
     * player reconnecting or renaming. Never render it; it is a key, not a
     * label. A guest has none, so a guest can only be muted for as long as
     * their connection lasts. */
    char account[40];

    int  ready;
    int  is_host;
    /* Round-trip ms from the local peer *to* this seat; -1 unknown / self. */
    int  latency_ms;
    /* 1 when this row is the local client's seat (never show self-RTT). */
    int  is_local;
    /* Peer BIOS offer from set_ready (0 if legacy / missing). */
    int  bios_offer_valid;
    int  bios_can_scph1001;
    int  bios_prefer_openbios;
    /* 1 when this row is a spectator rather than a player. `slot` stays the
     * seat index to pass back to move_member / kick_member either way -- the
     * two roles share one index namespace, so the UI never has to translate.
     * Always 0 against a host that predates spectators. */
    int  is_spectator;
    /* Peer memory-card offer (append-only; 0 = legacy / not advertised).
     * memcard_has_card: a slot-1 card is enabled on that peer.
     * memcard_share: that peer opted in to bring it. Drawn on seat 2 (P2):
     * the match uses P2's card as its slot-2 card when P2 offers it AND the
     * host allows it (guest_memcard_get). */
    int  memcard_offer_valid;
    int  memcard_has_card;
    int  memcard_share;
    /* Country, ISO 3166-1 alpha-2, from the server's GeoIP on this peer's
     * address. Empty when unknown, private, or LAN. Drawn as a flag. */
    char country[4];
} RecompLauncherCNetplayMember;

typedef struct RecompLauncherCNetplayNeedMod {
    char id[96];
    char version[32];
    char name[64];
    int  builtin;
    uint32_t size;
} RecompLauncherCNetplayNeedMod;

/* One entry of the lobby's host-authoritative mod plan, as seen by THIS
 * peer. `installed` is local: a guest missing a package still sees the row,
 * so it knows what to download instead of silently failing to seat. */
typedef struct RecompLauncherCNetplayLobbyMod {
    char id[96];
    char version[32];
    char name[64];
    int  installed;
    /* Why `installed` is 0 — "not installed" vs "does not match your game
     * image". Empty when installed. */
    char reason[96];
    int  builtin;
    uint32_t size;
    /* How the HOST has this package configured: the enabled features and the
     * option values it resolved them to, e.g. "localization language=en".
     *
     * Shown to guests because it is what they will actually run -- the host's
     * configuration is adopted before launch, so a guest reading its own local
     * settings here would be reading the wrong ones. Refreshed from the host's
     * published caps, so it tracks a host changing a dropdown without the
     * guest doing anything. Empty when the host published no configuration. */
    char options[192];
} RecompLauncherCNetplayLobbyMod;

/* One lobby chat line, oldest first. Backends keep a short ring (the last
 * 64 or so); the UI redraws the whole ring every frame, so `seq` only has to
 * be monotonic so the UI can notice a new line and scroll to it. */
/* One automatch queue type, as the server advertises it. The caps summary is
 * a short human line the server builds ("Delay 2 - Rollback on"), NOT a
 * parsed settings blob: the launcher shows what the player is signing up for
 * and the server remains the only thing that writes match_caps. */
typedef struct RecompLauncherCNetplayRuleset {
    char id[48];
    char label[64];
    char caps_summary[128];
    /* Empty when the ruleset accepts any release. */
    char game_version[48];
    int  max_slots;
} RecompLauncherCNetplayRuleset;

/* The opponent offered at the accept gate. */
typedef struct RecompLauncherCNetplayFound {
    char handle[64];
    /* Discord @handle, shown small under the name -- display names are not
     * unique, so this is the disambiguator. Empty when unknown. */
    char username[64];
    /* ISO 3166-1 alpha-2, drawn as a flag. Empty when the server has none. */
    char country[4];
    char ruleset_label[64];
    /* Round-trip estimate through the relay, summed for both peers. <0 when
     * the server did not offer one. */
    int  est_rtt_ms;
    /* Seconds left to answer. Counts down; 0 means it is about to lapse. */
    int  accept_secs_left;
} RecompLauncherCNetplayFound;

typedef struct RecompLauncherCNetplayChatMessage {
    char     from[64];   /* display name; empty for a system line */
    /* The SERVER's id for this line. A report names this and never the text:
     * chat already passes through the server, so it has the words, and
     * letting a report carry them would let anyone compose a message,
     * attribute it to somebody, and have them sanctioned for words they never
     * typed. Empty when the server predates ids -- such a line simply cannot
     * be reported, because there is no agreed referent for it. */
    char     mid[40];
    /* Opaque, stable id for the ACCOUNT behind this player; "" for a guest.
     *
     * Not a name and not a Discord identifier -- the server's own row key,
     * published precisely so a client can keep a list that survives the other
     * player reconnecting or renaming. Never render it; it is a key, not a
     * label. A guest has none, so a guest can only be muted for as long as
     * their connection lasts. */
    char account[40];

    char     text[256];
    int      is_local;   /* sent by this client */
    int      is_system;  /* join/leave/notice, not a player */
    uint32_t seq;
} RecompLauncherCNetplayChatMessage;

typedef struct RecompLauncherCNetplayLaunch {
    int      enabled;
    int      local_slot;
    /* Host PlayerInput index to sample for this peer. -1 = auto (prefer
     * dashboard P1 / NETPLAY card; else seat card; else sole assigned). */
    int      input_player;
    char     bind_hostport[64];
    char     peer_hostport[64];
    uint32_t session_id;
    int      input_delay;
    /* Rollback invent runway (P). Unused when rollback == 0. Clamped 2..16. */
    int      input_prediction;
    int      max_slots; /* lobby seat ceiling (2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS) */
    /* Seated players at launch — delay-sync slot_count. 0 = unknown / use max_slots. */
    int      player_count;
    /* Bit i = lobby seat i occupied at launch. 0 = treat all seats occupied.
     * Sparse rooms (moved seats) must set holes clear so netplay does not
     * wait on empty remotes. */
    uint32_t occupied_mask;
    /* Host match_caps: lobby UDP SFU star (0/1). Online WS start always
     * opens the SFU; this flag stays for launch/diagnostics. LAN/direct
     * keeps host-as-relay / P2P when unset. */
    int      force_input_relay;
    /* Host match_caps: Force TURN delay-floor hint (0/1). Online transport
     * is always lobby SFU (§108) — this no longer selects ICE relay. */
    int      force_turn;
    /* Host match_caps: rollback invent/episode path (0/1; lobby default on). */
    int      rollback;
    /* Lobby kind (match_caps.lobby_kind): 0 = standard; 1 = PSX-Link (two
     * consoles over the serial cable — 4 seats split into console A = {0,1}
     * and console B = {2,3}; each client runs its console visibly and drives
     * a headless follower for the other). Link behavior only engages when a
     * console-B seat is occupied at launch; otherwise the session degrades to
     * a standard lobby of the seated players. */
    int      lobby_kind;
    /* 1 when this client launches into the gallery: it runs the same
     * simulation from the same start and displays it, and contributes no
     * input to anybody. Its own controllers must not reach the guest.
     *
     * player_count / occupied_mask above stay PLAYERS ONLY. A spectator
     * counted there is a seat every peer waits on and nobody ever fills. */
    int      is_spectator;
    /* Spectator only: this client's slot in the input relay's namespace,
     * which is NOT local_slot (a lobby seat index) and NOT a player seat.
     * It sits at or above the relay's player count, which is what makes the
     * relay refuse to forward anything this peer sends.
     *
     * <= 0 with is_spectator set means the host published no relay base; the
     * engine must refuse to launch rather than fall back to a player slot. */
    int      spectator_wire_slot;
    /* 1 = seat 2 (P2) brings its own memory card this match: its local slot-1
     * card is uploaded to the host at launch and becomes every peer's slot-2
     * card. 0 = the host's slot choices only (default). Settled by the host
     * at start and delivered to every peer with the launch, so all peers
     * agree even if the toggle raced the start. */
    int      guest_memcard;
    /* 1 = the host watches from the gallery and still runs the match: it
     * holds session slot 0 with its pad muted (every host-only path keys on
     * slot 0), and player seats sit at lobby seat + 1. Settled by the server
     * at start; backends fold it into local_slot / occupied_mask. */
    int      host_spectates;
    /* Session slot -> controller port, when slot_port_valid. The lobby host
     * is always session slot 0 (the sim authority every host-only path keys
     * on) whatever seat it holds; the other players follow in lobby-seat
     * order; each drives the port of its LOBBY seat, so the game sees a
     * player where the lobby seated them. -1 = no port (a host in the
     * gallery). Without it, session slot == lobby seat == port. */
    int      slot_port_valid;
    int      slot_port[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1];
} RecompLauncherCNetplayLaunch;

typedef struct RecompLauncherCNetplayLocalAddress {
    /* Numeric address advertised to clients, currently normally IPv4. */
    char address[64];
    /* User-facing interface name, for example "Wi-Fi" or "Ethernet". */
    char label[64];
} RecompLauncherCNetplayLocalAddress;

typedef struct RecompLauncherCNetplayCallbacks {
    void* ctx;
    /* Configuration and connection state are host-owned and may be persisted. */
    const char* (*default_url)(void* ctx);
    void (*set_lobby_url)(void* ctx, const char* url);
    int  (*connect)(void* ctx);
    int  (*connected)(void* ctx);
    void (*pump)(void* ctx);
    void (*set_player_name)(void* ctx, const char* name);
    const char* (*player_name)(void* ctx);
    /* list_* merges remote server lobbies with any same-machine LAN registry
     * row. Hosts advertise to exactly one channel (see create lan_only). */
    void (*request_list)(void* ctx);
    int  (*list_count)(void* ctx);
    int  (*list_get)(void* ctx, int index, RecompLauncherCNetplayLobby* out);
    /* Address discovery used by the Host Lobby modal. */
    int  (*local_ip)(void* ctx, char* out, size_t out_len);
    int  (*external_ip)(void* ctx, char* out, size_t out_len);
    /* Lobby operations return 0 when the request was accepted.
     * create: host_endpoint is in/out (capacity >= 64). recomp-ui applies the
     * universal UDP port policy before calling this — LAN keeps the exact port
     * (UI blocks create when busy); online may already have rewritten the port
     * to the first free value in preferred..preferred+31. Hosts should publish
     * the given endpoint as-is. Returns -4 only as a defensive fallback when
     * the host itself cannot use the port (UI surfaces the same messages).
     * lan_only != 0: publish only the local LAN registry (no lobby server).
     * lan_only == 0: publish only on the lobby server (no LAN registry). */
    /* max_slots: 2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS, clamped by the host
     * to the game's supported player count. */
    int  (*create)(void* ctx, const char* lobby_name, char* host_endpoint,
                   const char* password, const RecompLauncherCSettings* settings,
                   int lan_only, int max_slots);
    /* join: guest_bind is in/out (capacity >= 64). recomp-ui applies the
     * universal guest UDP bind policy before calling — prefer 7778, then
     * 7778+1 .. +31, written as "0.0.0.0:<port>". Hosts should advertise that
     * bind on the lobby join (never rewrite to :0). Engines may still normalize
     * NULL/empty/host:0 as a defensive fallback. */
    int  (*join)(void* ctx, const char* lobby_id, const char* password,
                 char* guest_bind);
    int  (*leave)(void* ctx);
    int  (*in_lobby)(void* ctx);
    int  (*is_host)(void* ctx);
    int  (*member_count)(void* ctx);
    int  (*member_get)(void* ctx, int index, RecompLauncherCNetplayMember* out);
    int  (*move_member)(void* ctx, int from_slot, int to_slot);
    int  (*local_ready)(void* ctx);
    int  (*all_ready)(void* ctx);
    int  (*set_ready)(void* ctx, int ready);
    int  (*request_start)(void* ctx, const RecompLauncherCSettings* settings);
    /* All peers launch only after the host's start request becomes pending. */
    int  (*launch_pending)(void* ctx);
    void (*clear_launch_pending)(void* ctx);
    int  (*fill_launch)(void* ctx, RecompLauncherCNetplayLaunch* out);
    /*
     * Optional multi-interface address discovery. Called with indices starting
     * at zero until it returns 0. The launcher clears out before each call;
     * address must be non-empty on success and label may be empty. Append-only
     * for compatibility with positional callback-table initializers; local_ip
     * remains the fallback.
     */
    int  (*local_address_get)(void* ctx, int index,
                              RecompLauncherCNetplayLocalAddress* out);
    /* Host-only: remove the player in `slot` (not the host). Optional. */
    int  (*kick_member)(void* ctx, int slot);
    /* Optional: latest lobby error code (need_players, missing_endpoints, …).
     * Cleared by the host after the UI reads it, or when a later op succeeds. */
    const char* (*last_error)(void* ctx);
    void (*clear_last_error)(void* ctx);
    /* Optional host waiting-room settings. input_delay is frames, clamped 2..20. */
    int  (*input_delay_get)(void* ctx);
    int  (*input_delay_set)(void* ctx, int delay_frames);
    /* Optional: lobby UDP SFU preference (0/1). Online start always SFU;
     * LAN/direct may clear this for host-as-relay. */
    int  (*force_input_relay_get)(void* ctx);
    int  (*force_input_relay_set)(void* ctx, int force);
    /* Optional: current room seat ceiling (2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS).
     * 0 when not in a lobby / unknown. Prefer this over game num_players. */
    int  (*lobby_max_slots)(void* ctx);
    /* Optional: host Force TURN delay-floor hint (0/1). Server lobbies only;
     * published in match_caps.force_turn. Does not change online transport. */
    int  (*force_turn_get)(void* ctx);
    int  (*force_turn_set)(void* ctx, int force);
    /* Optional: host Rollback (0/1). Published in match_caps.rollback;
     * peers apply via PSX_NET_MODE / launch.rollback. Lobby default on. */
    int  (*rollback_get)(void* ctx);
    int  (*rollback_set)(void* ctx, int enable);
    /* Optional host invent runway (P), frames, clamped 2..16. Rollback only. */
    int  (*input_prediction_get)(void* ctx);
    int  (*input_prediction_set)(void* ctx, int prediction_frames);
    /* Optional: DualShock-on-multitap-tap hack (0/1). Host publishes in
     * match_caps.multitap_analog; peers apply at match start. */
    int  (*multitap_analog_get)(void* ctx);
    int  (*multitap_analog_set)(void* ctx, int enable);
    /* Optional: 1 while lobby DNS/TCP/WS upgrade runs off-thread. */
    int  (*connecting)(void* ctx);
    /* Optional pre-join missing-mod handshake. Join returns last_error
     * "need_mods" without seating; the peer is prompted to download. */
    int  (*need_mods_count)(void* ctx);
    int  (*need_mods_get)(void* ctx, int index, RecompLauncherCNetplayNeedMod* out);
    int  (*need_mods_can_transfer)(void* ctx);
    int  (*mod_xfer_start)(void* ctx);
    void (*mod_xfer_cancel)(void* ctx);
    /* -1 idle, -2 fail, 0..100 in flight. */
    int  (*mod_xfer_progress)(void* ctx);
    int  (*mod_xfer_failed)(void* ctx, char* err, size_t err_cap);
    /* Optional PSX-Link lobby type (append-only: positional initializers).
     * link_lobby_supported: 1 when this title offers the PSX-Link kind
     * (game.toml [netplay] link_lobby). lobby_kind_get/set: host-side kind
     * for the room being created / currently joined (0 standard, 1 link). */
    int  (*link_lobby_supported)(void* ctx);
    int  (*lobby_kind_get)(void* ctx);
    int  (*lobby_kind_set)(void* ctx, int kind);
    /* Optional (append-only): re-publish the host's match capabilities —
     * including the required-mod plan built from the currently enabled mod
     * features — to the lobby right now. The lobby Mods picker calls this
     * after every toggle so peers see the host's plan without waiting for an
     * unrelated settings change. No-op for guests. */
    void (*push_match_caps)(void* ctx);
    /* Optional (append-only): the lobby's required mod plan as seen by this
     * peer, live (works after seating, unlike the join-time need_mods flow).
     * lobby_mods_missing counts entries this peer does not have installed;
     * lobby_mods_download asks the host to send them over the lobby's mod
     * transfer channel (0 = started, <0 = unavailable). */
    int  (*lobby_mods_count)(void* ctx);
    int  (*lobby_mods_get)(void* ctx, int index,
                           RecompLauncherCNetplayLobbyMod* out);
    int  (*lobby_mods_missing)(void* ctx);
    int  (*lobby_mods_download)(void* ctx);
    /* Optional (append-only): would lobby_mods_download actually start a
     * transfer right now? 1 yes, 0 no. Asked BEFORE the button is drawn, so a
     * peer is never offered a download that cannot happen.
     *
     * This is a question about the current state, not about the build. The
     * lobby server grants a mod-transfer channel only to a peer it REFUSED to
     * seat (`pending_mod_lobby` is set on the need_mods rejection and cleared
     * the moment the peer is seated), so a peer that is already sitting in the
     * lobby has no channel even in a build that fully implements transfers —
     * mod_xfer_start would come back `need_mods`. That is the case this panel
     * shows when the host edits the plan mid-lobby.
     *
     * Absent (NULL) means "unknown": the button is drawn and the answer comes
     * from lobby_mods_download's return, which is how it behaved before. */
    int  (*lobby_mods_can_download)(void* ctx);
    /* Optional (append-only): per-row transfer, so a peer can pull one package
     * without re-fetching the ones it already has.
     *   lobby_mods_download_one : start row `index`; 0 started, <0 not started
     *   lobby_mods_progress_one : -1 idle, -2 failed, 0..100 in flight
     * A row's button is drawn only when lobby_mods_can_download says yes AND
     * the row is not installed, so neither call is a way to ask "is this
     * possible" -- that question has its own callback above. */
    int  (*lobby_mods_download_one)(void* ctx, int index);
    int  (*lobby_mods_progress_one)(void* ctx, int index);
    /* Optional (append-only): seat self-service. A player may move ITSELF to
     * a free seat; taking a seat somebody occupies requires that player's
     * consent, so it is a request/approve exchange rather than a move.
     *   seat_move_self(to)      : 0 ok, <0 refused (occupied/unsupported)
     *   seat_swap_request(to)   : ask the player in `to` to trade seats
     *   seat_swap_incoming(...) : 1 when somebody asked THIS player to trade;
     *                             fills their display name and seat
     *   seat_swap_respond(ok)   : answer the incoming request
     *   seat_swap_outgoing(...) : 0 idle, 1 waiting, 2 accepted, -1 declined
     *   seat_swap_clear()       : drop a finished outgoing result */
    int  (*seat_move_self)(void* ctx, int to_slot);
    int  (*seat_swap_request)(void* ctx, int target_slot);
    int  (*seat_swap_incoming)(void* ctx, char* who, size_t who_cap,
                               int* from_slot);
    int  (*seat_swap_respond)(void* ctx, int accept);
    int  (*seat_swap_outgoing)(void* ctx);
    void (*seat_swap_clear)(void* ctx);

    /* ---- spectators -----------------------------------------------------
     * Optional, and every one of them reports "no gallery" against a host or
     * server that predates the feature -- so the UI gates its whole spectator
     * section on lobby_allow_spectators() and otherwise renders as before.
     *
     * A spectator watches the match in sync and cannot affect it. The relay
     * enforces that; the launcher only has to stop offering it a controller.
     *
     *   allow_spectators_get/set : host toggle, applied on the NEXT create
     *   lobby_allow_spectators   : what the CURRENT lobby actually has
     *   lobby_max_spectators     : gallery seat count (0 = none)
     *   lobby_spectator_count    : occupied gallery seats
     *   local_is_spectator       : 1 when this client is watching
     *   spectator_slot           : seat index for gallery position `index`,
     *                              to pass to move_member / kick_member;
     *                              <0 when out of range.
     *
     * Moving between the tables is move_member(from_slot, to_slot) with a
     * seat index from either side -- there is no separate promote/demote
     * call, because a promotion IS a move and giving it its own path is how
     * the two end up behaving differently. */
    int  (*allow_spectators_get)(void* ctx);
    int  (*allow_spectators_set)(void* ctx, int allow);
    int  (*lobby_allow_spectators)(void* ctx);
    int  (*lobby_max_spectators)(void* ctx);
    int  (*lobby_spectator_count)(void* ctx);
    int  (*local_is_spectator)(void* ctx);
    int  (*spectator_slot)(void* ctx, int index);
    /* ---- bring-your-own memory card (PSX) ------------------------------
     * Optional (append-only).
     *   memcard_offer_set(has_card, share): publish THIS peer's offer.
     *       has_card = a slot-1 card is enabled locally (the UI knows; the
     *       backend does not read launcher state). share < 0 keeps the
     *       current opt-in; 0/1 sets it. Cheap to call every frame — the
     *       backend re-advertises only on change.
     *   guest_memcard_get()      : host's allow flag (1 unless the host
     *                              turned it off; host-authoritative).
     *   guest_memcard_set(allow) : host only; <0 refused otherwise.
     * Effective = seat-2 offer (has_card && share) && host allow. */
    int  (*memcard_offer_set)(void* ctx, int has_card, int share);
    int  (*guest_memcard_get)(void* ctx);
    int  (*guest_memcard_set)(void* ctx, int allow);
    /* ---- lobby chat --------------------------------------------------
     * Optional (append-only). Everyone seated (players and spectators) sees
     * every line. chat_send returns 0 when the line was accepted; the line
     * shows up through chat_get once the room has it (online: the server's
     * echo, so order is the server's; LAN: the host's relay), so the UI
     * never appends locally. chat_count/chat_get read the backend's ring,
     * oldest first; the ring is cleared on join/leave. */
    int  (*chat_send)(void* ctx, const char* text);
    int  (*chat_count)(void* ctx);
    int  (*chat_get)(void* ctx, int index, RecompLauncherCNetplayChatMessage* out);
    /* Optional (append-only): 1 when this backend can run the match with the
     * host seated in the gallery (host_spectates above). The UI lets the host
     * drag itself into the spectator table only when this says yes. */
    int  (*host_can_spectate)(void* ctx);

    /* ---- players online (optional, append-only) --------------------------
     * Everyone connected to the lobby server (the `players` array the server
     * sends with each lobby_list), for the browser's side panel. A backend
     * with no server presence (LAN-only) leaves both NULL and the panel is
     * not drawn. */
    int  (*online_count)(void* ctx);
    int  (*online_get)(void* ctx, int index, RecompLauncherCNetplayOnlinePlayer* out);

    /* ---- server chat (optional, append-only) ------------------------------
     * Per-game chat outside any room, for everyone on the lobby server
     * playing this title. Same contract as the lobby chat callbacks: send
     * returns 0 when queued and the line arrives through get; no history.
     * NULL (or send returning <0 while offline) hides the panel. */
    int  (*server_chat_send)(void* ctx, const char* text);
    int  (*server_chat_count)(void* ctx);
    int  (*server_chat_get)(void* ctx, int index, RecompLauncherCNetplayChatMessage* out);

    /* ---- name policy (optional, append-only) ------------------------------
     * 1 when `name` may not be used -- it trips the same word list the chat
     * filter uses (recomp-net's rnet_chat_filter). Asked for BOTH a player's
     * display name and a room title.
     *
     * Unlike a chat line, a refused name is NOT masked: a line is a moment
     * and a mask reads as one, while a player name sits in the seat table and
     * in front of every line that player sends, and a room title sits in the
     * lobby browser in front of everyone shopping for a game. Masking either
     * just publishes the same word with stars in it, so the client is asked
     * for a different one instead.
     *
     * The UI asks BEFORE it accepts a name, so the answer is immediate and
     * works in a LAN room with no server. This is a courtesy check, not the
     * gate: the lobby server refuses the name itself (a modified or older
     * client can send what this would stop) and answers `name_rejected` /
     * `lobby_name_rejected`, which arrive through last_error and reopen the
     * matching prompt. NULL leaves the local check off and the server's
     * refusal still lands. */
    int  (*name_rejected)(void* ctx, const char* name);

    /* ---- Discord account (optional, append-only) --------------------------
     * A host compiles against whatever recomp-ui its game pins, which may
     * predate these fields. RECOMP_LAUNCHER_HAS_ACCOUNT (below the struct)
     * lets a host wire them up when they exist and compile clean when they do
     * not, so a runner and a UI can be updated in either order.
     * Sign-in is OPTIONAL, always. A build with these NULL, a lobby server
     * that offers no logins, and a player who never signs in are all ordinary
     * supported cases: the launcher keeps its locally-typed player name and
     * plays as a guest, which is what it has always done. Nothing in the seat
     * table, the lobby list or the match path may be made to require an
     * account.
     *
     * The host owns the HTTP: the launcher only opens a URL in a browser and
     * asks the host how it is going. The flow, as the lobby server implements
     * it (POST /auth/discord/start, GET /auth/discord/callback, POST
     * /auth/discord/poll), is:
     *
     *   account_login_begin()  -> host asks the server to start a login, gets
     *                             back a URL, and opens it in the player's
     *                             browser. 0 = started; <0 = could not, and
     *                             account_error says why.
     *   account_state()        -> polled every frame while the modal is open.
     *   account_handle()       -> the seat name the SERVER owns once signed in.
     *   account_username()     -> the Discord @handle, shown as the
     *                             disambiguator when two players present the
     *                             same handle (Discord display names are not
     *                             unique).
     *   account_error()        -> one line for a human, when state is FAILED.
     *   account_sign_out()     -> forget the stored session. Returns to guest;
     *                             never fails in a way the player cares about.
     *   account_set_handle()   -> ask the server to change the presentational
     *                             handle. 0 = accepted; <0 = refused (it trips
     *                             the word list), and the player picks another.
     *
     * account_available() reports whether the CONFIGURED lobby server offers
     * logins at all -- the server answers 503 to a start when its operator has
     * not set up Discord. Draw no sign-in affordance when this says no, rather
     * than offering a button that cannot work. */
    int         (*account_available)(void* ctx);
    int         (*account_login_begin)(void* ctx);
    int         (*account_state)(void* ctx); /* RecompLauncherCAccountState */
    const char* (*account_handle)(void* ctx);
    const char* (*account_username)(void* ctx);
    const char* (*account_error)(void* ctx);
    int         (*account_sign_out)(void* ctx);
    int         (*account_set_handle)(void* ctx, const char* handle);

    /* ---- automatch (optional, append-only) --------------------------------
     * Server-run pairing: two players who want a match and do not care which
     * lobby it happens in. The server creates the room, owns its match_caps
     * (a named ruleset, not a host's settings) and is its host. Protocol:
     * recomp-net-server `docs/AUTOMATCH.md`.
     *
     * automatch_available() reports whether the CONFIGURED server offers it --
     * a deployment with no rulesets loaded has automatch off, and a signed-out
     * player cannot queue at all, because the dodge cost the accept gate
     * charges has to survive a reconnect and an ephemeral connection id does
     * not. Online netplay draws the ⚡ Automatch button from this answer.
     *
     * The queue / accept callbacks land with the flow itself; this one is
     * here first so the button is gated on a real capability rather than
     * offered and then found not to work. */
    /* Which lobbies the browser should be shown: 0 = every source (the
     * historical behaviour), 1 = LAN / Direct IP only, 2 = the lobby server
     * only.
     *
     * The backend merges its LAN registry and beacon rows with the server's
     * list, and only the UI knows which fork the player took on the way in --
     * so the scope has to travel. Without it a player who chose "LAN / Direct
     * IP" was shown online rooms they had no connection for, and a player who
     * chose online was shown LAN rooms from their own machine.
     *
     * Appended for ABI stability; a backend without it keeps merging. */
    /* Report chat lines for moderation. `mids` are RecompLauncherCNetplay-
     * ChatMessage.mid values -- several at once, because harassment is
     * usually a burst rather than a line, and making somebody file six
     * reports for one incident produces six rows that each look minor.
     *
     * `reason` is a category string; anything unrecognised is filed as
     * "other" rather than refused. `note` is optional.
     *
     * 0 means handed to the server, NOT accepted: it refuses a line that has
     * scrolled out of its ring, one from a signed-out sender, one that is
     * your own, and anything over the per-account rate limit.
     *
     * NULL hides the report affordance entirely. Appended for ABI stability. */
    int         (*chat_report)(void* ctx, const char* const* mids, int mid_count,
                               const char* reason, const char* note);

    /* Hand the backend the accounts this player has blocked, ';'-separated,
     * replacing the whole set. The server needs them because two of a
     * block's three effects cannot be done client-side: it must not pair the
     * two in automatch, and the blocked player must not see or be able to
     * join the blocker's room. A client can hide what it was sent; it cannot
     * know it was blocked. NULL/"" clears. Appended for ABI stability. */
    int         (*set_blocks)(void* ctx, const char* accounts);

    int         (*list_scope_set)(void* ctx, int scope);

    int         (*automatch_available)(void* ctx);
    /* The queue types this server offers for this title. Zero is a valid
     * answer and means the same as automatch_available saying no. */
    int         (*automatch_ruleset_count)(void* ctx);
    int         (*automatch_ruleset_get)(void* ctx, int index,
                                         RecompLauncherCNetplayRuleset* out);
    /* Join the queue for `ruleset_id` (NULL / "" = the first one). 0 =
     * queued; <0 = refused and automatch_error() has the line to show. The
     * launcher builds the opted-in title list, not the host: standalone
     * recomp-ui offers the running game and nothing else, and a multi-title
     * launcher offers what the player ticked. */
    int         (*automatch_queue)(void* ctx, const char* ruleset_id);
    int         (*automatch_cancel)(void* ctx);
    /* RecompLauncherCAutomatchState. Polled every frame while the page is up. */
    int         (*automatch_state)(void* ctx);
    /* Seconds this ticket has been waiting, and how many others are in the
     * same bucket. The population is what makes a wait legible rather than
     * indistinguishable from a broken feature, so show it. */
    int         (*automatch_queued_secs)(void* ctx);
    int         (*automatch_pool)(void* ctx);
    /* 1 when a pair is on offer and `out` was filled. */
    int         (*automatch_found_get)(void* ctx, RecompLauncherCNetplayFound* out);
    /* Answer the accept gate. Declining (or letting it lapse) costs a queue
     * cooldown that survives a reconnect, which is why automatch needs an
     * account at all -- so the button says "Decline", not "Skip". */
    int         (*automatch_accept)(void* ctx, int accept);
    /* One line for a human when state is FAILED, or after a refused queue. */
    const char* (*automatch_error)(void* ctx);
} RecompLauncherCNetplayCallbacks;

/* Present since the account callbacks were added. A host guards its wiring
 * with `#ifdef RECOMP_LAUNCHER_HAS_ACCOUNT` so it builds against an older
 * recomp-ui too -- the runner and the UI then land in either order. */
#define RECOMP_LAUNCHER_HAS_ACCOUNT 1

/* Present since automatch_available was added. Guarded the same way, for
 * the same reason: a game pins a recomp-ui and the two move separately. */
#define RECOMP_LAUNCHER_HAS_AUTOMATCH 1

/* Host may #ifdef this when wiring list_scope_set. */
#define RECOMP_LAUNCHER_HAS_LIST_SCOPE 1

/* Host may #ifdef this when filling the `account` key on player rows. */
#define RECOMP_LAUNCHER_HAS_PLAYER_ACCOUNT 1

/* Host may #ifdef this when wiring chat_report / filling ChatMessage.mid. */
#define RECOMP_LAUNCHER_HAS_CHAT_REPORT 1
/* Host may #ifdef this when wiring set_blocks. */
#define RECOMP_LAUNCHER_HAS_SET_BLOCKS 1
/* The categories the server matches on. Same list on every console; an
 * unrecognised one is stored as "other" rather than refused. */
#define RECOMP_LAUNCHER_REPORT_HARASSMENT     "harassment"
#define RECOMP_LAUNCHER_REPORT_HATE_SPEECH    "hate_speech"
#define RECOMP_LAUNCHER_REPORT_SEXUAL_CONTENT "sexual_content"
#define RECOMP_LAUNCHER_REPORT_SPAM           "spam"
#define RECOMP_LAUNCHER_REPORT_THREATS        "threats"
#define RECOMP_LAUNCHER_REPORT_CHEATING       "cheating_claim"
#define RECOMP_LAUNCHER_REPORT_OTHER          "other"
#define RECOMP_LAUNCHER_REPORT_NOTE_MAX 500
enum {
    RECOMP_LAUNCHER_LIST_SCOPE_ANY = 0,
    RECOMP_LAUNCHER_LIST_SCOPE_LAN = 1,
    RECOMP_LAUNCHER_LIST_SCOPE_ONLINE = 2
};

/* account_state() values. Guest is not an error and not a lesser state: it is
 * the launcher's original behaviour, and most players will sit in it. */
enum {
    RECOMP_LAUNCHER_ACCOUNT_GUEST = 0,
    RECOMP_LAUNCHER_ACCOUNT_WAITING = 1, /* browser open, polling */
    RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN = 2,
    RECOMP_LAUNCHER_ACCOUNT_FAILED = 3   /* account_error() has one line */
};

/* automatch_state() values. IDLE is the resting state and covers "this build
 * has no automatch" -- a page that never queues sits in it forever. */
enum {
    RECOMP_LAUNCHER_AUTOMATCH_IDLE = 0,
    RECOMP_LAUNCHER_AUTOMATCH_QUEUED = 1,   /* waiting for a pair */
    RECOMP_LAUNCHER_AUTOMATCH_FOUND = 2,    /* accept gate open; found_get fills */
    RECOMP_LAUNCHER_AUTOMATCH_ACCEPTED = 3, /* answered, waiting on the peer */
    RECOMP_LAUNCHER_AUTOMATCH_FAILED = 4    /* automatch_error() has one line */
};

/* ---- schema-driven mods --------------------------------------------------
 * The host owns package parsing, persistence, dependency resolution, and
 * installation. recomp-ui only renders this stable query/mutation surface.
 * All strings are copied into fixed buffers so provider implementations may
 * rebuild their catalogs after any mutation without dangling UI pointers. */
#define RECOMP_LAUNCHER_MOD_ID_MAX 96
#define RECOMP_LAUNCHER_MOD_VALUE_MAX 128
#define RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX 8
#define RECOMP_LAUNCHER_MOD_PATH_MAX 1024

typedef struct RecompLauncherCModAuthorLink {
    char name[64];
    char url[256];
} RecompLauncherCModAuthorLink;

typedef enum RecompLauncherCModOptionType {
    RECOMP_MOD_OPTION_BOOLEAN = 0,
    RECOMP_MOD_OPTION_CHOICE = 1,
    RECOMP_MOD_OPTION_INTEGER = 2,
    /* Free-text row (single line). The host's set_option is the validator:
     * the edited value is committed when the field loses focus after an
     * edit, and a rejected commit reverts the row to the model's value (the
     * host should surface why via last_error). Appended additively; only
     * games whose providers supply a TEXT option ever render one. */
    RECOMP_MOD_OPTION_TEXT = 3,
} RecompLauncherCModOptionType;

typedef struct RecompLauncherCModPackage {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char version[32];
    char name[128];
    char author[96];
    RecompLauncherCModAuthorLink author_links[RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX];
    int  author_link_count;
    char description[512];
    char license[64];
    char source_name[128];
    char source_url[256];
    char status[256];
    int  enabled;
    int  option_count;
    int  removable;
    int  has_error;
} RecompLauncherCModPackage;

/* A package may contribute any number of independently configurable features.
 * Feature identity is the (package_id, id) pair; feature ids only need to be
 * unique within their owning package. */
/* How finished a feature is. Zero is stable, so a provider that predates this
 * field reports every feature as stable under zero-init. */
typedef enum RecompLauncherCModChannel {
    RECOMP_MOD_CHANNEL_STABLE = 0,
    RECOMP_MOD_CHANNEL_EXPERIMENTAL = 1,
    RECOMP_MOD_CHANNEL_DEVELOPER = 2,
} RecompLauncherCModChannel;

typedef struct RecompLauncherCModFeature {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char package_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char package_version[32];
    char package_name[128];
    char name[128];
    char author[96];
    RecompLauncherCModAuthorLink author_links[RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX];
    int  author_link_count;
    char description[512];
    char source_name[128];
    char source_url[256];
    char group[96];
    char status[256];
    int  enabled;
    int  option_count;
    int  has_error;
    /* Feature exposes a live 3D camera. When enabled, the Controller page
     * conditionally presents camera bindings and the Mods detail links there.
     * Appended for ABI stability; zero keeps every existing feature unchanged. */
    int  camera_controls;
    /* Hidden features are omitted from normal picker lists while disabled.
     * Providers still expose them so an explicitly-enabled hidden feature can
     * be shown and turned off again. */
    int  hidden;
    /* RecompLauncherCModChannel. Stable is 0, so zero-init and older providers
     * both mean "stable" and need no special case. Developer-channel features
     * never reach a shipped build at all -- when one appears here, this is a
     * local developer build. Appended for ABI stability. */
    int  channel;
} RecompLauncherCModFeature;

typedef struct RecompLauncherCModOption {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char label[128];
    char description[512];
    char group[96];
    char value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    char default_value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    int  type;          /* RecompLauncherCModOptionType */
    int64_t min_value;
    int64_t max_value;
    int64_t step;
    int  choice_count;
    /* Non-zero when another option in the same feature currently overrides
     * this one (manifest key: disabled_by). The provider resolves it, so the
     * UI only has to grey the control out -- it never cross-references
     * options itself. Example: ticking "Instant" makes the speed box inert. */
    int  disabled;
} RecompLauncherCModOption;

typedef struct RecompLauncherCModChoice {
    char value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    char label[128];
} RecompLauncherCModChoice;

typedef struct RecompLauncherCModVersion {
    char version[32];
    int selected;
    int removable;
} RecompLauncherCModVersion;

typedef enum RecompLauncherCModDiagnosticSeverity {
    RECOMP_MOD_DIAGNOSTIC_INFO = 0,
    RECOMP_MOD_DIAGNOSTIC_WARNING = 1,
    RECOMP_MOD_DIAGNOSTIC_ERROR = 2,
} RecompLauncherCModDiagnosticSeverity;

typedef struct RecompLauncherCModDiagnostic {
    int  severity; /* RecompLauncherCModDiagnosticSeverity */
    char resource[192];
    char message[512];
    char related_package_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char related_feature_id[RECOMP_LAUNCHER_MOD_ID_MAX];
} RecompLauncherCModDiagnostic;

/* Owner-supplied files required by a feature, such as a source ROM used by a
 * character port. Providers validate identity; the UI only chooses a path and
 * displays the provider's verdict. Paths are never copied into a package. */
typedef struct RecompLauncherCModResource {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char label[128];
    char description[512];
    char path[RECOMP_LAUNCHER_MOD_PATH_MAX];
    char status[256];
    /* Comma-separated native-dialog patterns, e.g. "*.z64,*.v64,*.n64". */
    char file_patterns[128];
    char file_description[128];
    int required;
    int verified;
    /* "file" (default) or "directory". Appended for source compatibility. */
    char format[64];
} RecompLauncherCModResource;

typedef struct RecompLauncherCModProvider {
    void* ctx;
    int (*package_count)(void* ctx);
    int (*package_get)(void* ctx, int index, RecompLauncherCModPackage* out);
    int (*option_get)(void* ctx, const char* package_id, int index,
                      RecompLauncherCModOption* out);
    int (*choice_get)(void* ctx, const char* package_id, const char* option_id,
                      int index, RecompLauncherCModChoice* out);
    int (*version_count)(void* ctx, const char* package_id);
    int (*version_get)(void* ctx, const char* package_id, int index,
                       RecompLauncherCModVersion* out);
    /* Mutations return 1 on success and 0 on failure. */
    int (*install_archive)(void* ctx, const char* archive_path);
    int (*remove_package)(void* ctx, const char* package_id, const char* version);
    int (*set_enabled)(void* ctx, const char* package_id, int enabled);
    int (*select_version)(void* ctx, const char* package_id, const char* version);
    int (*set_option)(void* ctx, const char* package_id, const char* option_id,
                      const char* value);
    /* Resolve and persist the staged selection. Called before PLAY commits.
     * image_path is the ROM/disc currently selected in the launcher. */
    int (*commit)(void* ctx, const char* image_path);
    const char* (*last_error)(void* ctx);
    /* Feature-oriented surface. Appended for ABI stability. New providers
     * should implement this complete group; package callbacks above remain the
     * secondary install/version/removal surface. Feature identity is always
     * (package_id, feature_id). */
    int (*feature_count)(void* ctx);
    int (*feature_get)(void* ctx, int index, RecompLauncherCModFeature* out);
    int (*feature_option_get)(void* ctx, const char* package_id,
                              const char* feature_id, int index,
                              RecompLauncherCModOption* out);
    int (*feature_choice_get)(void* ctx, const char* package_id,
                              const char* feature_id, const char* option_id,
                              int index, RecompLauncherCModChoice* out);
    int (*feature_enable)(void* ctx, const char* package_id,
                          const char* feature_id, int enabled);
    int (*feature_set_option)(void* ctx, const char* package_id,
                              const char* feature_id, const char* option_id,
                              const char* value);
    int (*diagnostic_count)(void* ctx, const char* package_id,
                            const char* feature_id);
    int (*diagnostic_get)(void* ctx, const char* package_id,
                          const char* feature_id, int index,
                          RecompLauncherCModDiagnostic* out);
    /* Optional package vocabulary. NULL keeps the historical PSX defaults.
     * extension includes its leading dot, e.g. ".gbamod". */
    const char* archive_extension;
    const char* archive_description;
    /* Optional. Netplay lobbies (hosted / LAN / direct) call this instead of
     * commit() so matches stay vanilla. Must clear any in-session mod plan
     * without mutating the user's persisted offline selection. NULL means
     * "skip commit entirely" (no mods applied for that launch). Appended for
     * ABI stability — zero-init leaves it NULL. */
    int (*commit_netplay)(void* ctx, const char* image_path);
    /* Optional owner-resource surface. Appended for source compatibility in
     * the statically paired runner/UI build; this struct does not yet expose a
     * negotiated byte size for loading an older binary provider object.
     * Feature enable and commit remain authoritative gates; the UI verdict
     * alone is never sufficient. */
    int (*feature_resource_count)(void* ctx, const char* package_id,
                                  const char* feature_id);
    int (*feature_resource_get)(void* ctx, const char* package_id,
                                const char* feature_id, int index,
                                RecompLauncherCModResource* out);
    int (*feature_resource_set_path)(void* ctx, const char* package_id,
                                     const char* feature_id,
                                     const char* resource_id,
                                     const char* path);
    /* Problems with the CATALOG rather than with any one feature: a manifest
     * that could not be parsed, a legacy tree that could not be migrated.
     *
     * These need their own channel because the per-feature diagnostics above
     * are keyed on (package_id, feature_id), and a package that failed to
     * parse has neither -- which is why such failures used to be dropped in
     * silence, leaving an author with a mod that simply did not exist and
     * nothing anywhere explaining why. `resource` carries the path.
     *
     * Appended for ABI stability; NULL means the provider has none to report. */
    int (*catalog_diagnostic_count)(void* ctx);
    int (*catalog_diagnostic_get)(void* ctx, int index,
                                  RecompLauncherCModDiagnostic* out);
} RecompLauncherCModProvider;

// Plain-C mirror of the launcher's internal settings (bools as int).
struct RecompLauncherCSettings {
    int  output_method;     // 0 SDL, 1 SDL-software, 2 OpenGL
    int  window_scale;      // 1..N
    int  fullscreen;        // 0 off, 1 borderless, 2 exclusive
    int  ignore_aspect;     // bool
    int  linear_filter;     // bool
    int  widescreen;        // bool (EXPERIMENTAL, default 0)
    int  widescreen_hud;    // bool
    int  enable_audio;      // bool
    int  audio_freq;        // Hz
    int  volume;            // 0..100
    int  player_src[RECOMP_LAUNCHER_MAX_PLAYERS];  // 0 none, 1 keyboard, 2 gamepad
    int  deadzone[RECOMP_LAUNCHER_MAX_PLAYERS];    // 0..100
    int  skip_launcher;     // bool: boot straight to the game next time
    int  msu1_enabled;      // bool
    char msu1_dir[512];
    int  pad_mode[RECOMP_LAUNCHER_MAX_PLAYERS];    // per player: 0=Hybrid, 1=Analog(DualShock), 2=D-Pad(digital)
    int  aspect_index;      // 0 = 4:3, 1 = 16:9, 2 = 21:9

    // ---- deeper PSX-style settings (capability-gated; see RecompLauncherCGameInfo
    // has_* flags below — consoles that don't set the flags leave these unused) ----
    int  window_width;        // px window width (height follows aspect)
    int  renderer;            // 0 = software, 1 = OpenGL
    int  supersampling;       // 1..4
    int  antialiasing;        // MSAA sample count: 0 = off, else 2/4/8 (x). (A
                              // legacy on/off host may still write 0/1.)
    int  texture_filter;      // 0 = nearest, 1 = bilinear
    int  screen_kind;         // 0 raw, 1 CRT, 2 composite, 3 trinitron
    int  frame_interp;        // bool
    int  frame_interp_fps;    // 0=display, else 90/120/144/165/240
    int  spu_hq;              // bool
    int  auto_skip_fmv;       // bool
    int  turbo_loads;         // bool
    int  language_index;      // selected index into GameInfo.languages
    char bios_path[512];      // BIOS file path (empty = default)

    // ---- PSX-style memory-card save slots (SAVE_MEMCARD; see launcher_system.h
    // SaveSpec) — appended at the end to keep this struct additive/ABI-stable.
    // Per-slot card-image file path (empty = none picked yet), editable via the
    // Save panel's Browse/New controls; mirrors bios_path's pattern exactly.
    char memcard_path[2][512];
    // Per-slot enable/disable (mirrors the legacy PSX launcher's per-card
    // "Enabled" switch / SIO-port concept: a disabled slot reports no card
    // present). Tri-state on the way IN: 1 = enabled, -1 = disabled, 0 = unset
    // (host predates this field) -> the model defaults it to enabled at init.
    // The model normalizes to 0/1 and hands back 0/1; hosts should read the
    // result as `> 0` so a -1 passed through an older launcher still reads as
    // off. Appended additively; see launcher_model_toggle_memcard().
    int  memcard_enabled[2];

    // ---- audio output device (GameInfo.audio_device_labels consoles) --------
    // The chosen device's display name as enumerated by the HOST (SDL device
    // names are stable across runs on the same machine, not across machines —
    // exactly the contract the N64 SS Anne launcher's launcher.cfg used).
    // "" = system default. Appended additively.
    char audio_device[128];

    // ---- N64 Transfer Pak slots (GameInfo.tpak_slots consoles) --------------
    // Per controller port: the GB cartridge ROM inserted into that port's
    // Transfer Pak, its battery-save file, and whether the pak responds at
    // all. Mirrors the SS Anne launcher's per-player card set (launcher.cfg
    // pN_rom/pN_save/pN_enabled). Empty rom path = no cartridge inserted.
    // tpak_enabled: 0 = unset (host predates the field / fresh config) -> the
    // model defaults a slot WITH a rom to enabled; use -1 for explicit off.
    char tpak_rom_path[RECOMP_LAUNCHER_MAX_TPAKS][512];
    char tpak_save_path[RECOMP_LAUNCHER_MAX_TPAKS][512];
    int  tpak_enabled[RECOMP_LAUNCHER_MAX_TPAKS];

    // ---- mouse controls (GameInfo.has_mouse_controls games; Snap) -----------
    // Opt-in mouse-aim for a keyboard-family source. Only meaningful for
    // player 0 and only when the game sets has_mouse_controls; every other
    // consumer leaves these zero (memset default) and is byte-for-byte
    // unaffected. Appended additively at the end to keep the struct ABI-stable.
    int   mouse_enabled;      // 1 = "Keyboard + Mouse" source (mouse-aim on),
                              // 0 = plain "Keyboard" (mouse off). 0 = also the
                              // unset default; the host seeds the real default.
    float mouse_sensitivity;  // aim rate per mouse-pixel; default 0.06 (host
                              // seeds it). Model clamps to [0.01, 0.50].
                              // 0 = unset -> the model seeds 0.06.
    int   mouse_invert_x;     // bool: invert horizontal mouse aim
    int   mouse_invert_y;     // bool: invert vertical mouse aim (Snap default 1)
    // Left/Right/Middle mouse button -> index into the active profile's
    // ControllerSpec.buttons[] (0..button_count-1), or -1 = none/unbound.
    // NOTE: 0 is a VALID index (the n64 profile's "A"), so 0 is NOT "unset"
    // here — the host seeds real defaults ({A, Z, none} = {0, 2, -1}).
    int   mouse_bind[3];
    // ---- NES-style settings (capability-gated; see has_integer_scale /
    // hdpack_supported below) — appended additively, same ABI convention. ----
    int  integer_scale;       // bool: snap the game image to integer multiples
    int  hdpack_enabled;      // bool: load a Mesen-format HD texture pack
    char hdpack_dir[512];     // folder containing the pack's hires.txt
    // ---- Genesis-style widescreen width (SystemProfile.video.widescreen_cells
    // consoles only) — how many extra 8-px background cells EACH SIDE renders
    // while `widescreen` is on. 0 = unset (host predates this field) -> the
    // model defaults it to 8, the Genesis engine default. Appended additively.
    int  widescreen_cells;    // 1..16

    // ---- live aspect-driven extended view ---------------------------------
    // In a window, the fixed aspect selects the initial size before live
    // resizing takes over. Adaptive + fullscreen ignores the fixed aspect.
    int  adaptive_view;       // bool: logical width follows host drawable aspect

    // ---- netplay launch result (capability-gated by GameInfo.netplay_supported)
    // player_name is persistent host-owned identity; netplay_launch is a
    // transient output and is cleared by the launcher when it initializes.
    char netplay_player_name[64];
    RecompLauncherCNetplayLaunch netplay_launch;

    // ---- per-player SDL gamepad GUID (player_src==2). Empty when none/keyboard.
    // Appended additively; see RECOMP_LAUNCHER_HAS_PLAYER_GAMEPAD_GUID. Hosts
    // persist these as pN_device in settings.toml so multi-pad assignments
    // survive relaunches (instance IDs do not).
    char player_gamepad_guid[RECOMP_LAUNCHER_MAX_PLAYERS][40];

    // ---- controller motion (GameInfo.has_gyro_controls games) ------------
    // Multiplier applied by the host to a controller's angular-rate sensor.
    // 0 means unset and is seeded to 1.0 by the launcher model.
    float gyro_sensitivity;    // 0.25..4.00, 1.00 = game default

    // ---- optional presentation enhancements (capability-gated) -----------
    // Appended additively so zero-initialized existing consumers keep their
    // current settings surface and behavior.
    int sharp_filter;           // integer prescale + fractional linear finish
    int affine_filter;          // selective game-authorized affine smoothing

    // ---- cartridge light sensor (GameInfo.has_solar_sensor games) ---------
    // Appended additively. A few GBA cartridges carry a photodiode the game
    // reads as gameplay input -- Boktai's Gun del Sol charges from real
    // sunlight -- so "how bright is it where the player is" is a launch
    // setting, not an emulator preference.
    //
    // solar_zip is a POSTAL CODE, deliberately text: many are not numeric
    // ("SW1A", "K1A"). Empty means the host must not consult any network.
    char solar_zip[16];
    char solar_country[8];      // Zippopotam-style code: us, ca, gb, de, ...
    int  solar_source;          // 0 = live local weather, 1 = fixed level
    int  solar_manual_step;     // 0..8, used when solar_source == 1
    int  solar_full_sun;        // W/m^2 that reads as full sun; 0 = host default

    // ---- multi-display layout --------------------------------------------
    // Index into GameInfo.display_layout_labels. This is intentionally
    // independent of aspect/widescreen: it describes physical host windows,
    // not how a game's camera is rendered inside one of them.
    int display_layout;
    // ---- PSX geometry-precision settings (capability-gated by
    // GameInfo.has_geometry_precision) — appended additively, same ABI
    // convention as every block above. ----
    // The PS1's GTE projects in 16.16 and then discards the fraction when it
    // saturates screen coordinates to whole pixels, so a moving mesh shimmers;
    // and the GPU interpolates UVs affinely, so large floor/wall textures swim.
    // These are the two opt-in corrections. Both are visual only — the
    // guest-visible GTE screen coordinates stay integer and fully faithful.
    // 0 = off (the faithful default on a fresh config).
    int  geometry_correction;    // bool: sub-pixel vertex precision
    int  perspective_texturing;  // bool: perspective-correct UVs

    // ---- PSX multitap (SCPH-1070) ----------------------------------------
    // Appended additively. When a PSX game advertises num_players >= 3, the
    // launcher can hide seats beyond the two native ports until multitap is
    // on. 0 = off, 1 = on. Hosts should seed from settings (psxrecomp
    // defaults ON when unset). Netplay lobbies with more than 2 seats always
    // arm multitap in the runtime regardless of this flag.
    int  multitap_enabled;

    // Opt-in DualShock-on-multitap-tap hack (0 off, 1 on). Persisted to
    // settings.toml and game.toml [controller] multitap_analog. Hosts may
    // also publish it in match_caps for the session.
    int  multitap_analog;

    // ---- optional host assist/cheat gate ---------------------------------
    // A game that sets GameInfo.has_assist_tools exposes this value in a
    // dedicated launcher view. The host decides which runtime actions it
    // gates. Appended additively so existing consumers remain unchanged.
    int  assist_tools;        // bool: host-defined assists/cheats are enabled

    /* Optional host-consumed bindings. A GameInfo with settings_bindings=1
     * exposes keyboard + standard-controller chips on the Controller page.
     * Keyboard values are SDL scancodes; pad values use the encoding above. */
    int player_key_bind[RECOMP_LAUNCHER_MAX_PLAYERS]
                       [RECOMP_LAUNCHER_MAX_BINDINGS];
    int player_pad_bind[RECOMP_LAUNCHER_MAX_PLAYERS]
                       [RECOMP_LAUNCHER_MAX_BINDINGS];

    // Fast-forward speed as a multiplier of real time, for hosts whose
    // assist page offers a speed slider rather than a fixed rate. 0 = unset;
    // the model seeds it from GameInfo.assist_fast_forward_min. Appended for
    // ABI stability.
    int  assist_fast_forward_multiplier;

    int assist_key_bind[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
    int assist_pad_bind[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];

    /* Local rewind snap-ring capacity (PSX). UI offers 50/100/150/200;
     * 0 = unset -> model seeds 50. See RECOMP_LAUNCHER_HAS_REWIND_DEPTH. */
    int  rewind_depth;
    /* Frames between local rewind snaps. UI offers 1/4/8/12/15; 0 => 15. */
    int  rewind_interval;

    // ---- online identity (GameInfo.has_player_name games) -----------------
    // Persistent host-owned display name (the DS firmware nickname on NDS);
    // empty = the runtime's default. Edited on the dashboard IDENTITY card;
    // the host validates on use and persists it like bios_path. Appended
    // additively, same ABI convention as every block above.
    char player_name[64];

    /* GLSL .glsl/.glslp path selected from Display settings. Empty = disabled.
     * Appended for ABI stability; see GameInfo.has_shader. */
    char shader_path[512];

    /* How a low-res FMV is reconstructed when it is scaled up to the window.
     * Separate from texture_filter: that one is about the 3D rasterizer's
     * texture sampling, this one is about a decoded video frame, and the right
     * answer differs (a movie wants reconstruction, a PSX texture usually wants
     * the native look). Only consulted while antialiasing is on.
     *   0 = unset -> the model seeds RECOMP_LAUNCHER_FMV_FILTER_BICUBIC
     *   1 nearest, 2 bilinear, 3 sharp-bilinear, 4 bicubic
     * Stored 1-based so a zero-initialized host predating the field gets the
     * default rather than silently pinning "nearest". See GameInfo.has_fmv_filter.
     * Appended additively. */
    int  fmv_filter;

    /* Driver vsync at present time (GameInfo.has_vsync consoles).
     *   RECOMP_LAUNCHER_VSYNC_ON (1)        tear-free, swap waits on the panel
     *   RECOMP_LAUNCHER_VSYNC_OFF (2)       immediate swap, lowest display latency
     *   RECOMP_LAUNCHER_VSYNC_ADAPTIVE (3)  vsync above the refresh, immediate below
     * 0 = unset -> the model seeds ON. Deliberately NOT stored as the host's
     * own 1/0/-1 encoding: 0 is a meaningful value there ("off"), which a
     * zero-initialized host predating this field could not be told apart from
     * "no opinion". Appended additively. */
    int  vsync;
    // ---- optional source-ROM patch --------------------------------------
    // The launcher always verifies rom_patch_source_path as the stock image,
    // then prepares a cached effective image from rom_patch_path. The host
    // uses rom_patch_sha1 as the effective runtime identity gate.
    int  rom_patch_enabled;
    char rom_patch_path[512];
    char rom_patch_source_path[512];
    char rom_patch_sha1[41];
    char rom_patch_crc32[9];

    /* Local rewind on/off (GameInfo.has_rewind_depth consoles).
     *   0 = off, 1 = on.
     * Stored plainly rather than 1-based like vsync, because here "unset" and
     * "off" are the same answer: the host default is off, so a zero-initialized
     * host predating this field gets the default it would have picked anyway.
     * The ring holds whole-machine snapshots on a frame cadence, which is why
     * it is opt-in. Appended additively. */
    int  rewind_enabled;

    /* ---- selected disc (GameInfo.discs titles) ---------------------------
     * 1-based number of the disc the player has selected, so the choice is an
     * ordinary persisted setting the host writes to its settings file
     * alongside every other row here — which is what lets an external
     * launcher manage it, and what makes the last-played disc come back next
     * session. 0 = unset: the launcher seeds it by matching initial_rom
     * against the roster, falling back to disc 1. Appended additively. */
    int  disc_index;

    /* Scanline post-process on/off (GameInfo.has_scanlines consoles). 0 = off
     * (also the unset default, like rewind_enabled — the host default is off, so
     * a zero-initialized host predating this field gets the right answer), 1 =
     * on. Appended additively. */
    int  scanlines;
    /* Scanline dark-gap depth as a percent. 0 = unset -> the model seeds 50; the
     * effective range is 1..100. Stored as a percent (not 0..1) so the whole
     * settings struct stays plain-int. Appended additively. */
    int  scanline_strength_pct;
    /* NDS virtual stylus overlay. 0 is unset and means the launcher default
     * for the console, which is enabled; 1 is explicitly enabled, -1 is
     * explicitly disabled. Bindings live in assist_key_bind/assist_pad_bind so
     * they appear in the Controller page with the rest of host-owned binds. */
    int  virtual_stylus;

    /* Presentation frame blending (GameInfo.has_frame_blend consoles):
     * average each presented frame with the previous one, so a game's
     * alternate-frame flicker "transparency" (thrusters, explosions) reads
     * as steady translucency instead of breaking up on a tear or on the
     * duplicated frame a 60.00 Hz panel makes of the 60.0988 Hz guest.
     * Costs half a frame of motion ghosting. 0 = off (the faithful
     * default). Appended for ABI stability. */
    int  frame_blend;

    /* Run-ahead depth in frames (GameInfo.has_run_ahead consoles): how many
     * frames the runtime speculates past the one being shown, so that the
     * game's own internal input latency is hidden. The runtime advances the
     * machine, snapshots, runs N more frames with the same input, presents
     * the last one, then restores -- so the picture the player sees is the
     * one their input will have produced N frames from now.
     *
     * Costs N extra emulated frames per displayed frame, and it is strictly
     * a LOCAL prediction: a peer cannot be speculated about, so a host must
     * refuse it during netplay regardless of this value.
     *
     * 0 = off, which is both "unset" and the faithful default -- a
     * zero-initialized host predating this field gets exactly the behavior
     * it had. RECOMP_LAUNCHER_RUN_AHEAD_MAX bounds what the UI offers.
     * Appended for ABI stability. */
    int  run_ahead;
};

/* Largest run-ahead depth the launcher will offer for
 * RecompLauncherCSettings.run_ahead. Deeper than this and the cost (one full
 * extra emulated frame each) buys latency the player cannot feel, while the
 * mispredictions a deep speculation makes become visible. A host whose
 * runtime clamps lower still clamps on read; the UI never offers more. */
#define RECOMP_LAUNCHER_RUN_AHEAD_MAX 4

/* Values for RecompLauncherCSettings.vsync (1-based; 0 = unset). */
#define RECOMP_LAUNCHER_VSYNC_ON       1
#define RECOMP_LAUNCHER_VSYNC_OFF      2
#define RECOMP_LAUNCHER_VSYNC_ADAPTIVE 3
#define RECOMP_LAUNCHER_VSYNC_COUNT    3

/* Values for RecompLauncherSettings.fmv_filter (1-based; 0 = unset). */
#define RECOMP_LAUNCHER_FMV_FILTER_NEAREST  1
#define RECOMP_LAUNCHER_FMV_FILTER_BILINEAR 2
#define RECOMP_LAUNCHER_FMV_FILTER_SHARP    3
#define RECOMP_LAUNCHER_FMV_FILTER_BICUBIC  4
#define RECOMP_LAUNCHER_FMV_FILTER_COUNT    4
/* Hosts can #ifdef on this to stay source-compatible with older recomp-ui. */
#define RECOMP_LAUNCHER_HAS_FMV_FILTER 1

/* Hosts can #ifdef on this to stay source-compatible with older recomp-ui. */
#define RECOMP_LAUNCHER_HAS_FRAME_BLEND 1
/* Scanline post-process (Settings.scanlines / scanline_strength_pct,
 * GameInfo.has_scanlines). Hosts #ifdef on this to stay source-compatible with
 * older recomp-ui that lacks the fields. */
#define RECOMP_LAUNCHER_HAS_SCANLINES 1

// ---- host verification/inspection results (filled by the callbacks below) ----
// Plain-C structs so a host can implement the callbacks with zero launcher
// internal types. Mirror what the legacy launcher computed inline.
#define RECOMP_LAUNCHER_HAS_SBI_STATUS 1
enum { RECOMP_SBI_NA = 0, RECOMP_SBI_MISSING = 1, RECOMP_SBI_OK = 2 };
typedef struct RecompLauncherCDiscVerify {
    char serial[16];   // e.g. "SCUS-94423"; "" = unknown/unread
    char region[8];    // e.g. "NTSC-U"; "" = unknown
    int  iso_ok;       // ISO9660 / system header present
    int  verdict;      // 0 none, 1 ok, 2 warn, 3 bad
    /* Appended for ABI: TOC / netplay mount gate (memset 0 = legacy host). */
    int  track_count;      // mounted iso_track_count; 0 if TOC not opened
    int  netplay_ok;       // 1 = mount satisfies game.toml [netplay] policy
    char disc_fp[65];      // lowercase hex SHA-256 TOC fingerprint; "" if none
    char netplay_detail[160];
    int sbi_status; // RECOMP_SBI_*; zero means no requirement recorded by host.
} RecompLauncherCDiscVerify;

/* Host BIOS check for the first-run setup wizard (has_bios games). */
typedef struct RecompLauncherCBiosVerify {
    int  ok;           // 1 = usable BIOS present (linked / ready to Play)
    int  warn;         // 1 = size/CRC soft mismatch (still ok to boot)
    char detail[160];  // short status for the UI
    /* 1 = file looks valid but is not compiled into this binary — player must
     * Generate & rebuild (or switch back to a linked BIOS like OpenBIOS).
     * Appended for ABI compatibility; older hosts leave it 0 via memset. */
    int  needs_regen;
} RecompLauncherCBiosVerify;

/* Optional progress callback for prepare_with_progress (worker thread).
 * pct is 0..1 when known; negative means indeterminate. message may be NULL. */
typedef void (*RecompLauncherCPrepareProgressFn)(void* ctx, float pct,
                                                 const char* message);

typedef struct RecompLauncherCMemcard {
    int           valid;          // 128 KB + "MC" magic present
    int           used_blocks;    // 0..15
    unsigned char block_used[15]; // per-block: 1 = occupied
} RecompLauncherCMemcard;

// One Transfer Pak slot's inspection result (filled by the tpak_inspect
// callback below). The HOST owns all cartridge knowledge — header sniffing,
// which Gen-1 charmap decodes the trainer name (ASCII for Stadium US, kana
// for Pocket Monsters Stadium J), what the cart is called on screen — so the
// launcher stays console-generic and just renders these facts.
typedef struct RecompLauncherCTpak {
    int  valid;             // recognized GB cartridge
    char cart_label[96];    // display name, UTF-8 (kana ok; "" => show the file name)
    char trainer_name[32];  // decoded save-file trainer name ("" = no/unreadable save)
    char trainer_id[16];    // decoded trainer ID, ready to display ("" = none)
    // Cartridge art tint drawn by the launcher's native cart glyph:
    // 0 unknown/other (gray), 1 red, 2 blue, 3 yellow, 4 green.
    int  cart_kind;
} RecompLauncherCTpak;

// One image of a multi-image title, as the BUILD knows it. A PSX game built
// from a 3-disc set publishes three of these, in disc order, and the launcher
// renders a "Disc Selection" dropdown so the player picks which one Play
// boots. This is the build's roster — the discs the game was compiled
// against — not a scan of the player's folder; a player who moved one image
// still browses for it, and that browse rebinds only the selected slot.
typedef struct RecompLauncherCDisc {
    // Disc number as printed on the media (1-based). 0 => use the array
    // position + 1, so a host may leave this unset for an ordinary 1..N set.
    int         number;
    // Optional display name for the dropdown row. NULL/"" => the launcher
    // shows "Disc <number>". Borrowed; must outlive the run_window call.
    const char* label;
    // The image the build was made against (a .cue where one exists).
    // Borrowed; must outlive the run_window call.
    const char* path;
} RecompLauncherCDisc;

typedef struct RecompLauncherCGameInfo {
    const char*    name;
    const char*    region;
    uint32_t       expected_crc;
    int            has_expected_crc;
    const uint8_t (*known_sha256)[32];
    size_t         num_known_sha256;
    /* Accepted SHA-1 fingerprints as 40-char lowercase hex strings — the
     * identity cartridge consoles (GBA, SNES) actually gate on. The launcher
     * computes SHA-1 over the picked ROM and matches any entry, so its
     * "verified" check agrees with the game runtime's real gate. NULL/0 =>
     * no SHA-1 check. Preferred over expected_crc for those consoles (a
     * CRC32 is dump-specific; SHA-1 is the canonical ROM identity). */
    const char* const* known_sha1_hex;
    size_t         num_known_sha1;
    int            widescreen_supported;   /* hide Widescreen settings when 0 */
    /* How many players the GAME supports (1..RECOMP_LAUNCHER_MAX_PLAYERS),
     * additionally capped by the active console profile. The launcher hides
     * Player N+ rows when this is N — e.g. SMW Co-op is 2-player even though
     * the shared ABI can store 5. 0 means "unset" and is treated as 2 for
     * backward compatibility with callers that predate this field. */
    int            num_players;
    int            msu1_supported;
    const char*    msu1_note;          /* shown under MSU-1 settings (which patch) */
    const char*    msu1_patch_path;
    const char*    sram_path;          /* "saves/<title>.srm" (exe-anchored) for SAVES panel */
    const char*    platform;           /* console subtitle under the title, e.g. "PLAYSTATION",
                                          "SUPER NINTENDO". NULL => no subtitle. */
    const char*    theme;              /* built-in theme name: "psx" for the PlayStation look,
                                          NULL/other => default CRT-console theme. */
    /* config.ini path the hotkey editor reads/writes ([KeyMap] section only,
     * surgical edits). NULL => "config.ini" in cwd (exe-anchored by main).
     * Games pass their --config override here so hotkey edits follow it. */
    const char*    config_path;
    /* Keyboard-bind file path the Controller rebind page persists to. NULL
     * => "keybinds.ini" in cwd (exe-anchored), matching each runtime's own
     * default (recompui_keybinds_init(NULL) / psx_keybinds_init(NULL)) so the
     * launcher and the game agree on one file without a host having to set
     * this. The ON-DISK FORMAT is chosen automatically from the active
     * SystemProfile (launcher_system.h) — not from a separate flag here:
     * PSX games get psxrecomp's own psx_keybinds.c format (24 keys, section
     * [player1]/[player2], names up/down/.../rs_right) so rebinds actually
     * reach the game; every other console keeps this launcher's generic
     * keybinds.c format exactly as before. Pass a host-specific path only
     * when the game's cwd won't match the launcher's (e.g. a differently
     * anchored --keybinds override). */
    const char*    keybinds_path;

    // Controller pad-mode (PlayStation-style analog/digital emulation). Consoles
    // without pad modes (SNES) leave pad_mode_supported = 0 and the selector + the
    // analog/digital art are never shown (the generic pad.tga is used).
    int            pad_mode_supported;    // 0 = no pad-mode UI at all; 1 = show the selector + swapping art
    int            pad_mode_selectable;   // 0 = hide selector, force locked_pad_mode (game.lock_mode)
    int            locked_pad_mode;       // forced mode when !pad_mode_selectable
    int            lock_device;           // 1 = hide the player controller cards entirely (fixed pad)
    // Aspect ratios offered. bit0 = 4:3 (implied/always), bit1 = 16:9, bit2 = 21:9.
    // 0 = fall back to the legacy widescreen_supported bool (SNES: 16:9 toggle).
    int            aspect_mask;

    // ---- deeper PSX-style settings capability flags ----
    // 0 => that control is hidden entirely; SNES/other consoles that leave all
    // of these 0 keep exactly today's minimal settings surface.
    int  has_window_size;       // px window-size control (else the legacy window_scale cycle stays)
    int  has_renderer;          // Software/OpenGL toggle
    int  has_supersampling;
    int  has_antialiasing;
    int  has_texture_filter;    // Nearest/Bilinear (else the legacy Linear filtering checkbox stays)
    int  has_screen_kind;       // CRT/screen-model filter
    int  has_frame_interp;
    int  has_spu_hq;
    int  has_skip_fmv;          // Skip FMVs
    int  has_turbo_loads;
    int  has_fullscreen_toggle; // DEPRECATED, ignored: the Fullscreen row is universal
                                // (every console, tri-state 0 off/1 borderless/2 exclusive).
                                // Kept only for ABI layout compatibility.
    int  has_bios;              // BIOS path picker
    int  has_deadzone_pct;      // single analog-deadzone % control
    const char* rom_noun;       // "ROM" (default/NULL) | "Disc" | "Cartridge" — the Change-<noun>
                                 // button label + File row
    // Languages (Localization menu shown only when num_languages > 0).
    const char* const* language_labels;  // e.g. {"English","Japanese"}
    int  num_languages;

    // ---- host verification/inspection callbacks (optional; PSX uses them) ----
    // When set, the launcher shows REAL disc/memcard facts and RE-runs the
    // callback whenever the user changes the disc / a memory card (matching the
    // legacy launcher). NULL => the launcher falls back to a placeholder verdict
    // / empty card summary. `disc_verify` gets the current disc path; return 1
    // if `out` was filled. `memcard_inspect` gets one slot's card path; return
    // 1 if `out` was filled.
    int (*disc_verify)(const char* disc_path, RecompLauncherCDiscVerify* out);
    int (*memcard_inspect)(const char* card_path, RecompLauncherCMemcard* out);

    /* Box-art image path relative to the assets dir. NULL/"" => the default
     * "assets/img/boxart.tga". Multi-variant repos whose variants share one
     * build dir stage one file per variant (e.g. "assets/img/boxart_firered
     * .tga") and point each exe's GameInfo here. */
    const char* boxart_path;

    /* Game-supplied aspect vocabulary: overrides the built-in PSX-style
     * 4:3/16:9/21:9 set. Settings.aspect_index cycles 0..num_aspect_labels-1;
     * index 0 should be the native aspect. The host maps the committed index
     * onto its own render parameter (e.g. gbarecomp --view-width).
     * aspect_experimental=1 draws the amber EXPERIMENTAL tag next to the
     * cycle (the snesrecomp/psxrecomp widescreen convention for per-game
     * enhancement surfaces that are still maturing). */
    const char* const* aspect_labels;
    int  num_aspect_labels;
    int  aspect_experimental;

    /* ---- audio output device picker (N64/RT64 hosts) --------------------
     * When num_audio_devices > 0, Settings->Audio grows an "Output device"
     * dropdown over these HOST-enumerated display names (the host queries
     * SDL_GetAudioDeviceName itself, pre-launcher, exactly as the SS Anne
     * launcher did). The pick round-trips through Settings.audio_device by
     * NAME; a "(system default)" row is always offered first and commits "".
     * NULL/0 => no device row (every existing console unchanged). */
    const char* const* audio_device_labels;
    int  num_audio_devices;

    /* ---- renderer vocabulary override ------------------------------------
     * When set, the has_renderer cycle walks these 0..num_renderers-1 labels
     * instead of the built-in Software/OpenGL pair — e.g. the RT64 hosts'
     * {"Auto","Vulkan","D3D12"} graphics-API pick. Settings.renderer holds
     * the committed index. NULL/0 => the legacy 2-value toggle. */
    const char* const* renderer_labels;
    int  num_renderers;

    /* ---- N64 Transfer Pak (dashboard "tpak" panel) ------------------------
     * tpak_slots (0..4): how many controller ports offer a Transfer Pak GB
     * cartridge card. 0 => the panel never composes (Snap, Pikachu). Stadium
     * passes 4. The launcher edits Settings.tpak_* (ROM/save paths + enabled)
     * and calls tpak_inspect — the HOST's cartridge brain — on every change
     * to refresh the card's label/trainer/tint facts. tpak_inspect may be
     * NULL: cards then show file names with the neutral tint. */
    int  tpak_slots;
    int (*tpak_inspect)(const char* rom_path, const char* save_path,
                        RecompLauncherCTpak* out);

    /* ---- rebind-page opt-out ---------------------------------------------
     * 1 = hide the keyboard/controller bindings grid on the Configure page
     * (input source + deadzone remain). For games whose runtime consumes no
     * bind file at all (PMS-J today) an editor that writes a file nothing
     * reads would be a lying UI. 0 (default/memset) keeps the grid. */
    int  hide_rebind;

    /* ---- mouse controls (opt-in; Pokemon Snap) ---------------------------
     * 1 = this game supports mouse-aim: the input-source dropdown grows a
     * "Keyboard + Mouse" entry (the keyboard source with mouse-aim on) beside
     * the plain "Keyboard" one, and a "MOUSE" card (sensitivity / invert /
     * three rebindable mouse buttons) appears on the Controller page whenever
     * a keyboard-family source is selected. Drives Settings.mouse_* above.
     * 0 (default/memset) => none of that surface exists and every non-mouse
     * consumer (SNES/PSX/GBA/PSR/PMS-J) is byte-for-byte unchanged. */
    int  has_mouse_controls;
    // ---- NES-style capability flags (appended additively) ----
    int  has_integer_scale;   // Integer-scale checkbox in Display settings
    // HD texture packs (Mesen hires.txt format): 1 shows the HD-pack toggle +
    // folder picker in Display settings (NES defaults this ON per game; a
    // stock build that must not load packs passes 0 — e.g. unpatched Zelda).
    int  hdpack_supported;
    // Password/mantra save (e.g. Faxanadu): when password_save_path is
    // non-NULL the SAVES row shows the password text (read-only, editable
    // behind an Edit + confirm step) instead of the binary SRAM file UI.
    // The file is a single line of text. Independent of sram_path.
    const char* password_save_path;   // abs path to the 1-line password file
    const char* password_save_label;  // row label, e.g. "Password" / "Mantra"
    // Binary SRAM-backed password record. When password_sram_path is non-NULL
    // the same SAVES row reads/edits a MMXPASS v1 record inside the SRAM file.
    // password_sram_size is the minimum file size to create/maintain.
    const char* password_sram_path;
    const char* password_sram_label;
    int         password_sram_size;
    int         password_sram_offset;
    // Light-gun (NES Zapper) game: the controller config page shows a Zapper
    // block (mouse-as-gun + crosshair toggles, persisted to the engine's
    // keybinds.ini [zapper] section) alongside the pad UI.
    int  zapper;

    // Cartridge light sensor: 1 adds a Solar sensor panel to Settings, where
    // the player sets the location its brightness is read from. Games without
    // the hardware pass 0 and the panel never composes, so every existing
    // consumer is byte-for-byte unchanged.
    int  has_solar_sensor;

    // Live aspect-driven view capability. When present, Display settings show
    // an Adaptive view toggle. Adaptive + fullscreen leaves the fixed aspect
    // control visible but disabled because the display chooses the live width.
    int  adaptive_view_supported;
    // Netplay is a title/developer capability, not a user setting. When set,
    // the dashboard exposes lobby host/join controls through host-owned
    // callbacks.
    int netplay_supported;
    const RecompLauncherCNetplayCallbacks* netplay;
    /* Soft-return from a netplay match: open Netplay + LOBBY room if still
     * seated (WS or LAN). Optional resume_netplay_endpoint is "ip:port" for
     * LAN room header (NULL/empty => online Lobby Server URL). */
    int resume_netplay_room;
    const char* resume_netplay_endpoint;

    /* ---- first-run setup wizard -------------------------------------------
     * Opt-in product surface. When setup_wizard_supported is 0 (default), the
     * launcher never opens the first-run modal and never shows Generate /
     * rebuild — even if prepare_* callbacks are non-NULL. Hosts that ship a
     * self-build flow set this to 1 and fill prepare/rebuild/toolchain fields.
     *
     * When supported AND (needs_setup is 1 OR the launcher detects a missing
     * ROM/disc, and missing BIOS when has_bios), a blocking setup modal opens
     * before the dashboard. Cart-only games (has_bios=0) only prompt for a ROM.
     *
     * bios_verify (optional): host checks BIOS size/CRC. Return 1 and fill
     * `out` (ok/warn/detail). Called with an empty path when the player has
     * not chosen a dump — host should accept that when a bundled BIOS
     * (e.g. OpenBIOS) is available, or set ok=0 when a retail dump is
     * required. NULL => empty path = bundled OK; non-empty path must exist.
     *
     * prepare_disc (optional): convert a raw dump into a playable image.
     * Blocking host callback; the UI shows a busy state while it runs.
     * Return 1 and write the playable .cue/.bin/.img/.iso/.car path into out_disc_path.
     * prepare_disc_label / prepare_disc_note are button + help text (NULL =>
     * "Convert raw dump…" / default note).
     *
     * Path persistence: the moment the wizard's picks are confirmed (Confirm
     * disc / Continue to launcher), the launcher writes rom.cfg, disc.cfg and
     * bios.cfg beside the executable and in the cwd, and calls persist_setup
     * (or persist_setup_discs), so quitting without PLAY -- or a host relaunch
     * -- still remembers them and the wizard does not ask again. The same
     * flush runs on a BIOS change, before Generate and after a rebuild.
     * persist_setup lets the host also flush its own config (project-root
     * sidecars, config.ini); return 0 on success. */
    int needs_setup;
    int (*bios_verify)(const char* bios_path, RecompLauncherCBiosVerify* out);
    int (*prepare_disc)(const char* source_path, char* out_disc_path, size_t out_cap,
                        char* err_msg, size_t err_cap);
    const char* prepare_disc_label;
    const char* prepare_disc_note;
    const char* rom_cache_path; /* NULL => "rom.cfg" next to cwd/exe */
    int (*persist_setup)(void* ctx, const char* rom_path, const char* bios_path);
    void* persist_setup_ctx;

    /* Optional schema-driven mod provider. Appended for ABI stability. The
     * Mods view requires both RECOMP_UI_ENABLE_MODS=1 and a non-NULL provider;
     * default builds therefore remain inert even if a caller populates this
     * field. Features are the primary user-facing surface; packages are the
     * secondary installation/maintenance surface. */
    const RecompLauncherCModProvider* mods;

    /* ---- controller motion ----------------------------------------------
     * 1 adds a MOTION card to the Controller configuration page with a gyro
     * sensitivity slider. The launcher only edits Settings.gyro_sensitivity;
     * discovery, sensor selection, and axis mapping remain host-owned. */
    int has_gyro_controls;

    /* Add independent checkboxes to Display settings. The host maps their
     * committed Settings values onto renderer configuration. */
    int has_sharp_filter;
    int has_affine_filter;

    /* ---- multi-display layout -------------------------------------------
     * Optional host-defined Display row. Settings.display_layout cycles over
     * these labels. Nintendo DS uses {"Stacked window","Separate windows"}.
     * NULL/0 keeps every existing single-display launcher unchanged. */
    const char* const* display_layout_labels;
    int num_display_layouts;

    /* ---- prepare job UX (appended; disc convert / local codegen) ---------
     * prepare_use_selected_rom: 1 = the prepare button uses the already-
     * picked ROM/disc (no second file picker). Cart codegen hosts use this.
     * prepare_section_title / prepare_busy_status / prepare_success_status
     * override the default "Convert raw dump…" copy when non-NULL.
     *
     * prepare_with_progress: when non-NULL, preferred over prepare_disc.
     * Same success contract (return 1 + out_path); may invoke on_progress
     * from the worker thread. Zero-init leaves legacy prepare_disc behavior. */
    int prepare_use_selected_rom;
    const char* prepare_section_title;
    const char* prepare_busy_status;
    const char* prepare_success_status;
    int (*prepare_with_progress)(const char* source_path,
                                 char* out_path, size_t out_cap,
                                 char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx);

    /* ---- rebuild + relaunch after prepare (local codegen hosts) ----------
     * rebuild_with_progress: compile the project after sources are generated.
     * Return 1 and write the new/updated executable path into out_exe_path.
     * rebuild_after_prepare: when 1 and rebuild_with_progress is set, the
     * setup wizard auto-starts rebuild after a successful prepare.
     * relaunch_after_rebuild: when 1, a successful rebuild makes
     * recomp_launcher_run_window return RECOMP_LAUNCHER_RESULT_RELAUNCH (3);
     * call recomp_launcher_relaunch_exe() for the path to exec. */
    int (*rebuild_with_progress)(const char* rom_path,
                                 char* out_exe_path, size_t out_cap,
                                 char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx);
    int rebuild_after_prepare;
    int relaunch_after_rebuild;
    const char* rebuild_busy_status;     /* NULL => "Building game…" */
    const char* rebuild_success_status;  /* NULL => "Build complete." */

    /* ---- optional PGO optimize (MotK FMV; skip generate / setup wizard) ---
     * pgo_optimize_with_progress: instrument → train (video) → PGO use rebuild
     * on existing generated C. Same success/relaunch contract as rebuild. */
    int (*pgo_optimize_with_progress)(const char* rom_path,
                                      char* out_exe_path, size_t out_cap,
                                      char* err_msg, size_t err_cap,
                                      RecompLauncherCPrepareProgressFn on_progress,
                                      void* progress_ctx);
    const char* pgo_busy_status;         /* NULL => "Optimizing FMV…" */
    const char* pgo_success_status;      /* NULL => "FMV optimize complete." */

    /* ---- optional FMV timing opt (MotK VLC load-charge batch; regen+rebuild)
     * Unlike PGO, this regenerates C from game.toml then rebuilds (no train). */
    int (*fmv_timing_optimize_with_progress)(const char* rom_path,
                                             char* out_exe_path, size_t out_cap,
                                             char* err_msg, size_t err_cap,
                                             RecompLauncherCPrepareProgressFn on_progress,
                                             void* progress_ctx);
    const char* fmv_timing_busy_status;     /* NULL => "Applying FMV timing…" */
    const char* fmv_timing_success_status;  /* NULL => "FMV timing applied." */

    /* When 1, the setup modal hides "Continue to launcher" and requires
     * prepare (and rebuild when rebuild_after_prepare is set). Local codegen
     * first-run: Generate & rebuild, then relaunch — Quit is the only other exit.
     * When 0 but prepare_* is still wired (sources already generated), the
     * wizard opens only for a cleared BIOS/disc pick and shows a media-confirm
     * prompt instead of the full Generate & rebuild first-run page. */
    int prepare_required_before_continue;

    /* ---- portable toolchain step (appended; local codegen hosts) ----------
     * When setup_needs_toolchain is 1, the first-run wizard shows a page to
     * download cmake-clang-v1 or pick an offline zip before BIOS/ROM/generate.
     * toolchain_is_ready: optional quick check (usable local cmake/clang).
     * ensure_toolchain_with_progress: download==0 zip/cache only; 1 = download
     * if missing; 2 = force GitHub /releases/latest (update). Empty zip_path
     * with download==0 resolves cache only. See toolchain_update_available
     * (appended below) for remote newer-than-local prompts. */
    int setup_needs_toolchain;
    int (*toolchain_is_ready)(void);
    int (*ensure_toolchain_with_progress)(
        int download, const char* zip_path, char* err_msg, size_t err_cap,
        RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx);
    /* PSX geometry-precision controls (Settings.geometry_correction /
     * perspective_texturing). 0 => no row drawn, so every console that leaves
     * this unset keeps exactly today's settings surface. One flag still gates
     * both settings because they are two halves of the same enhancement, but
     * only perspective_texturing currently draws a control: geometry_correction
     * is known to crack meshes at the coverage the runtime can achieve and is
     * withdrawn from the UI while staying readable from game.toml/settings.toml
     * (psxrecomp ENHANCEMENTS.md G1.8/G1.9). Appended for ABI stability. */
    int  has_geometry_precision;

    /* Local rewind buffer size control (Settings.rewind_depth). PSX only. */
    int  has_rewind_depth;

    /* Driver-vsync control (Settings.vsync). 0 => no row drawn, so a console
     * that leaves this unset keeps exactly today's settings surface. Appended
     * for ABI stability. */
    int  has_vsync;

    /* Master switch for the first-run setup wizard + Generate & rebuild UI.
     * Appended for ABI stability; zero-init keeps every existing host dark. */
    int  setup_wizard_supported;

    /* Optional: return 1 when the installed cmake-clang-v1 pack is older than
     * GitHub /releases/latest (fills local/remote version strings). Wizard
     * keeps page 0 open to prompt Update / Skip. NULL => no update checks.
     * Appended for ABI stability. */
    int (*toolchain_update_available)(char* local_ver, size_t local_cap,
                                      char* remote_ver, size_t remote_cap);

    /* Optional: after toolchain_is_ready returns 0, a short note when the host
     * removed a broken cache (failed clang/lld smoke test). NULL/empty => none.
     * Appended for ABI stability. */
    const char* (*toolchain_repair_note)(void);

    /* Optional copy for a host-defined aspect cycle. When NULL, the row keeps
     * the historical "View mode" label and has no explanatory tooltip. These
     * are appended so older zero-initialized hosts retain their exact UI. */
    const char* aspect_setting_label;
    const char* aspect_setting_help;

    /* Optional complete host-owned defaults snapshot. When non-NULL, the
     * Settings footer exposes a confirmed "Restore Defaults" action that
     * copies this value into the launcher's editable settings. The launcher
     * copies the snapshot during initialization. ROM and save files are not
     * part of this structure and are never deleted. */
    const RecompLauncherCSettings* default_settings;

    /* Optional top-level launcher sections. `has_assist_tools` exposes a
     * dedicated opt-in page backed by Settings.assist_tools. `credits_text`
     * exposes a read-only Credits page and remains host-owned UTF-8 text. */
    int  has_assist_tools;
    const char* assist_tools_note;
    const char* credits_text;
    /* Host-owned binding mode. The active console profile supplies player
     * button names; assist_binding_labels supplies the optional global action
     * names (for example Rewind and Fast-forward). */
    int settings_bindings;
    const char* const* assist_binding_labels;
    int assist_binding_count;

    /* ---- online identity (opt-in, appended additively) ------------------
     * has_player_name: the game supports an online display name (a console
     * nickname the runtime applies at launch). Renders the dashboard
     * IDENTITY card for profiles whose panels_dashboard lists "identity" --
     * composition + availability, both layers, like has_bios. Most titles
     * have no online play and never set this (owner directive: shared
     * launcher features are per-game opt-in).
     * identity_detail: optional host-owned read-only line shown under the
     * name field (e.g. "Console MAC: 00:09:BF:xx:xx:xx"). NULL hides it. */
    int has_player_name;
    const char* identity_detail;

    /* Display row for Settings.shader_path. Appended for ABI stability. */
    int has_shader;

    /* Display row for Settings.fmv_filter. Only meaningful for a console whose
     * runtime decodes full-motion video into a low-res buffer it then scales
     * (PSX and friends); everything else leaves this 0 and the row is absent.
     * Appended for ABI stability. */
    int has_fmv_filter;

    /* Optional defaults for the assist bindings above, each an array of
     * assist_binding_count entries (keyboard = SDL scancodes, pad = the
     * portable encoding). NULL leaves a binding unbound until the user sets
     * it. Appended for ABI stability. */
    const int* assist_default_key_bind;
    const int* assist_default_pad_bind;
    /* Inclusive bounds for Settings.assist_fast_forward_multiplier. Both 0
     * hides the speed slider and leaves fast-forward at the host's fixed
     * rate. Appended for ABI stability. */
    int assist_fast_forward_min;
    int assist_fast_forward_max;

    /* Optional general ROM-patch surface. recomp-ui applies classic IPS,
     * IPS32, and checksum-verified BPS to a verified stock image. The cache
     * directory must already exist and should be owned by the host beside its
     * other mod data. NULL note uses the shared compatibility warning. */
    int rom_patch_supported;
    const char* rom_patch_note;
    const char* rom_patch_cache_dir;
    const char* rom_patch_required_sha1;

    /* ---- multi-image titles (appended additively) ------------------------
     * The roster of discs this build was made from, in disc order. When
     * num_discs > 1 the game panel grows a "Disc Selection" dropdown above
     * the identity checklist, the browse button names the selected disc
     * ("Browse For Disc 2"), and Play boots whichever disc is selected —
     * the chosen number rides back out in Settings.disc_index and the
     * chosen path in out_rom_path. NULL/0 (every single-image title, and
     * every host that predates this field) leaves the panel exactly as it
     * is today apart from the button's verb. */
    const RecompLauncherCDisc* discs;
    int num_discs;

    /* ---- window / taskbar icon (appended additively) ---------------------
     * Path to the image the HOST's own runtime applies as its window icon
     * (PNG/TGA/JPEG). The launcher applies the SAME file so the two windows
     * are one product in the task switcher instead of the game carrying the
     * real art and the launcher the toolkit's placeholder. The host resolves
     * it rather than the launcher guessing, because the file's name and
     * location are the host's convention.
     *
     * On Windows the executable's embedded .ico already covers the whole
     * process, so this mainly matters on Linux and macOS -- but it is applied
     * everywhere so the two windows can never disagree.
     *
     * NULL/"" (and every host that predates this field) leaves the launcher
     * window with the toolkit default, exactly as before. Borrowed; must
     * outlive the run_window call. */
    const char* window_icon_path;

    /* ---- multi-disc setup flush (appended additively) --------------------
     * persist_setup carries ONE path, which is all a single-image title has.
     * A multi-disc set needs every image the player located, not just the one
     * the wizard happened to have selected -- otherwise the other discs are
     * re-browsed on the next run, or worse, silently missing when the game
     * asks for disc 2.
     *
     * When this is non-NULL and num_discs > 1, the launcher calls it INSTEAD
     * of persist_setup after the wizard's picks are confirmed. disc_paths is
     * disc-ordered with disc_count entries; a slot the player has not located
     * is "" rather than NULL, so the host can still write a placeholder line
     * and keep the file's disc ordering intact.
     *
     * Hosts that predate this field, and single-image titles, keep going
     * through persist_setup unchanged -- so leaving this NULL is not a
     * degraded path, it is the correct one for a one-disc game.
     *
     * Return 0 on success, like persist_setup. Uses persist_setup_ctx. */
    int (*persist_setup_discs)(void* ctx, const char* const* disc_paths,
                               int disc_count, const char* bios_path);

    /* Display row for Settings.scanlines / scanline_strength_pct: a present-time
     * scanline post-process (a checkbox plus a strength slider). Only meaningful
     * for a console whose runtime implements it (PSX); everything else leaves
     * this 0 and the rows are absent. Appended for ABI stability. */
    int has_scanlines;
    /* NDS input convenience setting. When set, Settings shows a Virtual Stylus
     * opt-out and the Controller page may expose host-owned bindings for it. */
    int has_virtual_stylus;
    /* Optional host import: attach user-selected SBI to the current disc.
     * Return a private mounted-disc path; never replace the disc with the SBI. */
    int (*import_sbi)(const char* disc, const char* sbi, char* out_disc,
                      size_t out_cap, char* error, size_t error_cap);

    /* Display row (checkbox) for Settings.frame_blend. 0 => no row drawn,
     * so a console that leaves this unset keeps exactly today's settings
     * surface. Appended for ABI stability. */
    int has_frame_blend;

    /* Display row (cycle) for Settings.run_ahead. 0 => no row drawn, so a
     * console whose runtime cannot snapshot-and-restore a frame keeps
     * exactly today's settings surface. Appended for ABI stability. */
    int has_run_ahead;
} RecompLauncherCGameInfo;

/* recomp_launcher_run_window return codes */
#define RECOMP_LAUNCHER_RESULT_LAUNCH       0
#define RECOMP_LAUNCHER_RESULT_QUIT         1
#define RECOMP_LAUNCHER_RESULT_UNAVAILABLE  2
#define RECOMP_LAUNCHER_RESULT_RELAUNCH     3

// Returns: 0 = LAUNCH (boot out_rom_path with the edited *io),
//          1 = QUIT (caller should exit),
//          2 = UNAVAILABLE (assets/GL failed — caller boots as if skipped),
//          3 = RELAUNCH (host should exec recomp_launcher_relaunch_exe()).
int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len);

/* Valid after run_window returns RECOMP_LAUNCHER_RESULT_RELAUNCH. Copies the
 * executable path produced by rebuild_with_progress. Returns 1 on success. */
int recomp_launcher_relaunch_exe(char* out, size_t out_cap);

/* When preserve != 0, the launcher tears down its window/GL context but does
 * NOT call SDL_Quit(), so an in-process host can keep SDL subsystems across
 * launcher → game (and rematch soft-return). Default is 0 (full Quit). */
void recomp_launcher_set_preserve_sdl(int preserve);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_LAUNCHER_H
