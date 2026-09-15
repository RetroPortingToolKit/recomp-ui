// proto_main.c — standalone self-test driver for recomp-ui.
#include <stdlib.h>
//
// Console-agnostic by construction: it fabricates the SAME C ABI structs any
// consuming host passes (here seeded with a neutral placeholder game), then
// runs the compiled-in Dear ImGui backend. This is the harness the recomp-ui
// repo itself builds to self-verify; a real host implements its own main()
// that calls recomp_launcher_run_window() (see recomp_launcher.h) with its
// own game facts.

#include "launcher_backend.h"
#include "launcher_binds.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_profile.h"
#include "launcher_theme.h"

#include <stdio.h>
#include <string.h>

// Harness-only Transfer Pak "cartridge brain". A real N64 host (PSR sniffs the
// GB header @0x134 + decodes the Gen-1 save; PMS-J uses the kana charmap) fills
// these facts; here we fabricate them from the ROM file name so the dashboard's
// Transfer Pak card exercises every populated state without a real cart.
static int proto_tpak_inspect(const char* rom_path, const char* save_path,
                              RecompLauncherCTpak* out) {
    (void)save_path;
    memset(out, 0, sizeof(*out));
    if (!rom_path || !rom_path[0]) return 0;   // empty slot => nothing inserted
    out->valid = 1;
    int kind = 0; const char* label = "Game Boy Cartridge";
    if      (strstr(rom_path, "red"))    { kind = 1; label = "Pokemon Red"; }
    else if (strstr(rom_path, "blue"))   { kind = 2; label = "Pokemon Blue"; }
    else if (strstr(rom_path, "yellow")) { kind = 3; label = "Pokemon Yellow"; }
    else if (strstr(rom_path, "green"))  { kind = 4; label = "Pokemon Green"; }
    out->cart_kind = kind;
    snprintf(out->cart_label,   sizeof(out->cart_label),   "%s", label);
    snprintf(out->trainer_name, sizeof(out->trainer_name), "SATOSHI");
    snprintf(out->trainer_id,   sizeof(out->trainer_id),   "12345");
    return 1;
}

/* Harness stand-ins for the codegen host's setup callbacks (LNG_DEMO_SETUP).
 * Mirrors psxrecomp's ae_bios_verify: the empty path is bundled OpenBIOS, a
 * path naming "linked" is compiled into this build, anything else is a valid
 * retail dump with no backend linked yet -> needs_regen. */
static int proto_bios_verify(const char* path, RecompLauncherCBiosVerify* out) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        out->ok = 1;
        snprintf(out->detail, sizeof(out->detail), "Using bundled OpenBIOS.");
        return 1;
    }
    if (strstr(path, "linked")) {
        out->ok = 1;
        snprintf(out->detail, sizeof(out->detail), "SCPH1001.BIN (CRC OK).");
        return 1;
    }
    out->needs_regen = 1;
    snprintf(out->detail, sizeof(out->detail),
             "This BIOS is not compiled into the current build. "
             "Generate & rebuild to switch (or use OpenBIOS).");
    return 1;
}

static int proto_prepare(const char* source_path, char* out_path, size_t out_cap,
                         char* err_msg, size_t err_cap,
                         RecompLauncherCPrepareProgressFn on_progress,
                         void* progress_ctx) {
    (void)err_msg; (void)err_cap;
    if (on_progress) on_progress(progress_ctx, 0.5f, "Harness: no real SDK here…");
    snprintf(out_path, out_cap, "%s", source_path ? source_path : "");
    return 1;
}


/* ---- LNG_DEMO_LOBBY: a fake seated lobby for layout work ------------------
 * LNG_DEMO_LOBBY=host|guest. No network: these callbacks answer as if this
 * client were seated in a 4-seat room with two other players (one seat
 * open), so the full-screen lobby view can be exercised — and screenshotted
 * through LNG_SCRIPT — without a server. Nothing here reaches the launcher
 * unless the variable is set. */
static int demo_lobby_host  = 1;
static int demo_lobby_in    = 1;
static int demo_lobby_share = 1;  /* P2 offers its memory card */
static int demo_lobby_allow = 1;  /* host allows guest cards */
static int demo_lobby_delay = 6;
static int demo_lobby_pred  = 10;
static int demo_lobby_rb    = 1;
static const char* dl_default_url(void* c) { (void)c; return "ws://netplay.retcomm.net:8765"; }
static void dl_set_lobby_url(void* c, const char* u) { (void)c; (void)u; }
static int  dl_connect(void* c) { (void)c; return 0; }
/* Seats are mutable so self-moves and swaps show. Reimu watches from the
 * gallery (base 64, two seats). Swaps: an ask of ours is accepted by the
 * fake peer after a moment; LNG_DEMO_SWAP_ASK=<seconds> makes Reimu ask US
 * for our seat every that-many seconds (the incoming prompt + the decline
 * window). */
