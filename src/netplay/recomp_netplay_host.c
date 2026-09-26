/*
 * recomp_netplay_host.c -- see recomp_netplay_host.h.
 *
 * Lifted from snesrecomp runner/src/netplay/snes_host_lobby.c (snesrecomp
 * 36d6ce5). What was SNES there is a hook here (RecompNetplayHostHooks); the
 * rest -- and its comments, which carry the reasons -- came across as it was.
 * The gaps that file had are filled: rollback / input_prediction get+set,
 * connecting, name_rejected, the need_mods callbacks, and a launch that
 * carries input_prediction, rollback, occupied_mask and slot_port[] (host =
 * session slot 0 under RECOMP_NETPLAY_SLOTS_HOST_FIRST).
 */
#include "recomp_netplay_host.h"

#include "recomp_net/auth.h"
#include "recomp_net/lan_lobby.h"
#include "recomp_net/lan_direct.h"
#include "recomp_net/lan_beacon.h"
#include "recomp_net/chat_filter.h"
#include "recomp_net/address.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv: RECOMP_NETPLAY_LIST_DEBUG */
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

enum { kMaxLocalAddresses = 16 };

static RecompNetplayHostHooks g_h;
/* g_h.mods, for brevity: NULL is a vanilla-only build. */
static const RecompNetplayModHooks *g_mods;
static int g_inited;
static int g_hosting_lan;
static int g_joined_lan;
static int g_joined_direct; /* UDP Direct IP (remote); not file-registry */
static RecompLauncherCNetplayLaunch g_lan_launch;
static RNetLanLobby g_lan_room; /* host + direct-guest in-memory seat state */
static RNetLanDirectHost *g_direct_host;
static RNetLanDirectGuest *g_direct_guest;
/* LAN discovery across machines. The registry file below is visible to one
 * machine only, and a remote peer used to reach a LAN room solely by typing
 * its IP into Join Direct -- discovery "worked on the same machine, not on
 * the other PC". While hosting, the room is also announced by UDP broadcast
 * on RNET_LAN_BEACON_DEFAULT_PORT (one datagram a second); every launcher
 * listens and lists what it hears beside the file row. The row's lobby_id is
 * the same "lan:<ip:port>" Join Direct builds, so joining it takes the
 * JOIN_REQ path that already exists -- the beacon only replaces the typing. */
static RNetLanBeacon *g_beacon_pub;    /* host: announces g_lan_room */
static RNetLanBeacon *g_beacon_listen; /* browser: what other hosts announce */
static RNetLanBeaconRoom g_beacon_last; /* what the publisher was last told */
static char g_direct_peer_endpoint[64]; /* guest launch peer = typed IP:port */
static char g_lobby_url[256];
static char g_resume_endpoint[64];
static char g_runtime_error[64];
static RNetIpv4Address g_local_addresses[kMaxLocalAddresses];
static int g_local_address_count;
static char g_external_ip[RNET_IPV4_ADDRESS_TEXT_MAX];
static int g_lobby_input_delay = 6; /* waiting-room setting; clamped 2..20 */
static int g_lobby_force_turn = 0;  /* host: ICE relay-only for server lobbies */
static int g_lobby_force_input_relay = 0; /* host: server UDP input relay */
static int g_lobby_max_slots = 2;   /* seat ceiling for current/created room */
/* Host waiting-room settings the launcher's gaps used to drop: rollback on by
 * default (the lobby default), and a runway only once somebody set one --
 * 0 publishes nothing and the engine keeps its own default P. */
static int g_lobby_rollback = 1;
static int g_lobby_input_prediction = 0;
static int g_lan_guest_rtt_ms = -1;

/* ---- a LAN / Direct IP room across a soft return --------------------------
 *
 * arm_lan_launch closes BOTH Direct IP sockets: the host's because the game
 * session binds the same UDP port, the guest's with it (the room is over for
 * the length of the match). The rematch has to rebuild the channel from both
 * ends, and neither end knows when the other one is back:
 *
 *   host   prepare_rematch re-opens the listener and FREES the joiner seat --
 *          the new socket has no guest, and a seat still marked taken both
 *          lets the host start alone (connect timeout) and refuses the
 *          returning guest "full". If the port is not free yet, the re-open
 *          is retried from the pump (g_direct_reopen).
 *   guest  prepare_rematch starts a non-blocking re-join to the same host
 *          with the same password and bind (g_direct_rejoin); the pump polls
 *          it, re-sending JOIN_REQ while the host is not listening yet, for
 *          at most g_lan_rejoin_window_ms. Give-up and refusal are loud:
 *          stderr plus last_error, and the guest leaves the room rather than
 *          sitting in one that can never launch.
 *
 * Each START carries a fresh session id, allocated here by the host (no
 * server allocates one on LAN): see next_lan_session_id. */
static char g_direct_password[RNET_LAN_LOBBY_PASSWORD_MAX];
static char g_direct_bind[64];
static int g_direct_rejoin;
static uint64_t g_direct_rejoin_deadline_ms;
static unsigned g_lan_rejoin_window_ms = 30000;
static int g_direct_reopen;
static uint64_t g_direct_reopen_next_ms;
static uint32_t g_lan_session_last;

/* The title's own ceiling, from the hooks, inside the launcher's array. */
static int title_max_players(void)
{
  int n = g_h.max_players > 0 ? g_h.max_players : 2;
  if (n < 2)
    n = 2;
  if (n > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
    n = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
  return n;
}

static int clamp_lobby_max_slots(int slots)
{
  if (slots < 2)
    return 2;
  if (slots > title_max_players())
    return title_max_players();
  return slots;
}

static int clamp_input_prediction(int p)
{
  if (p <= 0)
    return 0; /* unpublished: the engine's default */
  if (p < 2)
    return 2;
  if (p > 16)
    return 16;
  return p;
}

static int clamp_input_delay(int delay)
{
  if (delay < 2)
    return 2;
  if (delay > 20)
    return 20;
  return delay;
}

/* The LAN room is a file. Two instances of one build only see the same room
 * if they read the same file, and the working directory is whatever each
 * was launched from -- a terminal in the repo root, a file manager in the
 * build dir -- so a relative registry path is anchored to the executable's
 * directory, not to the cwd. An absolute path from the game is kept as is. */
static const char *lan_path(void)
{
  static char resolved[1024];
  const char *p = g_h.lan_registry_path && g_h.lan_registry_path[0]
                      ? g_h.lan_registry_path
                      : "netplay_lan_lobby.txt";
  const int absolute = p[0] == '/' || p[0] == '\\' ||
                       (p[0] && p[1] == ':'); /* C:\... */
  if (absolute)
    return p;
  if (!resolved[0] &&
      (!g_h.exe_dir_path ||
       !g_h.exe_dir_path(g_h.ctx, p, resolved, sizeof(resolved))))
    return p; /* no exe dir known: the old cwd-relative behaviour */
  return resolved;
}

/* Defined with the rest of the chat plumbing further down; needed up here by
 * the pump and by the one place every LAN room teardown passes through. */
static void lan_chat_clear(void);
static void lan_chat_drain(void);
/* Defined below; the beacon filter needs the identity before then. */
static const char *game_name(void);
static const char *game_version(void);

/* LAN seat swap: the two-seat room has one trade. incoming = the guest asked
 * the host (host side); outgoing = 0 idle, 1 waiting, 2 accepted, -1 declined
 * (guest side, and the host's own instant result). */
static int g_lan_swap_incoming;
static int g_lan_swap_outgoing;

static uint64_t host_now_ms(void)
{
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* The session id of the host's next START. A LAN room has no server to hand
 * one out, and the rematch contract is the server's (recomp-net-server
 * docs/WS_LOBBY.md: a fresh id per match, so the next match's HELLO/BYE
 * cannot be confused with the last one's): the host allocates it, START and
 * the registry file carry it, the guest launches with the one it heard.
 * Monotonic per room; the room's FIRST id is seeded from the clock so a room
 * re-created on the same port does not reuse the previous room's ids. It is
 * a packet-header filter, never simulation input (NETPLAY.md §2). Never 0:
 * 0 reads as "a host that sent none". */
static uint32_t next_lan_session_id(void)
{
  if (g_lan_session_last == 0) {
    uint32_t x = (uint32_t)time(NULL) ^ (uint32_t)(host_now_ms() * 2654435761u);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    g_lan_session_last = x & 0x3fffffffu; /* room to count up */
  }
  if (++g_lan_session_last == 0)
    g_lan_session_last = 1;
  return g_lan_session_last;
}

static void close_direct_sockets(void)
{
  rnet_lan_direct_host_close(&g_direct_host);
  rnet_lan_direct_guest_close(&g_direct_guest);
  /* The announce follows the waiting-room socket: a room nobody can JOIN_REQ
   * (match running, host gone) must not keep appearing in browsers. */
  rnet_lan_beacon_close(&g_beacon_pub);
  memset(&g_beacon_last, 0, sizeof(g_beacon_last));
  g_lan_swap_incoming = 0;
  g_lan_swap_outgoing = 0;
  /* The room is over, so its log is too. Every LAN teardown -- leave, kick,
   * host close -- passes through here, which is why the clear lives here
   * rather than at each of those call sites. */
  lan_chat_clear();
}

static int publish_lan_room(void)
{
  /* The seated guest hears every room change over its socket; the file is
   * for browsers on this machine; the beacon (beacon_publish_step, from the
   * pump) is for browsers on the other machines. */
  if (g_direct_host)
    (void)rnet_lan_direct_host_notify_room(g_direct_host, &g_lan_room);
  return rnet_lan_lobby_publish(lan_path(), &g_lan_room) == RNET_LAN_LOBBY_OK;
}

/* The beacon's view of g_lan_room. lobby_id is exactly the Join Direct id so
 * cb_join needs no new case. */
static void beacon_room_from_lan(RNetLanBeaconRoom *out)
{
  memset(out, 0, sizeof(*out));
  snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", g_lan_room.endpoint);
  snprintf(out->endpoint, sizeof(out->endpoint), "%s", g_lan_room.endpoint);
  snprintf(out->game_name, sizeof(out->game_name), "%s", g_lan_room.game);
  snprintf(out->game_version, sizeof(out->game_version), "%s",
           g_lan_room.game_version);
  snprintf(out->room_name, sizeof(out->room_name), "%s", g_lan_room.name);
  out->has_password = g_lan_room.password[0] != '\0';
  out->player_count = g_lan_room.joiner_name[0] ? 2 : 1;
  out->max_slots = 2;
  out->started = g_lan_room.started;
}

/* Host, once per pump: announce the room while its waiting-room socket is
 * open. Opening is lazy and retried, so a transient socket failure costs one
 * second, not the session. The publisher refuses a non-private endpoint
 * (127.0.0.1 from a LAN-only room, a WAN address): that is the beacon's
 * RFC1918 rule, and such a room is not reachable by broadcast anyway. */
static void beacon_publish_step(void)
{
  RNetLanBeaconRoom room;
  if (!g_hosting_lan || !g_direct_host || !g_lan_room.endpoint[0])
    return;
  if (!g_beacon_pub) {
    if (rnet_lan_beacon_publish_open(&g_beacon_pub, 0) != 0) {
      static int s_said;
      if (!s_said++)
        fprintf(stderr, "recomp_netplay: LAN discovery beacon could not "
                        "open a UDP socket; other machines will not list "
                        "this room (Join Direct still works)\n");
      return;
    }
  }
  beacon_room_from_lan(&room);
  if (memcmp(&room, &g_beacon_last, sizeof(room)) != 0) {
    g_beacon_last = room;
    if (rnet_lan_beacon_publish_set_room(g_beacon_pub, &room) != 0) {
      static int s_said;
      if (!s_said++)
        fprintf(stderr, "recomp_netplay: LAN room endpoint %s is not a "
                        "private IPv4 address; not announcing it to the LAN\n",
                room.endpoint);
      return;
    }
    fprintf(stderr, "recomp_netplay: announcing LAN room %s on UDP %d\n",
            room.endpoint, RNET_LAN_BEACON_DEFAULT_PORT);
  }
  (void)rnet_lan_beacon_publish_tick(g_beacon_pub);
}

/* Browser, once per pump: hear other hosts. Opened lazily on the first pump
 * (the netplay page), kept for the launcher's life; the cache forgets a room
 * five seconds after its last announce, so a closed room disappears on its
 * own. Two launchers on one machine share the port (reuseaddr). */
static void beacon_listen_step(void)
{
  if (!g_beacon_listen) {
    static int s_tried;
    if (s_tried)
      return;  /* one failure is a firewall / port conflict, not a retry case */
    s_tried = 1;
    if (rnet_lan_beacon_listen_open(&g_beacon_listen, 0) != 0) {
      fprintf(stderr, "recomp_netplay: LAN discovery listener could not bind "
                      "UDP %d; rooms hosted on other machines will not be "
                      "listed (Join Direct still works)\n",
              RNET_LAN_BEACON_DEFAULT_PORT);
      return;
    }
  }
  (void)rnet_lan_beacon_listen_pump(g_beacon_listen);
}

/* A heard room is listed when it is this game at this version (the same
 * filter the registry read applies, and what JOIN_REQ will insist on), is
 * not started, and is not the room this launcher already shows another way:
 * its own hosted room, or the same-machine registry row. `skip_endpoint` is
 * that registry row's endpoint (or empty). */
static int beacon_row_listed(const RNetLanBeaconRoom *room,
                             const char *skip_endpoint)
{
  if (strcmp(room->game_name, game_name()) != 0)
    return 0;
  /* Same rule the lobby-server browser applies, and for the same reason.
   * This used to be an unconditional exact match, which is right for two
   * shipped releases and wrong for everything else: every development build
   * carries a dirty-diff hash of its own working tree, so two developers on
   * one LAN could never see each other's rooms -- the beacon arrived, the
   * row was dropped, and nothing said why. The join still refuses a real
   * mismatch; it just gets to explain itself. */
  if (rnet_lobby_version_filter_strict() && room->game_version[0] &&
      strcmp(room->game_version, game_version()) != 0)
    return 0;
  if (room->started)
    return 0;
  if (g_hosting_lan && strcmp(room->endpoint, g_lan_room.endpoint) == 0)
    return 0;
  if (skip_endpoint && skip_endpoint[0] &&
      strcmp(room->endpoint, skip_endpoint) == 0)
    return 0;
  return 1;
}

/* index-th listed beacon row (see beacon_row_listed). 1 = filled. */
static int fill_beacon_row(int index, const char *skip_endpoint,
                           RecompLauncherCNetplayLobby *out)
{
  RNetLanBeaconRoom room;
  int i;
  int n;
  if (!g_beacon_listen || index < 0)
    return 0;
  n = rnet_lan_beacon_count(g_beacon_listen);
  for (i = 0; i < n; ++i) {
    if (!rnet_lan_beacon_get(g_beacon_listen, i, &room))
      break;
    if (!beacon_row_listed(&room, skip_endpoint))
      continue;
    if (index-- > 0)
      continue;
    if (!out)
      return 1;
    memset(out, 0, sizeof(*out));
    snprintf(out->lobby_id, sizeof(out->lobby_id), "%s", room.lobby_id);
    snprintf(out->name, sizeof(out->name), "LAN - %s",
             room.room_name[0] ? room.room_name : room.endpoint);
    snprintf(out->game_name, sizeof(out->game_name), "%s", room.game_name);
    snprintf(out->game_version, sizeof(out->game_version), "%s",
             room.game_version[0] ? room.game_version : game_version());
    out->player_count = room.player_count > 0 ? room.player_count : 1;
    out->max_slots = room.max_slots >= 2 ? room.max_slots : 2;
    out->has_password = room.has_password;
    out->latency_ms = -1;
    return 1;
  }
  return 0;
}

static int beacon_row_count(const char *skip_endpoint)
{
  int n = 0;
  while (fill_beacon_row(n, skip_endpoint, NULL))
    ++n;
  return n;
}

static const char *game_name(void)
{
  return g_h.game_name && g_h.game_name[0] ? g_h.game_name : "Game";
}

static const char *game_version(void)
{
  return g_h.game_version && g_h.game_version[0] ? g_h.game_version
                                                    : "0.0.0";
}

static void dl_queue_step(void);
static void mod_set_sync_step(void);
static void desync_report_step(void);
static void cb_push_match_caps(void *ctx);
static void host_caps_watch_step(void);

/*
 * The host's required mod plan, carried in match caps.
 *
 * Filled here rather than only in push_match_caps so EVERY caps push carries
 * it: a plan that only rides along when someone happens to toggle a mod is a
 * plan that goes stale the first time anything else changes.
 *
 * One row per PACKAGE, which is both what the lobby server matches a joiner's
 * offer against and what a player actually installs. The runtime's own
 * effective-set text is per FEATURE and stays the basis of the simulation
 * equality check -- a package can be present and still be configured
 * differently. Two grains, two questions.
 */
static void fill_caps_mods(RNetLobbyMatchCaps *caps)
{
  int n;
  int i;

  if (!caps)
    return;
  caps->mod_count = 0;
  caps->mod_set[0] = '\0';
  caps->mod_cosmetic_allow[0] = '\0';
  if (!g_mods) {
    fprintf(stderr, "netplay: publishing mod plan (none) - this build has no "
                    "mod support\n");
    return;
  }
  /* Apply our own grant FIRST, because both the plan rows and the effective
   * set below are computed through it: a host that publishes an allowlist and
   * then reports a set built without it would advertise its own cosmetic mods
   * as requirements, and guests would be told to install them. */
  snprintf(caps->mod_cosmetic_allow, sizeof(caps->mod_cosmetic_allow), "%s",
           g_h.cosmetic_allow ? g_h.cosmetic_allow : "");
  if (g_mods->set_cosmetic_allow)
    g_mods->set_cosmetic_allow(g_mods->ctx, caps->mod_cosmetic_allow);
  n = g_mods->plan_rows
          ? g_mods->plan_rows(g_mods->ctx, caps->mods, RNET_LOBBY_MAX_MODS)
          : 0;
  if (n < 0)
    n = 0;
  if (n > RNET_LOBBY_MAX_MODS)
    n = RNET_LOBBY_MAX_MODS;
  /* The runtime refuses to emit a row whose id or version had to be cut, so
   * anything arriving here is whole; terminate defensively all the same. */
  for (i = 0; i < n; ++i) {
    RNetLobbyModPkg *dst = &caps->mods[i];
    dst->id[sizeof(dst->id) - 1] = '\0';
    dst->ver[sizeof(dst->ver) - 1] = '\0';
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->feats[sizeof(dst->feats) - 1] = '\0';
  }
  caps->mod_count = n;
  /* The exact configuration, not just the package list. */
  if (g_mods->effective_set) {
    char text[1024];
    const int need = g_mods->effective_set(g_mods->ctx, text,
                                           (uint32_t)sizeof(text));
    if (need > 0 && need < (int)sizeof(text) && strcmp(text, "(none)\n") != 0) {
      size_t o = 0;
      size_t k;
      for (k = 0; text[k] && o + 1 < sizeof(caps->mod_set); ++k) {
        char c = text[k];
        if (c == '\n') {
          if (o == 0 || caps->mod_set[o - 1] == ';') continue;
          c = ';';
        }
        caps->mod_set[o++] = c;
      }
      caps->mod_set[o] = '\0';
      if (o && caps->mod_set[o - 1] == ';') caps->mod_set[o - 1] = '\0';
      if (text[k] != '\0') {
        /* Publishing a PREFIX would be worse than publishing nothing: a guest
         * would adopt a partial configuration and believe it matched. */
        caps->mod_set[0] = '\0';
        fprintf(stderr, "netplay: mod set too long to publish; guests will be "
                        "asked to match it at launch instead\n");
      }
    }
  }

  /* Say what went on the wire. The caps line next to this one reported the
   * engine's settings and said nothing about mods, so a host with a plan and a
   * guest without the packages produced two clean-looking logs and a join
   * that failed for reasons neither of them recorded. */
  if (caps->mod_count > 0) {
    int k;
    fprintf(stderr, "netplay: publishing mod plan (%d package(s)) - peers "
                    "may join without these, but the match will not start "
                    "until they have them\n", caps->mod_count);
    for (k = 0; k < caps->mod_count; ++k)
      fprintf(stderr, "netplay:   requires %s@%s [%s]\n", caps->mods[k].id,
              caps->mods[k].ver, caps->mods[k].feats);
  } else {
    fprintf(stderr, "netplay: publishing mod plan (none) - vanilla match\n");
  }
}

/* What this peer already has, for the join's mod_offer. The lobby server
 * subtracts this from the host's plan and refuses to seat on any remainder, so
 * this is the half of the seat decision that speaks for US.
 *
 * Every installed (package, version) pair, enabled or not: the question the
 * server asks is possession. Which of them actually RUN is the host's plan to
 * decide, and a peer that owns a mod it has switched off can still play in a
 * lobby that requires it. */
static int mod_offer_rows(RNetLobbyModPkg *out, int max, void *ctx)
{
  RNetLobbyModPkg rows[RNET_LOBBY_MAX_MODS];
  int n;
  int i;
  int o = 0;
  (void)ctx;
  if (!out || max <= 0 || !g_mods || !g_mods->installed_rows)
    return 0;
  memset(rows, 0, sizeof(rows));
  n = g_mods->installed_rows(g_mods->ctx, rows, RNET_LOBBY_MAX_MODS);
  if (n > RNET_LOBBY_MAX_MODS)
    n = RNET_LOBBY_MAX_MODS;
  for (i = 0; i < n && o < max; ++i) {
    int dup = 0;
    int k;
    /* One row per package id. The runtime lists every (id, version) it
     * holds, but peers match on id, so a second version of the same package
     * says nothing new and would spend one of the few rows the offer has
     * room for. The first version stays as the one we report, so the row
     * can still say WHICH version we hold. */
    for (k = 0; k < o; ++k)
      if (!strcmp(out[k].id, rows[i].id)) { dup = 1; break; }
    if (dup)
      continue;
    memset(&out[o], 0, sizeof(out[o]));
    snprintf(out[o].id, sizeof(out[o].id), "%s", rows[i].id);
    snprintf(out[o].ver, sizeof(out[o].ver), "%s", rows[i].ver);
    /* Name and features are the host plan's business; an offer claims
     * possession and nothing else. */
    o++;
  }
  return o;
}

/* The mod runtime's cosmetic grant, through the hook when there is one. */
static void mods_set_cosmetic_allow(const char *allow)
{
  if (g_mods && g_mods->set_cosmetic_allow)
    g_mods->set_cosmetic_allow(g_mods->ctx, allow);
}

/* A plan or configuration this build cannot run: 1 and a reason. */
static int caps_require_mods(const RNetLobbyMatchCaps *caps)
{
  return caps && caps->valid && (caps->mod_count > 0 || caps->mod_set[0]);
}

static RNetLobbyMatchCaps default_caps(const RecompLauncherCSettings *settings)
{
  RNetLobbyMatchCaps caps;
  memset(&caps, 0, sizeof(caps));
  caps.valid = 1;
  /* The waiting room's settings; fill_match_caps may override. */
  caps.rollback = g_lobby_rollback ? 1 : 0;
  caps.input_delay = clamp_input_delay(g_lobby_input_delay);
  caps.input_prediction = clamp_input_prediction(g_lobby_input_prediction);
  if (g_h.fill_match_caps)
    g_h.fill_match_caps(g_h.ctx, settings, &caps);
  caps.input_delay = clamp_input_delay(caps.input_delay);
  caps.input_prediction = clamp_input_prediction(caps.input_prediction);
  caps.force_turn = g_lobby_force_turn ? 1 : 0;
  caps.force_input_relay = g_lobby_force_input_relay ? 1 : 0;
  fill_caps_mods(&caps);
  return caps;
}

static int read_lan(RNetLanLobby *state)
{
  return rnet_lan_lobby_read(lan_path(), game_name(), game_version(), state) ==
         RNET_LAN_LOBBY_OK;
}

static int create_lan(const char *name, const char *endpoint,
                      const char *password)
{
  char advertised[RNET_LAN_LOBBY_ENDPOINT_MAX];
  const char *stored_endpoint = endpoint;
  const char *colon;
  const char *port;
  char bind_hp[64];
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  close_direct_sockets();
  snprintf(g_lan_room.name, sizeof(g_lan_room.name), "%s",
           name && name[0]
               ? name
               : (g_h.default_lobby_name ? g_h.default_lobby_name
                                          : "LAN Lobby"));
  snprintf(g_lan_room.game, sizeof(g_lan_room.game), "%s", game_name());
  snprintf(g_lan_room.game_version, sizeof(g_lan_room.game_version), "%s",
           game_version());
  /* Online hosts bind 0.0.0.0, but that wildcard is not a routable guest
   * destination. Keep the bind; advertise a concrete local IPv4 in the LAN
   * registry row when present. */
  if (endpoint && strncmp(endpoint, "0.0.0.0:", 8) == 0) {
    RNetIpv4Address address;
    if (rnet_ipv4_enumerate(&address, 1) > 0 && address.address[0]) {
      snprintf(advertised, sizeof(advertised), "%s:%s", address.address,
               endpoint + 8);
      stored_endpoint = advertised;
    }
  }
  snprintf(g_lan_room.endpoint, sizeof(g_lan_room.endpoint), "%s",
           stored_endpoint && stored_endpoint[0] ? stored_endpoint
                                                 : "127.0.0.1:7777");
  snprintf(g_lan_room.host_name, sizeof(g_lan_room.host_name), "%s",
           rnet_lobby_display_name()[0] ? rnet_lobby_display_name() : "Host");
  snprintf(g_lan_room.password, sizeof(g_lan_room.password), "%s",
           password ? password : "");
  g_lan_room.host_slot = 0;
  g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
  if (!publish_lan_room())
    return 0;
  /* UDP waiting-room listen on the game port for remote Join Direct. */
  colon = strrchr(g_lan_room.endpoint, ':');
  port = colon ? colon + 1 : "7777";
  snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
  if (rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room) !=
      RNET_LAN_DIRECT_OK) {
    fprintf(stderr,
            "recomp_netplay: Direct IP listen failed on %s — same-machine "
            "file join still works; remote Join Direct will time out\n",
            bind_hp);
  }
  g_hosting_lan = 1;
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_peer_endpoint[0] = '\0';
  g_lan_guest_rtt_ms = -1;
  g_direct_rejoin = 0;
  g_direct_reopen = 0;
  g_lan_session_last = 0; /* a new room seeds a new id range */
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
           g_lan_room.endpoint);
  return 1;
}

