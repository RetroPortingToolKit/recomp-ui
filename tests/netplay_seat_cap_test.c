/* Netplay room seat ceiling vs. local controller cards.
 *
 * A GBA link title draws ONE controller card (num_players 1, profile
 * max_players 1) and seats up to four players online. The launcher learns the
 * online ceiling from the backend's create_max_slots(ctx, lan_only) -- which
 * recomp_netplay_host answers from hooks.netplay_max_players, 2 on LAN, and
 * 0 ("unknown") online when the engine left that unset -- and must
 *   - offer 2..that ceiling for an online room, 2 for a LAN / Direct IP one;
 *   - never add a controller card for it;
 *   - keep every backend without the callback on the old num_players rule.
 * Plus recomp_launcher_netplay_dense_position(): sparse lobby seats become
 * dense bus positions in seat order.
 *
 * Includes the model translation unit (as genesis_seats_test.c does). */
#include "launcher_model.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;

static void expect_int(int got, int want, const char* what) {
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
    ++fails;
}

/* recomp_netplay_host's answer shape: online = hooks.netplay_max_players
 * (0 when unset), LAN = 2. */
static int g_online_cap;
static int g_calls;
static int g_delay_sync_from;
static int fake_create_max_slots(void* ctx, int lan_only) {
    (void)ctx;
    ++g_calls;
    return lan_only ? 2 : g_online_cap;
}
static int fake_default_rollback(void* ctx, int max_slots) {
    (void)ctx;
    return max_slots >= g_delay_sync_from ? 0 : 1;
}

typedef struct Seats {
    int player_count;   /* m->player_count (local cards before multitap) */
    int visible;        /* launcher_model_visible_player_count */
    int host_default;   /* m->netplay_host_max_players after init */
    int online;         /* room ceiling, online */
    int lan;            /* room ceiling, LAN / Direct IP */
    int rollback;       /* initial online mode */
} Seats;

typedef enum { SYS_GBA, SYS_N64 } Sys;

static Seats seats_for(Sys sys, int num_players, int with_callback,
                       int online_cap) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    static RecompLauncherCNetplayCallbacks np;
    Seats out;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    memset(&out, 0, sizeof(out));
    if (!m) { fprintf(stderr, "FAIL: out of memory\n"); ++fails; return out; }

    memset(&game, 0, sizeof(game));
    memset(&io, 0, sizeof(io));
    memset(&np, 0, sizeof(np));
    if (sys == SYS_GBA) launcher_profile_apply_gba(&game);
    else                launcher_profile_apply_n64(&game);
    game.name = "Seat Cap Fixture";
    if (num_players > 0) game.num_players = num_players;
    g_online_cap = online_cap;
    if (with_callback) np.create_max_slots = fake_create_max_slots;
    if (g_delay_sync_from > 0) np.create_default_rollback = fake_default_rollback;
    game.netplay = &np;
    game.netplay_supported = 1;

    launcher_model_init(m, &io, &game, NULL);
    out.player_count = m->player_count;
    out.visible = launcher_model_visible_player_count(m);
    out.host_default = m->netplay_host_max_players;
    out.online = launcher_model_netplay_room_max_players(m, 0);
    out.lan = launcher_model_netplay_room_max_players(m, 1);
    out.rollback = m->netplay_rollback ? 1 : 0;
    free(m);
    return out;
}

static void case_gba_legacy(void) {
    /* An engine / backend that predates the callback: exactly as before. */
    Seats s = seats_for(SYS_GBA, 0, /*callback=*/0, 0);
    expect_int(s.player_count, 1, "gba legacy: one controller card");
    expect_int(s.visible, 1, "gba legacy: one visible card");
    expect_int(s.online, 2, "gba legacy: online room is two seats");
    expect_int(s.lan, 2, "gba legacy: LAN room is two seats");
    expect_int(s.host_default, 2, "gba legacy: host default two");
}

static void case_gba_backend_two(void) {
    /* A title that declares a two-console link. */
    Seats s = seats_for(SYS_GBA, 0, 1, 2);
    expect_int(s.online, 2, "gba cap 2: online two seats");
    expect_int(s.lan, 2, "gba cap 2: LAN two seats");
    expect_int(s.host_default, 2, "gba cap 2: host default two");
    expect_int(s.player_count, 1, "gba cap 2: still one card");
}