#define DEMO_SPEC_BASE 64
static int    demo_seats[3] = { 0, 1, DEMO_SPEC_BASE };
static int    demo_swap_out = 0;        /* 0 idle, 1 waiting, 2 accepted, -1 declined */
static int    demo_swap_target = -1;
static double demo_swap_out_at = 0.0;
static int    demo_swap_in = 0;
static double demo_swap_ask_every = 0.0;
static double demo_swap_next_ask = 0.0;
static int demo_local_index(void) { return demo_lobby_host ? 0 : 1; }
static int demo_index_at_seat(int seat) {
    for (int i = 0; i < 3; ++i) if (demo_seats[i] == seat) return i;
    return -1;
}
static void demo_swap_seats(int a, int b) {
    if (a < 0 || b < 0 || a > 2 || b > 2) return;
    int t = demo_seats[a]; demo_seats[a] = demo_seats[b]; demo_seats[b] = t;
}
static void demo_lobby_pump(void) {
    const double now = (double)SDL_GetTicks() / 1000.0;
    if (demo_swap_out == 1 && now >= demo_swap_out_at) {
        demo_swap_seats(demo_local_index(), demo_index_at_seat(demo_swap_target));
        demo_swap_out = 2;
    }
    if (demo_swap_ask_every > 0.0 && !demo_swap_in && now >= demo_swap_next_ask) {
        demo_swap_in = 1;
        demo_swap_next_ask = now + demo_swap_ask_every;
    }
}
static int dl_seat_move_self(void* c, int to) {
    (void)c;
    if (demo_index_at_seat(to) >= 0) return -1;
    if (!(to >= 0 && to < 4) && !(to >= DEMO_SPEC_BASE && to < DEMO_SPEC_BASE + 2)) return -1;
    demo_seats[demo_local_index()] = to;
    return 0;
}
static int dl_seat_swap_request(void* c, int to) {
    (void)c;
    if (demo_swap_out == 1) return -1;
    if (demo_index_at_seat(to) < 0 || demo_index_at_seat(to) == demo_local_index()) return -1;
    demo_swap_target = to;
    demo_swap_out = 1;
    demo_swap_out_at = (double)SDL_GetTicks() / 1000.0 + 1.5;
    return 0;
}
static int dl_seat_swap_incoming(void* c, char* who, size_t cap, int* from) {
    (void)c;
    if (!demo_swap_in) return 0;
    if (who && cap) snprintf(who, cap, "Reimu");
    if (from) *from = demo_seats[2];
    return 1;
}
static int dl_seat_swap_respond(void* c, int accept) {
    (void)c;
    if (!demo_swap_in) return -1;
    demo_swap_in = 0;
    fprintf(stderr, "[proto] swap ask answered: %s\n", accept ? "swap" : "keep");
    if (accept) demo_swap_seats(demo_local_index(), 2);
    return 0;
}
static int  dl_seat_swap_outgoing(void* c) { (void)c; return demo_swap_out; }
static void dl_seat_swap_clear(void* c) { (void)c; if (demo_swap_out != 1) demo_swap_out = 0; }
static int  dl_host_can_spectate(void* c) { (void)c; return 1; }
static int  dl_lobby_allow_spectators(void* c) { (void)c; return 1; }
static int  dl_lobby_max_spectators(void* c) { (void)c; return 2; }
static int  dl_lobby_spectator_count(void* c) {
    (void)c;
    int n = 0;
    for (int i = 0; i < 3; ++i) if (demo_seats[i] >= DEMO_SPEC_BASE) ++n;
    return n;
}
static int  dl_local_is_spectator(void* c) { (void)c; return demo_seats[demo_local_index()] >= DEMO_SPEC_BASE; }
static int  dl_spectator_slot(void* c, int index) { (void)c; return DEMO_SPEC_BASE + index; }
static int  dl_connected(void* c) { (void)c; return 1; }
static void dl_pump(void* c) { (void)c; demo_lobby_pump(); }
static void dl_set_player_name(void* c, const char* n) { (void)c; (void)n; }
static int demo_acct_preview = 0; /* LNG_DEMO_ACCOUNT set: leave the name empty
                                   * so the Player Name modal opens by itself
                                   * and the sign-in section is screenshottable */