static int fill_lan_row(RecompLauncherCNetplayLobby *out)
{
  RNetLanLobby state;
  if (!out)
    return 0;
  if (g_hosting_lan)
    state = g_lan_room;
  else if (!read_lan(&state))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", state.endpoint);
  snprintf(out->name, sizeof(out->name), "LAN - %s",
           state.name[0] ? state.name : "Lobby");
  snprintf(out->game_name, sizeof(out->game_name), "%s", state.game);
  snprintf(out->game_version, sizeof(out->game_version), "%s",
           state.game_version);
  out->player_count = state.joiner_name[0] ? 2 : 1;
  out->max_slots = 2;
  out->has_password = state.password[0] != '\0';
  return 1;
}

static void clear_lan_joiner(void)
{
  if (g_joined_direct && g_direct_guest)
    (void)rnet_lan_direct_guest_leave(g_direct_guest); /* no-op unseated */
  close_direct_sockets();
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_rejoin = 0;
  g_direct_peer_endpoint[0] = '\0';
  g_lan_guest_rtt_ms = -1;
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

/* Guest, after a soft return: poll the re-join prepare_rematch began. */
static void direct_rejoin_step(void)
{
  RNetLanLobby state;
  int rc;
  if (!g_direct_rejoin)
    return;
  if (!g_direct_guest) {
    g_direct_rejoin = 0;
    return;
  }
  rc = rnet_lan_direct_guest_join_poll(g_direct_guest, &state);
  if (rc == RNET_LAN_DIRECT_PENDING) {
    if (host_now_ms() < g_direct_rejoin_deadline_ms)
      return;
    fprintf(stderr,
            "recomp_netplay: LAN rematch: the host at %s did not take the "
            "guest back within %u ms -- leaving the room\n",
            g_direct_peer_endpoint, g_lan_rejoin_window_ms);
    snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
             "lan_rejoin_timeout");
    clear_lan_joiner(); /* unseated: no LEAVE goes out */
    return;
  }
  g_direct_rejoin = 0;
  if (rc == RNET_LAN_DIRECT_OK) {
    g_lan_room = state;
    fprintf(stderr, "recomp_netplay: LAN rematch: re-seated with the host at "
                    "%s\n", g_direct_peer_endpoint);
    return;
  }
  fprintf(stderr,
          "recomp_netplay: LAN rematch: the host at %s refused the guest "
          "back (rc=%d%s) -- leaving the room\n",
          g_direct_peer_endpoint, rc,
          rc == RNET_LAN_DIRECT_ERR_FULL ? ": its seat is taken" : "");
  snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
           "lan_rejoin_refused");
  clear_lan_joiner();
}

/* Guest: begin re-seating after the match closed our socket. */
static void direct_rejoin_begin(void)
{
  const char *name = rnet_lobby_display_name();
  int rc;
  if (!g_joined_direct || g_direct_guest || !g_direct_peer_endpoint[0])
    return;
  rc = rnet_lan_direct_guest_join_begin(
      g_direct_peer_endpoint, game_name(), game_version(), g_direct_password,
      name && name[0] ? name : "Player",
      g_direct_bind[0] ? g_direct_bind : NULL, &g_direct_guest);
  if (rc != RNET_LAN_DIRECT_OK && g_direct_bind[0]) {
    /* The bind we joined from is taken now: any port will do -- the host
     * answers the address the request came from. */
    fprintf(stderr, "recomp_netplay: LAN rematch: bind %s unavailable (%d); "
                    "re-joining from an ephemeral port\n", g_direct_bind, rc);
    rc = rnet_lan_direct_guest_join_begin(
        g_direct_peer_endpoint, game_name(), game_version(),
        g_direct_password, name && name[0] ? name : "Player", NULL,
        &g_direct_guest);
  }
  if (rc != RNET_LAN_DIRECT_OK) {
    fprintf(stderr, "recomp_netplay: LAN rematch: cannot open a socket to "
                    "re-join %s (%d) -- leaving the room\n",
            g_direct_peer_endpoint, rc);
    snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
             "lan_rejoin_failed");
    clear_lan_joiner();
    return;
  }
  g_direct_rejoin = 1;
  g_direct_rejoin_deadline_ms = host_now_ms() + g_lan_rejoin_window_ms;
  fprintf(stderr, "recomp_netplay: LAN rematch: re-joining the host at %s\n",
          g_direct_peer_endpoint);
}

/* Host: (re-)open the Direct IP listener the match closed. The seat the
 * closed socket knew is freed -- the guest re-joins into it. */
static void direct_reopen_step(void)
{
  const char *colon;
  const char *port;
  char bind_hp[64];
  const uint64_t now = host_now_ms();
  if (!g_direct_reopen)
    return;
  if (!g_hosting_lan || g_direct_host || !g_lan_room.endpoint[0]) {
    g_direct_reopen = 0;
    return;
  }
  if (now < g_direct_reopen_next_ms)
    return;
  colon = strrchr(g_lan_room.endpoint, ':');
  port = colon ? colon + 1 : "7777";
  snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
  if (rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room) !=
      RNET_LAN_DIRECT_OK) {
    if (g_direct_reopen_next_ms == 0)
      fprintf(stderr, "recomp_netplay: LAN rematch: Direct IP re-listen on %s "
                      "failed (port still held?) -- retrying\n", bind_hp);
    g_direct_reopen_next_ms = now + 500;
    return;
  }
  g_direct_reopen = 0;
  g_direct_reopen_next_ms = 0;
  (void)publish_lan_room();
  fprintf(stderr, "recomp_netplay: LAN rematch: listening on %s again; the "
                  "guest's seat is open for it to re-join\n", bind_hp);
}

/* Direct IP guest: 1 once THIS start's START message has been read. Only
 * START carries the match's session id; the host's ROOM refresh also says
 * "started" and is sent first (cb_request_start publishes the room, then
 * arms, which sends START), so arming on ROOM launched the guest with the
 * previous room's id -- 0, i.e. session 1 -- while the host ran a fresh one:
 * every LAN rematch timed out (nesrecomp rb_lobby.sh lan, 2026-09-25). */
static int g_lan_start_seen;

static void direct_guest_note_event(int ev)
{
  if (ev == 1)
    g_lan_start_seen = 1;
}

