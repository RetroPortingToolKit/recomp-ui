# Host netplay integration notes

Status: **active** · 2026-07-23 · shared backend added 2026-09-25

recomp-ui owns presentation and the universal UDP port policy. The **game**
owns soft-return and rematch reboot. Lobby transport is either the engine's
own, or -- the way to get it without writing a fourth copy -- the shared
backend below. This page lists contracts that bite every title wiring
MotK-style netplay through `RecompLauncherCNetplayCallbacks`.

---

## The shared backend: `recomp_netplay_host` (optional)

`src/recomp_netplay_host.h` implements `RecompLauncherCNetplayCallbacks` once,
over recomp-net's lobby protocol client (`recomp_net/lobby_client.h`) and its
LAN modules. It was snesrecomp's `snes_host_lobby.c`; snesrecomp is now a thin
adapter over it, and an engine that has no lobby yet (n64lle) binds it instead
of writing one.

It is **optional** and recomp-ui never needs recomp-net: a consumer opts in
with `recomp_target_launcher_netplay(<target> [RECOMP_NET_TARGET recomp_net])`
after the recomp-net target exists, which builds the static library
`recomp_launcher_netplay` once and links it. Nothing else changes for a build
that does not call it.

Wiring: fill a `RecompNetplayHostHooks`, call `recomp_netplay_host_init`, set
`gi.netplay = recomp_netplay_host_callbacks()` and `gi.netplay_supported = 1`.
After a launch, `recomp_netplay_host_begin_soft_return(&gi, 1)` is the
soft-return helper (prepare_rematch + `resume_netplay_room` / endpoint).

| Hook | What the engine supplies | Absent means |
| --- | --- | --- |
| `game_name`, `game_version` | scoping key and exact release pin (SHIPPING.md §2) | init fails without a name |
| `content_fingerprint` | 64-hex SHA-256 of the guest image | joins work; automatch refuses to queue |
| `platform`, `legacy_env_prefix` | moderation tag; older env spelling (`"SNES_NET_"`) | `"unknown"`; `RNET_LOBBY_*` only |
| `max_players` | online seat ceiling (N64: 4) | 2 |
| `slot_policy` | `HOST_FIRST` (default, the contract below) or `SEAT` | `HOST_FIRST` |
| `exe_dir_path` | resolve files beside the executable | LAN registry / account secret cwd-relative |
| `name_store` / `name_load` | persist the display name | name not remembered across runs |
| `caps_codec` | the engine's own `match_caps` keys, in `RNetLobbyMatchCaps.ext` | none sent |
| `fill_match_caps` | host: finish the caps the room publishes | the waiting room's delay/runway/rollback only |
| `apply_match_caps` | every peer: adjust the launch from the caps | launch as filled |
| `mods` (`RecompNetplayModHooks`) | plan / installed rows, effective set, adopt, grant, transfer | **vanilla only**: empty plan, and a launch into a room whose host requires mods is REFUSED (`last_error` `mods_unsupported`) |
| `mods_enabled`, `cosmetic_allow` | automatch sim-mod assertion; host's cosmetic grant | never / grants nothing |
| `last_fork` | first simulation fork (tick, partition, both digests) | no `desync_report` |
| `auto_ready_guests`, `rematch_set_ready` | rematch ready policy | off |

What the backend fills that the SNES copy left NULL: `rollback_get/set` and
`input_prediction_get/set` (published in `match_caps.rollback` /
`match_caps.input_prediction`; the runway is published only once somebody sets
it, so the launch carries 0 and the engine keeps its default until then),
`connecting` (WebSocket upgrade until `welcome`), `name_rejected` (recomp-net's
chat filter), `need_mods_count/get/can_transfer` and `mod_xfer_progress`. The
launch now carries `input_prediction`, `rollback`, `force_turn`,
`occupied_mask`, `slot_port[]` and `host_spectates`.

Not implemented, and said rather than faked:

- **LAN / Direct IP rooms are two seats.** recomp-net's `lan_lobby` /
  `lan_direct` carry one joiner. A title with more seats hosting on LAN gets a
  two-seat room, a stderr line and `last_error` `lan_two_seats_only`. More
  seats means hosting online.
- **A LAN room settles no mode.** It carries the delay only, so a LAN launch
  has `rollback = 0` and `input_prediction = 0` on both peers (the engine's
  own override, e.g. `SNES_NET_MODE`, still applies) rather than each peer
  inventing its own.
- **`host_can_spectate` stays NULL.** `HOST_FIRST` folds `host_spectates` into
  the launch the way psxrecomp does, but no engine on this backend has run a
  match that way yet, so the UI does not offer it. Under `SEAT`, a launch with
  the host in the gallery is refused (`host_spectates_unsupported`).
- **No pre-seat mod transfer.** The transfer rides the seated `signal` relay;
  a joiner the server refused (`need_mods`) has no seat, so
  `need_mods_can_transfer` answers 0 and `mod_xfer_start` is not offered. The
  seated path (`lobby_mods_*`) is the transfer.

### LAN / Direct IP rematch

A LAN launch closes BOTH Direct IP sockets (`arm_lan_launch`): the host's
because the game session binds the same UDP port, the guest's with it. The
soft return (`recomp_netplay_host_prepare_rematch`) rebuilds the channel from
both ends, since neither knows when the other is back:

- **Host:** re-opens the listener and **frees the joiner seat** -- the new
  socket has no guest, and a seat still marked taken let the host start alone
  (`connect_timeout_lan`) and refused the returning guest `full`. A port that
  is not free yet is retried from the pump every 500 ms, with one stderr line.
- **Guest:** starts a non-blocking re-join (recomp-net
  `rnet_lan_direct_guest_join_begin` / `_join_poll`) to the same host with the
  password and bind it first joined with, polled from the pump and from
  `launch_pending`, re-sending `JOIN_REQ` while the host is not listening yet.
  While it re-joins the guest stays `in_lobby`, with no chat or seat swap.
  It gives up after 30 s (`last_error` `lan_rejoin_timeout`) or on a refusal
  (`lan_rejoin_refused`), and leaves the room either way, rather than sitting
  in one that can never launch.

**Requires recomp-net with `rnet_lan_direct_guest_join_begin` and
`RNetLanLobby.session_id`** (recomp-net `feat/lan-direct-rematch`, 8342229);
an older recomp-net does not compile this backend.

**Correction 2026-09-25 (later): the guest armed its launch on the host's
ROOM refresh, not on START.** `cb_request_start` publishes the room (ROOM,
`started=1`, no session id) and only then arms, which sends START with the
fresh id. A Direct IP guest that read the ROOM first armed with the id it
already held -- 0 after the re-join, launched as session 1 -- while the host
ran the new id, so both timed out (`connect_timeout`). It passed or failed on
which datagram a pump call happened to read first: nesrecomp's
`tools/rb_lobby.sh lan 2` failed it every run, n64lle's passed. The guest now
arms only once START has been read (`g_lan_start_seen`);
`recomp-ui-netplay-host-test` sends ROOM-then-START as the real host does and
fails without the fix. The "fresh session_id per START" claim below held for
the HOST all along; it now holds for the guest too.

Found 2026-09-25 by n64lle's headless lobby harness (`tools/rb_lobby.sh lan
2`: host second match `connect_timeout_lan`, guest never launched) and fixed
the same day; `recomp-ui-netplay-host-test` drives both halves on loopback.

**`session_id` on LAN: fresh per START, allocated by the host.** The rematch
contract -- never reuse the last match's UDP `session_id`, so a late
HELLO/BYE from match N is not taken for match N+1's (recomp-net-server
`docs/WS_LOBBY.md`, recomp-net `README.md`) -- is a property of the session
protocol, not of the server, so it applies to LAN too; what LAN lacks is the
server that allocates. The room's host does: `cb_request_start` takes the next
id, `START` and the registry file carry it (recomp-net `RNetLanLobby.session_id`,
a trailing optional field on both wires), and the guest launches with the one
it heard. Monotonic per room, the first one seeded from the clock so a room
re-created on the same port does not reuse the previous room's ids; never 0.
The id only filters packets -- it is not simulation input. A host that
predates the field sends none, and its guest falls back to 1, which is what
such a host launches with. (Until 2026-09-25 every LAN match was session 1.)

For the engine-side checklist see snesrecomp
[`docs/RECOMP_NET.md`](https://github.com/mstan/snesrecomp/blob/main/docs/RECOMP_NET.md)
→ "Soft-return rematch checklist".

---

## `join()` guest bind

```c
int (*join)(void* ctx, const char* lobby_id, const char* password,
            char* guest_bind /* in/out, capacity >= 64 */);