static const char* dl_player_name(void* c) {
    (void)c;
    if (demo_acct_preview) return "";
    return demo_lobby_host ? "Alex" : "Marisa";
}
static void dl_request_list(void* c) { (void)c; }
static int  dl_list_count(void* c) { (void)c; return 1; }
static int  dl_list_get(void* c, int i, RecompLauncherCNetplayLobby* o) {
    (void)c;
    if (i != 0 || !o) return 0;
    memset(o, 0, sizeof(*o));
    snprintf(o->lobby_id, sizeof(o->lobby_id), "demo-1");
    snprintf(o->name, sizeof(o->name), "Reimu's Lobby");
    snprintf(o->game_name, sizeof(o->game_name), "Recomp UI Test");
    o->player_count = 1; o->max_slots = 2; o->latency_ms = 91;
    snprintf(o->host_country, sizeof(o->host_country), "JP");
    o->allow_spectators = 1; o->max_spectators = 4; o->spectator_count = 1;
    return 1;
}
/* Players online: a few names so the browser's side panel has rows. */
static int  dl_online_count(void* c) { (void)c; return 4; }
static int  dl_online_get(void* c, int i, RecompLauncherCNetplayOnlinePlayer* o) {
    static const char* names[4] = { "Reimu", "Marisa", "Alex", "Sakuya" };
    static const char* countries[4] = { "JP", "JP", "CA", "DE" };
    (void)c;
    if (i < 0 || i >= 4 || !o) return 0;
    memset(o, 0, sizeof(*o));
    snprintf(o->display_name, sizeof(o->display_name), "%s", names[i]);
    snprintf(o->country, sizeof(o->country), "%s", countries[i]);
    if (i == 0) { o->in_lobby = 1; o->hosting = 1; snprintf(o->lobby_name, sizeof(o->lobby_name), "Reimu's Lobby"); }
    if (i == 1) { o->in_lobby = 1; snprintf(o->lobby_name, sizeof(o->lobby_name), "Reimu's Lobby"); }
    o->is_local = (i == 2);
    return 1;
}
/* Server chat: its own small ring; a send lands locally as if echoed. */
static struct { char from[64]; char text[256]; int is_local; } demo_schat[32];
static int demo_schat_n = 0;
static void demo_schat_push(const char* from, const char* text, int is_local) {
    if (demo_schat_n >= 32) { memmove(&demo_schat[0], &demo_schat[1], sizeof(demo_schat[0]) * 31); demo_schat_n = 31; }
    snprintf(demo_schat[demo_schat_n].from, sizeof(demo_schat[demo_schat_n].from), "%s", from);
    snprintf(demo_schat[demo_schat_n].text, sizeof(demo_schat[demo_schat_n].text), "%s", text);
    demo_schat[demo_schat_n].is_local = is_local;
    ++demo_schat_n;
}
static int  dl_schat_send(void* c, const char* t) { (void)c; demo_schat_push("Alex", t, 1); return 0; }
static int  dl_schat_count(void* c) { (void)c; return demo_schat_n; }
static int  dl_schat_get(void* c, int i, RecompLauncherCNetplayChatMessage* out) {
    (void)c;
    if (i < 0 || i >= demo_schat_n || !out) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->from, sizeof(out->from), "%s", demo_schat[i].from);
    snprintf(out->text, sizeof(out->text), "%s", demo_schat[i].text);
    out->is_local = demo_schat[i].is_local;
    out->seq = (uint32_t)(i + 1);
    return 1;
}
static int  dl_leave(void* c) { (void)c; demo_lobby_in = 0; return 0; }
static int  dl_in_lobby(void* c) { (void)c; return demo_lobby_in; }
static int  dl_is_host(void* c) { (void)c; return demo_lobby_host; }
static int  dl_member_count(void* c) { (void)c; return 3; }
static int  dl_member_get(void* c, int i, RecompLauncherCNetplayMember* out) {
    static const char* names[3] = { "Alex", "Marisa", "Reimu" };
    (void)c;
    if (i < 0 || i >= 3 || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->slot = demo_seats[i];
    out->is_spectator = demo_seats[i] >= DEMO_SPEC_BASE;
    snprintf(out->display_name, sizeof(out->display_name), "%s", names[i]);
    out->ready = 1;
    out->is_host = (i == 0);
    out->is_local = demo_lobby_host ? (i == 0) : (i == 1);
    out->latency_ms = out->is_local ? -1 : 38 + i * 21;
    out->bios_offer_valid = 1;
    out->bios_can_scph1001 = (i != 2);
    out->bios_prefer_openbios = 0;
    if (i == 1) {
        out->memcard_offer_valid = 1;
        out->memcard_has_card = 1;
        out->memcard_share = demo_lobby_share;
    }
    {
        static const char* countries[3] = { "CA", "JP", "DE" };
        snprintf(out->country, sizeof(out->country), "%s", countries[i]);
    }
    return 1;
}
static int  dl_move_member(void* c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
static int  dl_local_ready(void* c) { (void)c; return 1; }
static int  dl_all_ready(void* c) { (void)c; return 1; }
static int  dl_set_ready(void* c, int r) { (void)c; (void)r; return 0; }
static int  dl_request_start(void* c, const RecompLauncherCSettings* s) { (void)c; (void)s; return -1; }
static int  dl_launch_pending(void* c) { (void)c; return 0; }
static void dl_clear_launch_pending(void* c) { (void)c; }
static int  dl_fill_launch(void* c, RecompLauncherCNetplayLaunch* o) { (void)c; (void)o; return 0; }
static int  dl_kick_member(void* c, int s) { (void)c; (void)s; return 0; }
static int  dl_input_delay_get(void* c) { (void)c; return demo_lobby_delay; }
static int  dl_input_delay_set(void* c, int d) { (void)c; demo_lobby_delay = d; return 0; }
static int  dl_lobby_max_slots(void* c) { (void)c; return 4; }
static int  dl_rollback_get(void* c) { (void)c; return demo_lobby_rb; }
static int  dl_rollback_set(void* c, int e) { (void)c; demo_lobby_rb = e ? 1 : 0; return 0; }
static int  dl_input_prediction_get(void* c) { (void)c; return demo_lobby_pred; }
static int  dl_input_prediction_set(void* c, int p) { (void)c; demo_lobby_pred = p; return 0; }
static int  dl_memcard_offer_set(void* c, int has_card, int share) {
    (void)c; (void)has_card;
    if (share >= 0) demo_lobby_share = share ? 1 : 0;
    return 0;
}
static int  dl_guest_memcard_get(void* c) { (void)c; return demo_lobby_allow; }
static int  dl_guest_memcard_set(void* c, int a) { (void)c; demo_lobby_allow = a ? 1 : 0; return 0; }
/* A few chat lines so the panel has something to show; sends append locally
 * (a real backend appends on the server echo instead). */
static struct { char from[64]; char text[256]; int is_local; } demo_chat[64];
static int demo_chat_n = 0;
static void demo_chat_push(const char* from, const char* text, int is_local) {
    if (demo_chat_n >= 64) return;
    snprintf(demo_chat[demo_chat_n].from, 64, "%s", from);
    snprintf(demo_chat[demo_chat_n].text, 256, "%s", text);
    demo_chat[demo_chat_n].is_local = is_local;
    ++demo_chat_n;
}
static int dl_chat_send(void* c, const char* text) {
    (void)c;
    /* What a real backend would put on the wire: must be plain UTF-8, never
     * the launcher's private-use atlas codepoints. */
    fprintf(stderr, "[demo] chat_send bytes:");
    for (const unsigned char* q = (const unsigned char*)text; *q; ++q) fprintf(stderr, " %02X", *q);
    fprintf(stderr, "\n[demo] chat_send text: %s\n", text);
    demo_chat_push(demo_lobby_host ? "Alex" : "Marisa", text, 1);
    return 0;
}
static int dl_chat_count(void* c) { (void)c; return demo_chat_n; }
static int dl_chat_get(void* c, int i, RecompLauncherCNetplayChatMessage* out) {
    (void)c;
    if (i < 0 || i >= demo_chat_n || !out) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->from, sizeof(out->from), "%s", demo_chat[i].from);
    snprintf(out->text, sizeof(out->text), "%s", demo_chat[i].text);
    out->is_local = demo_chat[i].is_local;
    out->is_system = demo_chat[i].from[0] == '\0';
    out->seq = (uint32_t)(i + 1);
    return 1;
}
/* ---- LNG_DEMO_ACCOUNT: preview the Discord sign-in states -----------------
 * guest | waiting | in | failed | off  (off = server offers no logins, so no
 * sign-in affordance is drawn at all). No network: the launcher never does the
 * HTTP itself, so a fake host is enough to exercise every state. */
static int  demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_GUEST;
static int  demo_acct_avail = 1;
static char demo_acct_handle[64] = "Reimu";
static int  dl_account_available(void* c) { (void)c; return demo_acct_avail; }
static int  dl_account_state(void* c) { (void)c; return demo_acct_state; }
static const char* dl_account_handle(void* c) { (void)c; return demo_acct_handle; }
static const char* dl_account_username(void* c) { (void)c; return "reimu_h"; }
static const char* dl_account_error(void* c) {
    (void)c;
    return "Discord did not complete the sign-in. Try again.";
}
static int dl_account_login_begin(void* c) {
    (void)c;
    demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_WAITING;
    fprintf(stderr, "[demo] would open the browser at the server's authorize URL\n");
    return 0;
}
static int dl_account_sign_out(void* c) {
    (void)c;
    demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_GUEST;
    return 0;
}
static int dl_account_set_handle(void* c, const char* h) {
    (void)c;
    /* The real host asks the server, which refuses a name on the word list. */
    if (!h || !h[0]) return -1;
    snprintf(demo_acct_handle, sizeof(demo_acct_handle), "%s", h);
    fprintf(stderr, "[demo] handle -> %s\n", demo_acct_handle);
    return 0;
}

static RecompLauncherCNetplayCallbacks demo_lobby_cb;

static void demo_lobby_install(RecompLauncherCGameInfo* gi, const char* mode) {
    demo_lobby_host = !(mode && strcmp(mode, "guest") == 0);
    /* "browse": connected, not seated -- the lobby browser with the demo row. */
    demo_lobby_in = !(mode && strcmp(mode, "browse") == 0);
    memset(&demo_lobby_cb, 0, sizeof(demo_lobby_cb));
    demo_lobby_cb.default_url = dl_default_url;
    demo_lobby_cb.set_lobby_url = dl_set_lobby_url;
    demo_lobby_cb.connect = dl_connect;
    demo_lobby_cb.connected = dl_connected;
    demo_lobby_cb.pump = dl_pump;
    demo_lobby_cb.set_player_name = dl_set_player_name;
    demo_lobby_cb.player_name = dl_player_name;
    demo_lobby_cb.request_list = dl_request_list;
    demo_lobby_cb.list_count = dl_list_count;
    demo_lobby_cb.list_get = dl_list_get;
    demo_lobby_cb.online_count = dl_online_count;
    demo_lobby_cb.online_get = dl_online_get;
    demo_lobby_cb.server_chat_send = dl_schat_send;
    demo_lobby_cb.server_chat_count = dl_schat_count;
    demo_lobby_cb.server_chat_get = dl_schat_get;
    {
        const char* a = SDL_getenv("LNG_DEMO_ACCOUNT");
        if (a && a[0]) {
            demo_acct_preview = 1;
            if (strcmp(a, "in") == 0) demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_SIGNED_IN;
            else if (strcmp(a, "waiting") == 0) demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_WAITING;
            else if (strcmp(a, "failed") == 0) demo_acct_state = RECOMP_LAUNCHER_ACCOUNT_FAILED;
            else if (strcmp(a, "off") == 0) demo_acct_avail = 0;
        }
    }
    demo_lobby_cb.account_available = dl_account_available;
    demo_lobby_cb.account_login_begin = dl_account_login_begin;
    demo_lobby_cb.account_state = dl_account_state;
    demo_lobby_cb.account_handle = dl_account_handle;
    demo_lobby_cb.account_username = dl_account_username;
    demo_lobby_cb.account_error = dl_account_error;
    demo_lobby_cb.account_sign_out = dl_account_sign_out;
    demo_lobby_cb.account_set_handle = dl_account_set_handle;
    demo_schat_push("Sakuya", "anyone up for a set? \xF0\x9F\x94\xA5", 0);
    demo_schat_push("Reimu", "hosting now, come in", 0);
    demo_lobby_cb.leave = dl_leave;
    demo_lobby_cb.in_lobby = dl_in_lobby;
    demo_lobby_cb.is_host = dl_is_host;
    demo_lobby_cb.member_count = dl_member_count;
    demo_lobby_cb.member_get = dl_member_get;
    demo_lobby_cb.move_member = dl_move_member;
    demo_lobby_cb.local_ready = dl_local_ready;
    demo_lobby_cb.all_ready = dl_all_ready;
    demo_lobby_cb.set_ready = dl_set_ready;
    demo_lobby_cb.request_start = dl_request_start;
    demo_lobby_cb.launch_pending = dl_launch_pending;
    demo_lobby_cb.clear_launch_pending = dl_clear_launch_pending;
    demo_lobby_cb.fill_launch = dl_fill_launch;
    demo_lobby_cb.kick_member = dl_kick_member;
    demo_lobby_cb.input_delay_get = dl_input_delay_get;
    demo_lobby_cb.input_delay_set = dl_input_delay_set;
    demo_lobby_cb.lobby_max_slots = dl_lobby_max_slots;
    demo_lobby_cb.rollback_get = dl_rollback_get;
    demo_lobby_cb.rollback_set = dl_rollback_set;
    demo_lobby_cb.input_prediction_get = dl_input_prediction_get;
    demo_lobby_cb.input_prediction_set = dl_input_prediction_set;
    demo_lobby_cb.memcard_offer_set = dl_memcard_offer_set;
    demo_lobby_cb.guest_memcard_get = dl_guest_memcard_get;
    demo_lobby_cb.guest_memcard_set = dl_guest_memcard_set;
    demo_lobby_cb.seat_move_self = dl_seat_move_self;
    demo_lobby_cb.seat_swap_request = dl_seat_swap_request;
    demo_lobby_cb.seat_swap_incoming = dl_seat_swap_incoming;
    demo_lobby_cb.seat_swap_respond = dl_seat_swap_respond;
    demo_lobby_cb.seat_swap_outgoing = dl_seat_swap_outgoing;
    demo_lobby_cb.seat_swap_clear = dl_seat_swap_clear;
    demo_lobby_cb.host_can_spectate = dl_host_can_spectate;
    demo_lobby_cb.lobby_allow_spectators = dl_lobby_allow_spectators;
    demo_lobby_cb.lobby_max_spectators = dl_lobby_max_spectators;
    demo_lobby_cb.lobby_spectator_count = dl_lobby_spectator_count;
    demo_lobby_cb.local_is_spectator = dl_local_is_spectator;
    demo_lobby_cb.spectator_slot = dl_spectator_slot;
    {
        const char* ask = SDL_getenv("LNG_DEMO_SWAP_ASK");
        if (ask && ask[0]) {
            demo_swap_ask_every = atof(ask);
            demo_swap_next_ask = (double)SDL_GetTicks() / 1000.0 + demo_swap_ask_every;
        }
    }
    demo_lobby_cb.chat_send = dl_chat_send;
    demo_lobby_cb.chat_count = dl_chat_count;
    demo_lobby_cb.chat_get = dl_chat_get;
    demo_chat_push("Marisa", "gg last time, ready when you are \xF0\x9F\x98\x80", 0);
    demo_chat_push("Alex", "one sec, swapping to my duel deck card \xF0\x9F\x94\xA5\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", demo_lobby_host);
    demo_chat_push("Reimu", "I'll take P4 and watch this one \xF0\x9F\x87\xAF\xF0\x9F\x87\xB5 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 \xE2\x9D\xA4\xEF\xB8\x8F", 0);
    demo_chat_push("", "Reimu has joined as a spectator.", 0);
    gi->num_players = 4;
    gi->netplay_supported = 1;
    gi->netplay = &demo_lobby_cb;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ---- seed the C ABI structs the same way a real host would ----
    RecompLauncherCSettings s;
    memset(&s, 0, sizeof(s));
    s.output_method  = 2;      // OpenGL
    s.window_scale   = 3;
    s.fullscreen     = 0;
    s.linear_filter  = 0;
    s.widescreen     = 0;
    s.enable_audio   = 1;
    s.audio_freq     = 32000;
    s.volume         = 100;
    s.player_src[0]  = 1;      // keyboard
    s.player_src[1]  = 0;      // none
    s.skip_launcher  = 0;
    s.pad_mode[0]    = 0;      // Hybrid
    s.pad_mode[1]    = 0;
    s.aspect_index   = 1;      // 16:9

    // Neutral placeholder game: no CRC/SHA pinning, no MSU-1, no SRAM. Just
    // enough to exercise every panel that doesn't require real game facts.
    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    gi.name                 = "Recomp UI Test";
    gi.region               = "";
    gi.has_expected_crc     = 0;
    gi.widescreen_supported = 0;
    gi.msu1_supported       = 0;
    gi.sram_path            = NULL;
    gi.num_players          = 1;
    // One coherent VARIANT PROFILE picks the whole launcher identity so nothing
    // drifts (theme + controller + platform + rom_noun + capability set):
    //   LNG_VARIANT=psx  -> PlayStation (blue theme, DualShock, Disc, full PS settings)
    //   LNG_VARIANT=snes -> Super Nintendo (CRT theme, SNES pad, widescreen)
    // Unset = neutral default. See launcher_profile.h — one row per system.
    static const char* kPreviewLanguages[2] = { "English", "Japanese" };
    // Set by a variant that manages its OWN LNG_DEMO_FULL preview so the generic
    // (SNES-shaped) demo block below doesn't clobber it with MSU-1 / 1-player.
    int variant_owns_demo = 0;
    const char* variant = getenv("LNG_VARIANT");
    if (variant && variant[0]) {
        launcher_profile_apply(variant, &gi);
        if (lpr_is(variant, "psx") || lpr_is(variant, "ps1") || lpr_is(variant, "playstation")) {
            // Preview a PS1 title that offers the full surface (both wide aspects,
            // a language menu, a "configured" settings state).
            gi.aspect_mask     = 0x7;   // 4:3 + 16:9 + 21:9
            gi.language_labels = kPreviewLanguages;
            gi.num_languages   = 2;
            gi.num_players     = 1;
            s.window_width = 1280; s.renderer = 1; s.supersampling = 1;
            s.screen_kind = 1; s.frame_interp = 0; s.spu_hq = 1; s.aspect_index = 1;
        }
        if (lpr_is(variant, "nes")) {
            // Preview an NES title exercising the whole NES surface: 2 players,
            // widescreen opt-in, HD-pack row (on via launcher_profile_apply),
            // and the Zapper block. LNG_DEMO_FULL=1 additionally previews the
            // password/mantra save row (Faxanadu-style; the file is created
            // next to the exe on the first Edit -> Save).
            gi.num_players = 2;
            gi.widescreen_supported = 1;
            gi.zapper = 1;
            const char* nd = getenv("LNG_DEMO_FULL");
            if (nd && nd[0] == '1') {
                gi.password_save_path  = "nes_password_demo.txt";
                gi.password_save_label = "Mantra";
            }
        }
        if (lpr_is(variant, "gba")) {
            // Preview a GBA title: the one battery .sav (save row in the GAME
            // card), a "configured" LCD screen model, 3x integer scale.
            gi.sram_path  = "saves/save.sav";
            s.screen_kind = 3;   // Backlit (kGbaScreenKindNames)
            s.window_scale = 3;
            // LNG_DEMO_FULL=1 additionally previews a game-supplied aspect
            // vocabulary (the Mega Man Zero extended-view shape).
            static const char* const kGbaAspectPreview[] = {
                "3:2 (Native)", "9:5 (288 px)", "12:5 (384 px)", "6:2 (480 px)"
            };
            const char* gd = getenv("LNG_DEMO_FULL");
            if (gd && gd[0] == '1') {
                gi.aspect_labels       = kGbaAspectPreview;
                gi.num_aspect_labels   = 4;
                gi.aspect_experimental = 1;
            }
        }
        if (lpr_is(variant, "gb") || lpr_is(variant, "gbc") ||
            lpr_is(variant, "gameboy") || lpr_is(variant, "gameboycolor") ||
            lpr_is(variant, "dmg") || lpr_is(variant, "cgb")) {
            // Preview a Game Boy family title: the one battery .sav (folds into
            // the GAME card), a "configured" LCD palette, 4x integer scale. The
            // "gbc" variant shows the GAME BOY COLOR branding + a color title;
            // "gb" shows the DMG branding + a DMG title.
            int is_color = lpr_is(variant, "gbc") ||
                           lpr_is(variant, "gameboycolor") || lpr_is(variant, "cgb");
            gi.name        = is_color ? "Megaman Xtreme 2" : "Tetris";
            gi.region      = "USA";
            gi.sram_path   = "saves/save.sav";
            gi.boxart_path = "assets/img/boxart.tga";  // placeholder if absent
            gi.widescreen_supported = is_color ? 1 : 0;  // preview MMX2's opt-in 16:9 (EXPERIMENTAL)
            s.screen_kind  = 0;    // DMG palette (kGbScreenKindNames)
            s.window_scale = 4;
            variant_owns_demo = 1;
        }
        if (lpr_is(variant, "n64")) {
            // Preview an N64 title with the full surface a Stadium-class host
            // contributes: 4 players (2x2 card grid), an SRAM save row, a host
            // audio-device picker, a renderer vocabulary, and 4 Transfer Pak
            // slots wired to the fake cartridge brain above. Two slots come
            // pre-populated so the tpak card shows its inserted/decoded state.
            gi.num_players         = 4;
            gi.sram_path           = "saves/game.sram";
            static const char* const kN64AudioDevices[2] = {
                "Speakers (Realtek High Definition Audio)", "Headphones (USB DAC)"
            };
            gi.audio_device_labels = kN64AudioDevices;
            gi.num_audio_devices   = 2;
            static const char* const kN64Renderers[3] = { "Auto", "Vulkan", "D3D12" };
            gi.renderer_labels     = kN64Renderers;
            gi.num_renderers       = 3;
            gi.tpak_slots          = 4;
            gi.tpak_inspect        = proto_tpak_inspect;
            gi.has_mouse_controls  = 1;   // preview the opt-in mouse-controls UI (Snap-style)
            // Box art is a per-GAME asset the host supplies (a real N64 host
            // passes its own gi.boxart_path); the shared preview ships none, so
            // point LNG_BOX at a local .tga to see the GAME card light up.
            const char* box = getenv("LNG_BOX");
            if (box && box[0]) gi.boxart_path = box;
            s.renderer     = 1;   // Vulkan
            s.supersampling = 1;
            s.player_src[1] = 1;  // a second active player so the 2x2 grid isn't all-empty
            snprintf(s.tpak_rom_path[0], sizeof(s.tpak_rom_path[0]), "carts/pokemon_red.gb");
            snprintf(s.tpak_rom_path[1], sizeof(s.tpak_rom_path[1]), "carts/pokemon_blue.gb");
        }
        if (lpr_is(variant, "genesis") || lpr_is(variant, "megadrive") ||
            lpr_is(variant, "md")) {
            // Preview a Sonic-class Genesis title: 2 players (both controller
            // cards, the 3-Button/6-Button pad-mode selector, and the KEY +
            // GAMEPAD rebind grid), widescreen ON so the "Extra cells / side"
            // stepper is visible, real Sonic 1 box art, the SEGA GENESIS brand.
            // LNG_GAME picks which title, to show the SAVE row present vs absent:
            //   sonic1  (default) — no battery SRAM  -> no SAVE row
            //   sonic3k           — battery SRAM      -> SAVE row (Import/Clear)
            gi.region      = "USA";
            gi.num_players = 2;
            gi.boxart_path = "assets/img/boxart_sonic1.tga";  // real cart art
            s.widescreen   = 1;      // reveals the Genesis widescreen-cells stepper
            s.window_scale = 3;
            const char* g = getenv("LNG_GAME");
            if (g && (g[0] == '3' || lpr_is(g, "sonic3k") || lpr_is(g, "s3k"))) {
                gi.name      = "Sonic 3 & Knuckles";
                gi.sram_path = "saves/sonic3k.srm";  // battery cart -> SAVE row
            } else {
                gi.name      = "Sonic The Hedgehog";  // no battery -> no SAVE row
            }
            variant_owns_demo = 1;   // keep the generic demo block from overriding
        }
    }

    // Flip these to preview the 2-player + SRAM module set (what a save-game
    // capable, 2-player host contributes), e.g. LNG_DEMO_FULL=1.
    const char* demo = SDL_getenv("LNG_DEMO_FULL");
    if (!variant_owns_demo && demo && demo[0] == '1') {
        gi.num_players = 1;
        gi.sram_path   = "saves/save.srm";
        gi.widescreen_supported = 1;
        gi.msu1_supported = 1;
        gi.msu1_note = "Place the MSU-1 pack (.pcm + .msu) in this folder.";
    }
    if (demo && demo[0] == '2') {   // 2-player variant for layout testing
        gi.num_players = 2;
        gi.sram_path   = "saves/save.srm";
    }

    // Harness-only: exercises the MSU-1 dashboard "Patch ROM"/"Skip" flow
    // (launcher_model_apply_msu1_patch / launcher_model_skip_msu1_patch, drawn
    // in draw_game_panel — launcher_imgui.cpp). Points at a tiny fixture ROM
    // + IPS patch (test_data/) whose expected_crc matches the vanilla fixture
    // byte-for-byte, so msu1_patch_available comes up true without a real
    // game. See test_data/README-ish note in the fixture generator this was
    // produced from (ips_apply unit test) for the exact byte layout.
    const char* demo_msu_rom = NULL;
    const char* demo_msu = SDL_getenv("LNG_DEMO_MSU");
    if (demo_msu && demo_msu[0] == '1') {
        gi.msu1_supported   = 1;
        gi.msu1_note        = "This demo ROM has a tiny synthetic MSU-1 IPS patch "
                              "(test_data/msu1_demo.ips) for harness verification.";
        gi.msu1_patch_path  = "test_data/msu1_demo.ips";
        gi.has_expected_crc = 1;
        gi.expected_crc     = 0xA2B10169u;   // CRC32 of test_data/msu1_demo_vanilla.rom
        demo_msu_rom = "test_data/msu1_demo_vanilla.rom";
    }

    /* Harness-only: the received-package first-run state. A build someone else
     * compiled opens the wizard asking for BIOS + disc; browsing a retail dump
     * this binary has no backend for must keep the wizard (and its disc rows)
     * up and turn its primary button into Generate & rebuild. Any BIOS path
     * except one containing "linked" reports needs_regen, so
     *   LNG_DEMO_SETUP=1 ./recomp-ui-launcher
     * then Browse BIOS -> pick any file reproduces it without a real game.
     * LNG_DEMO_SETUP=2 additionally previews the full first-run shape
     * (generated sources absent), where the "Generate from disc" section
     * carries the button instead. */
    const char* demo_setup = SDL_getenv("LNG_DEMO_SETUP");
    if (demo_setup && demo_setup[0] && demo_setup[0] != '0') {
        gi.setup_wizard_supported = 1;
        gi.has_bios               = 1;
        gi.needs_setup            = 1;
        gi.bios_verify            = proto_bios_verify;
        gi.prepare_with_progress  = proto_prepare;
        gi.prepare_use_selected_rom = 1;
        gi.prepare_section_title  = "Generate BIOS + game C & rebuild";
        gi.prepare_disc_label     = "Generate & rebuild…";
        if (demo_setup[0] == '2')
            gi.prepare_required_before_continue = 1;
        /* Seed the pick instead of browsing for it, so the state this exists to
         * show is one launch away (and screenshottable). */
        const char* seed = SDL_getenv("LNG_DEMO_SETUP_BIOS");
        if (seed && seed[0])
            snprintf(s.bios_path, sizeof(s.bios_path), "%s", seed);
    }

    LauncherModel model;
    const char* rom = SDL_getenv("LNG_ROM");
    if (!rom || !rom[0]) rom = demo_msu_rom ? demo_msu_rom : "test.rom";
    {
        const char* demo_lobby = SDL_getenv("LNG_DEMO_LOBBY");
        if (demo_lobby && demo_lobby[0]) demo_lobby_install(&gi, demo_lobby);
    }
    launcher_model_init(&model, &s, &gi, rom);
    launcher_binds_load(&model, NULL, NULL);   // keybinds.ini + config.ini [KeyMap]
    fprintf(stderr, "[proto] rom=%s present=%d crc_match=%d sha_match=%d verified=%d size=%s\n",
            rom, model.rom_present, model.crc_match, model.sha_match,
            launcher_model_rom_verified(&model), model.rom_size);

    LauncherTheme theme = launcher_theme_by_name(gi.theme);

    char title[128];
    snprintf(title, sizeof(title), "Recomp UI — Launcher [%s]", launcher_backend_name());

    LauncherPlatform plat;
    if (!launcher_platform_open(&plat, title, 1100, 880)) {
        fprintf(stderr, "[proto] platform init failed; a real host would boot as if skipped.\n");
        return 2;
    }

    LngAction act = launcher_backend_run(&plat, &model, &theme);
    launcher_platform_close(&plat);

    // In production this is the value recomp_launcher_run_window() returns to
    // the host, which then boots the game IN-PROCESS with the committed
    // settings (0=LAUNCH, 1=QUIT). The standalone harness just reports it.
    if (act == LNG_ACTION_LAUNCH) {
        launcher_model_commit(&model, &s);
        printf("[proto] LAUNCH  scale=%s fullscreen=%d filter=%d freq=%d skip=%d rom=%s\n",
               launcher_model_scale_label(&model), s.fullscreen, s.linear_filter,
               s.audio_freq, s.skip_launcher, launcher_model_rom_path(&model));
    } else {
        printf("[proto] QUIT\n");
    }
    return 0;
}