static void sync_lan_joiner(void)
{
  RNetLanLobby state;
  const char *name;
  int ev;
  direct_reopen_step();
  direct_rejoin_step();
  if (!g_joined_lan)
    return;
  if (g_joined_direct) {
    int rtt = -1;
    ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
    direct_guest_note_event(ev);
    if (ev == 2) /* KICK / CLOSE */
      clear_lan_joiner();
    else if (ev == 3 && rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    return;
  }
  if (!read_lan(&state)) {
    clear_lan_joiner();
    return;
  }
  name = rnet_lobby_display_name();
  if (!state.joiner_name[0] ||
      (name && name[0] && strcmp(state.joiner_name, name) != 0))
    clear_lan_joiner();
}

static int use_lan_members(RNetLanLobby *state)
{
  RNetLanLobby local;
  if (!state)
    state = &local;
  sync_lan_joiner();
  if (g_hosting_lan || g_joined_direct) {
    *state = g_lan_room;
    return g_hosting_lan || g_joined_lan;
  }
  if (!read_lan(state))
    return 0;
  if (g_joined_lan)
    return 1;
  if (!g_hosting_lan)
    return 0;
  return state->joiner_name[0] || rnet_lobby_member_count() < 2;
}

/* ---- session slots ------------------------------------------------------
 *
 * Lobby seat, session slot and controller port are three different numbers
 * (docs/HOST_NETPLAY.md, "Seats, session slots, and ports"). Every peer builds
 * this plan from the same seat table the start delivered, so they agree.
 *
 *   seats[]   occupied PLAYER seats, any order
 *   host_seat the host's player seat, or -1 when the host is in the gallery
 *   my_seat   this peer's player seat, or -1 (spectator)
 *
 * HOST_FIRST is psxrecomp's ae_np_plan_session_slots (runtime/src/main.cpp):
 * the host is slot 0 with its seat's port, the rest follow by ascending seat.
 * SEAT keeps session slot == seat, for an engine that ignores slot_port[]. */
typedef struct SlotPlan {
  int      local_slot;   /* -1: not a player */
  int      slot_count;
  uint32_t occupied;
  int      port[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1];
} SlotPlan;

static void plan_session_slots(const int *seats, int n, int host_seat,
                               int my_seat, int i_am_host, SlotPlan *out)
{
  const int cap = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1;
  int sorted[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS];
  int m = 0;
  int i;
  int j;
  memset(out, 0, sizeof(*out));
  out->local_slot = -1;
  for (i = 0; i < cap; ++i)
    out->port[i] = -1;
  /* Distinct, ascending seats. */
  for (i = 0; i < n && m < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS; ++i) {
    int dup = 0;
    if (seats[i] < 0 || seats[i] >= RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
      continue;
    for (j = 0; j < m; ++j)
      if (sorted[j] == seats[i]) dup = 1;
    if (!dup)
      sorted[m++] = seats[i];
  }
  for (i = 1; i < m; ++i)
    for (j = i; j > 0 && sorted[j - 1] > sorted[j]; --j) {
      const int t = sorted[j - 1];
      sorted[j - 1] = sorted[j];
      sorted[j] = t;
    }
  if (g_h.slot_policy == RECOMP_NETPLAY_SLOTS_SEAT) {
    for (i = 0; i < m; ++i) {
      out->port[sorted[i]] = sorted[i];
      out->occupied |= 1u << sorted[i];
      if (sorted[i] + 1 > out->slot_count)
        out->slot_count = sorted[i] + 1;
    }
    out->local_slot = my_seat;
  } else {
    /* Slot 0: the host, with its seat's port (none from the gallery). */
    out->port[0] = host_seat;
    out->slot_count = 1;
    if (i_am_host)
      out->local_slot = 0;
    for (i = 0; i < m && out->slot_count < cap; ++i) {
      int slot;
      if (sorted[i] == host_seat)
        continue;
      slot = out->slot_count++;
      out->port[slot] = sorted[i];
      if (!i_am_host && my_seat == sorted[i])
        out->local_slot = slot;
    }
    out->occupied = out->slot_count >= 32 ? 0xffffffffu
                                          : ((1u << out->slot_count) - 1u);
  }
  if (out->slot_count < 2)
    out->slot_count = 2;
}

static void launch_apply_plan(RecompLauncherCNetplayLaunch *out,
                              const SlotPlan *plan)
{
  int i;
  out->occupied_mask = plan->occupied;
  out->slot_port_valid = 1;
  for (i = 0; i < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1; ++i)
    out->slot_port[i] = plan->port[i];
}

static void arm_lan_launch(const RNetLanLobby *state)
{
  const char *colon;
  const char *port;
  const char *peer;
  int seats[2];
  SlotPlan plan;
  if (!state)
    return;
  /* Hand the UDP port to the game session socket. */
  if (g_hosting_lan)
    (void)rnet_lan_direct_host_notify_start(g_direct_host, state);
  close_direct_sockets();
  g_lan_start_seen = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_lan_launch.enabled = 1;
  /* Two seats, both occupied: the room does not start without the joiner. */
  seats[0] = state->host_slot;
  seats[1] = 1 - state->host_slot;
  plan_session_slots(seats, 2, state->host_slot,
                     g_hosting_lan ? state->host_slot : 1 - state->host_slot,
                     g_hosting_lan, &plan);
  g_lan_launch.local_slot = plan.local_slot;
  launch_apply_plan(&g_lan_launch, &plan);
  g_lan_launch.input_player = g_h.input_player;
  /* The id the host allocated for this START (cb_request_start); 1 from a
   * host that predates it, which is what such a host itself launches. */
  g_lan_launch.session_id = state->session_id ? state->session_id : 1u;
  g_lan_launch.input_delay =
      clamp_input_delay(state->input_delay >= 2 ? state->input_delay
                                                : g_lobby_input_delay);
  /* The LAN room carries a delay and nothing else -- no rollback flag and no
   * runway -- so neither is settled between the two peers. Both stay "unset"
   * (rollback 0, prediction 0) rather than each peer inventing its own: a
   * mode one side chose locally is a mode the other side never heard of.
   * The engine's own override (SNES_NET_MODE, say) still applies. */
  g_lan_launch.rollback = 0;
  g_lan_launch.input_prediction = 0;
  if (g_hosting_lan) {
    colon = strrchr(state->endpoint, ':');
    port = colon ? colon + 1 : "7777";
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:%s", port);
  } else {
    peer = g_direct_peer_endpoint[0] ? g_direct_peer_endpoint : state->endpoint;
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:0");
    snprintf(g_lan_launch.peer_hostport, sizeof(g_lan_launch.peer_hostport),
             "%s", peer);
  }
}

int recomp_netplay_host_init(const RecompNetplayHostHooks *hooks)
{
  RNetLobbyConfig cfg;
  if (!hooks || !hooks->game_name || !hooks->game_name[0])
    return -1;
  memset(&g_h, 0, sizeof(g_h));
  g_h = *hooks;
  g_mods = g_h.mods;
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  close_direct_sockets();
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_direct_peer_endpoint[0] = '\0';
  g_direct_password[0] = '\0';
  g_direct_bind[0] = '\0';
  g_direct_rejoin = 0;
  g_direct_reopen = 0;
  g_lan_session_last = 0;
  g_lobby_url[0] = '\0';
  g_resume_endpoint[0] = '\0';
  g_runtime_error[0] = '\0';
  g_external_ip[0] = '\0';
  g_local_address_count = 0;
  g_lobby_rollback = 1;
  g_lobby_input_prediction = 0;

  /* The lobby client learns what this build is before it can say anything:
   * title and pin for every op, the seat ceiling for create, the platform
   * for moderation metadata, the older environment spelling. */
  memset(&cfg, 0, sizeof(cfg));
  cfg.game_name = g_h.game_name;
  cfg.game_version = g_h.game_version;
  cfg.platform = g_h.platform;
  cfg.max_players = title_max_players();
  cfg.legacy_env_prefix = g_h.legacy_env_prefix;
  rnet_lobby_configure(&cfg);
  rnet_lobby_set_caps_codec(g_h.caps_codec);
  if (g_h.content_fingerprint && g_h.content_fingerprint[0])
    rnet_lobby_set_disc_fp(g_h.content_fingerprint);

  /* Installed before any join can be issued: a join that goes out without the
   * offer tells the server this peer owns no mods at all, and the server turns
   * it away from every lobby whose host enabled one. Installed for a vanilla
   * build too, where it truthfully offers nothing. */
  rnet_lobby_set_mod_offer_supplier(mod_offer_rows, NULL);
  if (g_mods && g_mods->export_package && g_mods->install_blob) {
    rnet_lobby_set_mod_transfer_hooks(g_mods->export_package,
                                      g_mods->free_blob, g_mods->install_blob,
                                      g_mods->ctx);
    fprintf(stderr, "netplay: mod transfer hooks installed (this build can "
                    "send and receive mods)\n");
  } else {
    rnet_lobby_set_mod_transfer_hooks(NULL, NULL, NULL, NULL);
    fprintf(stderr, "netplay: %s; this build cannot send or receive mods\n",
            g_mods ? "the mod runtime supplied no transfer hooks"
                   : "built without mod support");
  }
  if (title_max_players() > 2)
    fprintf(stderr, "netplay: %d-player rooms online; LAN / Direct IP rooms "
                    "are two seats in this build\n", title_max_players());
  g_inited = 1;
  return 0;
}

void recomp_netplay_host_shutdown(void)
{
  recomp_netplay_host_disconnect();
  rnet_lan_beacon_close(&g_beacon_listen);
  g_inited = 0;
}

/* The online launch this process last consumed (cb_fill_launch). */
static uint32_t g_consumed_launch_sid;
static int g_consumed_launch_valid;

/* Is the pending online launch the one the match that just ended ran? A
 * rematch's op:launch can arrive while a slow peer is still draining the
 * previous match (the host starts as soon as the room reads all-ready); that
 * launch belongs to the NEXT session and must survive the soft return.
 * Clearing it unconditionally left the slow peer in the room forever
 * (Genesis 4 players + 1 spectator, round 2: the last guest and the
 * spectator timed out waiting for a launch that had already come). */
static int launch_is_stale(int pending, uint32_t pending_sid,
                           int consumed_valid, uint32_t consumed_sid)
{
  if (!pending)
    return 1;
  if (!consumed_valid)
    return 1;          /* nothing consumed: keep the old behaviour */
  return pending_sid == consumed_sid;
}

void recomp_netplay_host_prepare_rematch(void)
{
  g_lan_start_seen = 0;
  if (g_hosting_lan || g_joined_lan) {
    g_lan_room.started = 0;
    (void)rnet_lan_lobby_set_started(lan_path(), 0);
  }
  /* The match closed the Direct IP sockets (arm_lan_launch); rebuild the
   * channel from both ends. See "a LAN / Direct IP room across a soft
   * return" at the top of this file. */
  if (g_hosting_lan && !g_direct_host && g_lan_room.endpoint[0]) {
    /* The closed socket was the only thing that knew the guest: free its
     * seat, so the host cannot start alone and the guest is not refused
     * "full" when it asks for the seat back. */
    g_lan_room.joiner_name[0] = '\0';
    g_lan_guest_rtt_ms = -1;
    g_direct_reopen = 1;
    g_direct_reopen_next_ms = 0;
    direct_reopen_step();
    if (g_direct_reopen)
      (void)publish_lan_room(); /* the freed seat, for the file browsers */
  }
  if (g_joined_direct && !g_direct_guest)
    direct_rejoin_begin();
  if (g_h.rematch_set_ready)
    rnet_lobby_set_ready(1);
  else
    rnet_lobby_set_ready(0);
  {
    const RNetLobbyJoinInfo *ji = rnet_lobby_join_info();
    uint32_t pending_sid = ji ? ji->session_id : 0u;
    if (launch_is_stale(rnet_lobby_launch_pending(), pending_sid,
                        g_consumed_launch_valid, g_consumed_launch_sid)) {
      rnet_lobby_clear_launch_pending();
    } else {
      fprintf(stderr, "netplay: soft return keeps the launch of session %u "
                      "(it arrived during session %u)\n",
              (unsigned)pending_sid, (unsigned)g_consumed_launch_sid);
    }
  }
  g_consumed_launch_valid = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

void recomp_netplay_host_begin_soft_return(RecompLauncherCGameInfo *gi,
                                           int set_resume_room)
{
  recomp_netplay_host_prepare_rematch();
  if (!gi || !set_resume_room)
    return;
  gi->resume_netplay_room = 1;
  {
    const char *ep = recomp_netplay_host_resume_endpoint();
    if (ep && ep[0])
      gi->resume_netplay_endpoint = ep;
  }
}

int recomp_netplay_host_leave(void)
{
  int rc;
  if (g_hosting_lan) {
    (void)rnet_lan_direct_host_notify_close(g_direct_host);
    close_direct_sockets();
    (void)rnet_lan_lobby_leave(lan_path(), 1);
  } else if (g_joined_direct) {
    if (g_direct_guest)
      (void)rnet_lan_direct_guest_leave(g_direct_guest);
    close_direct_sockets();
  } else if (g_joined_lan) {
    (void)rnet_lan_lobby_leave(lan_path(), 0);
  }
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_rejoin = 0;
  g_direct_reopen = 0;
  g_direct_peer_endpoint[0] = '\0';
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  rc = rnet_lobby_leave();
  return rc;
}

void recomp_netplay_host_disconnect(void)
{
  (void)recomp_netplay_host_leave();
  rnet_lobby_disconnect();
}

const char *recomp_netplay_host_resume_endpoint(void)
{
  if (g_resume_endpoint[0])
    return g_resume_endpoint;
  return "";
}

int recomp_netplay_host_in_lan(void)
{
  return g_hosting_lan || g_joined_lan;
}

void recomp_netplay_host_set_runtime_error(const char *error_code)
{
  snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
           error_code ? error_code : "");
}

static const char *cb_default_url(void *ctx)
{
  (void)ctx;
  return g_lobby_url[0] ? g_lobby_url : rnet_lobby_default_url();
}

static void cb_set_url(void *ctx, const char *url)
{
  (void)ctx;
  snprintf(g_lobby_url, sizeof(g_lobby_url), "%s",
           url && url[0] ? url : rnet_lobby_default_url());
}

static int cb_connect(void *ctx)
{
  int rc;
  (void)ctx;
  rnet_lobby_set_game_identity(game_name(), game_version());
  rc = rnet_lobby_connect(cb_default_url(NULL));
  /* connect() resets the client; the reset keeps the identity, and this
   * re-apply is the second lock on the same door -- a lobby created or a
   * chat sent with an empty title is invisible to everyone else. */
  rnet_lobby_set_game_identity(game_name(), game_version());
  return rc;
}

static int cb_connected(void *ctx)
{
  (void)ctx;
  return rnet_lobby_connected();
}

static void cb_pump(void *ctx)
{
  (void)ctx;
  /* Point the account client at the lobby host, and pump it.
   *
   * rnet_auth.c derives its HTTP host from the ws:// URL handed to
   * rnet_account_init, and its own comment says "Init runs from the netplay
   * pump" -- but nothing called it. The linker then dead-stripped
   * rnet_account_init out of the binary entirely, so g.host stayed the
   * zero-initialised empty string and every "/auth" POST died in
   * getaddrinfo("", "0"). That surfaces as "couldn't reach the lobby server
   * to sign in" no matter which host is configured, which is exactly the
   * wrong place to go looking.
   *
   * Re-init only when the URL actually changes rather than every pump: the
   * worker thread reads g.host while a login is in flight, and memset-ing it
   * under that read 60 times a second would be a data race for no gain. */
  {
    static char auth_url[256];
    const char *url = cb_default_url(NULL);
    if (url && url[0] && strcmp(url, auth_url) != 0) {
      /* Anchor the secret to the EXECUTABLE directory before the first init.
       * Its default is the bare relative name "netplay_secret", resolved
       * against the working directory -- so the same install signed itself
       * out depending on where it was launched from, and a rebuild run from a
       * different directory read as a lost login. rnet_auth.c migrates an old
       * CWD-relative file into this path on first load, so nobody is signed
       * out by the move. */
      char secret_path[512];
      if (g_h.exe_dir_path &&
          g_h.exe_dir_path(g_h.ctx, "netplay_secret", secret_path,
                           sizeof(secret_path)))
        rnet_account_set_secret_path(secret_path);
      snprintf(auth_url, sizeof(auth_url), "%s", url);
      rnet_account_init(url);
    }
  }
  rnet_account_pump();

  /* Publish the account name to the lobby.
   *
   * The lobby's display name is what seats and the players-online list show,
   * and it defaults to the literal "Host" when empty (rnet_lobby_client.c
   * create path). Nothing pushed the account handle into it: signing in --
   * including the automatic sign-in from a stored secret -- only updated the
   * ACCOUNT, so a signed-in player created a lobby and appeared as "Host".
   * Only an explicit rename through the name modal ever set it.
   *
   * Done here rather than at a sign-in edge because there is no single such
   * edge: interactive login, stored-secret redemption and a server-side
   * handle change all land asynchronously in the pump. Comparing against the
   * live name makes this idempotent -- set_display_name only re-sends hello
   * when the value actually changed. */
  if (rnet_account_state() == RNET_ACCOUNT_SIGNED_IN) {
    const char *handle = rnet_account_handle();
    const char *shown = rnet_lobby_display_name();
    if (handle && handle[0] && (!shown || strcmp(shown, handle) != 0))
      rnet_lobby_set_display_name(handle);
  }

  rnet_lobby_pump();
  dl_queue_step();
  host_caps_watch_step();
  mod_set_sync_step();
  desync_report_step();
  lan_chat_drain();
  beacon_listen_step();
  beacon_publish_step();
  if (g_hosting_lan && g_direct_host) {
    int rtt = -1;
    if (rnet_lan_direct_host_pump(g_direct_host, &g_lan_room, &rtt))
      (void)publish_lan_room();
    if (rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    /* Host probes guest RTT about once per second. */
    {
      /* rnet_os_monotonic_ms is in the static lib; use a coarse clock here. */
      static unsigned s_tick;
      s_tick++;
      if ((s_tick % 60) == 0) /* ~1s at 60fps pump */
        (void)rnet_lan_direct_host_ping(g_direct_host);
    }
  }
  if (g_joined_direct && g_direct_guest && !g_direct_rejoin) {
    static unsigned s_gtick;
    s_gtick++;
    if ((s_gtick % 60) == 0)
      (void)rnet_lan_direct_guest_ping(g_direct_guest);
  }
  sync_lan_joiner();
  if (g_h.auto_ready_guests && rnet_lobby_in_lobby() &&
      !rnet_lobby_is_host() && !rnet_lobby_local_ready())
    (void)rnet_lobby_set_ready(1);
}

static void cb_set_player_name(void *ctx, const char *name)
{
  (void)ctx;
  rnet_lobby_set_display_name(name && name[0] ? name : "Player");
  /* Persist immediately: the launcher may never reach PLAY (the player can
   * set a name, browse the lobby and quit), and a name that only survives a
   * successful launch still prompts on the next run. */
  if (name && name[0] && g_h.name_store)
    (void)g_h.name_store(g_h.ctx, name);
}

static const char *cb_player_name(void *ctx)
{
  static char persisted[64];
  (void)ctx;
  {
    const char *live = rnet_lobby_display_name();
    if (live && live[0])
      return live;
  }
  /* Nothing set this session: hand back what the last run stored, so the
   * launcher opens with the name already filled in. */
  if (g_h.name_load && g_h.name_load(g_h.ctx, persisted, sizeof(persisted)) &&
      persisted[0])
    return persisted;
  return rnet_lobby_display_name();
}

static void cb_request_list(void *ctx)
{
  (void)ctx;
  rnet_lobby_request_list();
}

/* The list is: hub rows, then the same-machine registry row (if any), then
 * the rooms heard on the LAN beacon. The registry row's endpoint is passed to
 * the beacon filter so a host on THIS machine is listed once, not twice. */
/* Which sources the browser is currently asking for. The UI sets it from the
 * fork the player took; 0 keeps the old merge-everything behaviour for a
 * launcher that never calls the setter. */
static int g_list_scope;

#if defined(RECOMP_LAUNCHER_HAS_SET_BLOCKS)
static int cb_set_blocks(void *ctx, const char *accounts)
{
  (void)ctx;
  return rnet_lobby_set_blocks(accounts);
}
#endif

#if defined(RECOMP_LAUNCHER_HAS_CHAT_REPORT)
static int cb_chat_report(void *ctx, const char *const *mids, int mid_count,
                          const char *reason, const char *note)
{
  (void)ctx;
  /* Thin on purpose: the frame is built once in recomp-net so the rule about
   * what may be reported does not end up written five times, and this layer's
   * whole job is to pass it on. */
  return rnet_lobby_report_chat(mids, mid_count, reason, note);
}
#endif

#if defined(RECOMP_LAUNCHER_HAS_LIST_SCOPE)
static int cb_list_scope_set(void *ctx, int scope)
{
  (void)ctx;
  g_list_scope = scope;
  return 0;
}
#endif

static int list_want_online(void)
{
  return g_list_scope != RECOMP_LAUNCHER_LIST_SCOPE_LAN;
}

static int list_want_lan(void)
{
  return g_list_scope != RECOMP_LAUNCHER_LIST_SCOPE_ONLINE;
}

static int cb_list_count(void *ctx)
{
  RecompLauncherCNetplayLobby lan;
  int have_lan;
  (void)ctx;
  have_lan = list_want_lan() && fill_lan_row(&lan);
  /* RECOMP_NETPLAY_LIST_DEBUG=1 (snesrecomp's SNESRECOMP_LOBBY_LIST_DEBUG is
   * read too): say where the browser's rows come from,
   * once per change. "The list is empty" has four possible causes -- wrong
   * scope, nothing on the server, no registry row, no beacons heard -- and
   * they are indistinguishable from the screen. Off by default; this is a
   * line to ask a player for, not one to print at everybody. */
  {
    static int enabled = -1;
    if (enabled < 0) {
      const char *e = getenv("RECOMP_NETPLAY_LIST_DEBUG");
      if (!e || !e[0])
        e = getenv("SNESRECOMP_LOBBY_LIST_DEBUG");
      enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (enabled) {
      static int last_scope = -1, last_remote = -1, last_lan = -1, last_beacon = -1;
      int remote = rnet_lobby_list_count();
      int beacons = beacon_row_count(have_lan ? lan.lobby_id + 4 : "");
      if (last_scope != g_list_scope || last_remote != remote ||
          last_lan != have_lan || last_beacon != beacons) {
        last_scope = g_list_scope; last_remote = remote;
        last_lan = have_lan; last_beacon = beacons;
        fprintf(stderr,
                "[lobby-list] scope=%s server_rows=%d local_registry=%d "
                "lan_beacons=%d version=\"%s\" strict=%d\n",
                g_list_scope == 1 ? "LAN" : g_list_scope == 2 ? "ONLINE" : "ANY",
                remote, have_lan, beacons, game_version(),
                rnet_lobby_version_filter_strict());
      }
    }
  }
  return (list_want_online() ? rnet_lobby_list_count() : 0) +
         (have_lan ? 1 : 0) +
         (list_want_lan()
              ? beacon_row_count(have_lan ? lan.lobby_id + 4 : "")
              : 0);
}

static int cb_list_get(void *ctx, int index, RecompLauncherCNetplayLobby *out)
{
  RNetLobbyRow row;
  int remote_count;
  (void)ctx;
  if (!out || index < 0)
    return 0;
  remote_count = list_want_online() ? rnet_lobby_list_count() : 0;
  if (index >= remote_count) {
    RecompLauncherCNetplayLobby lan;
    int have_lan = list_want_lan() && fill_lan_row(&lan);
    if (!list_want_lan())
      return 0;
    index -= remote_count;
    if (have_lan) {
      if (index == 0) {
        *out = lan;
        return 1;
      }
      --index;
    }
    return fill_beacon_row(index, have_lan ? lan.lobby_id + 4 : "", out);
  }
  if (!rnet_lobby_list_get(index, &row))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->lobby_id, sizeof(out->lobby_id), "%s", row.lobby_id);
  snprintf(out->name, sizeof(out->name), "%s", row.name);
  snprintf(out->game_name, sizeof(out->game_name), "%s", row.game_name);
  snprintf(out->game_version, sizeof(out->game_version), "%s",
           row.game_version);
  out->player_count = row.player_count;
  out->max_slots = row.max_slots;
  out->has_password = row.has_password;
  snprintf(out->host_country, sizeof(out->host_country), "%s", row.host_country);
  out->allow_spectators = row.allow_spectators;
  out->max_spectators = row.max_spectators;
  out->spectator_count = row.spectator_count;
  return 1;
}

/* Players online: the hub's `players` list. The LAN room has no hub. */
static int cb_online_count(void *ctx)
{
  (void)ctx;
  return rnet_lobby_connected() ? rnet_lobby_online_count() : 0;
}

static int cb_online_get(void *ctx, int index, RecompLauncherCNetplayOnlinePlayer *out)
{
  RNetLobbyOnlinePlayer p;
  const char *me;
  (void)ctx;
  if (!out || !rnet_lobby_online_get(index, &p))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->display_name, sizeof(out->display_name), "%s", p.display_name);
#if defined(RECOMP_LAUNCHER_HAS_PLAYER_ACCOUNT)
  /* The key a local ignore/block list uses. Empty for a guest. */
  snprintf(out->account, sizeof(out->account), "%s", p.account);
#endif
  snprintf(out->country, sizeof(out->country), "%s", p.country);
  snprintf(out->lobby_name, sizeof(out->lobby_name), "%s", p.lobby_name);
  out->in_lobby = p.lobby_id[0] != '\0';
  out->hosting = p.hosting;
  /* Which row is us: the hub tags each row with the first characters of
   * its connection id. A name match would mark every namesake. */
  me = rnet_lobby_player_id();
  out->is_local = me && me[0] && p.tag[0] && strncmp(me, p.tag, strlen(p.tag)) == 0;
  return 1;
}

static int refresh_local_addresses(void)
{
  int count = rnet_ipv4_enumerate(g_local_addresses, kMaxLocalAddresses);
  if (count < 0)
    count = 0;
  if (count > kMaxLocalAddresses)
    count = kMaxLocalAddresses;
  g_local_address_count = count;
  return count;
}

static int cb_local_address_get(void *ctx, int index,
                                RecompLauncherCNetplayLocalAddress *out)
{
  (void)ctx;
  if (!out || index < 0)
    return 0;
  if (index == 0)
    refresh_local_addresses();
  if (index >= g_local_address_count)
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->address, sizeof(out->address), "%s",
           g_local_addresses[index].address);
  snprintf(out->label, sizeof(out->label), "%s",
           g_local_addresses[index].interface_label);
  return 1;
}

static int cb_local_ip(void *ctx, char *out, size_t out_len)
{
  RecompLauncherCNetplayLocalAddress address;
  if (!out || !out_len || !cb_local_address_get(ctx, 0, &address))
    return 0;
  snprintf(out, out_len, "%s", address.address);
  return out[0] != '\0';
}

static int cb_external_ip(void *ctx, char *out, size_t out_len)
{
  RNetExternalIpv4Config config;
  int rc;
  (void)ctx;
  if (!out || !out_len)
    return 0;
  if (!g_external_ip[0]) {
    rnet_external_ipv4_config_init(&config);
    config.timeout_ms = 900;
    rc = rnet_external_ipv4_discover(&config, g_external_ip,
                                     sizeof(g_external_ip));
    if (rc != RNET_EXTERNAL_IPV4_OK) {
      snprintf(out, out_len, "Unavailable");
      return 0;
    }
  }
  snprintf(out, out_len, "%s", g_external_ip);
  return out[0] != '\0';
}

static int cb_create(void *ctx, const char *lobby_name, char *host_endpoint,
                     const char *password,
                     const RecompLauncherCSettings *settings, int lan_only,
                     int max_slots)
{
  RNetLobbyMatchCaps caps = default_caps(settings);
  (void)ctx;
  g_lobby_max_slots = clamp_lobby_max_slots(max_slots);
  if (!host_endpoint)
    return -1;
  if (!host_endpoint[0])
    snprintf(host_endpoint, 64, lan_only ? "127.0.0.1:7777" : "0.0.0.0:7777");
  if (lan_only) {
    /* recomp-net's LAN room (lan_lobby / lan_direct / lan_beacon) seats ONE
     * joiner. A title with more seats still gets a working two-seat room --
     * and is told so, loudly, rather than shown a bigger room that silently
     * never fills past two. Hosting online is the way to more seats. */
    if (max_slots > 2) {
      fprintf(stderr,
              "netplay: LAN / Direct IP rooms are TWO seats in this build "
              "(recomp-net lan_lobby / lan_direct carry one joiner); asked "
              "for %d, creating a two-seat room. Host online for more "
              "players.\n", max_slots);
      snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
               "lan_two_seats_only");
    }
    g_lobby_max_slots = 2;
    if (!create_lan(lobby_name, host_endpoint, password))
      return -1;
    return 0;
  }
  close_direct_sockets();
  (void)rnet_lan_lobby_leave(lan_path(), 1);
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_rejoin = 0;
  g_direct_reopen = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  return rnet_lobby_create(
      lobby_name && lobby_name[0]
          ? lobby_name
          : (g_h.default_lobby_name ? g_h.default_lobby_name : "Netplay Lobby"),
      game_name(), game_version(), password ? password : "", host_endpoint,
      &caps, g_lobby_max_slots);
}

