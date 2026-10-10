/*
 * recomp_netplay_host.c's launch planning, driven directly.
 *
 * Includes the module's translation unit (so the static seat planner and the
 * LAN launch are the real ones) and links recomp-net. Only the LAN rematch
 * cases open sockets, all on 127.0.0.1; nothing connects to a lobby server.
 *
 * Built only when RECOMP_UI_RECOMP_NET_DIR names a recomp-net checkout
 * (see CMakeLists.txt); recomp-ui itself does not need recomp-net.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "../src/netplay/recomp_netplay_host.c"

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

static void hooks_with_title(int policy, int max_players,
                             int netplay_max_players)
{
    RecompNetplayHostHooks h;
    memset(&h, 0, sizeof(h));
    h.game_name = "Test Game";
    h.game_version = "1.2.3";
    h.platform = "test";
    h.max_players = max_players;
    h.netplay_max_players = netplay_max_players;
    h.slot_policy = policy;
    ck(recomp_netplay_host_init(&h) == 0, "init");
}

static void hooks_with(int policy, int max_players)
{
    hooks_with_title(policy, max_players, 0);
}

/* The contract case: host is session slot 0 whatever seat it holds. */
static void case_host_first_standard_and_swapped(void)
{
    const int seats[2] = { 0, 1 };
    SlotPlan p;
    printf("  host first: two seats\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);

    plan_session_slots(seats, 2, 0, 0, 1, &p);
    ck(p.local_slot == 0 && p.slot_count == 2 && p.occupied == 0x3u,
       "host in seat 0 is slot 0 of two");
    ck(p.port[0] == 0 && p.port[1] == 1 && p.port[2] == -1, "ports by seat");
    plan_session_slots(seats, 2, 0, 1, 0, &p);
    ck(p.local_slot == 1, "the guest in seat 1 is slot 1");

    /* The host moved to seat 1. It stays slot 0 and drives port 1. */
    plan_session_slots(seats, 2, 1, 1, 1, &p);
    ck(p.local_slot == 0, "a host in seat 1 is still session slot 0");
    ck(p.port[0] == 1 && p.port[1] == 0, "and drives its seat's port");
    plan_session_slots(seats, 2, 1, 0, 0, &p);
    ck(p.local_slot == 1, "the guest in seat 0 is slot 1");
}

static void case_host_first_sparse_four(void)
{
    const int seats[3] = { 3, 0, 1 };  /* any order */
    SlotPlan p;
    printf("  host first: sparse four-seat room\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    plan_session_slots(seats, 3, 0, 3, 0, &p);
    ck(p.slot_count == 3, "three players, three slots (no hole)");
    ck(p.occupied == 0x7u, "every slot occupied");
    ck(p.local_slot == 2, "seat 3 is session slot 2");
    ck(p.port[0] == 0 && p.port[1] == 1 && p.port[2] == 3,
       "and drives port 3, where the lobby seated it");
    ck(p.port[3] == -1, "no fourth slot");
}

static void case_host_first_host_in_gallery(void)
{
    const int seats[2] = { 0, 1 };
    SlotPlan p;
    printf("  host first: host in the gallery\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    plan_session_slots(seats, 2, -1, -1, 1, &p);
    ck(p.local_slot == 0, "the host still runs slot 0");
    ck(p.port[0] == -1, "with no port");
    ck(p.slot_count == 3 && p.port[1] == 0 && p.port[2] == 1,
       "players sit at seat + 1");
    plan_session_slots(seats, 2, -1, 1, 0, &p);
    ck(p.local_slot == 2, "the player in seat 1 is slot 2");
}

/* Legacy: session slot == seat. Complete, but the seat-0 player is the
 * authority -- the reason HOST_FIRST is the default. */
static void case_seat_policy(void)
{
    const int two[2] = { 0, 1 };
    const int sparse[2] = { 2, 0 };
    SlotPlan p;
    printf("  seat policy\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_SEAT, 4);
    plan_session_slots(two, 2, 0, 0, 1, &p);
    ck(p.local_slot == 0 && p.slot_count == 2 && p.occupied == 0x3u,
       "host seat 0 -> slot 0");
    ck(p.port[0] == 0 && p.port[1] == 1, "identity ports");
    plan_session_slots(two, 2, 1, 1, 1, &p);
    ck(p.local_slot == 1, "a host in seat 1 is slot 1 (legacy)");
    plan_session_slots(sparse, 2, 0, 2, 0, &p);
    ck(p.local_slot == 2 && p.slot_count == 3, "seat 2 -> slot 2 of 3");
    ck(p.occupied == 0x5u && p.port[1] == -1, "the hole stays a hole");
}

/* The LAN room settles delay, rollback mode and prediction for both seats. */
static void case_lan_launch(void)
{
    RNetLanLobby room;
    printf("  LAN launch\n");
    memset(&room, 0, sizeof(room));
    snprintf(room.endpoint, sizeof(room.endpoint), "%s", "192.168.1.5:7777");
    room.host_slot = 1;          /* the host swapped into seat 1 */
    room.input_delay = 5;
    room.rollback = 1;
    room.input_prediction = 9;

    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    g_hosting_lan = 1;
    arm_lan_launch(&room);
    ck(g_lan_launch.enabled && g_lan_launch.local_slot == 0,
       "host first: the LAN host is slot 0");
    ck(g_lan_launch.slot_port_valid && g_lan_launch.slot_port[0] == 1 &&
       g_lan_launch.slot_port[1] == 0, "driving seat 1's port");
    ck(g_lan_launch.occupied_mask == 0x3u, "both seats occupied");
    ck(!strcmp(g_lan_launch.bind_hostport, "0.0.0.0:7777"), "host binds its port");
    ck(g_lan_launch.input_delay == 5, "the room's delay");
    ck(g_lan_launch.rollback == 1 && g_lan_launch.input_prediction == 9,
       "both peers use the room's mode and prediction runway");
    g_hosting_lan = 0;

    hooks_with(RECOMP_NETPLAY_SLOTS_SEAT, 2);
    g_hosting_lan = 1;
    arm_lan_launch(&room);
    ck(g_lan_launch.local_slot == 1, "seat policy: the LAN host is slot 1");
    ck(g_lan_launch.slot_port[1] == 1 && g_lan_launch.slot_port[0] == 0,
       "identity ports");
    g_hosting_lan = 0;
    g_joined_lan = 1;
    arm_lan_launch(&room);
    ck(g_lan_launch.local_slot == 0, "seat policy: the LAN guest is slot 0");
    ck(!strcmp(g_lan_launch.peer_hostport, "192.168.1.5:7777"),
       "the guest dials the room endpoint");
    g_joined_lan = 0;
}

static int fill_calls;
static void fill_hook(void *ctx, const RecompLauncherCSettings *settings,
                      RNetLobbyMatchCaps *caps)
{
    (void)ctx; (void)settings;
    fill_calls++;
    caps->input_delay = 99;      /* re-clamped after the hook */
    caps->ext.bytes[0] = 7;      /* the engine's own byte survives */
}

static void case_default_caps(void)
{
    RecompNetplayHostHooks h;
    RNetLobbyMatchCaps caps;
    printf("  default caps\n");
    memset(&h, 0, sizeof(h));
    h.game_name = "Test Game";
    h.max_players = 4;
    h.fill_match_caps = fill_hook;
    ck(recomp_netplay_host_init(&h) == 0, "init");
    caps = default_caps(NULL);
    ck(fill_calls == 1, "the engine's hook ran");
    ck(caps.valid && caps.rollback == 1, "rollback is the lobby default");
    ck(caps.input_delay == 20, "the hook's delay is clamped to 20");
    ck(caps.input_prediction == 0, "no runway until somebody sets one");
    ck(caps.ext.bytes[0] == 7, "ext is the engine's");
    ck(caps.mod_count == 0 && caps.mod_set[0] == '\0',
       "a vanilla build publishes an empty plan");

    ck(cb_rollback_set(NULL, 0) == 0, "rollback off before a room: pending");
    ck(cb_input_prediction_set(NULL, 40) == 0, "a runway before a room");
    caps = default_caps(NULL);
    ck(caps.rollback == 0, "the next create publishes rollback off");
    ck(caps.input_prediction == 16, "and the runway, clamped to 16");
    ck(cb_rollback_get(NULL) == 0 && cb_input_prediction_get(NULL) == 16,
       "and reads them back");
}

static void case_seat_ceiling(void)
{
    printf("  seat ceiling\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    ck(clamp_lobby_max_slots(8) == 4, "an N64-style title caps at 4");
    ck(clamp_lobby_max_slots(1) == 2, "and never below 2");
    hooks_with(RECOMP_NETPLAY_SLOTS_SEAT, 0);
    ck(clamp_lobby_max_slots(4) == 2, "an unset ceiling is two seats");

    /* create_max_slots: what the Host panel's Max Players offers. An
     * engine-wide max_players alone (SNES: 4 for a two-player title) answers
     * "unknown" online so the launcher keeps its num_players rule. */
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    ck(recomp_netplay_host_callbacks()->create_max_slots != NULL,
       "create_max_slots is wired");
    ck(recomp_netplay_host_callbacks()->create_max_slots(NULL, 0) == 0,
       "max_players alone is not a per-title offer (online: unknown)");
    ck(recomp_netplay_host_callbacks()->create_max_slots(NULL, 1) == 2,
       "LAN / Direct IP ceiling is two");
    ck(recomp_netplay_host_callbacks()->create_default_rollback(NULL, 4) == -1,
       "legacy titles keep the rollback default");
    ck(clamp_lobby_max_slots(8) == 4, "max_players still clamps create");

    /* netplay_max_players: the title declares its link size. */
    hooks_with_title(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 2, 4);
    ck(recomp_netplay_host_callbacks()->create_max_slots(NULL, 0) == 4,
       "a GBA link title offers four online");
    ck(recomp_netplay_host_callbacks()->create_max_slots(NULL, 1) == 2,
       "and two on LAN");
    ck(clamp_lobby_max_slots(4) == 4,
       "netplay_max_players replaces max_players as create's clamp");
    ck(clamp_lobby_max_slots(8) == 4, "and still caps create");
    g_h.netplay_delay_sync_from_players = 3;
    ck(recomp_netplay_host_callbacks()->create_default_rollback(NULL, 2) == 1,
       "two-seat rooms default to rollback");
    ck(recomp_netplay_host_callbacks()->create_default_rollback(NULL, 3) == 0 &&
       recomp_netplay_host_callbacks()->create_default_rollback(NULL, 4) == 0,
       "three and four-seat rooms default to delay-sync");
    hooks_with_title(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 0, 99);
    ck(recomp_netplay_host_callbacks()->create_max_slots(NULL, 0) ==
           RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS,
       "never past the lobby array");
}

static void case_table_and_names(void)
{
    const RecompLauncherCNetplayCallbacks *cb;
    RecompNetplayHostHooks h;
    printf("  callback table\n");
    memset(&h, 0, sizeof(h));
    ck(recomp_netplay_host_init(&h) == -1, "init refuses a nameless title");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);
    cb = recomp_netplay_host_callbacks();
    ck(cb != NULL, "a table after init");
    ck(cb && cb->rollback_get && cb->rollback_set && cb->input_prediction_get &&
       cb->input_prediction_set && cb->connecting && cb->name_rejected &&
       cb->need_mods_count && cb->need_mods_get && cb->need_mods_can_transfer &&
       cb->mod_xfer_progress,
       "the callbacks the SNES backend left NULL are wired");
    ck(cb && cb->host_can_spectate == NULL,
       "host_can_spectate stays NULL until an engine is measured doing it");
    ck(!strcmp(rnet_lobby_game_version(), "1.2.3"), "the pin reached the client");
    ck(cb_name_rejected(NULL, "fuck off") == 1, "a filtered name is refused");
    ck(cb_name_rejected(NULL, "Marisa") == 0, "an ordinary name is not");
    ck(cb_connecting(NULL) == 0, "not connecting while disconnected");
    ck(cb_need_mods_can_transfer(NULL) == 0,
       "no pre-seat transfer is offered (the transfer needs a seat)");
}

/* ---- a LAN / Direct IP room across a soft return ------------------------
 *
 * The rematch socket lifecycle, on loopback. The backend is one role per
 * process, so each case drives it in one role against a raw recomp-net peer
 * in the other: the backend HOSTING against a raw guest, then the backend
 * JOINED to a raw host. What was broken (docs/HOST_NETPLAY.md, "LAN / Direct
 * IP rematch"): after a match the host re-opened its listener with the old
 * guest still in its seat table, and the guest -- whose socket the launch
 * closed -- never asked for its seat back. */

static unsigned test_port_base(void)
{
    return 43800u + ((unsigned)getpid() % 600u) * 4u;
}

static void lan_hooks(const char *registry)
{
    RecompNetplayHostHooks h;
    memset(&h, 0, sizeof(h));
    h.game_name = "Test Game";
    h.game_version = "1.2.3";
    h.platform = "test";
    h.max_players = 2;
    h.lan_registry_path = registry;
    ck(recomp_netplay_host_init(&h) == 0, "init");
}

/* Pump the backend (the launcher's frame) and poll a raw guest's join. */
static int raw_join_while_pumping(RNetLanDirectGuest *g, RNetLanLobby *room)
{
    int i;
    int rc = RNET_LAN_DIRECT_PENDING;
    for (i = 0; i < 2000 && rc == RNET_LAN_DIRECT_PENDING; ++i) {
        cb_pump(NULL);
        rc = rnet_lan_direct_guest_join_poll(g, room);
        if (rc == RNET_LAN_DIRECT_PENDING)
            usleep(1000);
    }
    return rc;
}

static int raw_guest_hears_start(RNetLanDirectGuest *g, RNetLanLobby *room)
{
    int i;
    for (i = 0; i < 1000; ++i) {
        if (rnet_lan_direct_guest_pump(g, room, NULL) == 1)
            return 1;
        usleep(1000);
    }
    return 0;
}

static void case_lan_rematch_as_host(void)
{
    char registry[64];
    char ep[64];
    const unsigned port = test_port_base();
    RNetLanDirectGuest *g = NULL;
    RNetLanLobby groom;
    RecompLauncherCNetplayLaunch l;
    uint32_t first_sid;
    printf("  LAN rematch, backend hosting: the seat frees, the guest returns\n");
    snprintf(registry, sizeof(registry), "netplay_host_test_%u.txt", port);
    lan_hooks(registry);
    snprintf(ep, sizeof(ep), "127.0.0.1:%u", port);
    ck(cb_create(NULL, "Room", ep, "", NULL, 1, 2) == 0, "create a LAN room");
    ck(g_direct_host != NULL, "listening");

    ck(rnet_lan_direct_guest_join_begin(ep, "Test Game", "1.2.3", "", "Guesty",
                                        NULL, &g) == RNET_LAN_DIRECT_OK,
       "a guest asks");
    ck(g && raw_join_while_pumping(g, &groom) == RNET_LAN_DIRECT_OK, "seated");
    ck(cb_all_ready(NULL) == 1, "the room can start");
    ck(cb_request_start(NULL, NULL) == 0, "match 1 starts");
    ck(g_direct_host == NULL, "the launch closed the listener (the game "
                              "session takes the port)");
    ck(g && raw_guest_hears_start(g, &groom), "the guest hears START");
    memset(&l, 0, sizeof(l));
    ck(cb_fill_launch(NULL, &l) == 1, "the host's launch");
    first_sid = l.session_id;
    ck(first_sid != 0 && groom.session_id == first_sid,
       "both peers launch the host's session id");

    /* The match runs, and ends: the guest's socket is gone with it. */
    rnet_lan_direct_guest_close(&g);
    recomp_netplay_host_prepare_rematch();
    ck(g_direct_host != NULL, "the soft return re-opens the listener");
    ck(g_lan_room.joiner_name[0] == '\0', "and frees the joiner seat");
    ck(cb_all_ready(NULL) == 0, "so the room is NOT ready");
    ck(cb_request_start(NULL, NULL) == -1,
       "and the host cannot start match 2 alone (was: connect timeout)");
    ck(cb_in_lobby(NULL) == 1, "the host is still in its room");

    ck(rnet_lan_direct_guest_join_begin(ep, "Test Game", "1.2.3", "", "Guesty",
                                        NULL, &g) == RNET_LAN_DIRECT_OK,
       "the guest asks for its seat back");
    ck(g && raw_join_while_pumping(g, &groom) == RNET_LAN_DIRECT_OK,
       "and gets it (was: refused full)");
    ck(cb_all_ready(NULL) == 1, "the room is ready again");
    ck(cb_request_start(NULL, NULL) == 0, "match 2 starts");
    ck(g && raw_guest_hears_start(g, &groom), "the guest hears the rematch");
    memset(&l, 0, sizeof(l));
    ck(cb_fill_launch(NULL, &l) == 1, "the host's second launch");
    ck(l.session_id != 0 && l.session_id != first_sid,
       "with a fresh session id (a rematch never reuses the last one)");
    ck(groom.session_id == l.session_id, "that the guest heard too");

    rnet_lan_direct_guest_close(&g);
    (void)recomp_netplay_host_leave();
    remove(registry);
}

struct RawHostPump {
    RNetLanDirectHost *host;
    RNetLanLobby *room;
    volatile int stop;
};

static void *raw_host_pump_thread(void *p)
{
    struct RawHostPump *a = (struct RawHostPump *)p;
    while (!a->stop) {
        (void)rnet_lan_direct_host_pump(a->host, a->room, NULL);
        usleep(500);
    }
    return NULL;
}

static int backend_launches(void)
{
    int i;
    for (i = 0; i < 1000; ++i) {
        if (cb_launch_pending(NULL))
            return 1;
        usleep(1000);
    }
    return 0;
}

static void case_lan_rematch_as_guest(void)
{
    char registry[64];
    char ep[64];
    char id[80];
    char gbind[64];
    const unsigned port = test_port_base() + 2u;
    RNetLanDirectHost *host = NULL;
    RNetLanLobby hroom;
    RecompLauncherCNetplayLaunch l;
    int i;
    printf("  LAN rematch, backend joined: the guest re-joins by itself\n");
    snprintf(registry, sizeof(registry), "netplay_host_test_%u.txt", port);
    lan_hooks(registry);
    cb_set_player_name(NULL, "Guesty");
    snprintf(ep, sizeof(ep), "127.0.0.1:%u", port);
    snprintf(id, sizeof(id), "lan:%s", ep);
    snprintf(gbind, sizeof(gbind), "127.0.0.1:%u", port + 1u);

    memset(&hroom, 0, sizeof(hroom));
    snprintf(hroom.game, sizeof(hroom.game), "%s", "Test Game");
    snprintf(hroom.game_version, sizeof(hroom.game_version), "%s", "1.2.3");
    snprintf(hroom.host_name, sizeof(hroom.host_name), "%s", "Hostess");
    snprintf(hroom.endpoint, sizeof(hroom.endpoint), "%s", ep);
    ck(rnet_lan_direct_host_open(&host, ep, &hroom) == RNET_LAN_DIRECT_OK,
       "a raw host listens");
    {
        /* cb_join blocks until the host answers; pump it from a thread for
         * that call only. */
        struct RawHostPump a;
        pthread_t th;
        a.host = host;
        a.room = &hroom;
        a.stop = 0;
        (void)pthread_create(&th, NULL, raw_host_pump_thread, &a);
        ck(cb_join(NULL, id, "", gbind) == 0, "the backend joins");
        a.stop = 1;
        (void)pthread_join(th, NULL);
    }
    ck(strcmp(hroom.joiner_name, "Guesty") == 0, "seated at the host");

    hroom.started = 1;
    hroom.session_id = 0x51000001u;
    ck(rnet_lan_direct_host_notify_start(host, &hroom) == RNET_LAN_DIRECT_OK,
       "match 1 START");
    ck(backend_launches(), "the backend launches");
    memset(&l, 0, sizeof(l));
    ck(cb_fill_launch(NULL, &l) == 1 && l.session_id == 0x51000001u,
       "with the host's session id");
    ck(g_direct_guest == NULL, "the launch closed the guest's socket");

    /* The match ends; the host is slower getting back than the guest. */
    rnet_lan_direct_host_close(&host);
    recomp_netplay_host_prepare_rematch();
    ck(g_direct_rejoin == 1 && g_direct_guest != NULL,
       "the soft return starts a re-join (was: nothing, the guest sat in a "
       "room it had no socket to)");
    for (i = 0; i < 30; ++i) {
        cb_pump(NULL);
        usleep(2000);
    }
    ck(g_direct_rejoin == 1, "it keeps asking while the host is away");
    ck(cb_in_lobby(NULL) == 1, "still in the room meanwhile");
    ck(lan_chat_available() == 0, "with no chat until re-seated");

    hroom.started = 0;
    hroom.joiner_name[0] = '\0'; /* what the backend's host does on re-open */
    ck(rnet_lan_direct_host_open(&host, ep, &hroom) == RNET_LAN_DIRECT_OK,
       "the host listens again");
    for (i = 0; i < 2000 && g_direct_rejoin; ++i) {
        (void)rnet_lan_direct_host_pump(host, &hroom, NULL);
        cb_pump(NULL);
        usleep(1000);
    }
    ck(!g_direct_rejoin && g_direct_guest != NULL, "re-seated");
    ck(strcmp(hroom.joiner_name, "Guesty") == 0, "in its seat at the host");
    ck(lan_chat_available() == 1, "chat is back");

    /* What the backend's host really sends at START: the ROOM refresh
     * (started=1, no session id) FIRST, then START. A guest that armed on
     * ROOM launched with session 1 while the host ran a fresh id. */
    hroom.started = 1;
    hroom.session_id = 0x51000002u;
    ck(rnet_lan_direct_host_notify_room(host, &hroom) == RNET_LAN_DIRECT_OK,
       "match 2 ROOM refresh (started)");
    for (i = 0; i < 50; ++i) {
        cb_pump(NULL);
        ck(!cb_launch_pending(NULL),
           "ROOM alone does not launch (it carries no session id)");
        usleep(1000);
    }
    ck(rnet_lan_direct_host_notify_start(host, &hroom) == RNET_LAN_DIRECT_OK,
       "match 2 START");
    ck(backend_launches(), "the backend launches the rematch");
    memset(&l, 0, sizeof(l));
    ck(cb_fill_launch(NULL, &l) == 1 && l.session_id == 0x51000002u,
       "with the rematch's own session id");

    /* A host that never comes back: the guest gives up, loudly. */
    rnet_lan_direct_host_close(&host);
    g_lan_rejoin_window_ms = 150;
    recomp_netplay_host_prepare_rematch();
    for (i = 0; i < 400 && g_direct_rejoin; ++i) {
        cb_pump(NULL);
        usleep(1000);
    }
    ck(!g_direct_rejoin && !cb_in_lobby(NULL),
       "past the window the guest leaves the room");
    ck(!strcmp(cb_last_error(NULL), "lan_rejoin_timeout"),
       "and the room says why");
    g_lan_rejoin_window_ms = 30000;
    cb_clear_last_error(NULL);
    (void)recomp_netplay_host_leave();
    remove(registry);
}

#ifdef RNET_HAS_SESSION_VARIANT
static void case_session_variant(void)
{
    static const RecompNetplaySessionVariant variants[] = {{0,"Cable"},{7,"Radio"}};
    RecompNetplayHostHooks h;
    RecompLauncherCNetplayLaunch launch;
    RNetLanLobby room;
    memset(&h, 0, sizeof(h));
    h.game_name = "Variant Test";
    h.game_version = "1";
    h.session_variants = variants;
    h.session_variant_count = 2;
    ck(recomp_netplay_host_init(&h) == 0, "variant backend init");
    ck(cb_session_variant_set(NULL, 7) == 0, "host selects a supported variant");
    ck(default_caps(NULL).session_variant == 7, "online caps carry the host choice");
    ck(cb_session_variant_set(NULL, 1) == -1, "wire value is not an array index");
    g_joined_lan = 1;
    g_lan_room.session_variant = 0;
    ck(cb_session_variant_set(NULL, 0) == -1 && g_session_variant == 7,
       "guest cannot change host choice or mutate pending settings");
    ck(cb_session_variant_get(NULL) == 0, "guest sees the host's variant");
    memset(&room, 0, sizeof(room));
    snprintf(room.endpoint, sizeof(room.endpoint), "127.0.0.1:7777");
    room.session_variant = 7;
    arm_lan_launch(&room);
    ck(cb_fill_launch(NULL, &launch) == 1 && launch.session_variant == 7,
       "LAN launch inherits the settled host variant");
    room.session_variant = 9;
    arm_lan_launch(&room);
    ck(cb_fill_launch(NULL, &launch) == 0, "unsupported hardware refuses to launch");
    g_joined_lan = 0;
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    recomp_netplay_host_shutdown();
}
#endif

#ifdef RUI_HAVE_ICE_HUB
static void case_ice_relay(void)
{
    RNetHostIceStatus st;
    RecompLauncherCNetplayLaunch l;
    char line[512];
    printf("  host relay over ICE\n");
    hooks_with(RECOMP_NETPLAY_SLOTS_HOST_FIRST, 4);

    /* The pref reaches the caps the host publishes; default on. */
    rnet_lobby_set_relay_via_ice(1);
    ck(default_caps(NULL).relay_via_ice == 1, "caps carry relay_via_ice on");
    ck(cb_relay_via_ice_set(NULL, 0) == 0, "set off before a room: pending");
    ck(default_caps(NULL).relay_via_ice == 0, "caps carry relay_via_ice off");
    ck(cb_relay_via_ice_get(NULL) == 0,
       "getter answers the pref");
    (void)cb_relay_via_ice_set(NULL, 1);

    /* cb_fill_launch's copy of join.transport_ice_hub needs a live lobby
     * launch (recomp-net's join record is private), so it is not driven here. */
    memset(&l, 0, sizeof(l));
    ck(l.transport_ice_hub == 0, "launch record starts without the hub");

    /* Status line: idle -> nothing. */
    memset(&st, 0, sizeof(st));
    ck(recomp_netplay_ice_status_line(&st, line, sizeof(line)) == 0 && !line[0],
       "idle: no line");

    /* Host, three seats: connecting / connected direct / failed. */
    st.role = 1; st.peer_count = 3; st.completed = 1;
    st.peer[0].slot = 1; st.peer[0].state = RNET_ICE_STATE_CONNECTING;
    st.peer[1].slot = 2; st.peer[1].state = RNET_ICE_STATE_COMPLETED;
    strcpy(st.peer[1].path, "srflx");
    st.peer[2].slot = 3; st.peer[2].state = RNET_ICE_STATE_FAILED;
    ck(recomp_netplay_ice_status_line(&st, line, sizeof(line)) == 1, "host line");
    ck(strstr(line, "Seat 2: connecting") != NULL, "seat 2 connecting");
    ck(strstr(line, "Seat 3: connected (srflx)") != NULL, "seat 3 srflx");
    ck(strstr(line, "Seat 4: failed") != NULL, "seat 4 failed");
    ck(strstr(line, "forward") == NULL, "never says forward");

    st.completed = 3;
    st.peer[0].state = RNET_ICE_STATE_COMPLETED;
    strcpy(st.peer[0].path, "host");
    st.peer[2].state = RNET_ICE_STATE_COMPLETED;
    strcpy(st.peer[2].path, "prflx");
    recomp_netplay_ice_status_line(&st, line, sizeof(line));
    ck(strstr(line, "Seat 2: connected (host)") != NULL &&
       strstr(line, "Seat 4: connected (prflx)") != NULL, "host/prflx words");
    ck(strstr(line, "Play starts once") == NULL, "no waiting hint when all up");

    /* Guest. */
    memset(&st, 0, sizeof(st));
    st.role = 2; st.peer_count = 1;
    st.peer[0].state = RNET_ICE_STATE_CONNECTING;
    recomp_netplay_ice_status_line(&st, line, sizeof(line));
    ck(strstr(line, "connecting") != NULL, "guest connecting");
    st.peer[0].state = RNET_ICE_STATE_COMPLETED; strcpy(st.peer[0].path, "direct");
    recomp_netplay_ice_status_line(&st, line, sizeof(line));
    ck(strstr(line, "connected (direct)") != NULL, "guest connected direct");
    st.peer[0].state = RNET_ICE_STATE_FAILED;
    recomp_netplay_ice_status_line(&st, line, sizeof(line));
    ck(strstr(line, "failed") != NULL, "guest failed");

    /* Truncation stays terminated. */
    {
        char tiny[12];
        st.role = 1; st.peer_count = 1; st.peer[0].slot = 1;
        recomp_netplay_ice_status_line(&st, tiny, sizeof(tiny));
        ck(memchr(tiny, 0, sizeof(tiny)) != NULL, "tiny buffer terminated");
    }
}
#endif

int main(void)
{
    case_host_first_standard_and_swapped();
    case_host_first_sparse_four();
    case_host_first_host_in_gallery();
    case_seat_policy();
    case_lan_launch();
    case_default_caps();
    case_seat_ceiling();
    case_table_and_names();
    case_lan_rematch_as_host();
    case_lan_rematch_as_guest();
#ifdef RNET_HAS_SESSION_VARIANT
    case_session_variant();
#endif
#ifdef RUI_HAVE_ICE_HUB
    case_ice_relay();
#endif
    printf(fails ? "\n%d failure(s)\n" : "\nall netplay host cases passed\n",
           fails);
    return fails != 0;
}