static void case_gba_four_link(void) {
    g_delay_sync_from = 3;
    Seats s = seats_for(SYS_GBA, 0, 1, 4);
    expect_int(s.online, 4, "gba cap 4: online room offers four seats");
    expect_int(s.lan, 2, "gba cap 4: LAN / Direct IP stays two seats");
    expect_int(s.host_default, 4, "gba cap 4: host default is the online cap");
    expect_int(s.player_count, 1, "gba cap 4: NO extra controller card");
    expect_int(s.visible, 1, "gba cap 4: one visible card");
    expect_int(s.rollback, 0, "gba cap 4: delay-sync initially");
    s = seats_for(SYS_GBA, 0, 1, 2);
    expect_int(s.rollback, 1, "gba cap 2: rollback initially");
    g_delay_sync_from = 0;
}

static void case_backend_bounds(void) {
    Seats s = seats_for(SYS_GBA, 0, 1, 99);
    expect_int(s.online, RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS,
               "cap above the lobby array clamps to MAX_MEMBERS");
    s = seats_for(SYS_GBA, 0, 1, 0);
    expect_int(s.online, 2, "callback answering 0 (unknown) keeps the old rule");
    s = seats_for(SYS_GBA, 0, 1, 1);
    expect_int(s.online, 2, "callback answering 1 keeps the old rule");
}

static void case_n64(void) {
    /* A four-pad console without the callback: old rule, both kinds. */
    Seats s = seats_for(SYS_N64, 4, 0, 0);
    expect_int(s.player_count, 4, "n64 legacy: four cards");
    expect_int(s.online, 4, "n64 legacy: online four seats");
    expect_int(s.lan, 4, "n64 legacy: LAN four (backend clamps; unchanged)");
    expect_int(s.host_default, 4, "n64 legacy: host default four");
    /* Same title on recomp_netplay_host declaring netplay_max_players 4. */
    s = seats_for(SYS_N64, 4, 1, 4);
    expect_int(s.online, 4, "n64 backend: online four seats");
    expect_int(s.lan, 2, "n64 backend: LAN is the two seats create gives");
    expect_int(s.player_count, 4, "n64 backend: cards unchanged");
}

static void case_snes_engine_wide_ceiling(void) {
    /* snesrecomp on recomp_netplay_host: hooks.max_players = 4 for every
     * title, netplay_max_players unset -> online answers 0. A two-player
     * SNES title must keep offering two, and GBA today (unset) too. */
    Seats s = seats_for(SYS_GBA, 0, 1, 0);
    expect_int(s.online, 2, "unset netplay_max_players: GBA online stays two");
    expect_int(s.lan, 2, "unset netplay_max_players: GBA LAN two");
    expect_int(s.host_default, 2, "unset netplay_max_players: GBA host default two");
    s = seats_for(SYS_N64, 2, 1, 0);
    expect_int(s.online, 2, "engine-wide ceiling only: two-player title offers two");
    expect_int(s.lan, 2, "engine-wide ceiling only: LAN two");
}

static void case_no_netplay(void) {
    static RecompLauncherCGameInfo game;
    static RecompLauncherCSettings io;
    static RecompLauncherCNetplayCallbacks np;
    LauncherModel* m = (LauncherModel*)calloc(1, sizeof(LauncherModel));
    if (!m) { ++fails; return; }
    memset(&game, 0, sizeof(game));
    memset(&io, 0, sizeof(io));
    memset(&np, 0, sizeof(np));
    launcher_profile_apply_gba(&game);
    game.name = "No Netplay";
    np.create_max_slots = fake_create_max_slots;
    g_online_cap = 4;
    game.netplay = &np;
    game.netplay_supported = 0;   /* table present, netplay off */
    g_calls = 0;
    launcher_model_init(m, &io, &game, NULL);
    expect_int(launcher_model_netplay_room_max_players(m, 0), 2,
               "netplay_supported 0: callback not consulted");
    expect_int(g_calls, 0, "netplay_supported 0: callback never called");
    free(m);
    expect_int(launcher_model_netplay_room_max_players(NULL, 0), 2,
               "NULL model: two seats");
}

/* ---- dense bus positions ------------------------------------------------ */

static RecompLauncherCNetplayLaunch launch_host_first(const int* ports, int n) {
    RecompLauncherCNetplayLaunch l;
    int i;
    memset(&l, 0, sizeof(l));
    l.enabled = 1;
    l.player_count = n;
    l.max_slots = 4;
    l.occupied_mask = (1u << n) - 1u;
    l.slot_port_valid = 1;
    for (i = 0; i < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1; ++i)
        l.slot_port[i] = i < n ? ports[i] : -1;
    return l;
}