static int cb_join(void *ctx, const char *lobby_id, const char *password,
                   char *guest_bind)
{
  RNetLanLobby state;
  const char *name;
  const char *endpoint;
  int rc;
  (void)ctx;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  if (lobby_id && strncmp(lobby_id, "lan:", 4) == 0) {
    name = rnet_lobby_display_name();
    endpoint = lobby_id + 4;
    close_direct_sockets();
    g_hosting_lan = 0;
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_rejoin = 0;
    g_direct_reopen = 0;
    g_direct_peer_endpoint[0] = '\0';
    /* Kept for the rematch re-join (direct_rejoin_begin): the match closes
     * this socket, and the guest asks for its seat back the same way. */
    snprintf(g_direct_password, sizeof(g_direct_password), "%s",
             password ? password : "");
    snprintf(g_direct_bind, sizeof(g_direct_bind), "%s",
             guest_bind ? guest_bind : "");

    /* UDP JOIN_REQ to the host's socket first -- on the same machine too.
     * The host's seat table, chat and seat swaps all live on that socket;
     * a joiner that only wrote itself into the registry file is invisible
     * to the host and can neither talk nor trade seats. The endpoint comes
     * from the lobby id (the registry's advertised address, or what the
     * player typed for Join Direct). */
    rc = rnet_lan_direct_guest_join(
        endpoint, game_name(), game_version(), password ? password : "",
        name && name[0] ? name : "Player", guest_bind, 2500, &state,
        &g_direct_guest);
    if (rc == RNET_LAN_DIRECT_OK) {
      g_lan_room = state;
      g_joined_lan = 1;
      g_joined_direct = 1;
      snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
               endpoint);
      snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s", endpoint);
      return 0;
    }
    if (rc == RNET_LAN_DIRECT_ERR_PASSWORD)
      return -2;

    /* Registry-file fallback: a host without a direct socket (older build).
     * Seated, but with no channel to the host beyond the file. */
    rc = rnet_lan_lobby_join(lan_path(), game_name(), game_version(),
                             password ? password : "",
                             name && name[0] ? name : "Player", &state);
    if (rc == RNET_LAN_LOBBY_OK) {
      g_lan_room = state;
      g_joined_lan = 1;
      snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
               state.endpoint);
      snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
               endpoint[0] ? endpoint : state.endpoint);
      return 0;
    }
    if (rc == RNET_LAN_LOBBY_ERR_PASSWORD)
      return -2;
    if (rc == RNET_LAN_LOBBY_ERR_IO)
      return -3;
    return -1;
  }
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  return rnet_lobby_join(lobby_id, password ? password : "", guest_bind);
}

static int cb_leave(void *ctx)
{
  (void)ctx;
  return recomp_netplay_host_leave();
}

static int cb_in_lobby(void *ctx)
{
  (void)ctx;
  sync_lan_joiner();
  return g_hosting_lan || g_joined_lan || rnet_lobby_in_lobby();
}

static int cb_is_host(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return g_hosting_lan ? 1 : 0;
  return rnet_lobby_is_host();
}

static int cb_member_count(void *ctx)
{
  RNetLanLobby state;
  (void)ctx;
  return use_lan_members(&state) ? 2 : rnet_lobby_member_count();
}