```

Before every `join()` call, the ImGui backend fills `guest_bind` via
`launcher_udp_prepare_guest_bind()` (prefer UDP **7778**, then +1..+31 →
`0.0.0.0:<port>`). Hosts must advertise that bind on the lobby join.

| Path | Host behavior |
|------|----------------|
| Online / lobby-server join | Pass `guest_bind` through to the lobby client |
| LAN file-registry (`lan:…`) | May ignore `guest_bind` |
| Engine fallback | recomp-net `rnet_lobby_join` (was snesrecomp `snes_lobby_join`) still rewrites NULL/empty/`host:0` |

Never rewrite a prepared bind to `:0` — the server would publish `peer_ip:0`
and LAN session start rejects it.

Host **create** uses the same helper family (`launcher_udp_port.*`): LAN keeps
the exact UI port; online prefers **7777**..+31.

---

## Soft-return launcher and `SDL_Quit`

`launcher_platform_close()` (SDL2 and SDL3) ends with **`SDL_Quit()`** so the
game gets a clean slate (GL attributes reset, no leftover launcher window).

That is intentional. Side effect: **all** SDL subsystems — including
**audio** — are torn down when the waiting room closes for a rematch.

### Symptom

After Escape / peer leave → lobby → **Play**:

```text
audio outputs: -1 device(s)
Audio subsystem is not initialized
Failed to open audio device
```

(or a silent hang if the host treats audio open as fatal).

### Host fix

On the rematch `session_reboot` path (after `recomp_launcher_run_window`
returns with a new `netplay_launch`), re-init before creating the game window
or opening audio:

```c
if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_WasInit(SDL_INIT_AUDIO) ||
    !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
    return 1;
}
```

First boot usually already called `SDL_Init` once; the `WasInit` guard avoids
stacking unnecessary init refcounts.

Do **not** remove `SDL_Quit()` from the platform close without a coordinated
host change — games rely on a clean shutdown between launcher and gameplay.

---

## The lobby room is a view, not a popup

`LNG_VIEW_LOBBY` is the full-screen room. The launcher enters it whenever the
backend's `in_lobby()` reports the local player seated and leaves it when that
stops being true — there is no open/close call for hosts to make, and a
soft-return that lands on Netplay flips into the room on the next frame by the
same rule. The page is two columns (seat tables left; room address, match
settings inline, mod plan right) and its actions — Leave Lobby, Mods, PLAY —
live in the fixed footer. Every profile that opens a lobby shares it.

A host therefore only needs `in_lobby()` to be truthful: a backend that keeps
reporting seated after a kick or a dropped socket keeps the player on a room
page that will never start.

The right column is the lobby chat. The room address, match settings and mod
plan moved into the **Settings** popup (footer button; guests see it as
**Room Info**, read-only), and the header's top-right corner shows the mod
plan summary ("Mods: vanilla match", "Mods: Widescreen (16:9) +1 more") in
place of the old LOBBY label when the build has a mod provider.

### Seats, session slots, and ports

Lobby seat, session slot, and controller port are three different numbers.
Players may move themselves to a free seat in either table, gallery included
(`seat_move_self`; the server accepts any empty seat), or ask the
occupant of a taken one -- in either table -- to trade (`seat_swap_request`;
the occupant sees the prompt through `seat_swap_incoming` and answers with
`seat_swap_respond`), and the host may drag anyone anywhere, itself included.
So the lobby host can end up in any seat. The UI refuses, before asking, any
trade that would land the host in the gallery unless `host_can_spectate` says
the backend runs the match that way. "Keep my seat" declines that ask and,
unseen, every further ask from anyone for the next 30 seconds. At launch, backends must keep the host as **session slot
0** regardless (`launch.slot_port_valid` + `slot_port[]`: host first, then
the players in seat order, each driving the port of its *lobby* seat). PSX
does this on both the online and LAN paths, and so does `recomp_netplay_host`
under its default `RECOMP_NETPLAY_SLOTS_HOST_FIRST`. A backend that maps
session slot == lobby seat instead makes whoever sits in seat 0 the sim
authority, with the host's save-state and overlay controls in that player's
hands. snesrecomp still does (`RECOMP_NETPLAY_SLOTS_SEAT`, because its engine
does not read `slot_port[]` yet): its launch carries identity `slot_port[]`
and a seat `occupied_mask`, so it is complete, but a moved host is not slot 0
there -- a known gap, recorded here where the contract is stated.

### Host in the spectator table

A host may drag its own row into the spectator table when the backend answers
`host_can_spectate()` (append-only, optional). It then runs the match from the
gallery: `launch.host_spectates` is 1 for every peer, the host keeps **session
slot 0** — the seat every host-only path keys on (save states, card sync, the
start, overlay host controls) — with its pad muted and no controller port
mapped to it, and player seats sit at lobby seat + 1. The server settles the
flag at start and sizes the relay for the extra slot. Backends without the
callback keep the old rule (the host's seat stays in play; the UI says so).

### Country flags

`RecompLauncherCNetplayMember.country` and `RecompLauncherCNetplayLobby.host_country`
carry ISO 3166-1 alpha-2 codes (empty = unknown / LAN). The UI draws them as
flags before the player name in the seat tables and before the lobby name in
the browser. The flag image comes from the bundled sheet
`assets/img/flags.png` (staged with the common images; built from Noto Color
Emoji by `tools/gen_flag_sheet.py`) on every platform, never from the OS
emoji font: Segoe UI Emoji has no flag glyphs, so on Windows the provider
would draw two boxed letters. The sheet also serves flag emoji typed into
chat. A code the sheet lacks falls back to the provider where that can draw
flags, and otherwise shows a muted `[JP]`. Backends fill the codes from the
server's `country` / `host_country` fields; LAN rooms leave them empty.

### Lobby browser columns and players online

The browser lists Lobby (flag + name), Players (`2/4`), Spectators (`No`
when the host opened no gallery, else `1/4` from
`RecompLauncherCNetplayLobby.allow_spectators` / `spectator_count` /
`max_spectators`), Latency, and Join. There is no Game column: the list is
already filtered to this title. Beside it, when the backend provides
`online_count` / `online_get`, a "Players online" panel lists everyone
connected to the lobby server with a flag before the name and where they
are (hosting or in a room, or browsing). The server sends that list as the
`players` array of every `lobby_list`; a LAN-only backend leaves the two
callbacks NULL and the panel is not drawn.

### Server chat (per game) and players online

The browser page's players panel lists only players on the lobby server
for THIS title (the server tags each presence row with the title that
client listed for, and the backend drops the rest). Below the list and the
panel, a "Server Chat" band is the per-game room outside any lobby:
`server_chat_send` / `server_chat_count` / `server_chat_get`, the same
contract and message struct as lobby chat, drawn only when all three exist
and the backend is online (a LAN-only session hides it). The server relays
a line to everyone browsing for the same title; there is no history. Emoji
and the profanity filter apply exactly as in lobby chat.

### Chat filtering

Backends mask profanity and slurs before a line reaches `chat_get`, with
one shared filter: recomp-net's `rnet_chat_filter_apply` (word list
`data/chat_filter_words.txt`, many languages, leetspeak and spaced-out
letters folded) at every ring push, and the same rules on the lobby server
for relayed lines. Because the client side masks on arrival, LAN rooms and
older servers are covered too. The UI never sees an unmasked line and needs
no filter of its own. `RNET_CHAT_FILTER=0` in a client's environment turns
its local pass off (developer use).

### Names: refused, not masked

A name is not a chat line. A line is a moment and a mask reads as one. A
player name sits in the seat table, in the players-online panel, and in front
of every line that player sends; a room title sits in the lobby browser in
front of everyone shopping for a game. Masking either just publishes the same
word with stars in it, for as long as it exists — so a name that trips the
list is **refused**, and the client is asked for a different one.

The lobby server is the authority. `ws_lobby.rs` gates every client-supplied
string once, at the deserialization boundary, so `hello`, `create`, `join`
and anything added later inherit it instead of having to remember it:

| Field | Policy |
| --- | --- |
| `display_name` | control characters dropped, capped, trimmed; **refused** (`name_rejected`) if it trips the list |
| `name` (room title) | same, **refused** as `lobby_name_rejected` |
| `game_name` | hygiene only — it is a **scoping key**, matched by string equality, so touching it would split one game into two sets of rooms |
| `password` | **validated, never rewritten** — see below |
| `text` (chat) | masked, as before |

A refused message is **not dispatched**: a bad name cannot ride in on a
`create` only to be refused after the room exists. The cap is 32 characters
*and* 63 bytes, because clients store these in a fixed 64-byte field
(`PSX_LOBBY_NAME_LEN`); cutting on a character boundary here is what stops a
client cutting mid-sequence.

recomp-ui reopens the matching prompt when the refusal arrives through
`last_error` — Player Name for `name_rejected` (dropping the refused name
from settings but leaving it in the edit box to fix), Host Lobby for
`lobby_name_rejected`.

`name_rejected(ctx, name)` is the optional local half: a backend that owns the
word list (recomp-net's `rnet_chat_filter`) answers 1 for a name it would
refuse, and the UI says so on Save or Create without a round trip — which is
also what covers a LAN room with no server. It is a courtesy, not the gate;
leave it NULL and the server's refusal still lands.

### Passwords: validated, never rewritten

A password is never filtered and never edited. Silently dropping a character
would leave the host holding a password that is not the one they typed, and
both sides would then disagree about a secret. It is also never shown to
anyone — only `has_password` is published, and the value is salted and hashed
before it is stored — so the word list has no business in it.

What is checked is only what no honest client can produce: control characters,
and a length over 128 bytes. Either refuses the whole message with
`password_invalid`.

### Chat callbacks

Three optional, append-only members: `chat_send(text)`, `chat_count()`,
`chat_get(index, RecompLauncherCNetplayChatMessage*)`. The UI never appends
its own line: a send returns 0 when accepted and the line appears through
`chat_get` once the room has it (online: the server's `chat` echo, which is
the room's order; PSX LAN: the host's `MOTK5 CHAT` relay). Keep a ring of the
last few dozen lines, oldest first, cleared on create / join / leave, and make
`seq` monotonic across rooms — the panel scrolls to a line only when `seq`
changes, so reusing 1 for the first line of a new room would leave it unscrolled.
A backend with no way to deliver a line (SNES LAN rooms today) returns nonzero
from `chat_send`; the UI reports "Chat is not available in this room."

System lines ("Marisa has joined.", "… has left.", "… was kicked.") are ordinary
chat messages with `is_system` set and an empty `from`; the room, not the
client, emits them (online: the server; PSX LAN: the host relay). The UI draws
them muted.

### Color emoji

Chat lines draw emoji in color. `src/common/emoji/` renders each emoji
*sequence* (skin tones, ZWJ families, flags, keycaps) to an RGBA sprite through
the platform — DirectWrite/Direct2D on Segoe UI Emoji on Windows; FreeType on
the system Noto Color Emoji (plus HarfBuzz for sequence shaping) elsewhere when
the build finds them — and the ImGui side gives each sprite a private-use
codepoint and blits it into the font atlas as a custom glyph. A string is drawn
with those codepoints substituted (`emoji_display`), so text measuring and
wrapping are unchanged. The atlas is static in this ImGui, so the first sight
of a new emoji rebuilds fonts once on the next frame.

Fallback is automatic: no provider (no FreeType at build time, a console port,
a font the renderer cannot open) means nothing is substituted and the OpenMoji
outline glyphs draw as before. `RECOMP_UI_EMOJI_FONT=/path/to/font` overrides
the FreeType font search. The backend in use is logged at startup as
`[rui] color emoji backend: …`. The chat input box holds the substituted
form while typing (an edit callback restores and re-substitutes the whole
buffer on every edit, so an emoji typed in pieces still joins) and the real
UTF-8 is restored on send; the wire never sees an atlas codepoint.

For layout work without a server, the prototype launcher takes
`LNG_DEMO_LOBBY=host|guest` (a fake three-player room) and screenshots through
`LNG_SCRIPT`, e.g. `LNG_VARIANT=psx LNG_DEMO_LOBBY=host LNG_SCRIPT="wait:40;shot:/tmp/lobby.png;quit"`.

## Resume room fields

When soft-returning after a match, set on `RecompLauncherCGameInfo`:

- `resume_netplay_room = 1`
- `resume_netplay_endpoint` — optional LAN endpoint string for the header

Clear launch-pending / re-arm ready in the game’s lobby callbacks before
showing the waiting room again (`snes_lobby_clear_launch_pending`,
`snes_lobby_set_ready`, etc.).

The soft-return `RecompLauncherCSettings` is a fresh struct, so every field the
first-boot seed fills must be filled again here. For PSX that includes
`memcard_path[]` and `memcard_enabled[]` (1 on, -1 off): point them at the
player's **personal** cards, not the match-time netplay sandbox files. Left
empty, the Save panel has nothing to inspect; with a `memcard_inspect`
callback wired it then draws both cards blank, which is wrong for a player who
has saves — and hosts that also fold the launcher's card choices back in for
the rematch (as they do at first boot) must do that here too, or the rematch
replays the pre-match slot config.

---

## Bring-your-own memory card (PSX)

Three optional, append-only members at the end of
`RecompLauncherCNetplayCallbacks`, plus three `RecompLauncherCNetplayMember`
fields and `RecompLauncherCNetplayLaunch.guest_memcard`:

| Callback | Who | Meaning |
|----------|-----|---------|
| `memcard_offer_set(has_card, share)` | every peer | Publish this peer's offer. The launcher computes `has_card` (slot 1 enabled and, if a file is picked, it inspected valid) and calls this every frame the room is open with `share = -1` (keep); the seat-row glyph calls it with `share = 0/1`. Backends re-advertise only on change. |
| `guest_memcard_get()` | every peer | Host allow flag as this peer sees it (default 1). |
| `guest_memcard_set(allow)` | host | Flip the allow flag; refused (<0) elsewhere. |

The seat-row glyph is drawn on **seat 1** (P2) — by seat, not by who hosts:
seat 0 is always the sim authority whose cards are the match cards. It is lit
when `member[1].memcard_offer_valid && memcard_has_card && memcard_share &&
guest_memcard_get()`. P2's click toggles its own `share`; the host's click
toggles `allow`. A member with `memcard_offer_valid == 0` is an older build
and the tooltip says so.

`launch.guest_memcard` must come from the **host-settled** value delivered
with the launch (online: `match_caps.guest_memcard_active`; LAN: the trailing
`MOTK1 START` line), never from the local seat table — otherwise a toggle that
races the start splits the room into peers that wait for a card and peers
that never send one.

## Peer disconnect UX

recomp-ui does not own the in-game disconnect dialog. Hosts should treat
mid-match peer loss like a local quit: soft-return to the lobby **without** a
blocking `SDL_ShowSimpleMessageBox`. Reserve modals for connect-timeout /
firewall guidance before the session starts.