static void case_dense_positions(void) {
    /* Host in seat 0, guests in seats 1 and 3 (seat 2 empty). recomp_netplay_
     * host HOST_FIRST gives slots 0,1,2 with slot_port {0,1,3}. */
    {
        const int ports[3] = { 0, 1, 3 };
        RecompLauncherCNetplayLaunch l = launch_host_first(ports, 3);
        expect_int(recomp_launcher_netplay_dense_position(&l, 0), 0, "seats 0,1,3: seat 0 -> pos 0");
        expect_int(recomp_launcher_netplay_dense_position(&l, 1), 1, "seats 0,1,3: seat 1 -> pos 1");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), 2, "seats 0,1,3: seat 3 -> pos 2");
        expect_int(recomp_launcher_netplay_dense_position(&l, 3), -1, "seats 0,1,3: no slot 3");
        expect_int(recomp_launcher_netplay_dense_position(&l, -1), -1, "negative slot");
    }
    /* Host moved to seat 2; guests in 0 and 3. Host stays session slot 0 but
     * is bus position 1 (seat order 0,2,3). */
    {
        const int ports[3] = { 2, 0, 3 };
        RecompLauncherCNetplayLaunch l = launch_host_first(ports, 3);
        expect_int(recomp_launcher_netplay_dense_position(&l, 0), 1, "host in seat 2 -> pos 1");
        expect_int(recomp_launcher_netplay_dense_position(&l, 1), 0, "seat 0 -> pos 0 (parent)");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), 2, "seat 3 -> pos 2");
    }
    /* Full four. */
    {
        const int ports[4] = { 1, 0, 2, 3 };
        RecompLauncherCNetplayLaunch l = launch_host_first(ports, 4);
        int i, sum = 0;
        for (i = 0; i < 4; ++i) sum += 1 << recomp_launcher_netplay_dense_position(&l, i);
        expect_int(sum, 0xF, "four seated: positions are a permutation of 0..3");
        expect_int(recomp_launcher_netplay_dense_position(&l, 0), 1, "host in seat 1 -> pos 1");
    }
    /* Host in the gallery: slot 0 has no port and no position. */
    {
        const int ports[3] = { -1, 1, 3 };
        RecompLauncherCNetplayLaunch l = launch_host_first(ports, 3);
        expect_int(recomp_launcher_netplay_dense_position(&l, 0), -1, "gallery host: no position");
        expect_int(recomp_launcher_netplay_dense_position(&l, 1), 0, "gallery host: seat 1 -> pos 0");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), 1, "gallery host: seat 3 -> pos 1");
    }
    /* SEAT policy: identity ports, holes -1, player_count reaches the top seat. */
    {
        const int ports[4] = { 0, 1, -1, 3 };
        RecompLauncherCNetplayLaunch l = launch_host_first(ports, 4);
        l.occupied_mask = 0xBu;
        expect_int(recomp_launcher_netplay_dense_position(&l, 3), 2, "SEAT: seat 3 -> pos 2");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), -1, "SEAT: hole has no position");
    }
    /* No slot_port: occupied_mask decides, 0 = all occupied. */
    {
        RecompLauncherCNetplayLaunch l;
        memset(&l, 0, sizeof(l));
        l.player_count = 4;
        l.occupied_mask = 0xBu; /* seats 0,1,3 */
        expect_int(recomp_launcher_netplay_dense_position(&l, 3), 2, "mask 0xB: seat 3 -> pos 2");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), -1, "mask 0xB: seat 2 empty");
        l.occupied_mask = 0;
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), 2, "mask 0: identity");
        l.player_count = 0;
        l.max_slots = 2;
        expect_int(recomp_launcher_netplay_dense_position(&l, 1), 1, "player_count 0 falls back to max_slots");
        expect_int(recomp_launcher_netplay_dense_position(&l, 2), -1, "beyond max_slots");
    }
    expect_int(recomp_launcher_netplay_dense_position(NULL, 0), -1, "NULL launch");
}

int main(void) {
    case_gba_legacy();
    case_gba_backend_two();
    case_gba_four_link();
    case_backend_bounds();
    case_n64();
    case_snes_engine_wide_ceiling();
    case_no_netplay();
    case_dense_positions();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