static int cb_member_get(void *ctx, int index,
                         RecompLauncherCNetplayMember *out)
{
  RNetLobbyMember member;
  RNetLanLobby state;
  (void)ctx;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->latency_ms = -1;
  if (use_lan_members(&state)) {
    if (index < 0 || index > 1)
      return 0;
    out->slot = index == 0 ? state.host_slot : 1 - state.host_slot;
    out->ready = index == 0 || state.joiner_name[0] != '\0';
    out->is_host = index == 0;
    /* The launcher's self-service (drag your own row) keys on this. */
    out->is_local = index == 0 ? (g_hosting_lan ? 1 : 0) : (g_joined_lan ? 1 : 0);
    snprintf(out->display_name, sizeof(out->display_name), "%s",
             index == 0 ? state.host_name : state.joiner_name);
    /* Host row: N/A. Guest row: Direct-IP / LAN UDP RTT when known. */
    if (!out->is_host && out->display_name[0] && g_lan_guest_rtt_ms >= 0)
      out->latency_ms = g_lan_guest_rtt_ms;
    return 1;
  }
  if (!rnet_lobby_member_get(index, &member))
    return 0;
  out->slot = member.slot;
  out->ready = member.ready;
  out->is_spectator = member.is_spectator;
  out->is_host = rnet_lobby_member_is_host(&member);
  snprintf(out->country, sizeof(out->country), "%s", member.country);
  {
    const char *me = rnet_lobby_player_id();
    out->is_local = (me && me[0] && member.player_id[0] &&
                     strcmp(me, member.player_id) == 0) ? 1 : 0;
  }
  snprintf(out->display_name, sizeof(out->display_name), "%s",
           member.display_name);
  out->latency_ms = rnet_lobby_member_latency_ms(member.slot);
  return 1;
}

/* ---- lobby chat ---------------------------------------------------------
 *
 * Two transports, one discipline. Online the lobby server echoes every line
 * back and that echo is the copy we keep. On LAN the HOST plays the server's
 * part: a guest sends its line to the host, keeps nothing locally, and the
 * host stamps the seat name on it and echoes it back. Either way the UI never
 * appends its own send, so both peers hold the same lines in the same order.
 *
 * Direct-IP only on the LAN side. The same-machine file registry has no
 * channel between the two instances to carry a line over, so a room seated
 * through it reports no chat rather than a box that swallows what you type. */
#define LAN_CHAT_RING 64
static RNetLobbyChatMsg g_lan_chat[LAN_CHAT_RING];
static int g_lan_chat_head;
static int g_lan_chat_count;
static uint32_t g_lan_chat_seq;

static void lan_chat_clear(void)
{
  g_lan_chat_head = 0;
  g_lan_chat_count = 0;
  /* seq keeps counting across rooms -- see rnet_lobby_chat_clear. */
}

static void lan_chat_push(const char *player_id, const char *from,
                          const char *text)
{
  RNetLobbyChatMsg *m;
  int idx;
  if (!text || !text[0])
    return;
  if (g_lan_chat_count < LAN_CHAT_RING) {
    idx = (g_lan_chat_head + g_lan_chat_count) % LAN_CHAT_RING;
    g_lan_chat_count++;
  } else {
    idx = g_lan_chat_head;
    g_lan_chat_head = (g_lan_chat_head + 1) % LAN_CHAT_RING;
  }
  m = &g_lan_chat[idx];
  memset(m, 0, sizeof(*m));
  snprintf(m->player_id, sizeof(m->player_id), "%s", player_id ? player_id : "");
  snprintf(m->from, sizeof(m->from), "%s", from ? from : "");
  snprintf(m->text, sizeof(m->text), "%s", text);
  /* A LAN room has no server to mask for it: every peer masks the line as
   * it lands in the ring, the host included. */
  (void)rnet_chat_filter_apply(m->text, sizeof(m->text));
  /* On LAN a player id is whatever each side calls itself, so "mine" is
   * decided by the display name the host stamped -- the only identity both
   * sides agree on here. */
  {
    const char *me = rnet_lobby_display_name();
    m->is_local = (me && me[0] && m->from[0] && strcmp(m->from, me) == 0) ? 1 : 0;
  }
  m->seq = ++g_lan_chat_seq;
}

/* Drain whatever the direct-IP link delivered since the last pump. */
static void lan_chat_drain(void)
{
  RNetLanChatLine line;
  if (g_hosting_lan && g_direct_host) {
    while (rnet_lan_direct_host_take_chat(g_direct_host, &line))
      lan_chat_push(line.player_id, line.from, line.text);
    if (rnet_lan_direct_host_take_swap_request(g_direct_host))
      g_lan_swap_incoming = 1;
  } else if (g_joined_lan && g_joined_direct && g_direct_guest) {
    int accept = 0;
    while (rnet_lan_direct_guest_take_chat(g_direct_guest, &line))
      lan_chat_push(line.player_id, line.from, line.text);
    if (rnet_lan_direct_guest_take_swap_result(g_direct_guest, &accept))
      g_lan_swap_outgoing = accept ? 2 : -1;
  }
}

/* Host: trade the two seats and tell the room. */
static int lan_swap_seats(const char *why)
{
  if (!g_hosting_lan) return -1;
  fprintf(stderr, "netplay: LAN seat swap (%s): host_slot %d -> %d\n", why,
          g_lan_room.host_slot, 1 - g_lan_room.host_slot);
  g_lan_room.host_slot = 1 - g_lan_room.host_slot;
  g_lan_room.started = 0;
  return publish_lan_room() ? 0 : -1;
}

/* 1 when this LAN room can actually carry a line between the two peers. */
static int lan_chat_available(void)
{
  if (g_hosting_lan)
    return g_direct_host != NULL;
  if (g_joined_lan)
    return g_joined_direct && g_direct_guest != NULL && !g_direct_rejoin;
  return 0;
}

static int cb_chat_send(void *ctx, const char *text)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan) {
    if (!lan_chat_available())
      return -1;
    if (g_hosting_lan) {
      /* The host keeps its own line and sends it on -- it is the authority
       * here, exactly as the lobby server is online. */
      const char *me = rnet_lobby_display_name();
      return rnet_lan_direct_host_send_chat(g_direct_host, me ? me : "",
                                            me ? me : "Host",
                                            text) == RNET_LAN_DIRECT_OK
                 ? 0
                 : -1;
    }
    /* Guest: send and keep nothing. The host's echo is the copy we keep, so
     * the two logs cannot disagree about order. */
    {
      const char *me = rnet_lobby_display_name();
      return rnet_lan_direct_guest_send_chat(g_direct_guest, me ? me : "",
                                             text) == RNET_LAN_DIRECT_OK
                 ? 0
                 : -1;
    }
  }
  return rnet_lobby_send_chat(text);
}

static int cb_chat_count(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return lan_chat_available() ? g_lan_chat_count : 0;
  return rnet_lobby_chat_count();
}

static int cb_chat_get(void *ctx, int index,
                       RecompLauncherCNetplayChatMessage *out)
{
  RNetLobbyChatMsg msg;
  (void)ctx;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (g_hosting_lan || g_joined_lan) {
    if (!lan_chat_available() || index < 0 || index >= g_lan_chat_count)
      return 0;
    msg = g_lan_chat[(g_lan_chat_head + index) % LAN_CHAT_RING];
  } else if (!rnet_lobby_chat_get(index, &msg)) {
    return 0;
  }
  snprintf(out->from, sizeof(out->from), "%s", msg.from);
#if defined(RECOMP_LAUNCHER_HAS_PLAYER_ACCOUNT)
  snprintf(out->account, sizeof(out->account), "%s", msg.account);
#endif
#if defined(RECOMP_LAUNCHER_HAS_CHAT_REPORT)
  /* Empty against a server that predates message ids, which the UI reads as
   * "this line cannot be reported" rather than offering an action that would
   * be refused. */
  snprintf(out->mid, sizeof(out->mid), "%s", msg.mid);
#endif
  snprintf(out->text, sizeof(out->text), "%s", msg.text);
  out->is_local = msg.is_local;
  out->is_system = msg.is_system;
  out->seq = msg.seq;
  return 1;
}

/* Server chat: per-game, online only. A LAN room has no server and no wider
 * audience, so the panel is hidden there (send refuses, count 0). */
/* ---- optional Discord sign-in ------------------------------------------
 * Thin adapters over recomp-net's rnet_account (auth.h), which owns the HTTP, the worker
 * thread and the device key. */
static int cb_account_available(void *ctx) { (void)ctx; return rnet_account_available(); }
static int cb_account_login_begin(void *ctx) { (void)ctx; return rnet_account_login_begin(); }
static int cb_account_state(void *ctx) { (void)ctx; return rnet_account_state(); }
static const char *cb_account_handle(void *ctx) { (void)ctx; return rnet_account_handle(); }
static const char *cb_account_username(void *ctx) { (void)ctx; return rnet_account_username(); }
static const char *cb_account_error(void *ctx) { (void)ctx; return rnet_account_error(); }
static int cb_account_sign_out(void *ctx) { (void)ctx; return rnet_account_sign_out(); }
/* ── automatch ──────────────────────────────────────────────────────────────
 *
 * Thin: the lobby client owns the protocol and the state machine, and this
 * layer only translates its vocabulary into the launcher's. The one piece of
 * POLICY here is mods_enabled -- see cb_automatch_queue.
 */
#if defined(RECOMP_LAUNCHER_HAS_AUTOMATCH)
static int cb_automatch_available(void *ctx)
{
    (void)ctx;
    /* Ask once the answer could exist. The launcher polls this every frame
     * while the netplay page is up, which is exactly when a reply is useful,
     * and the client refuses to re-send while one is outstanding. */
    if (rnet_lobby_connected() && !rnet_lobby_automatch_available())
        rnet_lobby_automatch_request_rulesets();
    return rnet_lobby_automatch_available();
}

static int cb_automatch_ruleset_count(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_ruleset_count();
}

static int cb_automatch_ruleset_get(void *ctx, int index,
                                    RecompLauncherCNetplayRuleset *out)
{
    RNetLobbyRuleset r;
    (void)ctx;
    if (!out || !rnet_lobby_automatch_ruleset_get(index, &r)) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", r.id);
    snprintf(out->label, sizeof(out->label), "%s", r.label);
    snprintf(out->caps_summary, sizeof(out->caps_summary), "%s", r.caps_summary);
    snprintf(out->game_version, sizeof(out->game_version), "%s", r.game_version);
    /* The server pools two-seat matches only (slots_not_pooled otherwise),
     * and the client asks for exactly that. */
    out->max_slots = 2;
    return 1;
}

static int cb_automatch_queue(void *ctx, const char *ruleset_id)
{
    (void)ctx;
    /*
     * mods_enabled asserts that a SIM-AFFECTING mod feature is on locally
     * BEYOND whatever the chosen ruleset itself imposes.
     *
     * The distinction matters and is not pedantry. A ruleset that pins the
     * widescreen margin has both peers running the same patched sim by the
     * server's own instruction -- that is the ruleset, not a divergence. A
     * feature the ruleset says nothing about is a divergence, and it is the
     * desync §5 is about.
     *
     * The server cannot check either one. It takes this client's word, and
     * the assertion exists so that a modified client is making a deliberate
     * false statement rather than exploiting an omission. Which is why the
     * answer is computed by the GAME (RecompNetplayHostHooks.mods_enabled) rather
     * than assumed here: only the game knows what its own features do.
     */
    char why[160];
    int mods;
    why[0] = '\0';

    /*
     * The SERVER's grant, not ours, and applied before the game is asked
     * anything -- the whole assertion below is computed through it.
     *
     * An automatch room is the server's room. Whatever this build would allow
     * as a host is irrelevant here: a player queueing for a public pool does
     * not get to decide which of their own mods are harmless, because that is
     * precisely the decision a cheat would make in its own favour. A ruleset
     * with no list grants nothing, so an unapproved cosmetic claim counts as
     * simulation-affecting and the queue is refused.
     */
    {
        RNetLobbyRuleset r;
        char allow[512];
        int i, n = rnet_lobby_automatch_ruleset_count();
        allow[0] = '\0';
        for (i = 0; i < n; ++i) {
            if (!rnet_lobby_automatch_ruleset_get(i, &r)) continue;
            /* NULL/"" selects the first, matching the client's own rule. */
            if (ruleset_id && ruleset_id[0] && strcmp(r.id, ruleset_id) != 0)
                continue;
            snprintf(allow, sizeof(allow), "%s", r.caps.mod_cosmetic_allow);
            break;
        }
        mods_set_cosmetic_allow(allow);
    }

    /*
     * Refuse an ungranted cosmetic claim here, by name, before the game's own
     * assertion runs.
     *
     * The game's answer is derived from the effective set, which now excludes
     * anything the server DID grant -- so without this check a mod the server
     * never approved would simply be absent from the game's view and pass
     * unnoticed. It is checked here rather than in the game because it is the
     * framework's rule, not a per-title one, and a title that forgot to
     * implement it would be the hole.
     */
    if (g_mods && g_mods->unapproved_cosmetics) {
        char bad[512];
        bad[0] = '\0';
        if (g_mods->unapproved_cosmetics(g_mods->ctx, bad,
                                         (uint32_t)sizeof(bad)) > 0 &&
            bad[0]) {
            char *nl = strchr(bad, '\n');
            if (nl) *nl = '\0';
            snprintf(why, sizeof(why),
                     "%s is not on this queue's approved list", bad);
            rnet_lobby_automatch_refuse_local(why);
            return -1;
        }
    }

    mods = g_h.mods_enabled
               ? g_h.mods_enabled(g_h.ctx, ruleset_id, why, sizeof(why))
               : 0;
    if (mods) {
        /* Refuse here rather than sending a ticket the server will certainly
         * bounce: mods_not_pooled comes back as one generic line, and this
         * side knows exactly which feature caused it. */
        rnet_lobby_automatch_refuse_local(
            why[0] ? why : "Turn off sim-affecting mods to queue");
        return -1;
    }
    /* Declare the exemptions we are relying on, so the SERVER checks them
     * against its own allowlist rather than taking the verdict above on
     * trust. The two gates are layers: an old server ignores this field and
     * the assertion still holds, a new one stops needing to believe us. */
    {
        char exempt[768];
        exempt[0] = '\0';
        if (g_mods && g_mods->exempted_packages &&
            g_mods->exempted_packages(g_mods->ctx, exempt,
                                      (uint32_t)sizeof(exempt)) >=
                (int)sizeof(exempt)) {
            /* Never queue having declared less than we rely on. */
            rnet_lobby_automatch_refuse_local(
                "too many mod exemptions to declare to the server");
            return -1;
        }
        return rnet_lobby_automatch_queue(ruleset_id, 0, exempt) == 0 ? 0 : -1;
    }
}

static int cb_automatch_cancel(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_cancel();
}

static int cb_automatch_state(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_state();
}

static int cb_automatch_queued_secs(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_queued_secs();
}

static int cb_automatch_pool(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_pool();
}

static int cb_automatch_found_get(void *ctx, RecompLauncherCNetplayFound *out)
{
    RNetLobbyAutomatchFound f;
    (void)ctx;
    if (!out || !rnet_lobby_automatch_found_get(&f)) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->handle, sizeof(out->handle), "%s", f.opponent);
    snprintf(out->country, sizeof(out->country), "%s", f.opponent_country);
    snprintf(out->ruleset_label, sizeof(out->ruleset_label), "%s",
             f.ruleset_label);
    /* <0 rather than 0 when the server offered none: zero is a legitimate
     * estimate on a LAN and must not read as "unknown". */
    out->est_rtt_ms = f.est_rtt_ms > 0 ? f.est_rtt_ms : -1;
    out->accept_secs_left = f.accept_secs;
    return 1;
}

static int cb_automatch_accept(void *ctx, int accept)
{
    (void)ctx;
    return rnet_lobby_automatch_accept(accept);
}

static const char *cb_automatch_error(void *ctx)
{
    (void)ctx;
    return rnet_lobby_automatch_error();
}
#endif /* RECOMP_LAUNCHER_HAS_AUTOMATCH */

static int cb_account_set_handle(void *ctx, const char *h) {
    (void)ctx;
    return rnet_account_set_handle(h);
}

static int cb_server_chat_send(void *ctx, const char *text)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan || !rnet_lobby_connected())
    return -1;
  return rnet_lobby_send_server_chat(text);
}

static int cb_server_chat_count(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_server_chat_count();
}

static int cb_server_chat_get(void *ctx, int index,
                              RecompLauncherCNetplayChatMessage *out)
{
  RNetLobbyChatMsg msg;
  (void)ctx;
  if (!out || !rnet_lobby_server_chat_get(index, &msg))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->from, sizeof(out->from), "%s", msg.from);
#if defined(RECOMP_LAUNCHER_HAS_PLAYER_ACCOUNT)
  snprintf(out->account, sizeof(out->account), "%s", msg.account);
#endif
#if defined(RECOMP_LAUNCHER_HAS_CHAT_REPORT)
  /* Empty against a server that predates message ids, which the UI reads as
   * "this line cannot be reported" rather than offering an action that would
   * be refused. */
  snprintf(out->mid, sizeof(out->mid), "%s", msg.mid);
#endif
  snprintf(out->text, sizeof(out->text), "%s", msg.text);
  out->is_local = msg.is_local;
  out->is_system = msg.is_system;
  out->seq = msg.seq;
  return 1;
}

/* ---- spectators ---------------------------------------------------------
 * The gallery exists only on the lobby server. A LAN / direct-IP room has no
 * server to enforce "cannot affect the game" at, so it reports no gallery and
 * the UI's spectator section stays hidden there. */

static int cb_allow_spectators_get(void *ctx)
{
  (void)ctx;
  return rnet_lobby_allow_spectators_pref();
}

static int cb_allow_spectators_set(void *ctx, int allow)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return -1;
  rnet_lobby_set_allow_spectators(allow);
  return 0;
}

static int cb_lobby_allow_spectators(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_allow_spectators();
}

static int cb_lobby_max_spectators(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_max_spectators();
}

static int cb_lobby_spectator_count(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_spectator_count();
}

static int cb_local_is_spectator(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_local_is_spectator();
}

static int cb_spectator_slot(void *ctx, int index)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return -1;
  return rnet_lobby_spectator_slot(index);
}

static int cb_move_member(void *ctx, int from_slot, int to_slot)
{
  (void)ctx;
  if (g_hosting_lan && from_slot >= 0 && from_slot <= 1 && to_slot >= 0 &&
      to_slot <= 1 && from_slot != to_slot) {
    return lan_swap_seats("host move_member");
  }
  if (g_joined_lan)
    return -1;
  return rnet_lobby_move(from_slot, to_slot);
}

static int cb_kick_member(void *ctx, int slot)
{
  int guest_slot;
  (void)ctx;
  if (g_hosting_lan) {
    guest_slot = 1 - g_lan_room.host_slot;
    if (slot < 0 || slot > 1 || slot != guest_slot ||
        !g_lan_room.joiner_name[0])
      return -1;
    (void)rnet_lan_direct_host_notify_kick(g_direct_host);
    g_lan_room.joiner_name[0] = '\0';
    g_lan_room.started = 0;
    (void)publish_lan_room();
    return 0;
  }
  if (g_joined_lan)
    return -1;
  return rnet_lobby_kick(slot);
}

static int cb_local_ready(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 1;
  return rnet_lobby_local_ready();
}

static int cb_all_ready(void *ctx)
{
  RNetLanLobby state;
  (void)ctx;
  if (use_lan_members(&state))
    return state.joiner_name[0] != '\0';
  return rnet_lobby_all_ready();
}

static int cb_set_ready(void *ctx, int ready)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return rnet_lobby_set_ready(ready);
}

static int cb_request_start(void *ctx, const RecompLauncherCSettings *settings)
{
  RNetLobbyMatchCaps caps = default_caps(settings);
  (void)ctx;
  if (g_hosting_lan) {
    if (!g_lan_room.joiner_name[0])
      return -1;
    g_lan_room.started = 1;
    g_lan_room.session_id = next_lan_session_id();
    g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
    (void)publish_lan_room();
    arm_lan_launch(&g_lan_room);
    return 0;
  }
  return rnet_lobby_request_start(&caps);
}

static int cb_launch_pending(void *ctx)
{
  RNetLanLobby state;
  int ev;
  (void)ctx;
  direct_reopen_step();
  direct_rejoin_step();
  if (g_joined_direct && g_direct_guest && !g_lan_launch.enabled) {
    int rtt = -1;
    ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
    direct_guest_note_event(ev);
    if (ev == 3 && rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    if (g_lan_start_seen)
      arm_lan_launch(&g_lan_room);
  } else if (g_joined_lan && !g_joined_direct && !g_lan_launch.enabled &&
             read_lan(&state) && state.started) {
    g_lan_room = state;
    arm_lan_launch(&g_lan_room);
  } else if (g_hosting_lan && !g_lan_launch.enabled && g_lan_room.started) {
    arm_lan_launch(&g_lan_room);
  }
  return g_lan_launch.enabled || rnet_lobby_launch_pending();
}

static void cb_clear_launch_pending(void *ctx)
{
  (void)ctx;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  rnet_lobby_clear_launch_pending();
}

static const char *cb_last_error(void *ctx)
{
  const RNetLobbyJoinInfo *join;
  (void)ctx;
  if (g_runtime_error[0])
    return g_runtime_error;
  join = rnet_lobby_join_info();
  return (join && join->last_error[0]) ? join->last_error : "";
}

static void cb_clear_last_error(void *ctx)
{
  (void)ctx;
  g_runtime_error[0] = '\0';
  rnet_lobby_clear_last_error();
}

static int cb_input_delay_get(void *ctx)
{
  const RNetLobbyMatchCaps *caps;
  RNetLanLobby state;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan) {
    if (use_lan_members(&state) && state.input_delay >= 2)
      return clamp_input_delay(state.input_delay);
    return clamp_input_delay(g_lobby_input_delay);
  }
  caps = rnet_lobby_match_caps();
  if (caps && caps->valid)
    return clamp_input_delay(caps->input_delay);
  return clamp_input_delay(g_lobby_input_delay);
}

static int cb_input_delay_set(void *ctx, int delay_frames)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  g_lobby_input_delay = clamp_input_delay(delay_frames);
  if (g_hosting_lan) {
    g_lan_room.input_delay = g_lobby_input_delay;
    (void)publish_lan_room();
    if (g_direct_host && g_lan_room.joiner_name[0])
      /* notify_caps takes the room, not the delay. The delay it should carry
       * was already written into g_lan_room two lines up, and passing the int
       * here made the guest read an integer as a room pointer. Only a build
       * with recomp-ui compiles this branch, which is why it survived: GCC 14
       * turned int-conversion into an error and surfaced it. */
      (void)rnet_lan_direct_host_notify_caps(g_direct_host, &g_lan_room);
    return 0;
  }
  if (g_joined_lan)
    return 0;
  if (!rnet_lobby_is_host())
    return -1;
  caps = default_caps(NULL);
  caps.input_delay = g_lobby_input_delay;
  return rnet_lobby_set_match_caps(&caps);
}

static int cb_force_input_relay_get(void *ctx)
{
  const RNetLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  caps = rnet_lobby_match_caps();
  if (caps && caps->valid)
    return caps->force_input_relay ? 1 : 0;
  return g_lobby_force_input_relay ? 1 : 0;
}

static int cb_force_input_relay_set(void *ctx, int force)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0; /* LAN/Direct IP does not use server input relay */
  if (!rnet_lobby_is_host())
    return -1;
  g_lobby_force_input_relay = force ? 1 : 0;
  caps = default_caps(NULL);
  caps.force_input_relay = g_lobby_force_input_relay ? 1 : 0;
  return rnet_lobby_set_match_caps(&caps);
}

static int cb_force_turn_get(void *ctx)
{
  const RNetLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  caps = rnet_lobby_match_caps();
  if (caps && caps->valid)
    return caps->force_turn ? 1 : 0;
  return g_lobby_force_turn ? 1 : 0;
}

static int cb_force_turn_set(void *ctx, int force)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0; /* LAN/Direct IP does not use ICE TURN */
  if (!rnet_lobby_is_host())
    return -1;
  g_lobby_force_turn = force ? 1 : 0;
  caps = default_caps(NULL);
  caps.force_turn = g_lobby_force_turn ? 1 : 0;
  return rnet_lobby_set_match_caps(&caps);
}

static int cb_lobby_max_slots(void *ctx)
{
  const RNetLobbyJoinInfo *join;
  RNetLanLobby state;
  (void)ctx;
  /* LAN / Direct IP rooms are always two seats (see cb_create). */
  if (use_lan_members(&state) || g_hosting_lan || g_joined_lan || g_joined_direct)
    return 2;
  if (!rnet_lobby_in_lobby())
    return 0;
  join = rnet_lobby_join_info();
  if (join && join->ok && join->max_slots >= 2)
    return clamp_lobby_max_slots(join->max_slots);
  return clamp_lobby_max_slots(g_lobby_max_slots);
}

/* Refuse a launch with a reason the waiting room shows, said once per
 * session id so a launcher polling fill_launch every frame does not repeat
 * it. The launch is dropped (launch_pending cleared): the peers that did
 * start will time out waiting for this one, which is the honest outcome --
 * a desynced match is not. */
static int refuse_launch(uint32_t session_id, const char *code,
                         const char *why)
{
  static uint32_t said_for;
  if (said_for != session_id + 1u) {
    said_for = session_id + 1u;
    fprintf(stderr, "netplay: refusing to launch session %u - %s\n",
            (unsigned)session_id, why);
  }
  snprintf(g_runtime_error, sizeof(g_runtime_error), "%s", code);
  rnet_lobby_clear_launch_pending();
  return 0;
}

static int cb_fill_launch(void *ctx, RecompLauncherCNetplayLaunch *out)
{
  RNetLobbyJoinInfo join;
  const RNetLobbyMatchCaps *caps;
  const int i_am_host = rnet_lobby_is_host() ? 1 : 0;
  (void)ctx;
  if (!out)
    return 0;
  if (g_lan_launch.enabled) {
    *out = g_lan_launch;
    out->force_input_relay = 0;
    out->max_slots = 2;
    out->player_count = 2;
    return 1;
  }
  if (!rnet_lobby_try_fill_launch(&join))
    return 0;
  caps = rnet_lobby_match_caps();
  /* Host match_caps are required online -- do not silently default the
   * delay, the mode or the relay. */
  if (!caps || !caps->valid)
    return 0;
  /* A vanilla-only build cannot run a room whose host requires mods. The
   * host's own launch gate normally holds such a match (our offer names no
   * packages); this is the second lock, for a host that does not gate. */
  if (!g_mods && caps_require_mods(caps))
    return refuse_launch(join.session_id, "mods_unsupported",
                         "the host requires mods and this build has no mod "
                         "support");
  memset(out, 0, sizeof(*out));
  out->enabled = 1;
  out->local_slot = join.local_slot;
  out->input_player = g_h.input_player;
  out->session_id = join.session_id;
  out->input_delay = clamp_input_delay(caps->input_delay);
  /* 0 when the host published none: the engine keeps its own default. */
  out->input_prediction = clamp_input_prediction(caps->input_prediction);
  out->rollback = caps->rollback ? 1 : 0;
  out->force_turn = caps->force_turn ? 1 : 0;
  /* The launch's own statement, not the caps copy: the caps field is shared
   * with the host's UI toggle and is overwritten by any lobby_update that
   * arrives before we get here. See RNetLobbyJoinInfo::force_input_relay. */
  out->force_input_relay = join.force_input_relay ? 1 : 0;
  out->max_slots = join.max_slots >= 2 ? clamp_lobby_max_slots(join.max_slots)
                                       : clamp_lobby_max_slots(g_lobby_max_slots);
  out->player_count = join.player_count > 0 ? join.player_count : out->max_slots;
  /* player_count above is the PLAYER count the server sent, and it stays
   * that: it is what sizes every peer's rollback slot_count, and a spectator
   * counted in it is a seat the whole match waits on and nobody fills.
   *
   * The role rides separately, and the engine reads it to decide whether this
   * build contributes a row at all. */
  out->is_spectator = join.local_is_spectator ? 1 : 0;
  out->spectator_wire_slot = 0;
  /* Host in the gallery: it keeps session slot 0 with a muted pad (the seat
   * every host-only path keys on), so it is NOT a spectator to the engine,
   * and every player seat sits one session slot higher. The server said so
   * once, at start; every peer applies the same rule. Only a backend that
   * maps the host to slot 0 can run that, so SEAT refuses it. */
  out->host_spectates = join.host_spectates ? 1 : 0;
  if (out->host_spectates) {
    if (g_h.slot_policy == RECOMP_NETPLAY_SLOTS_SEAT)
      return refuse_launch(join.session_id, "host_spectates_unsupported",
                           "the host is in the gallery and this engine maps "
                           "session slots to seats");
    if (i_am_host) {
      out->local_slot = 0;
      out->is_spectator = 0;
    }
    out->max_slots += 1; /* the host's silent slot 0 */
  }
  if (out->is_spectator) {
    const int wire = rnet_lobby_local_wire_slot();
    if (wire <= 0) {
      /* No relay base published, so there is no slot we could send from that
       * the relay would recognise as a spectator. Falling back to a player
       * slot is the one thing a spectator must never do -- the relay would
       * forward it and the peers would take it as that seat's input. Refuse
       * instead: a lobby with a message beats a desynced match. */
      fprintf(stderr,
              "netplay: refusing to launch as a spectator - the host "
              "published no spectator relay slot (lobby seat %d)\n",
              join.local_slot);
      return 0;
    }
    out->spectator_wire_slot = wire;
  }
  /* Session slots from the seat table the start delivered. */
  {
    int seats[RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS];
    int n = 0;
    int host_seat = -1;
    int my_seat = out->is_spectator ? -1 : join.local_slot;
    const char *host_id = rnet_lobby_host_player_id();
    const char *self_id = rnet_lobby_player_id();
    const int mc = rnet_lobby_member_count();
    SlotPlan plan;
    int i;
    for (i = 0; i < mc; ++i) {
      RNetLobbyMember mem;
      if (!rnet_lobby_member_get(i, &mem))
        continue;
      if (mem.is_spectator)
        continue; /* gallery seats are not session slots */
      if (mem.slot < 0 || mem.slot >= RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
        continue;
      if (n < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
        seats[n++] = mem.slot;
      if (host_id && host_id[0] && strcmp(mem.player_id, host_id) == 0)
        host_seat = mem.slot;
      if (!out->is_spectator && self_id && self_id[0] &&
          strcmp(mem.player_id, self_id) == 0)
        my_seat = mem.slot;
    }
    plan_session_slots(seats, n, host_seat, my_seat,
                       i_am_host && out->local_slot >= 0 && !out->is_spectator,
                       &plan);
    if (!out->is_spectator) {
      if (plan.local_slot < 0)
        return refuse_launch(join.session_id, "not_in_seat_table",
                             "this peer is not in the start's seat table");
      out->local_slot = plan.local_slot;
    }
    if (g_h.slot_policy == RECOMP_NETPLAY_SLOTS_SEAT) {
      /* Session slot == seat: the count must reach the highest seat. */
      if (plan.slot_count > out->player_count)
        out->player_count = plan.slot_count;
    } else {
      out->player_count = plan.slot_count;
    }
    if (out->player_count > out->max_slots)
      out->max_slots = out->player_count;
    launch_apply_plan(out, &plan);
  }
  snprintf(out->bind_hostport, sizeof(out->bind_hostport), "%s",
           join.bind_hostport);
  snprintf(out->peer_hostport, sizeof(out->peer_hostport), "%s",
           join.peer_hostport);
  if (g_h.apply_match_caps)
    g_h.apply_match_caps(g_h.ctx, caps, out);
  g_consumed_launch_sid = join.session_id;
  g_consumed_launch_valid = 1;
  return 1;
}

/* ---- lobby mod plan -------------------------------------------------------
 *
 * recomp-ui already renders all of this -- a row per required mod, an
 * installed/missing mark, a missing count and a download button. This game
 * left the callbacks NULL, so the panel was simply dark and a guest learned
 * about a mod mismatch only when the match refused to start. Nothing new is
 * drawn here; the data is just supplied.
 */

/* Row `index` of the plan the HOST published. */
static const RNetLobbyModPkg *plan_row(int index)
{
  const RNetLobbyMatchCaps *caps = rnet_lobby_match_caps();
  if (!caps || !caps->valid || index < 0 || index >= caps->mod_count)
    return NULL;
  return &caps->mods[index];
}

static int cb_lobby_mods_count(void *ctx)
{
  const RNetLobbyMatchCaps *caps = rnet_lobby_match_caps();
  (void)ctx;
  if (!caps || !caps->valid)
    return 0;
  return caps->mod_count;
}

static int cb_lobby_mods_get(void *ctx, int index,
                             RecompLauncherCNetplayLobbyMod *out)
{
  const RNetLobbyModPkg *row = plan_row(index);
  const char *id;
  const char *version;
  (void)ctx;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!row)
    return 0;
  id = row->id;
  version = row->ver;
  snprintf(out->id, sizeof(out->id), "%s", id);
  snprintf(out->version, sizeof(out->version), "%s", version);
  /* The host published a display name; prefer it, and fall back to the id.
   * The local lookup below overrides it when this peer has the package, so a
   * player sees the same name the host sees either way. */
  snprintf(out->name, sizeof(out->name), "%s",
           row->name[0] ? row->name : id);
  out->installed = 0;

  /* Pull this package's lines out of the host's published set.
   *
   * caps.mod_set is the canonical text with ';' where newlines were, one entry
   * per enabled feature:  "<pkg>@<ver>/<feature> <opt>=<val> ..."
   * The row shows the part after the package prefix, so a guest reads the
   * host's actual choices -- "localization language=en" -- rather than its own
   * local settings, which are not what the match will run. */
  {
    const RNetLobbyMatchCaps *caps = rnet_lobby_match_caps();
    size_t o = 0;
    if (caps && caps->valid && caps->mod_set[0]) {
      const char *p = caps->mod_set;
      const size_t id_len = strlen(id);
      while (*p) {
        const char *end = strchr(p, ';');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        /* "<id>@" anchors the match, so a package whose id is a prefix of
         * another's cannot claim its line. */
        if (len > id_len + 1 && !strncmp(p, id, id_len) && p[id_len] == '@') {
          const char *slash = (const char *)memchr(p, '/', len);
          if (slash) {
            const size_t tail = len - (size_t)(slash + 1 - p);
            /* One entry per line. A package with several features, or one
             * feature with several options, is a list -- run together on one
             * line it wraps into a paragraph the player has to parse. */
            if (o && o + 1 < sizeof(out->options))
              out->options[o++] = '\n';
            if (o + tail < sizeof(out->options)) {
              memcpy(out->options + o, slash + 1, tail);
              o += tail;
            }
          }
        }
        if (!end) break;
        p = end + 1;
      }
    }
    out->options[o] = '\0';
  }

  if (g_mods && g_mods->have_package) {
    char name[64];
    /* NULL version: "do I have this package at all". A different version is
     * still HAVING it, so the lobby says installed and offers no download --
     * there is nothing to fetch. If the two versions genuinely simulate
     * differently, the mod-set exchange at session start says so precisely,
     * naming the versions and offering to adopt the host's selection. */
    int have;
    name[0] = '\0';
    have = g_mods->have_package(g_mods->ctx, id, NULL, name,
                                (uint32_t)sizeof(name));
    if (name[0])
      snprintf(out->name, sizeof(out->name), "%s", name);
    if (have > 0) {
      out->installed = 1;
      /* Say so when the versions differ. Not a blocker here, but it is the
       * first thing worth knowing if the match later refuses to start. */
      if (version[0] &&
          g_mods->have_package(g_mods->ctx, id, version, NULL, 0) == 0)
        snprintf(out->reason, sizeof(out->reason),
                 "host runs %s; you have another version", version);
    } else {
      snprintf(out->reason, sizeof(out->reason), "not installed");
    }
  } else {
    snprintf(out->reason, sizeof(out->reason), "this build has no mod support");
  }
  return 1;
}

static int cb_lobby_mods_missing(void *ctx)
{
  const int n = cb_lobby_mods_count(ctx);
  int missing = 0;
  int i;
  for (i = 0; i < n; ++i) {
    RecompLauncherCNetplayLobbyMod lm;
    if (cb_lobby_mods_get(ctx, i, &lm) && !lm.installed)
      missing++;
  }
  return missing;
}

/* Can this peer pull the plan's mods from the host RIGHT NOW?
 *
 * Not yet: this build has no mod transfer. Answered as a capability question
 * rather than left for the download call to fail, because the UI asks this
 * BEFORE drawing the button -- a button that cannot work is worse than no
 * button, and the message it used to print blamed the host for it.
 *
 * When the transfer lands this becomes "are we seated, is there a host, and is
 * the plan unmet". Note that it must NOT be answered from the lobby server's
 * `can_transfer` flag: the server hardcodes that true and is describing
 * itself, not this build's ability to drive a transfer.
 */
/* "Download All": the missing rows, fetched one after another.
 *
 * The queue lives here rather than in the lobby client because this is the
 * layer that knows which rows are missing -- the client moves one package at
 * a time and has no opinion about which. Advanced from the pump, since a
 * transfer finishing is not an event anything reports. */
static char g_dl_id[RNET_LOBBY_MAX_MODS][RNET_LOBBY_MOD_ID_LEN];
static char g_dl_ver[RNET_LOBBY_MAX_MODS][RNET_LOBBY_MOD_VER_LEN];
static int  g_dl_n;
static int  g_dl_next;

static void dl_queue_clear(void)
{
  g_dl_n = 0;
  g_dl_next = 0;
}

/* Start the next queued package once the previous one has left. Called every
 * pump; a no-op unless a queue is running and the link is idle. */
static void dl_queue_step(void)
{
  if (g_dl_next >= g_dl_n) {
    if (g_dl_n) dl_queue_clear();
    return;
  }
  if (rnet_lobby_mod_in_flight()[0])
    return;                        /* one at a time */
  {
    const int i = g_dl_next++;
    const int rc = rnet_lobby_mod_request(g_dl_id[i], g_dl_ver[i]);
    if (rc == 0) {
      fprintf(stderr, "netplay: download-all %d/%d: %s\n", i + 1, g_dl_n,
              g_dl_id[i]);
    } else if (rc == -2) {
      g_dl_next--;                 /* still busy; try again next pump */
    } else {
      fprintf(stderr, "netplay: download-all stopped at %s\n", g_dl_id[i]);
      dl_queue_clear();
    }
  }
}

/* Republish whenever the HOST's own configuration changes.
 *
 * push_match_caps was called from exactly one place in the launcher: the
 * feature enable checkbox. Changing an option -- picking a language from a
 * dropdown -- changed what the host would run and told nobody, so guests kept
 * showing and ADOPTING the previous value, and only found out at launch when
 * the mod-set check refused them.
 *
 * Watched here rather than wired into each control because publication is this
 * layer's job: any path that changes the effective set, present or future, is
 * covered by comparing the set itself. The comparison is against the canonical
 * text, so it fires on a real change and not on a redraw. */
static char g_published_set[1024];

static void host_caps_watch_step(void)
{
  char text[1024];
  int need;

  if (!g_mods || !g_mods->effective_set)
    return;
  if (!rnet_lobby_in_lobby() || !rnet_lobby_is_host()) {
    g_published_set[0] = '\0';
    return;
  }
  need = g_mods->effective_set(g_mods->ctx, text, (uint32_t)sizeof(text));
  if (need <= 0 || need >= (int)sizeof(text))
    return;
  if (!strcmp(text, g_published_set))
    return;
  snprintf(g_published_set, sizeof(g_published_set), "%s", text);
  fprintf(stderr, "netplay: mod configuration changed - republishing to the "
                  "lobby\n");
  cb_push_match_caps(NULL);
}

/* Bring this peer's mod configuration into line with the host's, in the
 * LOBBY, where it still costs nothing.
 *
 * Owning a package and running it are different things. The lobby gate asks
 * only whether a peer HAS each mod, so a guest that downloaded both and
 * enabled neither sails through it -- and is then refused by the session's
 * mod-set check, which compares enabled features and resolved options. That
 * refusal is correct; the problem is that it arrives after Play, having told
 * the player everything was ready.
 *
 * Adoption is the same policy the netplay layer already applies on that
 * refusal, moved earlier. It works here because mods commit and activate
 * after the launcher exits (in every engine that wires them: snesrecomp's
 * ports commit in src/main.c), so a selection changed in the lobby is the
 * selection the match runs -- no "start the game again".
 *
 * Only ever toward the host's set, only for a guest, and only when it
 * actually differs. */
static char g_adopted_set[512];

static void mod_set_sync_step(void)
{
  const RNetLobbyMatchCaps *caps;
  char want[1024];
  char reason[160];
  size_t i;
  size_t o = 0;

  if (!g_mods)
    return;
  if (!rnet_lobby_in_lobby() || rnet_lobby_is_host()) {
    g_adopted_set[0] = '\0';
    /* Out of a lobby, no authority is granting anything, so the exemption
     * lapses rather than lingering from the last host we spoke to. A host
     * sets its own grant in fill_caps_mods and must not be cleared here. */
    if (!rnet_lobby_in_lobby())
      mods_set_cosmetic_allow(NULL);
    return;
  }
  caps = rnet_lobby_match_caps();
  if (!caps || !caps->valid)
    return;
  /* The host is the authority here, so its grant governs before anything is
   * compared or adopted. Applied even when the host's own mod set is empty --
   * a vanilla host that permits an accessibility filter is the ordinary case,
   * and returning early on an empty set would leave a stale allowlist from a
   * previous lobby in force. */
  mods_set_cosmetic_allow(caps->mod_cosmetic_allow);
  if (!caps->mod_set[0])
    return;
  if (!strcmp(g_adopted_set, caps->mod_set))
    return;                      /* already tried this exact set */

  /* Back to the canonical newline form the runtime speaks. */
  for (i = 0; caps->mod_set[i] && o + 2 < sizeof(want); ++i)
    want[o++] = caps->mod_set[i] == ';' ? '\n' : caps->mod_set[i];
  want[o++] = '\n';
  want[o] = '\0';

  reason[0] = '\0';
  if (g_mods->check_set &&
      g_mods->check_set(g_mods->ctx, want, reason, (uint32_t)sizeof(reason)) == 0)
    return;                      /* already matches */

  snprintf(g_adopted_set, sizeof(g_adopted_set), "%s", caps->mod_set);
  if (g_mods->adopt_set &&
      g_mods->adopt_set(g_mods->ctx, want, reason, (uint32_t)sizeof(reason)) == 0) {
    fprintf(stderr, "netplay: matched the host's mod configuration:\n%s", want);
    /* The set changed, so what we announce has changed with it. */
    (void)rnet_lobby_set_ready(1);
  } else {
    fprintf(stderr, "netplay: cannot match the host's mod set: %s\n",
            reason[0] ? reason : "(no reason given)");
  }
}

/*
 * Report a simulation fork once per session, promptly.
 *
 * Promptly, not at teardown: a desync usually ENDS the match, and often takes
 * the connection with it, so a report deferred to a clean shutdown is a report
 * that mostly never gets sent. Once per session because the interesting fact
 * is that the peers diverged and where -- a hundred rows from one broken match
 * would drown the signal the rows exist to carry.
 *
 * Best-effort throughout. Nothing here may hold up a teardown or change what
 * the player sees; it is a diagnostic, and a diagnostic that costs a match is
 * not worth having.
 */
static void desync_report_step(void)
{
  static int reported;
  RNetLobbyDesyncReport r;
  uint32_t tick = 0;
  uint32_t mine = 0, theirs = 0;
  const char *partition = "?";
  char exempt[512];

  if (!rnet_lobby_connected()) {
    /* A fresh connection is a fresh session: arm again so the next match can
     * report its own fork. */
    reported = 0;
    return;
  }
  if (reported || !g_h.last_fork)
    return;
  if (!g_h.last_fork(g_h.ctx, &tick, &partition, &mine, &theirs))
    return;
  if (!partition)
    partition = "?";

  exempt[0] = '\0';
  if (g_mods && g_mods->exempted_packages) {
    /* What this peer was running, so the row can be read beside what the
     * match approved. A fork under an unapproved exemption and a fork under
     * none are different facts, and the row is worth little without which it
     * was. */
    char *p;
    (void)g_mods->exempted_packages(g_mods->ctx, exempt,
                                    (uint32_t)sizeof(exempt));
    exempt[sizeof(exempt) - 1] = '\0';
    /* Newlines to ';' -- the wire carries one line. */
    for (p = exempt; *p; ++p)
      if (*p == '\n') *p = ';';
    if (p > exempt && p[-1] == ';') p[-1] = '\0';
  }

  memset(&r, 0, sizeof(r));
  r.tick = tick;
  r.partition = partition;
  r.mine = mine;
  r.theirs = theirs;
  r.is_host = rnet_lobby_is_host() ? 1 : 0;
  r.mod_exempt = exempt;

  reported = 1;    /* set before the send: a failed send must not retry */
  if (rnet_lobby_report_desync(&r) == 0)
    fprintf(stderr,
            "netplay: reported a state fork at tick %u (%s), local %08x vs "
            "peer %08x -- this records that the two peers DIFFERED, not who "
            "was wrong\n",
            (unsigned)tick, partition, (unsigned)mine, (unsigned)theirs);
}

static int cb_lobby_mods_can_download(void *ctx)
{
  (void)ctx;
  if (!g_mods || !g_mods->install_blob)
    return 0;
  /* A guest seated in a server lobby, with a host to ask. Not derived from
   * the server's `can_transfer` flag: that is hardcoded true and describes
   * the server, not this build's ability to drive a transfer.
   *
   * LAN and direct-IP sessions have no signalling relay to carry the SDP, so
   * they answer no and say so in the panel rather than offering a button that
   * would sit at "connecting" forever. */
  if (g_hosting_lan || g_joined_lan || g_joined_direct)
    return 0;
  if (!rnet_lobby_in_lobby() || rnet_lobby_is_host())
    return 0;
  return rnet_lobby_host_player_id()[0] != '\0';
}

static int cb_lobby_mods_download_one(void *ctx, int index)
{
  const RNetLobbyModPkg *row = plan_row(index);
  (void)ctx;
  if (!row || !cb_lobby_mods_can_download(NULL))
    return -1;
  /* Passes -2 (busy) through untouched: the UI tells those two apart, and
   * flattening them here would report a working transfer as a broken one. */
  return rnet_lobby_mod_request(row->id, row->ver);
}

static int cb_lobby_mods_progress_one(void *ctx, int index)
{
  const RNetLobbyModPkg *row = plan_row(index);
  const char *moving = rnet_lobby_mod_in_flight();
  (void)ctx;
  /* Progress belongs to the row being transferred, not to every row: without
   * this every unmet row would show the same bar and the player could not
   * tell which one was actually moving. */
  if (!row || !moving[0] || strcmp(moving, row->id) != 0)
    return -1;
  return rnet_lobby_mod_progress();
}

static int cb_mod_xfer_failed(void *ctx, char *err, size_t err_cap)
{
  (void)ctx;
  return rnet_lobby_mod_failed(err, err_cap);
}

static void cb_mod_xfer_cancel(void *ctx)
{
  (void)ctx;
  rnet_lobby_mod_cancel();
}

static int cb_lobby_mods_download(void *ctx)
{
  int n;
  int i;
  (void)ctx;
  if (!cb_lobby_mods_can_download(NULL))
    return -1;
  dl_queue_clear();
  n = cb_lobby_mods_count(NULL);
  for (i = 0; i < n && g_dl_n < RNET_LOBBY_MAX_MODS; ++i) {
    RecompLauncherCNetplayLobbyMod lm;
    if (!cb_lobby_mods_get(NULL, i, &lm) || lm.installed)
      continue;
    snprintf(g_dl_id[g_dl_n], RNET_LOBBY_MOD_ID_LEN, "%s", lm.id);
    snprintf(g_dl_ver[g_dl_n], RNET_LOBBY_MOD_VER_LEN, "%s", lm.version);
    g_dl_n++;
  }
  if (g_dl_n == 0)
    return -1;                     /* nothing missing; nothing to start */
  fprintf(stderr, "netplay: downloading all %d missing mod(s) from the host\n",
          g_dl_n);
  dl_queue_step();
  return 0;
}

static void cb_push_match_caps(void *ctx)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  if (!rnet_lobby_is_host())
    return;                    /* guests do not publish a plan */
  caps = default_caps(NULL);   /* already carries the current mod plan */
  (void)rnet_lobby_set_match_caps(&caps);
}

/* Seat self-service: online rooms only. The LAN room is two seats with
 * the host as the only authority; there a swap is the host's move_member. */
static int cb_seat_move_self(void *ctx, int to_slot)
{
  (void)ctx;
  if (g_hosting_lan) {
    /* Two seats: the other one is free only while nobody has joined. */
    if (to_slot != 1 - g_lan_room.host_slot) return -1;
    if (g_lan_room.joiner_name[0]) return -1; /* occupied: ask instead */
    return lan_swap_seats("host move_self");
  }
  if (g_joined_lan) return -1; /* the only other seat is the host's: ask */
  return rnet_lobby_seat_move_self(to_slot);
}
static int cb_seat_swap_request(void *ctx, int target_slot)
{
  (void)ctx;
  if (g_hosting_lan) {
    /* The host is the authority of a LAN room: its own trade is immediate. */
    if (target_slot != 1 - g_lan_room.host_slot || !g_lan_room.joiner_name[0])
      return -1;
    if (lan_swap_seats("host swap_request") != 0) return -1;
    g_lan_swap_outgoing = 2;
    return 0;
  }
  if (g_joined_lan) {
    if (!g_joined_direct || !g_direct_guest) return -1;
    if (target_slot != g_lan_room.host_slot) return -1;
    if (g_lan_swap_outgoing == 1) return -1;
    if (rnet_lan_direct_guest_send_swap_request(g_direct_guest) != RNET_LAN_DIRECT_OK)
      return -1;
    g_lan_swap_outgoing = 1;
    return 0;
  }
  return rnet_lobby_seat_swap_request(target_slot);
}
static int cb_seat_swap_incoming(void *ctx, char *who, size_t who_cap, int *from_slot)
{
  (void)ctx;
  if (g_hosting_lan) {
    if (!g_lan_swap_incoming) return 0;
    if (who && who_cap)
      snprintf(who, who_cap, "%s",
               g_lan_room.joiner_name[0] ? g_lan_room.joiner_name : "Player");
    if (from_slot) *from_slot = 1 - g_lan_room.host_slot;
    return 1;
  }
  if (g_joined_lan) return 0;
  return rnet_lobby_seat_swap_incoming(who, who_cap, from_slot);
}
static int cb_seat_swap_respond(void *ctx, int accept)
{
  (void)ctx;
  if (g_hosting_lan) {
    int swapped = 0;
    if (!g_lan_swap_incoming) return -1;
    g_lan_swap_incoming = 0;
    if (accept && g_lan_room.joiner_name[0] && lan_swap_seats("host accepted") == 0) swapped = 1;
    if (g_direct_host)
      (void)rnet_lan_direct_host_send_swap_result(g_direct_host, swapped);
    return 0;
  }
  if (g_joined_lan) return -1;
  return rnet_lobby_seat_swap_respond(accept);
}
static int cb_seat_swap_outgoing(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan) return g_lan_swap_outgoing;
  return rnet_lobby_seat_swap_outgoing();
}
static void cb_seat_swap_clear(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan) {
    if (g_lan_swap_outgoing != 1) g_lan_swap_outgoing = 0;
    return;
  }
  rnet_lobby_seat_swap_clear();
}

/* ---- the callbacks the SNES backend left NULL ------------------------------
 *
 * Every one of these is optional in recomp-ui, so a NULL simply hid the
 * control -- and for rollback and the runway that meant the host could not
 * see or change the mode the room would actually run. */

/* Host toggle: published in match_caps.rollback. Read back from the caps the
 * room carries (what every peer will run), else the local pending value. */
static int cb_rollback_get(void *ctx)
{
  const RNetLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0; /* the LAN room settles no mode; see arm_lan_launch */
  caps = rnet_lobby_match_caps();
  if (rnet_lobby_in_lobby() && caps && caps->valid)
    return caps->rollback ? 1 : 0;
  return g_lobby_rollback ? 1 : 0;
}

static int cb_rollback_set(void *ctx, int enable)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return -1; /* nowhere to publish it: the LAN room has no caps */
  g_lobby_rollback = enable ? 1 : 0;
  if (!rnet_lobby_in_lobby())
    return 0; /* applies to the next create */
  if (!rnet_lobby_is_host())
    return -1;
  caps = default_caps(NULL);
  caps.rollback = g_lobby_rollback;
  return rnet_lobby_set_match_caps(&caps);
}

/* Host invent runway P (2..16), published in match_caps.input_prediction.
 * 0 while nobody has set one: the launch then carries 0 and the engine keeps
 * its own default, which is what every peer does alike. */
static int cb_input_prediction_get(void *ctx)
{
  const RNetLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  caps = rnet_lobby_match_caps();
  if (rnet_lobby_in_lobby() && caps && caps->valid)
    return caps->input_prediction;
  return clamp_input_prediction(g_lobby_input_prediction);
}

static int cb_input_prediction_set(void *ctx, int prediction_frames)
{
  RNetLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return -1;
  g_lobby_input_prediction = clamp_input_prediction(prediction_frames);
  if (!rnet_lobby_in_lobby())
    return 0;
  if (!rnet_lobby_is_host())
    return -1;
  caps = default_caps(NULL);
  caps.input_prediction = g_lobby_input_prediction;
  return rnet_lobby_set_match_caps(&caps);
}

/* 1 between connect() and the server's welcome: the TCP connect itself is
 * synchronous, the WebSocket upgrade and `welcome` arrive through the pump. */
static int cb_connecting(void *ctx)
{
  (void)ctx;
  return rnet_lobby_connected() && !rnet_lobby_ready();
}

/* The courtesy half of the name policy; the server's refusal is the gate. */
static int cb_name_rejected(void *ctx, const char *name)
{
  char masked[256];
  (void)ctx;
  if (!name || !name[0])
    return 0;
  return rnet_chat_filter_copy(name, masked, sizeof(masked)) > 0 ? 1 : 0;
}

/* The join-time need_mods refusal: what the server said is missing. */
static int cb_need_mods_count(void *ctx)
{
  (void)ctx;
  return rnet_lobby_need_mods_count();
}

static int cb_need_mods_get(void *ctx, int index,
                            RecompLauncherCNetplayNeedMod *out)
{
  const RNetLobbyModPkg *row = rnet_lobby_need_mods_get(index);
  (void)ctx;
  if (!out || !row)
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->id, sizeof(out->id), "%s", row->id);
  snprintf(out->version, sizeof(out->version), "%s", row->ver);
  snprintf(out->name, sizeof(out->name), "%s", row->name[0] ? row->name : row->id);
  return 1;
}

/* Can this build pull those packages BEFORE being seated? No: the transfer
 * rides the seated `signal` relay, which a refused joiner does not have, and
 * mod_xfer_start is therefore not offered. Not answered from the server's
 * can_transfer flag, which describes the server, not this build. This title
 * policy seats everyone (the plan rides as mod_plan, not the server-enforced
 * `mods`), so a refusal here means an older host -- say so and stop. */
static int cb_need_mods_can_transfer(void *ctx)
{
  (void)ctx;
  return 0;
}

static int cb_mod_xfer_progress(void *ctx)
{
  (void)ctx;
  return rnet_lobby_mod_progress();
}

static RecompLauncherCNetplayCallbacks g_callbacks = {
    NULL,
    cb_default_url,
    cb_set_url,
    cb_connect,
    cb_connected,
    cb_pump,
    cb_set_player_name,
    cb_player_name,
    cb_request_list,
    cb_list_count,
    cb_list_get,
    cb_local_ip,
    cb_external_ip,
    cb_create,
    cb_join,
    cb_leave,
    cb_in_lobby,
    cb_is_host,
    cb_member_count,
    cb_member_get,
    cb_move_member,
    cb_local_ready,
    cb_all_ready,
    cb_set_ready,
    cb_request_start,
    cb_launch_pending,
    cb_clear_launch_pending,
    cb_fill_launch,
    cb_local_address_get,
    cb_kick_member,
    cb_last_error,
    cb_clear_last_error,
    cb_input_delay_get,
    cb_input_delay_set,
    cb_force_input_relay_get,
    cb_force_input_relay_set,
    cb_lobby_max_slots,
    cb_force_turn_get,
    cb_force_turn_set,
    /* Designated from here: the struct is append-only, and positional entries
     * past this point would silently shift if a field were ever inserted. */
    .rollback_get = cb_rollback_get,
    .rollback_set = cb_rollback_set,
    .input_prediction_get = cb_input_prediction_get,
    .input_prediction_set = cb_input_prediction_set,
    .connecting = cb_connecting,
    .need_mods_count = cb_need_mods_count,
    .need_mods_get = cb_need_mods_get,
    .need_mods_can_transfer = cb_need_mods_can_transfer,
    .mod_xfer_progress = cb_mod_xfer_progress,
    /* host_can_spectate stays NULL: the UI then never offers the host the
     * gallery. HOST_FIRST can run it (fill_launch folds host_spectates in),
     * but no engine on this module has been measured doing so yet. */
    .name_rejected = cb_name_rejected,
    .push_match_caps = cb_push_match_caps,
    .lobby_mods_count = cb_lobby_mods_count,
    .lobby_mods_get = cb_lobby_mods_get,
    .lobby_mods_missing = cb_lobby_mods_missing,
    .lobby_mods_download = cb_lobby_mods_download,
    .lobby_mods_can_download = cb_lobby_mods_can_download,
    .lobby_mods_download_one = cb_lobby_mods_download_one,
    .lobby_mods_progress_one = cb_lobby_mods_progress_one,
    .mod_xfer_failed = cb_mod_xfer_failed,
    .mod_xfer_cancel = cb_mod_xfer_cancel,
    .allow_spectators_get = cb_allow_spectators_get,
    .allow_spectators_set = cb_allow_spectators_set,
    .lobby_allow_spectators = cb_lobby_allow_spectators,
    .lobby_max_spectators = cb_lobby_max_spectators,
    .lobby_spectator_count = cb_lobby_spectator_count,
    .local_is_spectator = cb_local_is_spectator,
    .spectator_slot = cb_spectator_slot,
    .chat_send = cb_chat_send,
    .chat_count = cb_chat_count,
    .chat_get = cb_chat_get,
    .seat_move_self = cb_seat_move_self,
    .seat_swap_request = cb_seat_swap_request,
    .seat_swap_incoming = cb_seat_swap_incoming,
    .seat_swap_respond = cb_seat_swap_respond,
    .seat_swap_outgoing = cb_seat_swap_outgoing,
    .seat_swap_clear = cb_seat_swap_clear,
    .online_count = cb_online_count,
    .online_get = cb_online_get,
    .server_chat_send = cb_server_chat_send,
    .server_chat_count = cb_server_chat_count,
    .server_chat_get = cb_server_chat_get,
#if defined(RECOMP_LAUNCHER_HAS_ACCOUNT)
    /* Optional Discord sign-in. Guarded on the launcher ABI macro so this
     * runner still builds against a recomp-ui that predates the callbacks --
     * the UI and the runners can land in any order. */
    .account_available = cb_account_available,
    .account_login_begin = cb_account_login_begin,
    .account_state = cb_account_state,
    .account_handle = cb_account_handle,
    .account_username = cb_account_username,
    .account_error = cb_account_error,
    .account_sign_out = cb_account_sign_out,
    .account_set_handle = cb_account_set_handle,
#endif
#if defined(RECOMP_LAUNCHER_HAS_SET_BLOCKS)
    .set_blocks = cb_set_blocks,
#endif
#if defined(RECOMP_LAUNCHER_HAS_CHAT_REPORT)
    .chat_report = cb_chat_report,
#endif
#if defined(RECOMP_LAUNCHER_HAS_LIST_SCOPE)
    .list_scope_set = cb_list_scope_set,
#endif
#if defined(RECOMP_LAUNCHER_HAS_AUTOMATCH)
    /* Guarded like the account block above, and for the same reason: this
     * runner still builds against a recomp-ui that predates the callbacks. */
    .automatch_available = cb_automatch_available,
    .automatch_ruleset_count = cb_automatch_ruleset_count,
    .automatch_ruleset_get = cb_automatch_ruleset_get,
    .automatch_queue = cb_automatch_queue,
    .automatch_cancel = cb_automatch_cancel,
    .automatch_state = cb_automatch_state,
    .automatch_queued_secs = cb_automatch_queued_secs,
    .automatch_pool = cb_automatch_pool,
    .automatch_found_get = cb_automatch_found_get,
    .automatch_accept = cb_automatch_accept,
    .automatch_error = cb_automatch_error,
#endif
};

const RecompLauncherCNetplayCallbacks *recomp_netplay_host_callbacks(void)
{
  return g_inited ? &g_callbacks : NULL;
}

static uint64_t auto_now_ms(void)
{
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void auto_sleep_ms(unsigned ms)
{
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}

static void auto_log(const char *role, const char *stage)
{
  const RecompLauncherCNetplayCallbacks *cb = recomp_netplay_host_callbacks();
  const char *err = (cb && cb->last_error) ? cb->last_error(NULL) : "";
  fprintf(stderr,
          "[netplay selftest] role=%s stage=%s members=%d ready=%d "
          "in_lobby=%d error=%s\n",
          role, stage, rnet_lobby_member_count(), rnet_lobby_all_ready(),
          rnet_lobby_in_lobby(), err ? err : "");
}

int recomp_netplay_host_auto_launch(const char *role, const char *player_name,
                                    const char *lobby_name, unsigned timeout_ms,
                                    RecompLauncherCNetplayLaunch *out)
{
  const RecompLauncherCNetplayCallbacks *cb;
  RecompLauncherCSettings settings;
  RecompLauncherCNetplayLobby row;
  uint64_t deadline;
  uint64_t next_list = 0;
  int is_host;
  int joined = 0;
  int host_ready_sent = 0;
  int start_sent = 0;
  int i;

  cb = recomp_netplay_host_callbacks();
  if (!cb || !g_inited)
    return -1;
  if (!role || !lobby_name || !lobby_name[0] || !out)
    return -1;
  is_host = strcmp(role, "host") == 0;
  if (!is_host && strcmp(role, "guest") != 0)
    return -1;
  if (timeout_ms < 1000u)
    timeout_ms = 60000u;
  deadline = auto_now_ms() + timeout_ms;
  memset(&settings, 0, sizeof(settings));
  memset(out, 0, sizeof(*out));
  cb->set_player_name(NULL, player_name && player_name[0]
                                ? player_name
                                : (is_host ? "HostTest" : "GuestTest"));
  if (cb->connect(NULL) != 0) {
    auto_log(role, "connect_failed");
    return -2;
  }

  while (!rnet_lobby_player_id()[0]) {
    cb->pump(NULL);
    if (!cb->connected(NULL)) {
      auto_log(role, "handshake_disconnected");
      return -3;
    }
    if (auto_now_ms() >= deadline) {
      auto_log(role, "welcome_timeout");
      return -4;
    }
    auto_sleep_ms(10);
  }
  auto_log(role, "connected");

  if (is_host) {
    char endpoint[64] = "0.0.0.0:7777";
    if (cb->create(NULL, lobby_name, endpoint, "", &settings, 0, 2) != 0) {
      auto_log(role, "create_failed");
      return -5;
    }
  }

  while (!cb->launch_pending(NULL)) {
    cb->pump(NULL);
    if (!cb->connected(NULL)) {
      auto_log(role, "lobby_disconnected");
      return -6;
    }

    if (!is_host && !joined && auto_now_ms() >= next_list) {
      cb->request_list(NULL);
      next_list = auto_now_ms() + 500u;
    }
    if (!is_host && !joined) {
      for (i = 0; i < cb->list_count(NULL); ++i) {
        if (cb->list_get(NULL, i, &row) && strcmp(row.name, lobby_name) == 0 &&
            strcmp(row.game_name, game_name()) == 0) {
          char guest_bind[64];
          guest_bind[0] = '\0';
          if (cb->join(NULL, row.lobby_id, "", guest_bind) != 0) {
            auto_log(role, "join_failed");
            return -7;
          }
          joined = 1;
          auto_log(role, "join_sent");
          break;
        }
      }
    }

    if (is_host && cb->in_lobby(NULL) && cb->member_count(NULL) >= 2 &&
        !host_ready_sent) {
      if (cb->set_ready(NULL, 1) != 0) {
        auto_log(role, "ready_failed");
        return -8;
      }
      host_ready_sent = 1;
      auto_log(role, "ready_sent");
    }
    if (is_host && host_ready_sent && cb->all_ready(NULL) && !start_sent) {
      if (cb->request_start(NULL, &settings) != 0) {
        auto_log(role, "start_failed");
        return -9;
      }
      start_sent = 1;
      auto_log(role, "start_sent");
    }

    if (auto_now_ms() >= deadline) {
      auto_log(role, "launch_timeout");
      return -10;
    }
    auto_sleep_ms(10);
  }
  if (!cb->fill_launch(NULL, out)) {
    auto_log(role, "invalid_launch");
    return -11;
  }
  fprintf(stderr,
          "[netplay selftest] role=%s stage=launch slot=%d session=%u "
          "bind=%s peer=%s\n",
          role, out->local_slot, (unsigned)out->session_id, out->bind_hostport,
          out->peer_hostport);
  return 0;
}

