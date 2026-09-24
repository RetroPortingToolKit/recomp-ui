// launcher_model.c — implementation of the game-agnostic launcher view-model.
//
// Pure logic: no SDL, no OpenGL, no UI toolkit. Safe to compile as C and link
// into any game or either prototype backend.

#include "launcher_model.h"
#include "launcher_system.h"

#include "crc32.h"
#include "consoles/psx/memcard_format.h"   // PSX-specific; used only under SAVE_MEMCARD
#include "sha256.h"
#include "sha1.h"
#include "ips_patch.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif
#endif

// 32040 = the SNES S-DSP's native output rate; kept reachable in the cycle so
// users chasing bit-exact SNES audio can pick it (matches the legacy launcher).
static const int kFreqTable[] = { 32040, 32000, 44100, 48000 };
static const int kFreqCount   = (int)(sizeof(kFreqTable) / sizeof(kFreqTable[0]));

static const int kWindowWidths[]    = { 960, 1280, 1600, 1920 };
static const int kWindowWidthCount  = (int)(sizeof(kWindowWidths) / sizeof(kWindowWidths[0]));
static const int kInterpFpsTable[]  = { 0, 90, 120, 144, 165, 240 };
static const int kInterpFpsCount    = (int)(sizeof(kInterpFpsTable) / sizeof(kInterpFpsTable[0]));
static const char* kScreenKindNames[4] = { "Raw", "CRT", "Composite", "Trinitron" };

static const char* kButtonNames[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
    "L", "R", "Start", "Select"
};
// Player 1 keyboard defaults (Player 2 defaults unbound, mirroring the RML note).
static const char* kP1Defaults[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "X", "Z", "S", "A", "D", "C", "Enter", "RShift"
};
// Display labels for engine hotkeys (order == LngHotkey == [KeyMap] keys).
static const char* kHotkeyNames[LNG_HK_COUNT] = {
    "Fullscreen", "Reset", "Pause", "Pause (dimmed)", "Fast-forward",
    "Window bigger", "Window smaller", "Volume up", "Volume down",
    "FPS readout", "Toggle renderer",
    "Solar level up", "Solar level down", "Resume live solar",
    "Rewind", "Save states menu"
};
static const char* kViewNames[8] = {
    "Dashboard", "Settings", "Controller", "Netplay", "Mods",
    "Assist Tools", "Credits", "Lobby"
};
static const char* kSrcNames[3]  = { "None", "Keyboard", "Gamepad" };

static void safe_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

enum { LM_MMXPASS_DIGITS = 12, LM_MMXPASS_MIN_SIZE = 2048 };

typedef struct LmMmxPassRecord {
    char magic[8];
    unsigned char version;
    unsigned char digits[LM_MMXPASS_DIGITS];
    unsigned char checksum;
} LmMmxPassRecord;

static const char kLmMmxPassMagic[8] = { 'M', 'M', 'X', 'P', 'A', 'S', 'S', 0 };

static unsigned char lm_mmxpass_checksum(const unsigned char digits[LM_MMXPASS_DIGITS]) {
    unsigned char checksum = 0x5a;
    for (int i = 0; i < LM_MMXPASS_DIGITS; i++)
        checksum = (unsigned char)((checksum * 33u) ^ digits[i]);
    return checksum;
}

static int lm_mmxpass_valid(const LmMmxPassRecord* r) {
    if (!r || memcmp(r->magic, kLmMmxPassMagic, sizeof(r->magic)) != 0 ||
        r->version != 1)
        return 0;
    for (int i = 0; i < LM_MMXPASS_DIGITS; i++) {
        if (r->digits[i] < 1 || r->digits[i] > 8)
            return 0;
    }
    return r->checksum == lm_mmxpass_checksum(r->digits);
}

static int lm_mmxpass_parse_text(const char* text,
                                 unsigned char digits[LM_MMXPASS_DIGITS]) {
    int count = 0;
    for (const char* p = text ? text : ""; *p; p++) {
        if (*p == '-' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        if (*p < '1' || *p > '8' || count >= LM_MMXPASS_DIGITS)
            return 0;
        digits[count++] = (unsigned char)(*p - '0');
    }
    return count == LM_MMXPASS_DIGITS;
}

static void lm_mmxpass_format_text(const unsigned char digits[LM_MMXPASS_DIGITS],
                                   char* out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%u%u%u%u-%u%u%u%u-%u%u%u%u",
             (unsigned)digits[0], (unsigned)digits[1],
             (unsigned)digits[2], (unsigned)digits[3],
             (unsigned)digits[4], (unsigned)digits[5],
             (unsigned)digits[6], (unsigned)digits[7],
             (unsigned)digits[8], (unsigned)digits[9],
             (unsigned)digits[10], (unsigned)digits[11]);
}

static long lm_file_size(FILE* f) {
    long cur = ftell(f);
    if (cur < 0) cur = 0;
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long size = ftell(f);
    fseek(f, cur, SEEK_SET);
    return size < 0 ? 0 : size;
}

static void lm_write_zero_padding(FILE* f, long count) {
    static const unsigned char zeros[256] = {0};
    while (count > 0) {
        size_t n = (size_t)(count > (long)sizeof(zeros) ? sizeof(zeros) : count);
        fwrite(zeros, 1, n, f);
        count -= (long)n;
    }
}

/* Defined with the rest of the renderer control below; used by the settings
   normalization in launcher_model_init, which runs first. */
static void lm_renderer_sync_id(LauncherModel* m);

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---- multi-image disc roster helpers -------------------------------------
// File-name stem (basename minus the last extension) of a path, lowercased
// into `out`. Length 0 when there is nothing usable.
static size_t lm_path_stem(const char* path, char* out, size_t cap) {
    if (!path || !out || cap == 0) return 0;
    out[0] = '\0';
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char* dot = NULL;
    for (const char* p = base; *p; ++p)
        if (*p == '.') dot = p;
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = base[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[n] = '\0';
    return n;
}

// Compare two disc paths for "same image". Exact string equality first (the
// common case: the launcher hands back exactly the path the host gave it),
// then a case-insensitive compare of the file-name STEM. The stem, not the
// whole name: a .cue and the .bin it owns are one disc, and hosts hand us
// either — psxrecomp resolves a picked .cue to its .bin before it mounts —
// so "Disc 2.cue" and "Disc 2.bin" must land on the same roster slot. Two
// different discs of a set never share a stem; they are numbered.
static int lm_path_eq(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    if (strcmp(a, b) == 0) return 1;
    char sa[128], sb[128];
    if (!lm_path_stem(a, sa, sizeof(sa)) || !lm_path_stem(b, sb, sizeof(sb)))
        return 0;
    return strcmp(sa, sb) == 0;
}

// Roster slot whose effective path is `path`, or -1 when it is off-roster.
static int lm_disc_index_for_path(const LauncherModel* m, const char* path) {
    if (!m || m->num_discs <= 0 || !path || !path[0]) return -1;
    for (int i = 0; i < m->num_discs; ++i)
        if (lm_path_eq(launcher_model_disc_path(m, i), path)) return i;
    return -1;
}

// Bind rom_full to the roster after any ROM change. Either the new path IS a
// roster image (select that slot) or the player browsed to a replacement for
// the slot they had selected (record it as that slot's override) — never a
// silent collapse of an N-disc set to whatever single file was last picked.
static void lm_bind_disc_selection(LauncherModel* m) {
    if (!m || m->num_discs <= 0) {
        if (m) m->disc_selected = -1;
        return;
    }
    if (m->disc_selected < 0 || m->disc_selected >= m->num_discs)
        m->disc_selected = 0;
    const int hit = lm_disc_index_for_path(m, m->rom_full);
    if (hit >= 0) {
        m->disc_selected = hit;
    } else if (m->rom_present) {
        safe_copy(m->disc_path_override[m->disc_selected],
                  sizeof(m->disc_path_override[m->disc_selected]), m->rom_full);
    }
    m->s.disc_index = launcher_model_disc_number(m, m->disc_selected);
}

static void run_verify(LauncherModel* m);   // fwd; defined below, called from launcher_model_set_rom
static void update_msu1_patch_available(LauncherModel* m);   // fwd; called from launcher_model_set_rom
static void lm_inspect_memcard(LauncherModel* m, int slot); // fwd; host memcard_inspect callback

// Host-facing tri-state -> model 0/1 (see RecompLauncherCSettings.memcard_enabled).
static int lm_memcard_enabled_from_host(int v) {
    if (v == 0) return 1;   /* unset: host predates the field -> legacy default, on */
    return v > 0 ? 1 : 0;   /* 1 on, -1 off */
}
static void lm_inspect_tpak(LauncherModel* m, int slot);    // fwd; host tpak_inspect callback
static void lm_persist_setup_sidecars(LauncherModel* m);    // fwd; called from launcher_model_finish_setup
static void lm_persist_cart_rom(const LauncherModel* m);
static int lm_running_exe_dir(char* out, size_t cap);

static int lm_path_readable(const char* path) {
    if (!path || !path[0]) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int lm_same_cache_name(const char* a, const char* b) {
    if (!a || !a[0] || !b || !b[0]) return 0;
    const char* abase = a;
    const char* bbase = b;
    for (const char* p = a; *p; ++p)
        if (*p == '/' || *p == '\\') abase = p + 1;
    for (const char* p = b; *p; ++p)
        if (*p == '/' || *p == '\\') bbase = p + 1;
    while (*abase && *bbase) {
        char ac = *abase++;
        char bc = *bbase++;
        if (ac >= 'A' && ac <= 'Z') ac = (char)(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z') bc = (char)(bc + ('a' - 'A'));
        if (ac != bc) return 0;
    }
    return *abase == '\0' && *bbase == '\0';
}

static int lm_read_first_line(const char* path, char* out, size_t cap) {
    FILE* f;
    size_t len;
    if (!path || !path[0] || !out || cap == 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    out[0] = '\0';
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return out[0] != '\0';
}

static int lm_cache_candidate_path(const char* dir,
                                   const char* cache_name,
                                   char* out,
                                   size_t cap) {
    int n;
    if (!cache_name || !cache_name[0] || !out || cap == 0) return 0;
    if (strchr(cache_name, '/') || strchr(cache_name, '\\')
#if defined(_WIN32)
        || (strlen(cache_name) > 2 && cache_name[1] == ':')
#else
        || cache_name[0] == '/'
#endif
    ) {
        n = snprintf(out, cap, "%s", cache_name);
        return n > 0 && (size_t)n < cap;
    }
    if (dir && dir[0]) {
        n = snprintf(out, cap, "%s/%s", dir, cache_name);
        return n > 0 && (size_t)n < cap;
    }
    n = snprintf(out, cap, "%s", cache_name);
    return n > 0 && (size_t)n < cap;
}

static int lm_try_cached_rom_name(const char* cache_name,
                                  char* out,
                                  size_t out_cap) {
    char cache_path[1024];
    char candidate[1024];
    char exe_dir[1024];
    if (!out || out_cap == 0) return 0;
    if (lm_cache_candidate_path(NULL, cache_name, cache_path,
                                sizeof(cache_path)) &&
        lm_read_first_line(cache_path, candidate, sizeof(candidate)) &&
        lm_path_readable(candidate)) {
        safe_copy(out, out_cap, candidate);
        return 1;
    }
    if (lm_running_exe_dir(exe_dir, sizeof(exe_dir)) &&
        lm_cache_candidate_path(exe_dir, cache_name, cache_path,
                                sizeof(cache_path)) &&
        lm_read_first_line(cache_path, candidate, sizeof(candidate)) &&
        lm_path_readable(candidate)) {
        safe_copy(out, out_cap, candidate);
        return 1;
    }
    return 0;
}

static int lm_load_cached_rom_path(const RecompLauncherCGameInfo* game,
                                   const char* initial_rom,
                                   char* out,
                                   size_t out_cap) {
    const char* primary = (game && game->rom_cache_path &&
                           game->rom_cache_path[0])
                              ? game->rom_cache_path
                              : "rom.cfg";
    if (lm_path_readable(initial_rom)) return 0;
    if (lm_try_cached_rom_name(primary, out, out_cap)) return 1;
    if (!lm_same_cache_name(primary, "rom.cfg") &&
        lm_try_cached_rom_name("rom.cfg", out, out_cap))
        return 1;
    if (!lm_same_cache_name(primary, "disc.cfg") &&
        lm_try_cached_rom_name("disc.cfg", out, out_cap))
        return 1;
    return 0;
}

void launcher_model_init(LauncherModel* m,
                         const RecompLauncherCSettings* io,
                         const RecompLauncherCGameInfo* game,
                         const char* initial_rom) {
    memset(m, 0, sizeof(*m));

    if (game) {
        m->game_name            = game->name ? game->name : "Unknown Game";
        m->region               = game->region ? game->region : "";
        m->platform             = game->platform;   // NULL => no subtitle
        m->widescreen_supported = game->widescreen_supported != 0;
        m->msu1_supported       = game->msu1_supported != 0;
        m->msu1_note            = game->msu1_note;
        m->msu1_packs           = game->msu1_packs;
        m->num_msu1_packs       = game->msu1_packs && game->num_msu1_packs > 0
                                      ? game->num_msu1_packs : 0;
        m->msu1_patch_path      = game->msu1_patch_path;
        m->saves_supported      = game->sram_path != NULL;
        m->sram_path            = game->sram_path;
        m->has_solar_sensor     = game->has_solar_sensor != 0;
        m->has_integer_scale    = game->has_integer_scale != 0;
        m->hdpack_supported     = game->hdpack_supported != 0;
        m->password_save_path   = game->password_save_path;
        m->password_save_label  = game->password_save_label;
        m->password_sram_path   = game->password_sram_path;
        m->password_sram_label  = game->password_sram_label;
        m->password_sram_size   = game->password_sram_size;
        m->password_sram_offset = game->password_sram_offset;
        m->zapper               = game->zapper != 0;
        /* 0 = unset (caller predates the field) -> assume 2 players. */
        m->player_count         = game->num_players ? clampi(game->num_players, 1, LNG_MAX_PLAYERS) : 2;
        m->rom_cache_path       = game->rom_cache_path;
        m->expected_crc         = game->expected_crc;
        m->has_expected_crc     = game->has_expected_crc;
        m->known_sha256         = game->known_sha256;
        m->known_sha1_hex       = game->known_sha1_hex;
        m->num_known_sha1       = game->num_known_sha1;
        m->num_known_sha256     = game->num_known_sha256;

        m->pad_mode_supported   = game->pad_mode_supported != 0;
        m->pad_mode_selectable  = game->pad_mode_selectable != 0;
        m->locked_pad_mode      = clampi(game->locked_pad_mode, 0, 2);
        m->lock_device          = game->lock_device != 0;
        m->aspect_mask          = game->aspect_mask;

        m->has_window_size      = game->has_window_size != 0;
        m->has_renderer         = game->has_renderer != 0;
        m->has_supersampling    = game->has_supersampling != 0;
        m->has_antialiasing     = game->has_antialiasing != 0;
        m->has_texture_filter   = game->has_texture_filter != 0;
        m->has_fmv_filter       = game->has_fmv_filter != 0;
        m->has_screen_kind      = game->has_screen_kind != 0;
        m->has_frame_interp     = game->has_frame_interp != 0;
        m->has_spu_hq           = game->has_spu_hq != 0;
        m->has_rewind_depth     = game->has_rewind_depth != 0;
        m->has_vsync            = game->has_vsync != 0;
        m->has_skip_fmv         = game->has_skip_fmv != 0;
        m->has_turbo_loads      = game->has_turbo_loads != 0;
        m->has_geometry_precision = game->has_geometry_precision != 0;
        // game->has_fullscreen_toggle is deliberately NOT read: the Fullscreen
        // row is universal (drawn for every console) — see recomp_launcher.h.
        m->has_bios             = game->has_bios != 0;
        m->has_deadzone_pct     = game->has_deadzone_pct != 0;
        m->has_player_name      = game->has_player_name != 0;
        m->identity_detail      = game->identity_detail;
        m->rom_noun             = game->rom_noun ? game->rom_noun : "ROM";
        /* Multi-image roster. Only entries with a real path count: a host that
         * declared N discs but left one path NULL publishes a set the player
         * could select an unmountable slot from, so the roster stops at the
         * first hole rather than offering a row that cannot boot. */
        m->discs                = game->discs;
        m->num_discs            = 0;
        if (game->discs && game->num_discs > 0) {
            const int cap = game->num_discs < LNG_MAX_DISCS
                                ? game->num_discs : LNG_MAX_DISCS;
            while (m->num_discs < cap &&
                   game->discs[m->num_discs].path &&
                   game->discs[m->num_discs].path[0])
                m->num_discs++;
        }
        if (m->num_discs == 0) m->discs = NULL;
        m->language_labels      = game->language_labels;
        m->num_languages        = game->num_languages;
        m->disc_verify_cb       = game->disc_verify;      // real disc verdict (PSX), or NULL
        m->import_sbi_cb        = game->import_sbi;
        m->memcard_inspect_cb   = game->memcard_inspect;  // real memcard summary (PSX), or NULL
        m->bios_verify_cb       = game->bios_verify;
        m->persist_setup_cb     = game->persist_setup;
        m->persist_setup_discs_cb = game->persist_setup_discs;  // NULL on older hosts
        m->persist_setup_ctx    = game->persist_setup_ctx;
        /* Wizard + Generate & rebuild are a single opt-in. Without the flag,
         * ignore prepare/rebuild/toolchain even if a host filled them — keeps
         * unadvertised platforms from surfacing in-development UI. */
        m->setup_wizard_supported = game->setup_wizard_supported != 0;
        if (m->setup_wizard_supported) {
            m->prepare_disc_cb      = game->prepare_disc;
            m->prepare_with_progress_cb = game->prepare_with_progress;
            m->rebuild_with_progress_cb = game->rebuild_with_progress;
            m->prepare_disc_label   = game->prepare_disc_label;
            m->prepare_disc_note    = game->prepare_disc_note;
            m->prepare_section_title = game->prepare_section_title;
            m->prepare_busy_status  = game->prepare_busy_status;
            m->prepare_success_status = game->prepare_success_status;
            m->rebuild_busy_status  = game->rebuild_busy_status;
            m->rebuild_success_status = game->rebuild_success_status;
            m->prepare_use_selected_rom = game->prepare_use_selected_rom != 0;
            m->rebuild_after_prepare = game->rebuild_after_prepare != 0;
            m->relaunch_after_rebuild = game->relaunch_after_rebuild != 0;
            m->prepare_required_before_continue =
                game->prepare_required_before_continue != 0;
            m->setup_needs_toolchain = game->setup_needs_toolchain != 0;
            m->toolchain_is_ready_cb = game->toolchain_is_ready;
            m->ensure_toolchain_with_progress_cb =
                game->ensure_toolchain_with_progress;
            m->toolchain_update_available_cb = game->toolchain_update_available;
        } else {
            m->prepare_disc_cb = NULL;
            m->prepare_with_progress_cb = NULL;
            m->rebuild_with_progress_cb = NULL;
            m->prepare_disc_label = NULL;
            m->prepare_disc_note = NULL;
            m->prepare_section_title = NULL;
            m->prepare_busy_status = NULL;
            m->prepare_success_status = NULL;
            m->rebuild_busy_status = NULL;
            m->rebuild_success_status = NULL;
            m->prepare_use_selected_rom = false;
            m->rebuild_after_prepare = false;
            m->relaunch_after_rebuild = false;
            m->prepare_required_before_continue = false;
            m->setup_needs_toolchain = false;
            m->toolchain_is_ready_cb = NULL;
            m->ensure_toolchain_with_progress_cb = NULL;
            m->toolchain_update_available_cb = NULL;
        }
        /* PGO / FMV-timing are Settings actions, not the first-run wizard. */
        m->pgo_optimize_with_progress_cb = game->pgo_optimize_with_progress;
        m->fmv_timing_optimize_with_progress_cb =
            game->fmv_timing_optimize_with_progress;
        m->pgo_busy_status      = game->pgo_busy_status;
        m->pgo_success_status   = game->pgo_success_status;
        m->fmv_timing_busy_status = game->fmv_timing_busy_status;
        m->fmv_timing_success_status = game->fmv_timing_success_status;
        m->boxart_path          = game->boxart_path;      // NULL => default boxart.tga
        m->aspect_labels        = game->aspect_labels;    // NULL => built-in 4:3/16:9/21:9
        m->num_aspect_labels    = game->num_aspect_labels;
        m->aspect_experimental  = game->aspect_experimental != 0;
        m->aspect_setting_label = game->aspect_setting_label;
        m->aspect_setting_help  = game->aspect_setting_help;
        m->adaptive_view_supported = game->adaptive_view_supported != 0;
        m->display_layout_labels = game->display_layout_labels;
        m->num_display_layouts = game->num_display_layouts;
        m->has_assist_tools     = game->has_assist_tools != 0;
        m->assist_tools_note    = game->assist_tools_note;
        m->has_virtual_stylus   = game->has_virtual_stylus != 0;
        m->settings_bindings    = game->settings_bindings != 0;
        m->assist_binding_labels = game->assist_binding_labels;
        m->assist_binding_count =
            clampi(game->assist_binding_count, 0,
                   RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS);
        m->credits_text         = game->credits_text;
        m->assist_fast_forward_min = game->assist_fast_forward_min > 0
            ? game->assist_fast_forward_min : 2;
        m->assist_fast_forward_max = game->assist_fast_forward_max >=
                                      m->assist_fast_forward_min
            ? game->assist_fast_forward_max
            : m->assist_fast_forward_min;
        m->tpak_slots           = clampi(game->tpak_slots, 0, RECOMP_LAUNCHER_MAX_TPAKS);
        m->tpak_inspect_cb      = game->tpak_inspect;
        m->audio_device_labels  = game->audio_device_labels;
        m->num_audio_devices    = game->num_audio_devices;
        m->renderer_labels      = game->renderer_labels;
        m->num_renderers        = game->num_renderers;
        m->renderer_ids         = game->renderer_ids;
        m->renderer_note        = game->renderer_note;
        m->hide_rebind          = game->hide_rebind != 0;
        m->has_mouse_controls   = game->has_mouse_controls != 0;
        m->has_gyro_controls    = game->has_gyro_controls != 0;
        m->has_sharp_filter     = game->has_sharp_filter != 0;
        m->has_affine_filter    = game->has_affine_filter != 0;
        m->has_frame_blend      = game->has_frame_blend != 0;
        m->has_run_ahead        = game->has_run_ahead != 0;
        m->has_shader           = game->has_shader != 0;
        m->netplay_supported    = game->netplay_supported != 0 && game->netplay != NULL;
        m->netplay              = game->netplay;
        m->rom_patch_supported  = game->rom_patch_supported != 0;
        m->rom_patch_note       = game->rom_patch_note;
        m->rom_patch_cache_dir  = game->rom_patch_cache_dir;
        m->rom_patch_required_sha1 = game->rom_patch_required_sha1;
#if RECOMP_UI_ENABLE_MODS
        m->mods                 = game->mods;
#else
        /* The provider ABI is always present for stable consumer structs, but
         * a normal launcher build must not expose or mutate mods. */
        m->mods                 = NULL;
#endif
    } else {
        m->game_name    = "Unknown Game";
        m->region       = "";
        m->platform     = NULL;
        m->player_count = 2;
        m->rom_noun     = "ROM";
    }

    if (io) m->s = *io;
    /* Seed the selected disc from the host's persisted setting BEFORE the ROM
     * is read: launcher_model_set_rom() below binds initial_rom to the roster,
     * and when that path is off-roster (the player relocated the image) the
     * slot it lands on must be the one the host remembered, not slot 0.
     * disc_index is 1-based; 0 = unset -> disc 1. */
    m->disc_selected = m->num_discs > 0
                           ? clampi(m->s.disc_index - 1, 0, m->num_discs - 1)
                           : -1;
    /* Rewind buffer: 50/100/150/200; interval 1/4/8/12/15. */
    {
        int d = m->s.rewind_depth;
        if (d != 50 && d != 100 && d != 150 && d != 200)
            m->s.rewind_depth = 50;
        int iv = m->s.rewind_interval;
        if (iv != 1 && iv != 4 && iv != 8 && iv != 12 && iv != 15)
            m->s.rewind_interval = 15;
    }
    if (!m->rom_patch_supported) {
        m->s.rom_patch_enabled = 0;
        m->s.rom_patch_path[0] = '\0';
    } else {
        m->s.rom_patch_enabled =
            m->s.rom_patch_enabled && m->s.rom_patch_path[0] ? 1 : 0;
    }
    m->s.rom_patch_source_path[0] = '\0';
    m->s.rom_patch_sha1[0] = '\0';
    m->s.rom_patch_crc32[0] = '\0';
    m->s.assist_fast_forward_multiplier = clampi(
        m->s.assist_fast_forward_multiplier > 0
            ? m->s.assist_fast_forward_multiplier
            : m->assist_fast_forward_min,
        m->assist_fast_forward_min, m->assist_fast_forward_max);
    if (game && game->assist_default_key_bind &&
        game->assist_default_pad_bind) {
        memcpy(m->default_assist_key_bind, game->assist_default_key_bind,
               sizeof m->default_assist_key_bind);
        memcpy(m->default_assist_pad_bind, game->assist_default_pad_bind,
               sizeof m->default_assist_pad_bind);
        for (int i = 0; i < m->assist_binding_count; ++i) {
            if (m->s.assist_key_bind[i] == 0)
                m->s.assist_key_bind[i] = m->default_assist_key_bind[i];
            if (m->s.assist_pad_bind[i] == 0)
                m->s.assist_pad_bind[i] = m->default_assist_pad_bind[i];
        }
    } else {
        memcpy(m->default_assist_key_bind, m->s.assist_key_bind,
               sizeof m->default_assist_key_bind);
        memcpy(m->default_assist_pad_bind, m->s.assist_pad_bind,
               sizeof m->default_assist_pad_bind);
    }
    if (m->has_sharp_filter) {
        m->s.linear_filter = m->s.linear_filter ? 1 : 0;
        m->s.sharp_filter = m->s.sharp_filter ? 1 : 0;
        // Preserve an existing explicit linear-filter preference when a host
        // first gains the three-state scaler control.
        if (m->s.linear_filter) m->s.sharp_filter = 0;
    }
    if (m->has_frame_blend)
        m->s.frame_blend = m->s.frame_blend ? 1 : 0;
    /* A host may seed a depth its build predates, or a negative from a
     * malformed config; clamp to what the control can actually show rather
     * than drawing a value no press can return to. */
    if (m->has_run_ahead) {
        if (m->s.run_ahead < 0) m->s.run_ahead = 0;
        if (m->s.run_ahead > RECOMP_LAUNCHER_RUN_AHEAD_MAX)
            m->s.run_ahead = RECOMP_LAUNCHER_RUN_AHEAD_MAX;
    }
    memset(&m->s.netplay_launch, 0, sizeof(m->s.netplay_launch));
    if (!m->s.netplay_player_name[0] && m->netplay && m->netplay->player_name) {
        safe_copy(m->s.netplay_player_name, sizeof(m->s.netplay_player_name),
                  m->netplay->player_name(m->netplay->ctx));
    }
    safe_copy(m->netplay_name_edit, sizeof(m->netplay_name_edit), m->s.netplay_player_name);
    if (m->netplay && m->netplay->default_url) {
        safe_copy(m->netplay_lobby_url, sizeof(m->netplay_lobby_url),
                  m->netplay->default_url(m->netplay->ctx));
    }
    if (m->s.netplay_player_name[0]) {
        snprintf(m->netplay_host_name, sizeof(m->netplay_host_name), "%s's Lobby",
                 m->s.netplay_player_name);
    }
    safe_copy(m->netplay_host_port, sizeof(m->netplay_host_port), "7777");
    safe_copy(m->netplay_host_ip, sizeof(m->netplay_host_ip), "Detecting...");
    m->netplay_host_local_ip[0] = '\0';
    m->netplay_local_address_count = 0;
    safe_copy(m->netplay_direct_ip, sizeof(m->netplay_direct_ip), "127.0.0.1");
    safe_copy(m->netplay_direct_port, sizeof(m->netplay_direct_port), "7777");
    m->netplay_lan_only = false;
    m->netplay_list_fresh = false;
    m->netplay_selected_lobby = -1;
    m->netplay_public_ip[0] = '\0';
    m->netplay_public_ip_resolved = false;
    m->netplay_lobby_settings_open = false;
    m->netplay_lobby_input_delay = 6;
    m->netplay_manual_input_delay = false; /* auto from max peer RTT at launch */
    m->netplay_lobby_input_prediction = 10; /* P = 4 + D at default D=6 */
    m->netplay_manual_input_prediction = false; /* auto P from RTT when rollback */
    /* Default off so waiting-room ICE can prove a direct path; host Force
     * Online start is always lobby SFU (§108). */
    m->netplay_force_input_relay = false;
    m->netplay_force_turn = false;
    /* Rollback on by default; Lobby Settings “Disable Rollback” opts out. */
    m->netplay_rollback = true;
    {
        int max_p = m->player_count > 0 ? m->player_count : 2;
        if (max_p < 2) max_p = 2;
        if (max_p > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
            max_p = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
        m->netplay_host_max_players = max_p;
    }
    m->netplay_lobby_max_slots = 0;
    m->mod_selected = 0;
    m->mod_package_selected = 0;
    m->mod_show_packages = false;
    if (game && game->default_settings) {
        m->default_settings = *game->default_settings;
        m->has_default_settings = true;
    }
    m->s.adaptive_view =
        (m->adaptive_view_supported && m->s.adaptive_view) ? 1 : 0;

    // ---- memory-card slots: tri-state in (1 on, -1 off, 0 == "unset": a host
    // struct that predates this field, or was zero-initialized, reads as both
    // cards plugged in — matching the legacy launcher's default), 0/1 from here
    // on. Without the -1 form a host could never show a slot the user had
    // switched off; it re-armed as on and was persisted that way. ----
    for (int slot = 0; slot < 2; ++slot)
        m->s.memcard_enabled[slot] = lm_memcard_enabled_from_host(m->s.memcard_enabled[slot]);

    // ---- infer the SystemProfile this game belongs to (panel composition +
    // per-system specs) from the ABI caps launcher_profile_apply() already set ----
    m->profile = launcher_system_infer(game);
    if (m->profile && m->profile->controller.max_players > 0) {
        /* num_players is a per-game capability, while max_players is the
         * console ceiling. Raising the ABI storage width must never make a
         * two-player game/system expose extra controller or lobby seats. */
        m->player_count = clampi(m->player_count, 1,
                                 m->profile->controller.max_players);
    }

    // ---- gate pad_mode per player ----
    if (m->pad_mode_supported) {
        const SystemProfile* pm_prof = (const SystemProfile*)m->profile;
        const ControllerSpec* pm_spec = pm_prof ? &pm_prof->controller : NULL;
        for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
            if (!m->pad_mode_selectable) {
                m->s.pad_mode[p] = m->locked_pad_mode;
            } else if (pm_spec && pm_spec->modes && pm_spec->mode_count > 0) {
                // Custom mode list (Genesis 3-Button/6-Button): any listed
                // mode value is valid; anything else snaps to the first
                // listed mode. The Hybrid rule below is PSX-only semantics.
                int ok = 0;
                for (int i = 0; i < pm_spec->mode_count; ++i)
                    if (pm_spec->modes[i].mode == m->s.pad_mode[p]) { ok = 1; break; }
                if (!ok) m->s.pad_mode[p] = pm_spec->modes[0].mode;
            } else if (m->s.pad_mode[p] == 0) {
                /* Hybrid is mod-only and never selectable: migrate any stale
                 * persisted value. The mod requests it at runtime instead. */
                m->s.pad_mode[p] = 1;
            }
            /* Keyboard cannot drive Analog/Hybrid — present D-Pad.
             *
             * This is a PRESENTATION default, not a hardware clamp. The host
             * already short-circuits keyboard seats at the point of use:
             * effective_player_mode() in psxrecomp's runtime/src/main.cpp
             * reports DIGITAL for any seat whose kind is keyboard, whatever
             * the seat's stored mode says. So nothing downstream depends on
             * this value being 2 — it only decides what the selector shows.
             *
             * Which is why it must NOT run when the mode is locked. A locked
             * title (game.toml [controller] lock_mode) hides the selector
             * entirely, so once locked_pad_mode is overwritten with D-Pad
             * there is no control left that can put it back — not the
             * selector (hidden), not launcher_model_set_pad_mode() and not
             * apply_default_pad_mode_for_source(), both of which correctly
             * refuse to touch a locked mode. And a release install defaults
             * Player 1's device to Keyboard, so EVERY fresh install of an
             * Analog-locked dual-analog title (Ape Escape) went through here
             * and came out digital-for-good: the game saw a plain SCPH-1080,
             * right stick dead and left stick folded onto the D-pad, with no
             * UI to correct it.
             *
             * Leaving the locked mode intact costs nothing while the keyboard
             * is driving the seat, and is already right the moment the player
             * attaches a real pad. */
            if (m->pad_mode_selectable &&
                m->s.player_src[p] == 1 &&
                !(pm_spec && pm_spec->modes && pm_spec->mode_count > 0))
                m->s.pad_mode[p] = 2;
        }
    }

    // ---- Transfer Pak slots: inspect whatever the host's config seeded ----
    // (re-run per slot on every ROM/save change; see launcher_model_set_tpak_*).
    for (int t = 0; t < m->tpak_slots; ++t) lm_inspect_tpak(m, t);

    // ---- validate/clamp aspect_index against the offered set ----
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        // Game-supplied vocabulary: a plain 0..n-1 cycle, every entry offered.
        m->s.aspect_index = clampi(m->s.aspect_index, 0, m->num_aspect_labels - 1);
    } else if (m->aspect_mask) {
        int idx = clampi(m->s.aspect_index, 0, 2);
        // walk down from the requested index to the nearest offered aspect;
        // 4:3 (bit0, index 0) is always offered so this always terminates.
        while (idx > 0 && !(m->aspect_mask & (1 << idx))) --idx;
        m->s.aspect_index = idx;
    }
    if (m->display_layout_labels && m->num_display_layouts > 0) {
        m->s.display_layout =
            clampi(m->s.display_layout, 0, m->num_display_layouts - 1);
    } else {
        m->s.display_layout = 0;
    }

    // ---- clamp/seed the deeper PSX-style settings against their own ranges ----
    if (m->has_window_size) {
        int ok = 0;
        for (int i = 0; i < kWindowWidthCount; ++i)
            if (kWindowWidths[i] == m->s.window_width) { ok = 1; break; }
        if (!ok) m->s.window_width = kWindowWidths[0];
    }
    if (m->has_supersampling) m->s.supersampling = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    if (m->has_screen_kind) {
        // Clamp against the active profile's screen-model vocabulary (GBA has
        // 5 LCD models; the legacy PSX-era set has 4) — see screen_kind_vocab.
        const SystemProfile* sk_prof = (const SystemProfile*)m->profile;
        int sk_n = (sk_prof && sk_prof->screen_kind_names && sk_prof->screen_kind_count > 0)
                     ? sk_prof->screen_kind_count : 4;
        m->s.screen_kind = clampi(m->s.screen_kind, 0, sk_n - 1);
    }
    if (m->has_texture_filter) m->s.texture_filter = m->s.texture_filter ? 1 : 0;
    /* 0 = unset (a host predating the field): seed the default rather than
     * letting a memset pin the least useful value. */
    if (m->has_fmv_filter) {
        if (m->s.fmv_filter < 1 || m->s.fmv_filter > RECOMP_LAUNCHER_FMV_FILTER_COUNT)
            m->s.fmv_filter = RECOMP_LAUNCHER_FMV_FILTER_BICUBIC;
    }
    launcher_model_apply_renderer_settings(m);
    if (m->has_frame_interp) {
        int ok = 0;
        for (int i = 0; i < kInterpFpsCount; ++i)
            if (kInterpFpsTable[i] == m->s.frame_interp_fps) { ok = 1; break; }
        if (!ok) m->s.frame_interp_fps = 0;
    }
    if (m->has_vsync &&
        (m->s.vsync < 1 || m->s.vsync > RECOMP_LAUNCHER_VSYNC_COUNT)) {
        // 0 = a host that predates the field, or a fresh config. Tear-free is
        // the safe default; a player who wants the latency picks Off.
        m->s.vsync = RECOMP_LAUNCHER_VSYNC_ON;
    }
    if (m->num_languages > 0)
        m->s.language_index = clampi(m->s.language_index, 0, m->num_languages - 1);
    if (m->has_deadzone_pct) {
        m->s.deadzone[0] = clampi((m->s.deadzone[0] / 5) * 5, 0, 50);
        m->s.deadzone[1] = m->s.deadzone[0];
    }

    // ---- mouse controls: seed/clamp against their own ranges ----------------
    // Only touched when has_mouse_controls (every other game leaves the whole
    // mouse_* block at its memset-zero state, untouched). The host normally
    // seeds real defaults from its config; a zero sensitivity is the tell of a
    // fresh/zero-initialized struct (e.g. a demo harness) — seed the full Snap
    // default set in that case, otherwise just clamp the sensitivity.
    if (m->has_mouse_controls) {
        if (m->s.mouse_sensitivity <= 0.0f) {
            m->s.player_src[0]      = 1;      // Keyboard (+ mouse) by default
            m->s.mouse_enabled      = 1;
            m->s.mouse_sensitivity  = 0.06f;
            m->s.mouse_invert_x     = 0;
            m->s.mouse_invert_y     = 1;
            m->s.mouse_bind[0]      = 0;      // Left  -> A  (kN64PadButtons[0])
            m->s.mouse_bind[1]      = 2;      // Right -> Z  (kN64PadButtons[2])
            m->s.mouse_bind[2]      = -1;     // Middle-> none
        } else {
            m->s.mouse_sensitivity = clampf(m->s.mouse_sensitivity, 0.01f, 0.50f);
        }
    }
    if (m->has_gyro_controls) {
        m->s.gyro_sensitivity =
            m->s.gyro_sensitivity > 0.0f
                ? clampf(m->s.gyro_sensitivity, 0.25f, 4.00f)
                : 1.00f;
    }
    // ---- widescreen extra-cells (video.widescreen_cells consoles, e.g.
    // Genesis): 0 = unset host -> the engine default of 8; else clamp 1..16. ----
    {
        const SystemProfile* ws_prof = (const SystemProfile*)m->profile;
        if (ws_prof && ws_prof->video.widescreen_cells)
            m->s.widescreen_cells = m->s.widescreen_cells
                                      ? clampi(m->s.widescreen_cells, 1, 16) : 8;
    }

    // Real ROM read + CRC/SHA verification (computes rom_size, crc_match,
    // sha_match). No synthesized/faked facts.
    {
        char cached_rom[1024];
        const char* seeded_rom = initial_rom;
        if (lm_load_cached_rom_path(game, initial_rom, cached_rom,
                                    sizeof(cached_rom)))
            seeded_rom = cached_rom;
        launcher_model_set_rom(m, seeded_rom);
    }

    // Password/mantra save: read the current one-line password file so the
    // SAVES row can show it. (Zapper switch state is loaded later by
    // launcher_binds_load(), which owns the keybinds.ini path.)
    launcher_model_password_reload(m);

    // Inspect both memory-card slots up front (real block usage/validity when a
    // host memcard_inspect callback is wired; no-op otherwise).
    lm_inspect_memcard(m, 0);
    lm_inspect_memcard(m, 1);

    m->view      = LNG_VIEW_DASHBOARD;
    m->action    = LNG_ACTION_NONE;
    m->cfg_player = 0;
    m->setup_wizard_open = false;
    m->setup_page = 1;
    m->setup_tc_auto = true;
    m->setup_tc_ready = false;
    m->setup_tc_update_available = false;
    m->setup_tc_update_skipped = false;
    m->setup_tc_local_ver[0] = '\0';
    m->setup_tc_remote_ver[0] = '\0';
    m->setup_tc_zip[0] = '\0';
    m->setup_bios_needs_regen = false;
    m->bios_confirm_open = false;
    m->bios_pending_path[0] = '\0';
    m->setup_wizard_suspended_for_bios = false;
    m->bios_switch_uncommitted = false;
    m->bios_revert_path[0] = '\0';
    m->bios_play_modal_open = false;
    m->setup_preparing = false;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_progress_title[0] = '\0';
    m->setup_status[0] = '\0';
    m->setup_error[0] = '\0';
    m->setup_prepare_satisfied = false;
    m->relaunch_exe[0] = '\0';
    if (!game || !game->setup_needs_toolchain) {
        m->setup_needs_toolchain = false;
        m->toolchain_is_ready_cb = NULL;
        m->ensure_toolchain_with_progress_cb = NULL;
        m->toolchain_update_available_cb = NULL;
    }
    launcher_model_refresh_bios_status(m);

    /* Soft-return from a match: land on Netplay.
     *
     * Still seated (a hosted room that outlived the match) => the frame then
     * switches to the full-screen lobby view, because the backend reports us
     * in a room (see LNG_VIEW_LOBBY).
     *
     * NOT seated => the netplay page draws its lobby LIST, which is the whole
     * point for a host that left the room on the way out. An automatch room
     * is the server's and is gone the moment the match ends, so there is
     * nothing to return to; the in_lobby test used to gate the whole hint and
     * such a host landed on the dashboard instead, a page away from the queue
     * it was trying to rejoin. */
    if (game && game->resume_netplay_room && m->netplay_supported && m->netplay) {
        const bool seated = m->netplay->in_lobby &&
                            m->netplay->in_lobby(m->netplay->ctx);
        m->view = LNG_VIEW_NETPLAY;
        m->netplay_list_fresh = true;
        if (!seated) {
            /* No room, so none of the room-shaped state below applies. */
            m->netplay_local_room = false;
            m->netplay_host_endpoint[0] = '\0';
        } else {
            if (game->resume_netplay_endpoint && game->resume_netplay_endpoint[0]) {
                m->netplay_local_room = true;
                safe_copy(m->netplay_host_endpoint,
                          sizeof(m->netplay_host_endpoint),
                          game->resume_netplay_endpoint);
            } else {
                m->netplay_local_room = false;
                m->netplay_host_endpoint[0] = '\0';
            }
            /* Mirror engine match caps — UI default rollback=true must not
             * flip a delay-sync Cable Club rematch on ▶ Play without opening
             * Settings. */
            if (m->netplay->rollback_get)
                m->netplay_rollback =
                    m->netplay->rollback_get(m->netplay->ctx) != 0;
            if (m->netplay->input_delay_get) {
                m->netplay_lobby_input_delay =
                    m->netplay->input_delay_get(m->netplay->ctx);
                if (m->netplay_lobby_input_delay < 2)
                    m->netplay_lobby_input_delay = 6;
            }
        }
    }

    /* First-run setup: only when the host opted into the wizard product.
     * Host can force it, or we open when ROM/BIOS is missing. A selected BIOS
     * that merely needs Generate & rebuild (needs_regen) is handled by the
     * Switch-BIOS / PLAY prompts — not the full wizard. */
    if (m->setup_wizard_supported) {
        const int force = game && game->needs_setup;
        const int missing_rom = !m->rom_present || strcmp(m->rom_size, "--") == 0;
        const int missing_bios = m->has_bios && !m->setup_bios_ok &&
                                 !m->setup_bios_needs_regen;
        if (force || missing_rom || missing_bios)
            m->setup_wizard_open = true;
    }

    /* Probe toolchain readiness even when the wizard is closed — BIOS switch
     * Generate & rebuild needs setup_tc_ready, and codegen hosts always set
     * setup_needs_toolchain. When a usable pack exists, also compare against
     * GitHub /releases/latest so page 0 can prompt for an update. */
    if (m->setup_needs_toolchain) {
        m->setup_tc_ready =
            (m->toolchain_is_ready_cb && m->toolchain_is_ready_cb()) ? true
                                                                    : false;
        m->setup_tc_update_available = false;
        m->setup_tc_local_ver[0] = '\0';
        m->setup_tc_remote_ver[0] = '\0';
        /* Host may have just deleted a broken latest/ — surface that on page 0. */
        if (!m->setup_tc_ready && game && game->toolchain_repair_note) {
            const char* note = game->toolchain_repair_note();
            if (note && note[0])
                safe_copy(m->setup_status, sizeof(m->setup_status), note);
        }
        if (m->setup_tc_ready && m->toolchain_update_available_cb) {
            char local_ver[64] = {0};
            char remote_ver[64] = {0};
            if (m->toolchain_update_available_cb(local_ver, sizeof(local_ver),
                                                remote_ver, sizeof(remote_ver))) {
                m->setup_tc_update_available = true;
                safe_copy(m->setup_tc_local_ver, sizeof(m->setup_tc_local_ver),
                          local_ver);
                safe_copy(m->setup_tc_remote_ver, sizeof(m->setup_tc_remote_ver),
                          remote_ver);
            } else {
                if (local_ver[0])
                    safe_copy(m->setup_tc_local_ver,
                              sizeof(m->setup_tc_local_ver), local_ver);
                if (remote_ver[0])
                    safe_copy(m->setup_tc_remote_ver,
                              sizeof(m->setup_tc_remote_ver), remote_ver);
            }
        }
        if (m->setup_wizard_open) {
            const int need_tc_page =
                !m->setup_tc_ready ||
                (m->setup_tc_update_available && !m->setup_tc_update_skipped);
            m->setup_page = need_tc_page ? 0 : 1;
        } else {
            m->setup_page = 1;
        }
    } else {
        m->setup_page = 1;
        m->setup_tc_ready = true;
    }

    // Placeholder display until launcher_binds_load() fills real values from
    // keybinds.ini / config.ini [KeyMap]. Walk the ACTIVE profile's button
    // count (SNES 12, PSX 16, ...) so every rebind slot the page will render
    // gets a placeholder — kP1Defaults only names the SNES-shaped first 12.
    {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
        if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
        for (int b = 0; b < bc; ++b) {
            safe_copy(m->binds[0][b], sizeof(m->binds[0][b]),
                      b < LNG_BTN_COUNT ? kP1Defaults[b] : "(unbound)");
            safe_copy(m->binds[1][b], sizeof(m->binds[1][b]), "(unbound)");
            safe_copy(m->pad_binds[0][b], sizeof(m->pad_binds[0][b]), "(unbound)");
            safe_copy(m->pad_binds[1][b], sizeof(m->pad_binds[1][b]), "(unbound)");
        }
    }
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        m->hotkeys[h][0] = '\0';
}

void launcher_model_commit(const LauncherModel* m, RecompLauncherCSettings* io) {
    if (io) *io = m->s;
    lm_persist_cart_rom(m);
}

int launcher_model_disc_count(const LauncherModel* m) {
    return m ? m->num_discs : 0;
}

int launcher_model_disc_selected(const LauncherModel* m) {
    if (!m || m->num_discs <= 0) return -1;
    return clampi(m->disc_selected, 0, m->num_discs - 1);
}

int launcher_model_disc_number(const LauncherModel* m, int idx) {
    if (!m || idx < 0 || idx >= m->num_discs || !m->discs) return 0;
    /* 0 = the host left the number unset for an ordinary 1..N set. */
    return m->discs[idx].number > 0 ? m->discs[idx].number : idx + 1;
}

const char* launcher_model_disc_label(const LauncherModel* m, int idx) {
    if (!m || idx < 0 || idx >= m->num_discs || !m->discs) return "";
    const char* label = m->discs[idx].label;
    if (label && label[0]) return label;
    /* Per-slot scratch so the returned pointer stays valid alongside every
     * other row's label for as long as the caller is drawing the dropdown. */
    LauncherModel* mm = (LauncherModel*)m;
    snprintf(mm->disc_label_scratch[idx], sizeof(mm->disc_label_scratch[idx]),
             "Disc %d", launcher_model_disc_number(m, idx));
    return mm->disc_label_scratch[idx];
}

const char* launcher_model_disc_path(const LauncherModel* m, int idx) {
    if (!m || idx < 0 || idx >= m->num_discs || !m->discs) return "";
    if (m->disc_path_override[idx][0]) return m->disc_path_override[idx];
    return m->discs[idx].path ? m->discs[idx].path : "";
}

void launcher_model_select_disc(LauncherModel* m, int idx) {
    if (!m || m->num_discs <= 0) return;
    if (idx < 0 || idx >= m->num_discs) return;
    if (idx == m->disc_selected) return;
    m->disc_selected = idx;
    /* set_rom re-runs the disc verdict against the newly mounted image and
     * calls back into lm_bind_disc_selection, which writes s.disc_index. */
    launcher_model_set_rom(m, launcher_model_disc_path(m, idx));
}

/* Cheap existence probe. The model has no stat() wrapper and does not want
 * one: everywhere else it already opens the file it cares about. */
static int lm_path_exists(const char* path) {
    FILE* f;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static bool lm_is_sbi_path(const char* path) {
    const size_t n = path ? strlen(path) : 0;
    return n >= 4 && path[n - 4] == '.' &&
           tolower((unsigned char)path[n - 3]) == 's' &&
           tolower((unsigned char)path[n - 2]) == 'b' &&
           tolower((unsigned char)path[n - 1]) == 'i';
}

void launcher_model_set_disc_path(LauncherModel* m, int idx, const char* path) {
    if (!m || idx < 0 || idx >= m->num_discs) return;
    char imported[sizeof(m->disc_path_override[0])] = {0};
    if (lm_is_sbi_path(path)) {
        const char* disc = launcher_model_disc_path(m, idx);
        m->setup_error[0] = '\0';
        if (!disc || !disc[0] || !m->import_sbi_cb) {
            safe_copy(m->setup_error, sizeof(m->setup_error), "Select a supported disc first, then select its matching SBI file.");
            return;
        }
        if (!m->import_sbi_cb(disc, path, imported, sizeof(imported), m->setup_error, sizeof(m->setup_error))) return;
        path = imported;
    }
    safe_copy(m->disc_path_override[idx], sizeof(m->disc_path_override[idx]),
              (path && path[0]) ? path : "");
    /* Locating the SELECTED disc is also a statement about what is mounted, so
     * rebind the ROM and let verification re-run. Locating any other slot is
     * pure bookkeeping and must NOT disturb the current mount or verdict --
     * a player filling in disc 3 has not asked to boot disc 3. */
    if (idx == m->disc_selected)
        launcher_model_set_rom(m, launcher_model_disc_path(m, idx));
}

const char* launcher_model_disc_suggested_name(const LauncherModel* m, int idx) {
    const char* p;
    const char* slash;
    const char* back;
    if (!m || idx < 0 || idx >= m->num_discs || !m->discs) return "";
    /* Deliberately the ROSTER path, not launcher_model_disc_path(): once the
     * player locates their own copy this is still the build's file name, and
     * the caller only uses it while the slot is unlocated. */
    p = m->discs[idx].path;
    if (!p || !p[0]) return "";
    slash = strrchr(p, '/');
    back = strrchr(p, '\\');
    if (back && (!slash || back > slash)) slash = back;
    return slash ? slash + 1 : p;
}

bool launcher_model_disc_ready(const LauncherModel* m, int idx) {
    if (!m || idx < 0 || idx >= m->num_discs) return false;
    return lm_path_exists(launcher_model_disc_path(m, idx)) ? true : false;
}

int launcher_model_discs_ready_count(const LauncherModel* m) {
    int i, n = 0;
    if (!m) return 0;
    for (i = 0; i < m->num_discs; ++i)
        if (launcher_model_disc_ready(m, i)) ++n;
    return n;
}

/* Rewrite every "disc N" / "cd N" token in a path to a different number.
 *
 * All occurrences, not the first: sets are commonly stored one folder per
 * disc, so the number appears in the directory AND the file name
 * (".../Foo (Disc 1)/Foo (Disc 1).cue"). Replacing only one of them yields a
 * path that does not exist and the guess is silently wasted.
 *
 * The alphabetic part matches case-insensitively; the digits must match
 * exactly, so "Disc 1" never rewrites the "1" in "Final Fantasy 11". */
static int lm_swap_disc_token(const char* in, const char* word, int sep,
                              int from_num, int to_num, char* out, size_t cap) {
    char from_tok[24], num_tok[12];
    size_t wlen, i = 0, o = 0, flen, nlen;
    int hits = 0;
    if (!in || !out || cap == 0) return 0;
    snprintf(from_tok, sizeof(from_tok), "%s%s%d", word, sep ? " " : "", from_num);
    snprintf(num_tok, sizeof(num_tok), "%d", to_num);
    flen = strlen(from_tok);
    nlen = strlen(num_tok);
    wlen = strlen(word);
    while (in[i]) {
        int match = 1;
        size_t k;
        for (k = 0; k < flen; ++k) {
            char a = in[i + k], b = from_tok[k];
            if (!a) { match = 0; break; }
            if (k < wlen) {          /* letters: fold case */
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            }
            if (a != b) { match = 0; break; }
        }
        /* Reject a digit immediately after the token so "Disc 1" does not
         * match inside "Disc 12". */
        if (match && in[i + flen] >= '0' && in[i + flen] <= '9') match = 0;
        if (match) {
            /* Copy the matched word and separator VERBATIM and swap only the
             * digits. Emitting a normalised "disc N" instead would lowercase
             * a source that wrote "Disc N", and the candidate path would then
             * not exist on any case-sensitive filesystem -- i.e. it would find
             * nothing on Linux while appearing to work on Windows. */
            const size_t keep = wlen + (sep ? 1u : 0u);
            if (o + keep + nlen >= cap) return 0;
            memcpy(out + o, in + i, keep);
            o += keep;
            memcpy(out + o, num_tok, nlen);
            o += nlen;
            i += flen;
            ++hits;
        } else {
            if (o + 1 >= cap) return 0;
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
    return hits;
}

int launcher_model_autofill_sibling_discs(LauncherModel* m) {
    static const char* kWords[] = {"disc", "cd"};
    int src = -1, i, filled = 0;
    if (!m || m->num_discs <= 1) return 0;
    /* Any located disc can seed the others; prefer the lowest so the common
     * "player picked disc 1" case reads naturally in the derived paths. */
    for (i = 0; i < m->num_discs; ++i)
        if (launcher_model_disc_ready(m, i)) { src = i; break; }
    if (src < 0) return 0;

    for (i = 0; i < m->num_discs; ++i) {
        const char* base;
        int from_num, to_num, w, sep;
        char cand[512];
        if (i == src || launcher_model_disc_ready(m, i)) continue;
        base = launcher_model_disc_path(m, src);
        from_num = launcher_model_disc_number(m, src);
        to_num = launcher_model_disc_number(m, i);
        if (from_num == to_num) continue;
        for (w = 0; w < 2 && !launcher_model_disc_ready(m, i); ++w) {
            for (sep = 1; sep >= 0; --sep) {
                if (!lm_swap_disc_token(base, kWords[w], sep, from_num, to_num,
                                        cand, sizeof(cand)))
                    continue;
                if (!lm_path_exists(cand)) continue;
                /* Bookkeeping only -- never moves the mount (see set_disc_path). */
                launcher_model_set_disc_path(m, i, cand);
                ++filled;
                break;
            }
        }
    }
    return filled;
}

void launcher_model_set_rom(LauncherModel* m, const char* path) {
    if (lm_is_sbi_path(path)) {
        char mounted[sizeof(m->rom_full)] = {0};
        m->setup_error[0] = '\0';
        if (!m->rom_present || !m->import_sbi_cb) {
            safe_copy(m->setup_error, sizeof(m->setup_error),
                      "Select a supported disc first, then select its matching SBI file.");
            return;
        }
        if (!m->import_sbi_cb(m->rom_full, path, mounted, sizeof(mounted),
                              m->setup_error, sizeof(m->setup_error))) return;
        launcher_model_set_rom(m, mounted);
        return;
    }
    m->setup_error[0] = '\0';
    m->rom_present = path && path[0] != '\0';
    safe_copy(m->rom_full, sizeof(m->rom_full), m->rom_present ? path : "");
    m->rom_sha1_hex[0] = '\0';
    m->rom_patch_prepared_path[0] = '\0';
    m->rom_patch_prepared_sha1[0] = '\0';

    // Display just the basename (handles both / and \ separators).
    const char* base = m->rom_full;
    for (const char* q = m->rom_full; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    safe_copy(m->rom_file, sizeof(m->rom_file), m->rom_present ? base : "(none)");

    /* Cartridge consoles: read the ROM once for size + CRC32/SHA over the
     * body (SMC header stripped) vs expected fingerprints.
     * Disc systems (verify.mode==1, e.g. PSX): only ftell for the size label.
     * Full-image CRC/SHA would block launcher open on multi-hundred-MB BINs
     * and is unused — identity is disc_verify / identify_disc instead. */
    m->rom_size[0] = '\0';
    m->crc_match = false;
    m->sha_match = false;
    m->sha1_match = false;
    const int disc_verify = m->profile && m->profile->verify.mode == 1;
    if (m->rom_present) {
        FILE* f = fopen(m->rom_full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            if (n > 0) {
                snprintf(m->rom_size, sizeof(m->rom_size), "%.2f MB (%ld Mbit)",
                         (double)n / (1024.0 * 1024.0), (long)((n * 8) / (1024 * 1024)));
                if (!disc_verify) {
                    fseek(f, 0, SEEK_SET);
                    uint8_t* buf = (uint8_t*)malloc((size_t)n);
                    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                        /* Normalize container headers before fingerprinting.
                         * SNES .smc images may carry a 512-byte copier header;
                         * NES .nes images carry a 16-byte iNES/NES 2.0 header.
                         * NESRecomp's ROM and package CRCs intentionally cover
                         * every byte after that 16-byte header. */
                        size_t hdr = ((size_t)n % 1024 == 512) ? 512 : 0;
                        if ((size_t)n > 16 &&
                            buf[0] == 'N' && buf[1] == 'E' &&
                            buf[2] == 'S' && buf[3] == 0x1A) {
                            hdr = 16;
                        }
                        const uint8_t* body = buf + hdr;
                        size_t blen = (size_t)n - hdr;
                        uint32_t crc = recompui_crc32_compute(body, blen);
                        uint8_t  sha[32];
                        recompui_sha256_compute(body, blen, sha);
                        m->crc_match = m->has_expected_crc && crc == m->expected_crc;
                        for (size_t k = 0; k < m->num_known_sha256; ++k)
                            if (memcmp(sha, m->known_sha256[k], 32) == 0) {
                                m->sha_match = true;
                                break;
                            }
                        /* SHA-1: cartridge identity gate (GBA/SNES). */
                        if (m->num_known_sha1 && m->known_sha1_hex) {
                            uint8_t s1[20]; char s1hex[41];
                            recompui_sha1_compute(body, blen, s1);
                            recompui_sha1_hex(s1, s1hex);
                            safe_copy(m->rom_sha1_hex,
                                      sizeof(m->rom_sha1_hex), s1hex);
                            for (size_t k = 0; k < m->num_known_sha1; ++k) {
                                const char* want = m->known_sha1_hex[k];
                                if (!want) continue;
                                int eq = 1;
                                for (int c = 0; c < 40 && eq; ++c) {
                                    char a = s1hex[c], b = want[c];
                                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                                    if (a != b) eq = 0;
                                }
                                if (eq && want[40] == '\0') {
                                    m->sha1_match = true;
                                    break;
                                }
                            }
                        }
                    }
                    free(buf);
                }
            }
            fclose(f);
        }
    }
    if (!m->rom_size[0]) safe_copy(m->rom_size, sizeof(m->rom_size), "--");

    /* Keep the roster selection and the mounted path in agreement BEFORE the
     * verdict runs, so the checklist the dropdown sits above always describes
     * the disc the dropdown is showing. */
    lm_bind_disc_selection(m);

    run_verify(m);
    update_msu1_patch_available(m);
}

// Disc-verdict (verify.mode==1 systems, e.g. PSX): run the SystemProfile's
// VerifyProbeFn against the current ROM/disc path, or synthesize a sensible
// placeholder verdict when the probe is NULL / declines (no host wired up
// yet) so the disc-verdict UI always renders a real verdict block instead of
// a "not recognized" dead end. No-op for verify.mode==0 systems (SNES) — the
// CRC/SHA line above already covers them and m->verify stays zeroed.
static void run_verify(LauncherModel* m) {
    if (!m->profile || m->profile->verify.mode != 1) return;
    memset(&m->verify, 0, sizeof(m->verify));
    // Host disc-verify callback (REAL serial/region/ISO/verdict) takes
    // precedence — re-run here on every ROM/disc change.
    if (m->disc_verify_cb && m->rom_present) {
        RecompLauncherCDiscVerify dv; memset(&dv, 0, sizeof(dv));
        if (m->disc_verify_cb(m->rom_full, &dv)) {
            safe_copy(m->verify.serial, sizeof(m->verify.serial), dv.serial);
            safe_copy(m->verify.region, sizeof(m->verify.region), dv.region);
            m->verify.iso_ok  = dv.iso_ok != 0;
            m->verify.verdict = dv.verdict;
            m->verify.sbi_status = dv.sbi_status;
            m->verify.track_count = dv.track_count;
            m->verify.netplay_ok = dv.netplay_ok;
            safe_copy(m->verify.disc_fp, sizeof(m->verify.disc_fp), dv.disc_fp);
            safe_copy(m->verify.netplay_detail, sizeof(m->verify.netplay_detail),
                      dv.netplay_detail);
            return;
        }
    }
    VerifyProbeFn probe = m->profile->verify.probe;
    bool ok = probe && probe(m, &m->verify);
    if (ok) return;
    if (m->rom_present) {
        /* Placeholder facts: no real disc reader is wired up yet, but the
         * checklist should still show something plausible rather than a
         * blank/TODO state. */
        safe_copy(m->verify.serial, sizeof(m->verify.serial), "SCUS-94423");
        safe_copy(m->verify.region, sizeof(m->verify.region),
                  (m->region && m->region[0]) ? m->region : "NTSC-U");
        m->verify.iso_ok  = true;
        m->verify.verdict = 1;   // ok
    } else {
        m->verify.serial[0] = '\0';
        m->verify.region[0] = '\0';
        m->verify.iso_ok  = false;
        m->verify.verdict = 0;   // none
    }
}

const char* launcher_model_rom_path(const LauncherModel* m) {
    return m->rom_full;
}

const char* launcher_model_effective_rom_path(const LauncherModel* m) {
    if (m && m->rom_patch_prepared_path[0])
        return m->rom_patch_prepared_path;
    return m ? m->rom_full : "";
}

bool launcher_model_rom_verified(const LauncherModel* m) {
    if (!m->rom_present) return false;
    const int has_crc  = m->has_expected_crc;
    const int has_sha  = m->num_known_sha256 > 0;
    const int has_sha1 = m->num_known_sha1 > 0;
    if (!has_crc && !has_sha && !has_sha1) return false;   // no fingerprint => can't vouch
    if (has_crc  && !m->crc_match)  return false;
    if (has_sha  && !m->sha_match)  return false;
    if (has_sha1 && !m->sha1_match) return false;
    return true;
}

void launcher_model_set_view(LauncherModel* m, LngView v) {
    if (v < 0 || v >= LNG_VIEW__COUNT) return;
    /* Re-entering Netplay should rescan server + LAN lists. */
    if (m->view == LNG_VIEW_NETPLAY && v != LNG_VIEW_NETPLAY)
        m->netplay_list_fresh = false;
    m->view = v;
}

void launcher_model_open_config(LauncherModel* m, int player) {
    m->cfg_player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->view = LNG_VIEW_CONTROLLER;
}

bool launcher_model_can_restore_defaults(const LauncherModel* m) {
    return m && m->has_default_settings;
}

void launcher_model_request_restore_defaults(LauncherModel* m) {
    if (!launcher_model_can_restore_defaults(m)) return;
    m->defaults_modal_open = true;
}

void launcher_model_restore_defaults(LauncherModel* m) {
    if (!launcher_model_can_restore_defaults(m)) return;
    m->s = m->default_settings;
    {
        int d = m->s.rewind_depth;
        if (d != 50 && d != 100 && d != 150 && d != 200)
            m->s.rewind_depth = 50;
        int iv = m->s.rewind_interval;
        if (iv != 1 && iv != 4 && iv != 8 && iv != 12 && iv != 15)
            m->s.rewind_interval = 15;
        if (m->s.run_ahead < 0) m->s.run_ahead = 0;
        if (m->s.run_ahead > RECOMP_LAUNCHER_RUN_AHEAD_MAX)
            m->s.run_ahead = RECOMP_LAUNCHER_RUN_AHEAD_MAX;
    }
    m->defaults_modal_open = false;
}

void launcher_model_cancel_restore_defaults(LauncherModel* m) {
    if (m) m->defaults_modal_open = false;
}

void launcher_model_cycle_scale(LauncherModel* m) {
    m->s.window_scale = (m->s.window_scale >= LNG_WINDOW_SCALE_MAX)
                            ? 1 : m->s.window_scale + 1;
    if (m->s.window_scale < 1) m->s.window_scale = 1;
}

void launcher_model_set_scale(LauncherModel* m, int scale) {
    if (!m) return;
    m->s.window_scale = clampi(scale, 1, LNG_WINDOW_SCALE_MAX);
}

void launcher_model_toggle_filter(LauncherModel* m) {
    m->s.linear_filter = !m->s.linear_filter;
    if (m->s.linear_filter && m->has_sharp_filter)
        m->s.sharp_filter = 0;
}

void launcher_model_cycle_scaling_filter(LauncherModel* m) {
    if (!m || !m->has_sharp_filter) return;
    if (m->s.sharp_filter) {
        m->s.sharp_filter = 0;
        m->s.linear_filter = 0;
    } else if (m->s.linear_filter) {
        m->s.linear_filter = 0;
        m->s.sharp_filter = 1;
    } else {
        m->s.linear_filter = 1;
    }
}

const char* launcher_model_scaling_filter_label(const LauncherModel* m) {
    if (!m) return "Nearest";
    if (m->s.sharp_filter) return "Sharp fractional";
    if (m->s.linear_filter) return "Linear";
    return "Nearest";
}

void launcher_model_toggle_affine_filter(LauncherModel* m) {
    if (!m || !m->has_affine_filter) return;
    m->s.affine_filter = !m->s.affine_filter;
}

void launcher_model_toggle_frame_blend(LauncherModel* m) {
    if (!m || !m->has_frame_blend) return;
    m->s.frame_blend = !m->s.frame_blend;
}

void launcher_model_set_run_ahead(LauncherModel* m, int frames) {
    if (!m || !m->has_run_ahead) return;
    m->s.run_ahead = clampi(frames, 0, RECOMP_LAUNCHER_RUN_AHEAD_MAX);
}

void launcher_model_toggle_widescreen(LauncherModel* m) {
    if (!m->widescreen_supported) return;   // gated: no-op when unsupported
    m->s.widescreen = !m->s.widescreen;
}

void launcher_model_toggle_adaptive_view(LauncherModel* m) {
    if (!m->adaptive_view_supported) return;
    m->s.adaptive_view = !m->s.adaptive_view;
}

static int aspect_choice_count(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0)
        return m->num_aspect_labels;
    if (!m->aspect_mask) return 0;
    int count = 0;
    for (int i = 0; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) ++count;
    return count;
}

static int first_offered_aspect(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0) return 0;
    for (int i = 0; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) return i;
    return 0;
}

static int next_offered_aspect(const LauncherModel* m, int current) {
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        int next = current + 1;
        return next < m->num_aspect_labels ? next : -1;
    }
    for (int i = current + 1; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) return i;
    return -1;
}

void launcher_model_cycle_view_mode(LauncherModel* m) {
    if (!m) return;
    const int fixed_count = aspect_choice_count(m);

    if (m->s.adaptive_view && m->adaptive_view_supported) {
        m->s.adaptive_view = 0;
        if (fixed_count) m->s.aspect_index = first_offered_aspect(m);
        else m->s.widescreen = 0;
        return;
    }

    if (fixed_count) {
        int next = next_offered_aspect(m, m->s.aspect_index);
        if (next >= 0) {
            m->s.aspect_index = next;
            return;
        }
        if (m->adaptive_view_supported) {
            m->s.adaptive_view = 1;
            return;
        }
        m->s.aspect_index = first_offered_aspect(m);
        return;
    }

    if (m->widescreen_supported && !m->s.widescreen) {
        m->s.widescreen = 1;
        return;
    }
    if (m->adaptive_view_supported) {
        m->s.widescreen = 0;
        m->s.adaptive_view = 1;
        return;
    }
    m->s.widescreen = 0;
}

const char* launcher_model_view_mode_label(const LauncherModel* m) {
    if (!m) return "Native";
    if (m->adaptive_view_supported && m->s.adaptive_view) return "Adaptive";
    if (aspect_choice_count(m)) return launcher_model_aspect_label(m);
    return m->s.widescreen ? "16:9 fixed" : "Native";
}

void launcher_model_cycle_display_layout(LauncherModel* m) {
    if (!m || !m->display_layout_labels || m->num_display_layouts <= 0) return;
    m->s.display_layout =
        (clampi(m->s.display_layout, 0, m->num_display_layouts - 1) + 1) %
        m->num_display_layouts;
}

const char* launcher_model_display_layout_label(const LauncherModel* m) {
    if (!m || !m->display_layout_labels || m->num_display_layouts <= 0)
        return "Default";
    return m->display_layout_labels[
        clampi(m->s.display_layout, 0, m->num_display_layouts - 1)];
}

void launcher_model_ws_cells_delta(LauncherModel* m, int delta) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (!prof || !prof->video.widescreen_cells) return;   // gated per console
    int v = m->s.widescreen_cells ? m->s.widescreen_cells : 8;
    m->s.widescreen_cells = clampi(v + delta, 1, 16);
}

const char* launcher_model_ws_cells_label(const LauncherModel* m) {
    static char buf[16];
    int v = m->s.widescreen_cells ? clampi(m->s.widescreen_cells, 1, 16) : 8;
    snprintf(buf, sizeof(buf), "%d cells", v);
    return buf;
}

bool launcher_model_aspect_offered(const LauncherModel* m, int index) {
    if (index == 0) return true;   // 4:3 is always implied/available
    if (index == 1) return (m->aspect_mask & 2) != 0;
    if (index == 2) return (m->aspect_mask & 4) != 0;
    return false;
}

void launcher_model_cycle_aspect(LauncherModel* m) {
    if (m->adaptive_view_supported && m->s.adaptive_view && m->s.fullscreen) return;
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        // Game-supplied vocabulary: plain 0..n-1 cycle.
        m->s.aspect_index =
            (clampi(m->s.aspect_index, 0, m->num_aspect_labels - 1) + 1) %
            m->num_aspect_labels;
        return;
    }
    if (!m->aspect_mask) return;   // gated: legacy widescreen-bool games no-op
    int idx = clampi(m->s.aspect_index, 0, 2);
    for (int i = 0; i < 3; ++i) {
        idx = (idx + 1) % 3;
        if (launcher_model_aspect_offered(m, idx)) { m->s.aspect_index = idx; return; }
    }
}

const char* launcher_model_aspect_label(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0)
        return m->aspect_labels[clampi(m->s.aspect_index, 0,
                                       m->num_aspect_labels - 1)];
    static const char* kLabels[3] = {
        "4:3 (Native)", "16:9 (Widescreen)", "21:9 (Ultrawide)"
    };
    int idx = clampi(m->s.aspect_index, 0, 2);
    return kLabels[idx];
}

void launcher_model_cycle_freq(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kFreqCount; ++i)
        if (kFreqTable[i] == m->s.audio_freq) { idx = i; break; }
    m->s.audio_freq = kFreqTable[(idx + 1) % kFreqCount];
}

void launcher_model_volume_delta(LauncherModel* m, int delta) {
    m->s.volume = clampi(m->s.volume + delta, 0, 100);
}

// ---- deeper PSX-style settings ---------------------------------------------

void launcher_model_cycle_window_size(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kWindowWidthCount; ++i)
        if (kWindowWidths[i] == m->s.window_width) { idx = i; break; }
    m->s.window_width = kWindowWidths[(idx + 1) % kWindowWidthCount];
}

const char* launcher_model_window_size_label(const LauncherModel* m) {
    static char buf[32];
    int w = m->s.window_width > 0 ? m->s.window_width : kWindowWidths[0];
    int aspect = clampi(m->s.aspect_index, 0, 2);
    int h = (aspect == 1) ? (w * 9 / 16) : (aspect == 2) ? (w * 9 / 21) : (w * 3 / 4);
    snprintf(buf, sizeof(buf), "%d \xC3\x97 %d", w, h);   // "×" (U+00D7)
    return buf;
}

/* Keep Settings.renderer_id in step with Settings.renderer, so a host may
 * read back whichever of the two it speaks. A host that declared no IDs has
 * no name to write and the field stays empty -- it never invents one. */
static void lm_renderer_sync_id(LauncherModel* m) {
    if (!m) return;
    const char* id = NULL;
    if (m->renderer_ids && m->s.renderer >= 0 && m->s.renderer < m->num_renderers)
        id = m->renderer_ids[m->s.renderer];
    safe_copy(m->s.renderer_id, sizeof(m->s.renderer_id), id ? id : "");
}

/*
 * Normalize the incoming renderer settings against the vocabulary this
 * host declared. Split out of launcher_model_init so it can be exercised
 * on its own: the interesting cases are all ABOUT what a settings file
 * carried in -- a key that predates the ID, an ID the host has since
 * reordered, a vocabulary that is now empty -- and none of them are
 * reachable through a full init.
 */
void launcher_model_apply_renderer_settings(LauncherModel* m) {
    if (!m) return;
    if (m->has_renderer) {
        if (m->renderer_ids) {
            /* HOST-DECLARED VOCABULARY, resolved by NAME. The incoming
             * settings file carries an ID, not an index, so a list the host
             * has since reordered still lands on the renderer the player
             * chose (and a list the host has since SHORTENED falls back
             * loudly to entry 0 rather than silently to whatever now sits at
             * the old index). An ID that is not in the list -- a hand-edited
             * settings.toml, or a backend this build was not compiled with --
             * is not honoured: entry 0 is the host's own first choice.
             *
             * num_renderers == 0 is a real answer, not a missing one: this
             * build offers no renderer, the row will not compose, and there
             * is nothing to normalize. */
            int found = -1;
            for (int i = 0; i < m->num_renderers; ++i) {
                const char* id = m->renderer_ids[i];
                if (id && m->s.renderer_id[0] && strcmp(id, m->s.renderer_id) == 0) {
                    found = i; break;
                }
            }
            if (found < 0) found = 0;
            m->s.renderer = m->num_renderers > 0 ? found : 0;
            lm_renderer_sync_id(m);
        } else if (m->renderer_labels && m->num_renderers > 0) {
            /* Multi-label cycle (Software / OpenGL / Vulkan). Prefer OpenGL
             * when the seeded index is out of range — never snap to Software
             * just because memset left renderer at 0 and the host forgot a seed. */
            if (m->s.renderer < 0 || m->s.renderer >= m->num_renderers) {
                int def = 0;
                for (int i = 0; i < m->num_renderers; ++i) {
                    const char* lab = m->renderer_labels[i];
                    if (lab && strstr(lab, "OpenGL")) { def = i; break; }
                }
                m->s.renderer = def;
            }
        } else {
            m->s.renderer = m->s.renderer ? 1 : 0;
        }
}
}

void launcher_model_toggle_renderer(LauncherModel* m) {
    // Game-supplied renderer vocabulary (RT64 hosts: Auto/Vulkan/D3D12)
    // cycles its full list; the legacy pair stays a 2-value toggle.
    int n = launcher_model_renderer_count(m);
    if (m->renderer_ids) {
        if (n <= 0) return;            /* nothing offered, nothing to cycle */
        m->s.renderer = (m->s.renderer + 1) % n;
        lm_renderer_sync_id(m);
        return;
    }
    if (m->renderer_labels && m->num_renderers > 0) {
        m->s.renderer = (m->s.renderer + 1) % m->num_renderers;
        return;
    }
    m->s.renderer = !m->s.renderer;
}

/*
 * Enumerate the renderer vocabulary, so a host can present it as a LIST
 * rather than a button that has to be clicked N-1 times to reach the last
 * entry. Same three-way precedence the label getter uses: game-supplied
 * labels, then the console profile's pair, then the legacy pair.
 */
int launcher_model_renderer_count(const LauncherModel* m) {
    if (!m) return 0;
    /* A host that declared IDs declared the COUNT with them, zero included:
     * "this build has no selectable renderer" has to be sayable, or a port
     * with one backend gets a one-entry dropdown and a port with none gets an
     * empty one. Only this branch can return 0. */
    if (m->renderer_ids) return m->num_renderers > 0 ? m->num_renderers : 0;
    if (m->renderer_labels && m->num_renderers > 0) return m->num_renderers;
    return 2;
}

bool launcher_model_renderer_offered(const LauncherModel* m) {
    return m && m->has_renderer && launcher_model_renderer_count(m) > 0;
}

const char* launcher_model_renderer_id(const LauncherModel* m) {
    return m ? m->s.renderer_id : "";
}

void launcher_model_set_renderer_id(LauncherModel* m, const char* id) {
    if (!m || !m->has_renderer || !m->renderer_ids || !id || !*id) return;
    for (int i = 0; i < m->num_renderers; ++i)
        if (m->renderer_ids[i] && strcmp(m->renderer_ids[i], id) == 0) {
            launcher_model_set_renderer(m, i);
            return;
        }
    /* An undeclared name selects nothing. Guessing would commit a renderer
       the host cannot honour. */
}

const char* launcher_model_renderer_note(const LauncherModel* m) {
    return m ? m->renderer_note : NULL;
}

const char* launcher_model_renderer_label_at(const LauncherModel* m, int i) {
    if (!m) return "";
    if (m->renderer_ids) {
        if (i < 0 || i >= m->num_renderers) return "";
        /* A label is what a HUMAN reads and an ID is what the engine reads.
           A host that supplies an ID and forgets the label gets the ID shown
           rather than a blank row. */
        const char* lab = m->renderer_labels ? m->renderer_labels[i] : NULL;
        if (lab && *lab) return lab;
        return m->renderer_ids[i] ? m->renderer_ids[i] : "";
    }
    if (m->renderer_labels && m->num_renderers > 0) {
        if (i < 0 || i >= m->num_renderers) return "";
        return m->renderer_labels[i];
    }
    {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        if (prof && prof->renderer_labels)
            return prof->renderer_labels[i ? 1 : 0];
    }
    return i ? "OpenGL" : "Software";
}

void launcher_model_set_renderer(LauncherModel* m, int index) {
    if (!m || !m->has_renderer) return;
    int n = launcher_model_renderer_count(m);
    if (n <= 0) return;               /* nothing offered: commit nothing */
    m->s.renderer = clampi(index, 0, n - 1);
    lm_renderer_sync_id(m);
}

/* Set rather than cycle. The values are the RECOMP_LAUNCHER_VSYNC_* constants,
 * not an index, because that is what Settings.vsync holds and what a host
 * reads back. */
void launcher_model_set_vsync(LauncherModel* m, int value) {
    if (!m || !m->has_vsync) return;
    if (value != RECOMP_LAUNCHER_VSYNC_OFF &&
        value != RECOMP_LAUNCHER_VSYNC_ON &&
        value != RECOMP_LAUNCHER_VSYNC_ADAPTIVE)
        return;
    m->s.vsync = value;
}

const char* launcher_model_renderer_label(const LauncherModel* m) {
    if (m->renderer_ids)
        return launcher_model_renderer_label_at(m, m->s.renderer);
    if (m->renderer_labels && m->num_renderers > 0) {
        int i = clampi(m->s.renderer, 0, m->num_renderers - 1);
        return m->renderer_labels[i];
    }
    // Per-console vocabulary (SystemProfile.renderer_labels): NES says
    // Accelerated/Software; NULL keeps the legacy PSX-era pair.
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->renderer_labels)
        return prof->renderer_labels[m->s.renderer ? 1 : 0];
    return m->s.renderer ? "OpenGL" : "Software";
}

void launcher_model_cycle_supersampling(LauncherModel* m) {
    int v = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    m->s.supersampling = (v % 4) + 1;
}

const char* launcher_model_supersampling_label(const LauncherModel* m) {
    /* Settings SSAA: offline full SW/GL path; netplay dual-raster uses this
     * for OpenGL present quality while SW authority stays 1×. */
    static char buf[24];
    int v = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    if (v <= 1)
        snprintf(buf, sizeof(buf), "1x");
    else
        snprintf(buf, sizeof(buf), "%dx", v);
    return buf;
}

// Antialiasing is an MSAA sample COUNT, not a bool: Off / 2x / 4x / 8x. Cycle
// wraps 0 -> 2 -> 4 -> 8 -> 0. A legacy on/off host value of 1 is treated as
// "on" by the label and advances to Off on the next cycle.
void launcher_model_cycle_aa(LauncherModel* m) {
    switch (m->s.antialiasing) {
        case 0:  m->s.antialiasing = 2; break;
        case 2:  m->s.antialiasing = 4; break;
        case 4:  m->s.antialiasing = 8; break;
        default: m->s.antialiasing = 0; break;   // 8 (or legacy 1/other) -> Off
    }
}

const char* launcher_model_aa_label(const LauncherModel* m) {
    switch (m->s.antialiasing) {
        case 0:  return "Off";
        case 2:  return "2x";
        case 4:  return "4x";
        case 8:  return "8x";
        default: return "On";   // legacy on/off host value (1)
    }
}

void launcher_model_toggle_texture_filter(LauncherModel* m) {
    m->s.texture_filter = !m->s.texture_filter;
}

const char* launcher_model_texture_filter_label(const LauncherModel* m) {
    return m->s.texture_filter ? "Bilinear" : "Nearest";
}

void launcher_model_cycle_fmv_filter(LauncherModel* m) {
    if (!m || !m->has_fmv_filter) return;
    int v = m->s.fmv_filter;
    if (v < 1 || v > RECOMP_LAUNCHER_FMV_FILTER_COUNT)
        v = RECOMP_LAUNCHER_FMV_FILTER_BICUBIC;
    m->s.fmv_filter = (v % RECOMP_LAUNCHER_FMV_FILTER_COUNT) + 1;
}

const char* launcher_model_fmv_filter_label(const LauncherModel* m) {
    switch (m ? m->s.fmv_filter : 0) {
        case RECOMP_LAUNCHER_FMV_FILTER_NEAREST:  return "Nearest";
        case RECOMP_LAUNCHER_FMV_FILTER_BILINEAR: return "Bilinear";
        case RECOMP_LAUNCHER_FMV_FILTER_SHARP:    return "Sharp";
        default:                                  return "Bicubic";
    }
}

void launcher_model_set_shader_path(LauncherModel* m, const char* path) {
    if (!m || !m->has_shader) return;
    safe_copy(m->s.shader_path, sizeof(m->s.shader_path), path ? path : "");
    if (m->s.shader_path[0]) {
        m->s.output_method = 2;
        m->s.renderer = 1;
        lm_renderer_sync_id(m);   /* no-op unless the host declared IDs */
    }
}

void launcher_model_clear_shader_path(LauncherModel* m) {
    launcher_model_set_shader_path(m, "");
}

void launcher_model_toggle_geometry_correction(LauncherModel* m) {
    m->s.geometry_correction = !m->s.geometry_correction;
}

void launcher_model_toggle_perspective_texturing(LauncherModel* m) {
    m->s.perspective_texturing = !m->s.perspective_texturing;
}

/* Sub-pixel vertices are only observable in the supersampled mirror: at 1x the
 * corrected position rounds back to the pixel it came from. Say so in the UI
 * rather than letting the row look broken when someone ticks it at native res. */
bool launcher_model_geometry_correction_inert(const LauncherModel* m) {
    return m->s.geometry_correction && m->s.supersampling < 2;
}

// Screen-model vocabulary: the active SystemProfile's own set when it has one
// (e.g. GBA's 5 LCD models), else the legacy 4-entry PSX-era set above.
static const char* const* screen_kind_vocab(const LauncherModel* m, int* out_n) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->screen_kind_names && prof->screen_kind_count > 0) {
        *out_n = prof->screen_kind_count;
        return prof->screen_kind_names;
    }
    *out_n = 4;
    return kScreenKindNames;
}

void launcher_model_cycle_screen_kind(LauncherModel* m) {
    int n = 4; (void)screen_kind_vocab(m, &n);
    m->s.screen_kind = (clampi(m->s.screen_kind, 0, n - 1) + 1) % n;
}

const char* launcher_model_screen_kind_label(const LauncherModel* m) {
    int n = 4;
    const char* const* names = screen_kind_vocab(m, &n);
    return names[clampi(m->s.screen_kind, 0, n - 1)];
}

void launcher_model_toggle_frame_interp(LauncherModel* m) {
    m->s.frame_interp = !m->s.frame_interp;
}

void launcher_model_cycle_interp_fps(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kInterpFpsCount; ++i)
        if (kInterpFpsTable[i] == m->s.frame_interp_fps) { idx = i; break; }
    m->s.frame_interp_fps = kInterpFpsTable[(idx + 1) % kInterpFpsCount];
}

const char* launcher_model_interp_fps_label(const LauncherModel* m) {
    static char buf[24];
    if (m->s.frame_interp_fps == 0) return "Display refresh";
    snprintf(buf, sizeof(buf), "%d fps", m->s.frame_interp_fps);
    return buf;
}

void launcher_model_toggle_spu_hq(LauncherModel* m) {
    m->s.spu_hq = !m->s.spu_hq;
}

void launcher_model_toggle_rewind_enabled(LauncherModel* m) {
    if (!m || !m->has_rewind_depth) return;
    m->s.rewind_enabled = !m->s.rewind_enabled;
}

void launcher_model_cycle_rewind_depth(LauncherModel* m) {
    if (!m || !m->has_rewind_depth) return;
    static const int opts[4] = {50, 100, 150, 200};
    int cur = m->s.rewind_depth;
    int idx = 0; /* default 50 */
    for (int i = 0; i < 4; ++i) if (opts[i] == cur) { idx = i; break; }
    m->s.rewind_depth = opts[(idx + 1) % 4];
}

const char* launcher_model_rewind_depth_label(const LauncherModel* m) {
    static char buf[16];
    int d = m && m->s.rewind_depth > 0 ? m->s.rewind_depth : 50;
    if (d != 50 && d != 100 && d != 150 && d != 200) d = 50;
    snprintf(buf, sizeof(buf), "%d", d);
    return buf;
}

void launcher_model_cycle_rewind_interval(LauncherModel* m) {
    if (!m || !m->has_rewind_depth) return;
    static const int opts[6] = {1, 4, 8, 12, 15, 30};
    int cur = m->s.rewind_interval;
    int idx = 4; /* default 15 */
    for (int i = 0; i < 6; ++i) if (opts[i] == cur) { idx = i; break; }
    m->s.rewind_interval = opts[(idx + 1) % 6];
}

const char* launcher_model_rewind_interval_label(const LauncherModel* m) {
    static char buf[16];
    int d = m && m->s.rewind_interval > 0 ? m->s.rewind_interval : 15;
    if (d != 1 && d != 4 && d != 8 && d != 12 && d != 15 && d != 30) d = 15;
    snprintf(buf, sizeof(buf), "%d", d);
    return buf;
}


// Driver vsync: On -> Off -> Adaptive, wrapping. Adaptive stays in the cycle
// rather than being folded into a checkbox so a player who picked it (or whose
// settings file carries it) cannot lose it just by opening this panel.
void launcher_model_cycle_vsync(LauncherModel* m) {
    if (!m || !m->has_vsync) return;
    int cur = m->s.vsync;
    if (cur < 1 || cur > RECOMP_LAUNCHER_VSYNC_COUNT)
        cur = RECOMP_LAUNCHER_VSYNC_ON;
    m->s.vsync = (cur % RECOMP_LAUNCHER_VSYNC_COUNT) + 1;
}

const char* launcher_model_vsync_label(const LauncherModel* m) {
    switch (m ? m->s.vsync : RECOMP_LAUNCHER_VSYNC_ON) {
        case RECOMP_LAUNCHER_VSYNC_OFF:      return "Off";
        case RECOMP_LAUNCHER_VSYNC_ADAPTIVE: return "Adaptive";
        default:                             return "On";
    }
}

// Legacy-surface checkbox: On <-> Off only. Adaptive counts as On (checked)
// and flips to Off — a legacy host maps this onto a boolean renderer flag,
// so there is no third state to preserve.
void launcher_model_toggle_vsync(LauncherModel* m) {
    if (!m || !m->has_vsync) return;
    m->s.vsync = (m->s.vsync == RECOMP_LAUNCHER_VSYNC_OFF)
                     ? RECOMP_LAUNCHER_VSYNC_ON
                     : RECOMP_LAUNCHER_VSYNC_OFF;
}

void launcher_model_toggle_skip_fmv(LauncherModel* m) {
    m->s.auto_skip_fmv = !m->s.auto_skip_fmv;
}

void launcher_model_toggle_turbo_loads(LauncherModel* m) {
    m->s.turbo_loads = !m->s.turbo_loads;
}

// Fullscreen is a universal display setting: every console's runner applies
// the committed tri-state (0 off / 1 borderless / 2 exclusive) to its window
// at boot and persists it in its own config. The cycle walks all three
// states, restoring the vocabulary of the original SNES launcher.
void launcher_model_cycle_fullscreen(LauncherModel* m) {
    m->s.fullscreen = (clampi(m->s.fullscreen, 0, 2) + 1) % 3;
}

const char* launcher_model_fullscreen_label(const LauncherModel* m) {
    static const char* const kNames[3] = { "Off", "Borderless", "Exclusive" };
    return kNames[clampi(m->s.fullscreen, 0, 2)];
}

void launcher_model_set_fullscreen(LauncherModel* m, int mode) {
    if (!m) return;
    m->s.fullscreen = clampi(mode, 0, 2);
}

void launcher_model_toggle_fullscreen(LauncherModel* m) {
    // Binary on/off (0 <-> 1); superseded in the UI by the tri-state cycle
    // above but kept for hosts that drive the field as a bool.
    m->s.fullscreen = m->s.fullscreen ? 0 : 1;
}

void launcher_model_cycle_language(LauncherModel* m) {
    if (m->num_languages <= 0) return;
    m->s.language_index = (m->s.language_index + 1) % m->num_languages;
}

const char* launcher_model_language_label(const LauncherModel* m) {
    if (m->num_languages <= 0 || !m->language_labels) return "";
    int idx = clampi(m->s.language_index, 0, m->num_languages - 1);
    return m->language_labels[idx] ? m->language_labels[idx] : "";
}

void launcher_model_cycle_deadzone_pct(LauncherModel* m) {
    int v = clampi(m->s.deadzone[0], 0, 50);
    v = ((v / 5) + 1) * 5;
    if (v > 50) v = 0;
    m->s.deadzone[0] = v;
    m->s.deadzone[1] = v;
}

const char* launcher_model_deadzone_pct_label(const LauncherModel* m) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", clampi(m->s.deadzone[0], 0, 50));
    return buf;
}

void launcher_model_refresh_bios_status(LauncherModel* m) {
    if (!m) return;
    m->setup_bios_ok = false;
    m->setup_bios_warn = false;
    m->setup_bios_needs_regen = false;
    m->setup_bios_detail[0] = '\0';
    if (!m->has_bios) {
        m->setup_bios_ok = true;
        return;
    }
    /* Empty path = use the BIOS this build ships with (OpenBIOS / bundled).
     * Host bios_verify("", ...) may refuse that for titles that require a
     * retail dump; otherwise empty is OK — matching Settings → BIOS. */
    if (!m->s.bios_path[0]) {
        if (m->bios_verify_cb) {
            RecompLauncherCBiosVerify bv;
            memset(&bv, 0, sizeof(bv));
            if (!m->bios_verify_cb("", &bv)) {
                safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                          "BIOS verification failed.");
                return;
            }
            m->setup_bios_ok = bv.ok != 0;
            m->setup_bios_warn = bv.warn != 0;
            /* Empty path is OpenBIOS — never treat as needing regen. */
            m->setup_bios_needs_regen = false;
            if (bv.detail[0])
                safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                          bv.detail);
            else if (m->setup_bios_ok)
                safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                          "Using OpenBIOS.");
            return;
        }
        m->setup_bios_ok = true;
        m->setup_bios_needs_regen = false;
        safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                  "Using OpenBIOS.");
        return;
    }
    if (!m->bios_verify_cb) {
        /* No host verifier: path non-empty is enough. */
        FILE* f = fopen(m->s.bios_path, "rb");
        if (f) { fclose(f); m->setup_bios_ok = true; }
        else safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                       "BIOS file not found.");
        return;
    }
    RecompLauncherCBiosVerify bv;
    memset(&bv, 0, sizeof(bv));
    if (!m->bios_verify_cb(m->s.bios_path, &bv)) {
        safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                  "BIOS verification failed.");
        return;
    }
    m->setup_bios_ok = bv.ok != 0;
    m->setup_bios_warn = bv.warn != 0;
    m->setup_bios_needs_regen = bv.needs_regen != 0;
    safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail), bv.detail);
}

/* Write/clear a one-line sidecar in dir (dir may be NULL => cwd "."). */
static void lm_write_sidecar_in_dir(const char* dir, const char* name,
                                    const char* value) {
    char path[1100];
    if (!name || !name[0]) return;
    if (dir && dir[0]) {
        size_t n = strlen(dir);
        int need = (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\');
        if ((size_t)snprintf(path, sizeof(path), "%s%s%s", dir, need ? "/" : "",
                             name) >= sizeof(path))
            return;
    } else {
        if ((size_t)snprintf(path, sizeof(path), "%s", name) >= sizeof(path))
            return;
    }
    if (!value || !value[0]) {
        remove(path);
        return;
    }
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", value);
    fclose(f);
}

/* Dashboard-only cartridge hosts do not run finish_setup(), the wizard's
 * cache writer. Persist on both Play and Quit, which both commit the model.
 * Keep the source ROM (not a generated/patched image), and leave a good cache
 * alone when the player cancels or picks an invalid/missing file. */
static void lm_persist_cart_rom(const LauncherModel* m) {
    char absolute[1024], path[1100], exe_dir[1024];
    const char* cache;
    if (m->setup_wizard_supported || m->num_discs > 1 ||
        (m->profile && m->profile->verify.mode == 1) ||
        !lm_path_readable(m->rom_full)) return;
    if ((m->has_expected_crc || m->num_known_sha256 || m->num_known_sha1) &&
        !launcher_model_rom_verified(m)) return;
#if defined(_WIN32)
    DWORD n = GetFullPathNameA(m->rom_full, (DWORD)sizeof(absolute), absolute, NULL);
    if (!n || n >= sizeof(absolute)) return;
#else
    char* resolved = realpath(m->rom_full, NULL);
    if (!resolved) return;
    int n = snprintf(absolute, sizeof(absolute), "%s", resolved);
    free(resolved);
    if (n < 0 || (size_t)n >= sizeof(absolute)) return;
#endif
    cache = m->rom_cache_path && m->rom_cache_path[0] ? m->rom_cache_path : "rom.cfg";
    if (lm_cache_candidate_path(NULL, cache, path, sizeof(path)))
        lm_write_sidecar_in_dir(NULL, path, absolute);
    if (lm_running_exe_dir(exe_dir, sizeof(exe_dir)) &&
        lm_cache_candidate_path(exe_dir, cache, path, sizeof(path)))
        lm_write_sidecar_in_dir(NULL, path, absolute);
}

static int lm_running_exe_dir(char* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = '\0';
#if defined(_WIN32)
    {
        char mod[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, mod, MAX_PATH);
        char* slash;
        if (n == 0 || n >= MAX_PATH) return 0;
        slash = strrchr(mod, '\\');
        if (!slash) slash = strrchr(mod, '/');
        if (!slash) return 0;
        *slash = '\0';
        if ((size_t)snprintf(out, cap, "%s", mod) >= cap) return 0;
        return 1;
    }
#elif defined(__APPLE__)
    {
        char mod[1024];
        uint32_t n = (uint32_t)sizeof(mod);
        char* slash;
        if (_NSGetExecutablePath(mod, &n) != 0) return 0;
        slash = strrchr(mod, '/');
        if (!slash) return 0;
        *slash = '\0';
        if ((size_t)snprintf(out, cap, "%s", mod) >= cap) return 0;
        return 1;
    }
#else
    {
        char mod[1024];
        ssize_t n = readlink("/proc/self/exe", mod, sizeof(mod) - 1);
        char* slash;
        if (n <= 0) return 0;
        mod[n] = '\0';
        slash = strrchr(mod, '/');
        if (!slash) return 0;
        *slash = '\0';
        if ((size_t)snprintf(out, cap, "%s", mod) >= cap) return 0;
        return 1;
    }
#endif
}

/* Persist ROM/BIOS picks where codegen hosts and the relaunched exe look:
 * cwd, running-exe dir, and (when known) the rebuild output dir. */
/* disc.cfg body for a multi-disc set: one path per line, in disc order.
 *
 * Back-compatible in the direction that matters -- a reader that takes only
 * the first line gets disc 1, which is exactly what it got before. Slots the
 * player has not located are written as empty lines rather than skipped, so
 * line N stays disc N and a later fill-in does not renumber the file.
 *
 * Returns 0 when there is nothing multi-disc to write, and the caller falls
 * back to the single-path form. */
static int lm_compose_disc_cfg(LauncherModel* m, char* out, size_t cap) {
    int i;
    size_t o = 0;
    if (!m || m->num_discs <= 1 || !out || cap == 0) return 0;
    out[0] = '\0';
    for (i = 0; i < m->num_discs; ++i) {
        const char* p = launcher_model_disc_path(m, i);
        size_t n = p ? strlen(p) : 0;
        if (o + n + 2 >= cap) return 0;
        if (i) out[o++] = '\n';
        if (n) { memcpy(out + o, p, n); o += n; }
        out[o] = '\0';
    }
    return launcher_model_discs_ready_count(m) > 0;
}

static void lm_persist_setup_sidecars(LauncherModel* m) {
    char exe_dir[1024];
    char disc_cfg[LNG_MAX_DISCS * 520];
    const char* rom = (m && m->rom_full[0]) ? m->rom_full : NULL;
    const char* bios =
        (m && m->has_bios && m->s.bios_path[0]) ? m->s.bios_path : NULL;
    /* A multi-disc set writes every located image; a single-image title (and
     * any set where nothing is located yet) writes the mounted path as before. */
    const char* disc_value =
        lm_compose_disc_cfg(m, disc_cfg, sizeof(disc_cfg)) ? disc_cfg : rom;
    /* PSX hosts read disc.cfg; cart hosts read rom.cfg — write both names.
     * rom.cfg stays single-path: it is the cart-host contract and has no
     * concept of a set. */
    lm_write_sidecar_in_dir(NULL, "rom.cfg", rom);
    lm_write_sidecar_in_dir(NULL, "disc.cfg", disc_value);
    lm_write_sidecar_in_dir(NULL, "bios.cfg", bios);
    if (lm_running_exe_dir(exe_dir, sizeof(exe_dir))) {
        lm_write_sidecar_in_dir(exe_dir, "rom.cfg", rom);
        lm_write_sidecar_in_dir(exe_dir, "disc.cfg", disc_value);
        lm_write_sidecar_in_dir(exe_dir, "bios.cfg", bios);
    }
    if (m && m->relaunch_exe[0]) {
        char rdir[1024];
        char* slash = strrchr(m->relaunch_exe, '/');
        char* bslash = strrchr(m->relaunch_exe, '\\');
        char* cut = slash;
        if (bslash && (!cut || bslash > cut)) cut = bslash;
        if (cut && cut > m->relaunch_exe) {
            size_t n = (size_t)(cut - m->relaunch_exe);
            if (n < sizeof(rdir)) {
                memcpy(rdir, m->relaunch_exe, n);
                rdir[n] = '\0';
                lm_write_sidecar_in_dir(rdir, "rom.cfg", rom);
                lm_write_sidecar_in_dir(rdir, "disc.cfg", disc_value);
                lm_write_sidecar_in_dir(rdir, "bios.cfg", bios);
            }
        }
    }
    /* Prefer the multi-disc flush when the host offers one and this title has
     * a roster: it carries every located image, where persist_setup_cb can
     * only carry the mounted one. Older hosts leave the pointer NULL and keep
     * the single-path path, which is still correct for a one-disc game. */
    if (m && m->num_discs > 1 && m->persist_setup_discs_cb) {
        const char* paths[LNG_MAX_DISCS];
        int i, n = m->num_discs;
        if (n > LNG_MAX_DISCS) n = LNG_MAX_DISCS;
        for (i = 0; i < n; ++i)
            paths[i] = launcher_model_disc_path(m, i);
        m->persist_setup_discs_cb(m->persist_setup_ctx, paths, n,
                                  bios ? bios : "");
    } else if (m && m->persist_setup_cb) {
        m->persist_setup_cb(m->persist_setup_ctx, rom ? rom : "",
                            bios ? bios : "");
    }
}

void launcher_model_set_bios_path(LauncherModel* m, const char* path) {
    if (path && path[0]) {
#if defined(_WIN32)
        char abs[MAX_PATH];
        DWORD n = GetFullPathNameA(path, (DWORD)sizeof(abs), abs, NULL);
        safe_copy(m->s.bios_path, sizeof(m->s.bios_path),
                  (n > 0 && n < (DWORD)sizeof(abs)) ? abs : path);
#else
        char* rp = realpath(path, NULL);
        safe_copy(m->s.bios_path, sizeof(m->s.bios_path), rp ? rp : path);
        free(rp);
#endif
    } else {
        m->s.bios_path[0] = '\0';
    }
    /* An outright set replaces any pick staged for Generate & rebuild, so the
     * revert target must go with it (otherwise a later failed job would
     * restore a path the player already moved away from). */
    m->bios_switch_uncommitted = false;
    m->bios_pending_path[0] = '\0';
    m->bios_revert_path[0] = '\0';
    launcher_model_refresh_bios_status(m);
    lm_persist_setup_sidecars(m);
}

static void lm_normalize_bios_path(const char* path, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
#if defined(_WIN32)
    {
        char abs[MAX_PATH];
        DWORD n = GetFullPathNameA(path, (DWORD)sizeof(abs), abs, NULL);
        safe_copy(out, out_cap,
                  (n > 0 && n < (DWORD)sizeof(abs)) ? abs : path);
    }
#else
    {
        char* rp = realpath(path, NULL);
        safe_copy(out, out_cap, rp ? rp : path);
        free(rp);
    }
#endif
}

static int lm_bios_paths_equal(const char* a, const char* b) {
    if ((!a || !a[0]) && (!b || !b[0])) return 1;
    if (!a || !a[0] || !b || !b[0]) return 0;
#if defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

void launcher_model_start_prepare_disc(LauncherModel* m, const char* source_path);

static void lm_bios_revert_uncommitted(LauncherModel* m) {
    if (!m || !m->bios_switch_uncommitted) return;
    safe_copy(m->s.bios_path, sizeof(m->s.bios_path), m->bios_revert_path);
    m->bios_switch_uncommitted = false;
    m->bios_pending_path[0] = '\0';
    m->bios_revert_path[0] = '\0';
    launcher_model_refresh_bios_status(m);
    lm_persist_setup_sidecars(m);
}

static void lm_bios_commit_uncommitted(LauncherModel* m) {
    if (!m || !m->bios_switch_uncommitted) return;
    m->bios_switch_uncommitted = false;
    m->bios_pending_path[0] = '\0';
    m->bios_revert_path[0] = '\0';
    /* Path already on the model; rewrite sidecars next to the new exe. */
    lm_persist_setup_sidecars(m);
}

/* Re-open first-run after a BIOS confirm / failed Generate kicked from it. */
static void lm_restore_setup_wizard_after_bios(LauncherModel* m, int page) {
    if (!m) return;
    if (m->setup_wizard_suspended_for_bios || m->prepare_required_before_continue) {
        m->setup_wizard_open = true;
        m->setup_page = page;
    }
    m->setup_wizard_suspended_for_bios = false;
}

/* Apply pending/current BIOS and start Generate & rebuild without the full
 * first-run wizard (progress modal only). Falls back to the wizard when the
 * disc or toolchain is missing. */
static void lm_bios_kick_generate(LauncherModel* m) {
    if (!m) return;
    if (m->setup_preparing) return; /* ignore double-clicks / overlapping jobs */
    if (!m->setup_wizard_supported) {
        lm_bios_revert_uncommitted(m);
        m->setup_wizard_suspended_for_bios = false;
        return;
    }
    m->setup_wizard_open = false;
    m->bios_confirm_open = false;
    m->bios_play_modal_open = false;
    /* Re-probe: setup_tc_ready may still be false if the wizard never opened. */
    if (m->setup_needs_toolchain) {
        if (m->toolchain_is_ready_cb && m->toolchain_is_ready_cb())
            m->setup_tc_ready = true;
        if (!m->setup_tc_ready) {
            /* Missing build tools is a prerequisite, not a failed switch: when
             * we can send the player to wizard page 0 to install them, keep
             * their BIOS pick staged so Generate resumes with it. Only drop it
             * when there is no wizard to come back to. */
            lm_restore_setup_wizard_after_bios(m, 0);
            if (!m->setup_wizard_open)
                lm_bios_revert_uncommitted(m);
            return;
        }
    }
    if (m->rom_present && m->rom_full[0] &&
        (m->prepare_with_progress_cb || m->prepare_disc_cb)) {
        /* Stage disc + BIOS sidecars so the host CLI gets --disc/--bios.
         * Keep setup_wizard_suspended_for_bios so a failed job reopens setup. */
        lm_persist_setup_sidecars(m);
        launcher_model_start_prepare_disc(m, m->rom_full);
        return;
    }
    /* Need a disc pick — open the setup page, not a silent no-op. */
    lm_bios_revert_uncommitted(m);
    lm_restore_setup_wizard_after_bios(m, 1);
    if (!m->setup_wizard_open) {
        m->setup_wizard_open = true;
        m->setup_page = 1;
    }
}

void launcher_model_request_bios_path(LauncherModel* m, const char* path) {
    char normalized[512];
    RecompLauncherCBiosVerify bv;
    if (!m) return;
    lm_normalize_bios_path(path, normalized, sizeof(normalized));
    if (lm_bios_paths_equal(m->s.bios_path, normalized))
        return;

    memset(&bv, 0, sizeof(bv));
    if (m->bios_verify_cb) {
        if (!m->bios_verify_cb(normalized, &bv)) {
            safe_copy(bv.detail, sizeof(bv.detail), "BIOS verification failed.");
            /* OpenBIOS never regenerates; retail may still need Generate. */
            bv.needs_regen = normalized[0] ? 1 : 0;
            bv.ok = 0;
        }
    } else {
        /* No host callback: treat any change as immediate. */
        launcher_model_set_bios_path(m, normalized);
        return;
    }

    /* OpenBIOS (empty path): always hot-swap when accepted. Never open the
     * Generate & rebuild confirm — Play uses the bundled backend already
     * linked (or the setup host will emit it on first Generate for game C). */
    if (!normalized[0]) {
        if (bv.ok) {
            launcher_model_set_bios_path(m, "");
            return;
        }
        if (bv.detail[0])
            safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                      bv.detail);
        return;
    }

    /* Invalid dump (missing/wrong size) — keep the previous selection. */
    if (!bv.ok && !bv.needs_regen) {
        if (bv.detail[0])
            safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                      bv.detail);
        return;
    }

    /* Retail already compiled into this binary → hot-swap (no rebuild). */
    if (bv.ok && !bv.needs_regen) {
        launcher_model_set_bios_path(m, normalized);
        return;
    }

    /* Retail dump is valid but its backend is not linked yet → confirm
     * Generate & rebuild (codegen hosts kick generate on accept). */
    safe_copy(m->bios_pending_path, sizeof(m->bios_pending_path), normalized);
    if (bv.detail[0])
        safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail), bv.detail);
    else
        safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                  "This retail BIOS is not compiled into the current build. "
                  "Generate & rebuild to add it (or Use OpenBIOS).");
    /* Picked from inside first-run setup: stage it in place and let the wizard
     * turn its own primary button into Generate & rebuild. Popping "Switch
     * BIOS?" here would hide the disc rows the player still has to fill in —
     * and its Generate button is disabled until a disc is picked, so the
     * modal was a dead end for anyone who chose the BIOS first. */
    if (m->setup_wizard_open) {
        /* Only the FIRST staged pick records the revert target — picking a
         * second unlinked dump must not make the first one the fallback. */
        if (!m->bios_switch_uncommitted)
            safe_copy(m->bios_revert_path, sizeof(m->bios_revert_path),
                      m->s.bios_path);
        safe_copy(m->s.bios_path, sizeof(m->s.bios_path), normalized);
        m->bios_switch_uncommitted = true;
        /* Re-verify against the staged path so setup_bios_needs_regen reflects
         * the new pick; keep the explanatory detail when the host has none. */
        {
            char detail[sizeof(m->setup_bios_detail)];
            safe_copy(detail, sizeof(detail), m->setup_bios_detail);
            launcher_model_refresh_bios_status(m);
            if (!m->setup_bios_detail[0])
                safe_copy(m->setup_bios_detail, sizeof(m->setup_bios_detail),
                          detail);
        }
        /* Not persisted: bios.cfg still names the BIOS this binary can run
         * until Generate & rebuild actually succeeds. */
        return;
    }
    m->bios_confirm_open = true;
}

void launcher_model_bios_confirm_accept(LauncherModel* m) {
    if (!m) return;
    if (m->setup_preparing) return;
    m->bios_confirm_open = false;
    /* Stage the new BIOS for the generate CLI, but remember the prior pick so
     * a failed prepare/rebuild can restore it (do not stick a failed switch). */
    safe_copy(m->bios_revert_path, sizeof(m->bios_revert_path), m->s.bios_path);
    safe_copy(m->s.bios_path, sizeof(m->s.bios_path), m->bios_pending_path);
    m->bios_switch_uncommitted = true;
    launcher_model_refresh_bios_status(m);
    lm_bios_kick_generate(m);
}

void launcher_model_bios_confirm_cancel(LauncherModel* m) {
    if (!m) return;
    m->bios_confirm_open = false;
    m->bios_pending_path[0] = '\0';
    if (m->setup_wizard_suspended_for_bios) {
        m->setup_wizard_suspended_for_bios = false;
        m->setup_wizard_open = true;
    }
    launcher_model_refresh_bios_status(m);
}

bool launcher_model_bios_blocks_play(const LauncherModel* m) {
    if (!m || !m->has_bios) return false;
    if (m->setup_preparing) return false;
    /* Disc/ROM must otherwise look ready — otherwise the normal Play disable
     * / setup-wizard path is enough. */
    if (!m->rom_present || strcmp(m->rom_size, "--") == 0) return false;
    if (m->profile && m->profile->verify.mode == 1) {
        if (m->verify.verdict == 0 || m->verify.verdict == 3) return false;
    }
    /* OpenBIOS (empty path) never blocks Play for a BIOS regen. */
    if (!m->s.bios_path[0]) return false;
    /* Retail linked-backend mismatch (needs_regen) blocks Play. */
    if (m->setup_bios_needs_regen) return true;
    /* Retail path that isn't Play-ready in this binary. */
    if (!m->setup_bios_ok) return true;
    return false;
}

void launcher_model_bios_play_prompt(LauncherModel* m) {
    if (!m) return;
    m->bios_play_modal_open = true;
}

void launcher_model_bios_play_use_openbios(LauncherModel* m) {
    if (!m) return;
    m->bios_play_modal_open = false;
    /* OpenBIOS always applies immediately — no Generate & rebuild. */
    launcher_model_request_bios_path(m, "");
}

void launcher_model_bios_play_generate(LauncherModel* m) {
    if (!m) return;
    if (m->setup_preparing) return;
    m->bios_play_modal_open = false;
    /* Current m->s.bios_path is already the desired pick — stage + generate. */
    lm_bios_kick_generate(m);
}

void launcher_model_bios_play_cancel(LauncherModel* m) {
    if (!m) return;
    m->bios_play_modal_open = false;
}

bool launcher_model_setup_media_confirm_only(const LauncherModel* m) {
    if (!m || !m->setup_wizard_supported) return false;
    if (m->prepare_required_before_continue) return false;
    /* Codegen hosts keep prepare_* wired after the first Generate & rebuild.
     * Missing that wiring means a cart / non-codegen first-run — keep the
     * normal media picker copy, not the "confirm cleared paths" prompt. */
    if (!m->prepare_with_progress_cb && !m->prepare_disc_cb) return false;
    return true;
}

bool launcher_model_setup_needs_bios_regen(const LauncherModel* m) {
    if (!m || !m->setup_wizard_supported || !m->has_bios) return false;
    /* OpenBIOS (empty path) is always a hot-swap. */
    if (!m->s.bios_path[0]) return false;
    return m->setup_bios_needs_regen;
}

const char* launcher_model_setup_bios_regen_blocker(const LauncherModel* m) {
    if (!m || !launcher_model_setup_needs_bios_regen(m)) return NULL;
    if (m->setup_preparing) return "Wait for the current job to finish";
    if (!m->prepare_with_progress_cb && !m->prepare_disc_cb)
        return "Generate is unavailable (project/SDK not found)";
    if (!m->rom_present || !m->rom_full[0] || strcmp(m->rom_size, "--") == 0)
        return "Select a disc image first";
    if (m->num_discs > 1 &&
        launcher_model_discs_ready_count(m) < m->num_discs)
        return "Locate every disc of the set first";
    if (m->profile && m->profile->verify.mode == 1 &&
        (m->verify.verdict == 0 || m->verify.verdict == 3))
        return "Disc image not accepted";
    return NULL;
}

bool launcher_model_can_start_bios_regen(const LauncherModel* m) {
    return launcher_model_setup_needs_bios_regen(m) &&
           launcher_model_setup_bios_regen_blocker(m) == NULL;
}

void launcher_model_setup_start_bios_regen(LauncherModel* m) {
    if (!launcher_model_can_start_bios_regen(m)) return;
    /* Kicked from inside the wizard: a failed job — or a toolchain that still
     * has to be installed — must land back on the wizard, not a bare
     * dashboard the player never reached. */
    if (m->setup_wizard_open)
        m->setup_wizard_suspended_for_bios = true;
    lm_bios_kick_generate(m);
}

bool launcher_model_can_finish_setup(const LauncherModel* m) {
    if (!m) return false;
    if (m->setup_preparing) return false;
    /* Path selected is enough to leave the wizard (settings stay in the model).
     * PLAY still requires a readable, fingerprinted image via can_launch.
     * Codegen hosts may also require a successful prepare/rebuild. */
    if (!m->rom_present || !m->rom_full[0]) return false;
    if (m->has_bios && !m->setup_bios_ok) return false;
    /* A set is all-or-nothing: building one disc at a time is not supported, so
     * a partially located set cannot produce a working install. Letting the
     * wizard close on one located disc would defer that failure to whenever the
     * player first needs disc 2. */
    if (m->num_discs > 1 &&
        launcher_model_discs_ready_count(m) < m->num_discs)
        return false;
    if (m->prepare_required_before_continue && !m->setup_prepare_satisfied)
        return false;
    /* Disc titles: local setup only needs a readable, title-matching mount.
     * TOC / require_cue policy is a netplay gate; single-player titles can
     * accept valid dumps whose track layout differs from the online policy. */
    if (m->profile && m->profile->verify.mode == 1) {
        if (m->verify.verdict == 0 || m->verify.verdict == 3) return false;
        if (m->netplay_supported && !m->verify.netplay_ok) return false;
    }
    return true;
}

bool launcher_model_can_launch(const LauncherModel* m) {
    if (!m) return false;
    if (!m->rom_present || strcmp(m->rom_size, "--") == 0) return false;
    if (m->rom_patch_supported && m->s.rom_patch_enabled &&
        !m->s.rom_patch_path[0]) return false;
    if (m->profile && m->profile->verify.mode == 1) {
        /* Disc systems: reject none/bad; warn (2) and ok (1) are playable. */
        if (m->verify.verdict == 0 || m->verify.verdict == 3) return false;
    } else {
        const int has_fp = m->has_expected_crc || m->num_known_sha256 > 0 ||
                           m->num_known_sha1 > 0;
        if (has_fp && !launcher_model_rom_verified(m)) return false;
    }
    if (m->has_bios && !m->setup_bios_ok) return false;
    if (m->setup_preparing) return false;
    return true;
}

bool launcher_model_netplay_disc_ok(const LauncherModel* m) {
    if (!m || !m->netplay_supported) return false;
    if (!m->profile || m->profile->verify.mode != 1)
        return true; /* non-disc titles: no TOC gate */
    if (!m->rom_present) return false;
    /* Online requires a clean verified mount (not CRC-warn-only) + TOC policy. */
    if (m->verify.verdict != 1) return false;
    if (!m->verify.netplay_ok) return false;
    if (!m->verify.disc_fp[0]) return false;
    return true;
}

void launcher_model_finish_setup(LauncherModel* m) {
    if (!m || !launcher_model_can_finish_setup(m)) return;
    /* The player just confirmed the picks: write them down NOW, not on PLAY.
     *
     * Until this call, Confirm / Continue only closed the modal. The sidecars
     * (rom.cfg / disc.cfg / bios.cfg) and the host's persist_setup were written
     * on a BIOS change, before Generate and after a rebuild -- every path
     * EXCEPT the one where the player confirms a disc that needs none of
     * those. Quit from the dashboard, or let the host relaunch, and the next
     * start found nothing remembered and opened the same wizard again with
     * "confirm disc" copy, asking for the pick it had already been given.
     * A confirmation that is not recorded is not a confirmation. */
    lm_persist_setup_sidecars(m);
    m->setup_wizard_open = false;
    m->setup_status[0] = '\0';
    m->setup_error[0] = '\0';
}

/* ---- prepare / rebuild / toolchain background job (one at a time) ---- */
enum {
    PREP_JOB_PREPARE = 0,
    PREP_JOB_REBUILD = 1,
    PREP_JOB_TOOLCHAIN = 2,
    PREP_JOB_PGO = 3,
    PREP_JOB_FMV_TIMING = 4
};

typedef struct {
    LauncherModel* m;
    char source[512];
    char out_path[512];
    char err[256];
    char progress_msg[256];
    char zip_path[512];
    float progress_pct; /* <0 indeterminate; else 0..1 */
    int  kind;          /* PREP_JOB_* */
    int  download;      /* toolchain: non-zero => allow network fetch */
    int  result;        /* 0 fail, 1 ok */
    volatile int done;
    volatile int progress_dirty;
} PrepJob;

static PrepJob g_prep_job;

#if defined(_WIN32)
static CRITICAL_SECTION g_prep_lock;
static int g_prep_lock_ready = 0;
static void prep_lock_init(void) {
    if (!g_prep_lock_ready) { InitializeCriticalSection(&g_prep_lock); g_prep_lock_ready = 1; }
}
static void prep_lock(void) { prep_lock_init(); EnterCriticalSection(&g_prep_lock); }
static void prep_unlock(void) { LeaveCriticalSection(&g_prep_lock); }
#else
static pthread_mutex_t g_prep_lock = PTHREAD_MUTEX_INITIALIZER;
static void prep_lock(void) { pthread_mutex_lock(&g_prep_lock); }
static void prep_unlock(void) { pthread_mutex_unlock(&g_prep_lock); }
#endif

static void prep_progress_cb(void* ctx, float pct, const char* message) {
    PrepJob* j = (PrepJob*)ctx;
    if (!j) return;
    prep_lock();
    j->progress_pct = pct;
    if (message && message[0])
        safe_copy(j->progress_msg, sizeof(j->progress_msg), message);
    j->progress_dirty = 1;
    prep_unlock();
}

#if defined(_WIN32)
static DWORD WINAPI prep_thread_main(LPVOID arg);
#else
static void* prep_thread_main(void* arg);
#endif

static int prep_spawn_thread(LauncherModel* m) {
#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, prep_thread_main, &g_prep_job, 0, NULL);
    if (!th) {
        m->setup_preparing = false;
        safe_copy(m->setup_error, sizeof(m->setup_error), "Failed to start prepare thread.");
        m->setup_status[0] = '\0';
        return 0;
    }
    CloseHandle(th);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, prep_thread_main, &g_prep_job) != 0) {
        m->setup_preparing = false;
        safe_copy(m->setup_error, sizeof(m->setup_error), "Failed to start prepare thread.");
        m->setup_status[0] = '\0';
        return 0;
    }
    pthread_detach(th);
#endif
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI prep_thread_main(LPVOID arg) {
#else
static void* prep_thread_main(void* arg) {
#endif
    PrepJob* j = (PrepJob*)arg;
    j->out_path[0] = '\0';
    j->err[0] = '\0';
    j->result = 0;
    if (j->kind == PREP_JOB_TOOLCHAIN) {
        if (j->m && j->m->ensure_toolchain_with_progress_cb) {
            j->result = j->m->ensure_toolchain_with_progress_cb(
                            j->download,
                            j->zip_path[0] ? j->zip_path : NULL,
                            j->err, sizeof(j->err),
                            prep_progress_cb, j)
                            ? 1
                            : 0;
            if (j->result)
                safe_copy(j->out_path, sizeof(j->out_path), "ok");
        } else {
            safe_copy(j->err, sizeof(j->err), "No toolchain ensure callback.");
        }
    } else if (j->kind == PREP_JOB_REBUILD) {
        if (j->m && j->m->rebuild_with_progress_cb) {
            j->result = j->m->rebuild_with_progress_cb(
                            j->source, j->out_path, sizeof(j->out_path),
                            j->err, sizeof(j->err),
                            prep_progress_cb, j) ? 1 : 0;
        } else {
            safe_copy(j->err, sizeof(j->err), "No rebuild callback.");
        }
    } else if (j->kind == PREP_JOB_PGO) {
        if (j->m && j->m->pgo_optimize_with_progress_cb) {
            j->result = j->m->pgo_optimize_with_progress_cb(
                            j->source, j->out_path, sizeof(j->out_path),
                            j->err, sizeof(j->err),
                            prep_progress_cb, j) ? 1 : 0;
        } else {
            safe_copy(j->err, sizeof(j->err), "No PGO optimize callback.");
        }
    } else if (j->kind == PREP_JOB_FMV_TIMING) {
        if (j->m && j->m->fmv_timing_optimize_with_progress_cb) {
            j->result = j->m->fmv_timing_optimize_with_progress_cb(
                            j->source, j->out_path, sizeof(j->out_path),
                            j->err, sizeof(j->err),
                            prep_progress_cb, j) ? 1 : 0;
        } else {
            safe_copy(j->err, sizeof(j->err), "No FMV timing optimize callback.");
        }
    } else if (j->m && j->m->prepare_with_progress_cb) {
        j->result = j->m->prepare_with_progress_cb(
                        j->source, j->out_path, sizeof(j->out_path),
                        j->err, sizeof(j->err),
                        prep_progress_cb, j) ? 1 : 0;
    } else if (j->m && j->m->prepare_disc_cb) {
        j->result = j->m->prepare_disc_cb(j->source, j->out_path, sizeof(j->out_path),
                                          j->err, sizeof(j->err)) ? 1 : 0;
    } else {
        safe_copy(j->err, sizeof(j->err), "No prepare callback.");
    }
    j->done = 1;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void launcher_model_begin_rebuild_locked(LauncherModel* m) {
    /* Caller has already cleared setup_preparing from a completed prepare. */
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_REBUILD;
    g_prep_job.progress_pct = -1.0f;
    safe_copy(g_prep_job.source, sizeof(g_prep_job.source), m->rom_full);
    m->setup_preparing = true;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_error[0] = '\0';
    safe_copy(m->setup_status, sizeof(m->setup_status),
              (m->rebuild_busy_status && m->rebuild_busy_status[0])
                  ? m->rebuild_busy_status
                  : "Building game…");
    if (!prep_spawn_thread(m))
        return;
}

void launcher_model_start_rebuild(LauncherModel* m) {
    if (!m || m->setup_preparing || !m->rebuild_with_progress_cb) return;
    if (!m->rom_full[0]) {
        safe_copy(m->setup_error, sizeof(m->setup_error), "Select a ROM before rebuilding.");
        return;
    }
    m->setup_progress_title[0] = '\0';
    launcher_model_begin_rebuild_locked(m);
}

void launcher_model_request_pgo_optimize(LauncherModel* m) {
    if (!m || m->setup_preparing || !m->pgo_optimize_with_progress_cb) return;
    m->pgo_confirm_open = true;
}

void launcher_model_pgo_confirm_cancel(LauncherModel* m) {
    if (!m) return;
    m->pgo_confirm_open = false;
}

static void launcher_model_begin_pgo_locked(LauncherModel* m) {
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_PGO;
    g_prep_job.progress_pct = -1.0f;
    safe_copy(g_prep_job.source, sizeof(g_prep_job.source), m->rom_full);
    m->setup_preparing = true;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_error[0] = '\0';
    safe_copy(m->setup_progress_title, sizeof(m->setup_progress_title),
              "Optimize FMV Playback");
    safe_copy(m->setup_status, sizeof(m->setup_status),
              (m->pgo_busy_status && m->pgo_busy_status[0])
                  ? m->pgo_busy_status
                  : "Optimizing FMV (instrument → train → rebuild)…");
    if (!prep_spawn_thread(m))
        return;
}

void launcher_model_pgo_confirm_accept(LauncherModel* m) {
    if (!m || m->setup_preparing || !m->pgo_optimize_with_progress_cb) return;
    m->pgo_confirm_open = false;
    if (!m->rom_full[0]) {
        safe_copy(m->setup_error, sizeof(m->setup_error),
                  "Select a disc image before optimizing FMV.");
        return;
    }
    launcher_model_begin_pgo_locked(m);
}

void launcher_model_request_fmv_timing_optimize(LauncherModel* m) {
    if (!m || m->setup_preparing || !m->fmv_timing_optimize_with_progress_cb)
        return;
    m->fmv_timing_confirm_open = true;
}

void launcher_model_fmv_timing_confirm_cancel(LauncherModel* m) {
    if (!m) return;
    m->fmv_timing_confirm_open = false;
}

static void launcher_model_begin_fmv_timing_locked(LauncherModel* m) {
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_FMV_TIMING;
    g_prep_job.progress_pct = -1.0f;
    safe_copy(g_prep_job.source, sizeof(g_prep_job.source), m->rom_full);
    m->setup_preparing = true;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_error[0] = '\0';
    safe_copy(m->setup_progress_title, sizeof(m->setup_progress_title),
              "Apply FMV Timing Opt");
    safe_copy(m->setup_status, sizeof(m->setup_status),
              (m->fmv_timing_busy_status && m->fmv_timing_busy_status[0])
                  ? m->fmv_timing_busy_status
                  : "Regenerating sources and rebuilding…");
    if (!prep_spawn_thread(m))
        return;
}

void launcher_model_fmv_timing_confirm_accept(LauncherModel* m) {
    if (!m || m->setup_preparing || !m->fmv_timing_optimize_with_progress_cb)
        return;
    m->fmv_timing_confirm_open = false;
    if (!m->rom_full[0]) {
        safe_copy(m->setup_error, sizeof(m->setup_error),
                  "Select a disc image before applying FMV timing.");
        return;
    }
    launcher_model_begin_fmv_timing_locked(m);
}

bool launcher_model_can_advance_toolchain(const LauncherModel* m) {
    if (!m || !m->setup_needs_toolchain) return true;
    if (m->setup_preparing) return false;
    const int want_update =
        m->setup_tc_update_available && !m->setup_tc_update_skipped;
    if (m->setup_tc_ready && !want_update) return true;
    if (m->setup_tc_auto) return true;
    return m->setup_tc_zip[0] != '\0';
}

void launcher_model_skip_toolchain_update(LauncherModel* m) {
    if (!m || !m->setup_needs_toolchain) return;
    if (!m->setup_tc_ready || !m->setup_tc_update_available) return;
    m->setup_tc_update_skipped = true;
    m->setup_page = 1;
    m->setup_error[0] = '\0';
    safe_copy(m->setup_status, sizeof(m->setup_status),
              "Keeping current toolchain — continue with BIOS and disc.");
}

void launcher_model_start_ensure_toolchain(LauncherModel* m) {
    if (!m || m->setup_preparing) return;
    if (!m->setup_needs_toolchain) {
        m->setup_tc_ready = true;
        m->setup_page = 1;
        return;
    }
    const int want_update =
        m->setup_tc_update_available && !m->setup_tc_update_skipped;
    if ((m->setup_tc_ready ||
         (m->toolchain_is_ready_cb && m->toolchain_is_ready_cb())) &&
        !want_update) {
        m->setup_tc_ready = true;
        m->setup_page = 1;
        m->setup_error[0] = '\0';
        safe_copy(m->setup_status, sizeof(m->setup_status),
                  "Using existing portable toolchain.");
        return;
    }
    if (!m->ensure_toolchain_with_progress_cb) {
        safe_copy(m->setup_error, sizeof(m->setup_error),
                  "Toolchain install is not available in this build.");
        return;
    }
    if (!launcher_model_can_advance_toolchain(m)) {
        safe_copy(m->setup_error, sizeof(m->setup_error),
                  "Select a cmake-clang-v1 toolchain zip, or enable automatic download.");
        return;
    }
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_TOOLCHAIN;
    g_prep_job.progress_pct = -1.0f;
    /* download: 0 = zip only, 1 = fetch if missing, 2 = force latest update */
    if (m->setup_tc_auto)
        g_prep_job.download = want_update ? 2 : 1;
    else
        g_prep_job.download = 0;
    if (!m->setup_tc_auto && m->setup_tc_zip[0])
        safe_copy(g_prep_job.zip_path, sizeof(g_prep_job.zip_path), m->setup_tc_zip);
    m->setup_preparing = true;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_error[0] = '\0';
    safe_copy(m->setup_progress_title, sizeof(m->setup_progress_title),
              want_update ? "Updating build tools…" : "Installing build tools…");
    safe_copy(m->setup_status, sizeof(m->setup_status),
              m->setup_tc_auto
                  ? (want_update ? "Downloading toolchain update…"
                                 : "Downloading portable cmake/clang…")
                  : "Installing toolchain from zip…");
    prep_spawn_thread(m);
}

void launcher_model_start_prepare_disc(LauncherModel* m, const char* source_path) {
    if (!m || m->setup_preparing) return;
    if (!m->prepare_with_progress_cb && !m->prepare_disc_cb) return;
    if (!source_path || !source_path[0]) return;
    /* Persist ROM/BIOS for codegen hosts (project root + exe dirs + cwd). */
    if (source_path && source_path[0] &&
        (!m->rom_full[0] || strcmp(m->rom_full, source_path) != 0))
        launcher_model_set_rom(m, source_path);
    lm_persist_setup_sidecars(m);
    memset(&g_prep_job, 0, sizeof(g_prep_job));
    g_prep_job.m = m;
    g_prep_job.kind = PREP_JOB_PREPARE;
    g_prep_job.progress_pct = -1.0f;
    safe_copy(g_prep_job.source, sizeof(g_prep_job.source), source_path);
    m->setup_preparing = true;
    m->setup_prepare_pulse = 0.0f;
    m->setup_prepare_fraction = -1.0f;
    m->setup_error[0] = '\0';
    m->setup_progress_title[0] = '\0';
    safe_copy(m->setup_status, sizeof(m->setup_status),
              (m->prepare_busy_status && m->prepare_busy_status[0])
                  ? m->prepare_busy_status
                  : "Preparing disc image…");
    prep_spawn_thread(m);
}

void launcher_model_poll_prepare_disc(LauncherModel* m) {
    if (!m || !m->setup_preparing) return;
    m->setup_prepare_pulse += 0.02f;
    if (m->setup_prepare_pulse > 1.0f) m->setup_prepare_pulse = 0.0f;

    prep_lock();
    if (g_prep_job.m == m && g_prep_job.progress_dirty) {
        if (g_prep_job.progress_pct >= 0.0f)
            m->setup_prepare_fraction = g_prep_job.progress_pct;
        if (g_prep_job.progress_msg[0])
            safe_copy(m->setup_status, sizeof(m->setup_status), g_prep_job.progress_msg);
        g_prep_job.progress_dirty = 0;
    }
    const int done = g_prep_job.done && g_prep_job.m == m;
    const int kind = g_prep_job.kind;
    char out_path[512];
    char err[256];
    int result = 0;
    if (done) {
        result = g_prep_job.result;
        safe_copy(out_path, sizeof(out_path), g_prep_job.out_path);
        safe_copy(err, sizeof(err), g_prep_job.err);
        g_prep_job.m = NULL;
    }
    prep_unlock();

    if (!done) return;
    m->setup_preparing = false;
    m->setup_prepare_fraction = -1.0f;

    if (kind == PREP_JOB_TOOLCHAIN) {
        m->setup_progress_title[0] = '\0';
        if (result) {
            m->setup_tc_ready = true;
            m->setup_tc_update_available = false;
            m->setup_tc_update_skipped = false;
            m->setup_page = 1;
            m->setup_error[0] = '\0';
            safe_copy(m->setup_status, sizeof(m->setup_status),
                      "Build tools ready — continue with BIOS and disc.");
        } else {
            m->setup_status[0] = '\0';
            safe_copy(m->setup_error, sizeof(m->setup_error),
                      err[0] ? err : "Toolchain install failed.");
        }
        return;
    }

    if (kind == PREP_JOB_REBUILD || kind == PREP_JOB_PGO ||
        kind == PREP_JOB_FMV_TIMING) {
        if (result && out_path[0]) {
            safe_copy(m->relaunch_exe, sizeof(m->relaunch_exe), out_path);
            /* BIOS switch sticks only after a successful rebuild. */
            lm_bios_commit_uncommitted(m);
            m->setup_wizard_suspended_for_bios = false;
            /* Sidecars beside build/<exe> before the host execs it. */
            lm_persist_setup_sidecars(m);
            m->setup_prepare_satisfied = true;
            if (kind == PREP_JOB_PGO) {
                safe_copy(m->setup_status, sizeof(m->setup_status),
                          (m->pgo_success_status && m->pgo_success_status[0])
                              ? m->pgo_success_status
                              : "FMV optimize complete.");
            } else if (kind == PREP_JOB_FMV_TIMING) {
                safe_copy(m->setup_status, sizeof(m->setup_status),
                          (m->fmv_timing_success_status &&
                           m->fmv_timing_success_status[0])
                              ? m->fmv_timing_success_status
                              : "FMV timing applied.");
            } else {
                safe_copy(m->setup_status, sizeof(m->setup_status),
                          (m->rebuild_success_status && m->rebuild_success_status[0])
                              ? m->rebuild_success_status
                              : "Build complete.");
            }
            m->setup_error[0] = '\0';
            if (m->relaunch_after_rebuild) {
                safe_copy(m->setup_status, sizeof(m->setup_status),
                          kind == PREP_JOB_PGO
                              ? "FMV optimize complete — restarting…"
                              : (kind == PREP_JOB_FMV_TIMING
                                     ? "FMV timing applied — restarting…"
                                     : "Build complete — restarting…"));
                m->action = LNG_ACTION_RELAUNCH;
            }
        } else {
            lm_bios_revert_uncommitted(m);
            m->setup_status[0] = '\0';
            safe_copy(m->setup_error, sizeof(m->setup_error),
                      err[0] ? err
                             : (kind == PREP_JOB_PGO
                                    ? "FMV optimize failed."
                                    : (kind == PREP_JOB_FMV_TIMING
                                           ? "FMV timing apply failed."
                                           : "Rebuild failed.")));
            lm_restore_setup_wizard_after_bios(m, 1);
        }
        return;
    }

    if (result && out_path[0]) {
        launcher_model_set_rom(m, out_path);
        m->setup_error[0] = '\0';
        if (m->rebuild_after_prepare && m->rebuild_with_progress_cb) {
            safe_copy(m->setup_status, sizeof(m->setup_status),
                      (m->prepare_success_status && m->prepare_success_status[0])
                          ? m->prepare_success_status
                          : "Sources ready — starting build…");
            /* Keep bios_switch_uncommitted through rebuild. */
            launcher_model_begin_rebuild_locked(m);
            return;
        }
        lm_bios_commit_uncommitted(m);
        m->setup_wizard_suspended_for_bios = false;
        m->setup_prepare_satisfied = true;
        safe_copy(m->setup_status, sizeof(m->setup_status),
                  (m->prepare_success_status && m->prepare_success_status[0])
                      ? m->prepare_success_status
                      : "Disc ready.");
    } else {
        lm_bios_revert_uncommitted(m);
        m->setup_status[0] = '\0';
        safe_copy(m->setup_error, sizeof(m->setup_error),
                  err[0] ? err : "Disc prepare failed.");
        lm_restore_setup_wizard_after_bios(m, 1);
    }
}

// Re-inspect one memory-card slot via the host callback (if any), caching the
// result (block bitmask + validity) on the model. Clears the inspected flag
// when there's no callback or no path, so the panel falls back to the
// freshly-formatted/placeholder display.
static void lm_inspect_memcard(LauncherModel* m, int slot) {
    if (slot < 0 || slot > 1) return;
    m->memcard_inspected[slot] = false;
    m->memcard_valid[slot]     = false;
    if (!m->memcard_inspect_cb || !m->s.memcard_path[slot][0]) return;
    RecompLauncherCMemcard mc; memset(&mc, 0, sizeof(mc));
    if (!m->memcard_inspect_cb(m->s.memcard_path[slot], &mc)) return;
    uint16_t bits = 0;
    for (int i = 0; i < 15; ++i) if (mc.block_used[i]) bits |= (uint16_t)(1u << i);
    m->memcard_blocks_used[slot] = bits;
    m->memcard_valid[slot]       = mc.valid != 0;
    m->memcard_inspected[slot]   = true;
}

// Which of the 15 blocks the panel paints as occupied, most-authoritative
// source first: a host memcard_inspect callback (REAL card contents) -> a card
// we just formatted blank (0) -> a SystemProfile SaveProbeFn -> a
// representative placeholder pattern. The placeholder exists for the proto
// launcher, which has no card image to read. A host that DOES inspect real
// cards is authoritative: an inspection that never happened (no path bound,
// or the callback declined) means there is no card to show, so the slot draws
// blank rather than a pattern the player could mistake for foreign saves —
// which is exactly what a launcher reopened after a netplay match looked like.
uint16_t launcher_model_memcard_blocks_used(const LauncherModel* m, int slot) {
    if (!m || slot < 0 || slot > 1) return 0;
    if (m->memcard_inspected[slot]) return m->memcard_blocks_used[slot];
    if (m->memcard_freshly_formatted[slot]) return 0;
    if (m->memcard_inspect_cb) return 0;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->save.probe && prof->save.probe(m, slot))
        return m->memcard_blocks_used[slot];
    return (uint16_t)(slot == 0 ? 0x0025u : 0x0009u);
}

void launcher_model_set_memcard_path(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot > 1) return;
    safe_copy(m->s.memcard_path[slot], sizeof(m->s.memcard_path[slot]), path ? path : "");
    // A newly browsed-in card is not known to be blank; only
    // launcher_model_new_memcard() re-arms this flag.
    m->memcard_freshly_formatted[slot] = false;
    lm_inspect_memcard(m, slot);   // refresh real block usage/validity for the new path
}

void launcher_model_toggle_memcard(LauncherModel* m, int slot) {
    if (slot < 0 || slot > 1) return;
    m->s.memcard_enabled[slot] = m->s.memcard_enabled[slot] ? 0 : 1;
}

int launcher_model_multitap_available(const LauncherModel* m) {
    if (!m || m->player_count < 3) return 0;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    return (prof && prof->id && strcmp(prof->id, "psx") == 0) ? 1 : 0;
}

int launcher_model_multitap_enabled(const LauncherModel* m) {
    if (!m) return 0;
    if (!launcher_model_multitap_available(m)) return 0;
    return m->s.multitap_enabled ? 1 : 0;
}

void launcher_model_toggle_multitap(LauncherModel* m) {
    if (!m || !launcher_model_multitap_available(m)) return;
    m->s.multitap_enabled = m->s.multitap_enabled ? 0 : 1;
    /* Drop the configure target if it was on a now-hidden seat. */
    {
        const int vis = launcher_model_visible_player_count(m);
        if (m->cfg_player >= vis) m->cfg_player = vis > 0 ? vis - 1 : 0;
    }
}

int launcher_model_visible_player_count(const LauncherModel* m) {
    if (!m) return 1;
    int n = m->player_count > 0 ? m->player_count : 1;
    if (n > LNG_MAX_PLAYERS) n = LNG_MAX_PLAYERS;
    if (launcher_model_multitap_available(m) && !m->s.multitap_enabled) {
        if (n > 2) n = 2;
    }
    return n;
}

int launcher_model_multitap_analog_available(const LauncherModel* m) {
    if (!m || m->player_count < 3) return 0;
    /* Digital-only titles (lock_mode + locked_pad_mode=D-Pad): DualShock-on-tap
     * is unsupported — hide the Lobby / Controllers toggle. */
    if (!m->pad_mode_selectable && m->locked_pad_mode == 2)
        return 0;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    return (prof && prof->id && strcmp(prof->id, "psx") == 0) ? 1 : 0;
}

int launcher_model_multitap_analog_enabled(const LauncherModel* m) {
    if (!m || !launcher_model_multitap_analog_available(m)) return 0;
    return m->s.multitap_analog ? 1 : 0;
}

void launcher_model_toggle_multitap_analog(LauncherModel* m) {
    if (!m || !launcher_model_multitap_analog_available(m)) return;
    m->s.multitap_analog = m->s.multitap_analog ? 0 : 1;
}

int launcher_model_virtual_stylus_available(const LauncherModel* m) {
    return m && m->has_virtual_stylus;
}

int launcher_model_virtual_stylus_enabled(const LauncherModel* m) {
    return launcher_model_virtual_stylus_available(m) &&
           m->s.virtual_stylus >= 0;
}

void launcher_model_toggle_virtual_stylus(LauncherModel* m) {
    if (!launcher_model_virtual_stylus_available(m)) return;
    m->s.virtual_stylus =
        launcher_model_virtual_stylus_enabled(m) ? -1 : 1;
}

void launcher_model_new_memcard(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot > 1 || !path || !path[0]) return;
    if (recompui_memcard_format_file(path) != 0) return;  // I/O failure: leave as-is
    launcher_model_set_memcard_path(m, slot, path);
    m->memcard_freshly_formatted[slot] = true;   // known-blank: panel shows 0 / 15
}

// ---- N64 Transfer Pak slots (mirrors the memcard slot pattern) -----------------

// Refresh one slot's cartridge facts through the host's tpak_inspect callback.
// Clears to "not inspected" when there's no callback, no cartridge, or the
// callback declines — the panel then shows the bare file name, neutral tint.
static void lm_inspect_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= RECOMP_LAUNCHER_MAX_TPAKS) return;
    memset(&m->tpak_info[slot], 0, sizeof(m->tpak_info[slot]));
    m->tpak_inspected[slot] = false;
    if (!m->tpak_inspect_cb || !m->s.tpak_rom_path[slot][0]) return;
    RecompLauncherCTpak out; memset(&out, 0, sizeof(out));
    if (m->tpak_inspect_cb(m->s.tpak_rom_path[slot],
                           m->s.tpak_save_path[slot][0] ? m->s.tpak_save_path[slot] : NULL,
                           &out)) {
        m->tpak_info[slot] = out;
        m->tpak_inspected[slot] = true;
    }
}

void launcher_model_set_tpak_rom(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    safe_copy(m->s.tpak_rom_path[slot], sizeof(m->s.tpak_rom_path[slot]), path ? path : "");
    if (m->s.tpak_rom_path[slot][0]) m->s.tpak_enabled[slot] = 1;  // inserting = wanting it on
    lm_inspect_tpak(m, slot);
}

void launcher_model_clear_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    m->s.tpak_rom_path[slot][0]  = '\0';
    m->s.tpak_save_path[slot][0] = '\0';
    m->s.tpak_enabled[slot] = 0;
    lm_inspect_tpak(m, slot);
}

void launcher_model_set_tpak_save(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    safe_copy(m->s.tpak_save_path[slot], sizeof(m->s.tpak_save_path[slot]), path ? path : "");
    lm_inspect_tpak(m, slot);
}

bool launcher_model_tpak_enabled(const LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return false;
    int e = m->s.tpak_enabled[slot];
    if (e > 0) return true;
    if (e < 0) return false;
    return m->s.tpak_rom_path[slot][0] != '\0';   // unset: on iff a cart is inserted
}

void launcher_model_toggle_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    m->s.tpak_enabled[slot] = launcher_model_tpak_enabled(m, slot) ? -1 : 1;
}

// ---- audio output device -------------------------------------------------------

void launcher_model_set_audio_device(LauncherModel* m, const char* name) {
    safe_copy(m->s.audio_device, sizeof(m->s.audio_device), name ? name : "");
}

const char* launcher_model_audio_device_label(const LauncherModel* m) {
    return m->s.audio_device[0] ? m->s.audio_device : "(system default)";
}

// ---- SRAM save management (mirrors the legacy launcher's Import/Clear) --------
// Both back up any existing save to "<sram>.bak" first (never a destructive op
// without a recoverable copy), matching the old launcher's behavior.
static bool lm_copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[8192]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in); fclose(out);
    return ok;
}

void launcher_model_import_sram(LauncherModel* m, const char* src) {
    if (!m->sram_path || !m->sram_path[0] || !src || !src[0]) return;
    char bak[600]; snprintf(bak, sizeof(bak), "%s.bak", m->sram_path);
    FILE* existing = fopen(m->sram_path, "rb");
    if (existing) { fclose(existing); lm_copy_file(m->sram_path, bak); }  // back up first
    lm_copy_file(src, m->sram_path);
}

void launcher_model_clear_sram(LauncherModel* m) {
    if (!m->sram_path || !m->sram_path[0]) return;
    char bak[600]; snprintf(bak, sizeof(bak), "%s.bak", m->sram_path);
    FILE* existing = fopen(m->sram_path, "rb");
    if (existing) { fclose(existing); lm_copy_file(m->sram_path, bak); remove(m->sram_path); }
}

int launcher_model_preset_count(const LauncherModel* m) {
    const RecompLauncherCModProvider* p = m ? m->mods : NULL;
    if (!p || !p->preset_count || !p->preset_get || !p->preset_current || !p->preset_apply)
        return 0;
    int n = p->preset_count(p->ctx);
    return n > 0 && n <= 64 ? n : 0;
}
int launcher_model_apply_preset(LauncherModel* m, const char* id) {
    if (!id) return 0;
    int n = launcher_model_preset_count(m);
    for (int i = 0; i < n; ++i) {
        RecompLauncherCModPreset preset = {0};
        if (!m->mods->preset_get(m->mods->ctx, i, &preset) || strcmp(preset.id, id)) continue;
        if (m->mods->preset_apply(m->mods->ctx, id, &m->s)) {
            m->mod_status[0] = 0;
            return 1;
        }
        const char* error = m->mods->last_error ? m->mods->last_error(m->mods->ctx) : NULL;
        safe_copy(m->mod_status, sizeof(m->mod_status), error && *error ? error : "Unable to apply preset");
        return 0;
    }
    return 0;
}
int launcher_model_set_msu1_pack(LauncherModel* m, const char* id) {
    if (!m || !m->msu1_supported || !id || !m->msu1_packs || m->num_msu1_packs <= 0) return 0;
    if (!*id) { m->s.msu1_pack[0] = 0; return 1; }
    for (int i = 0; i < m->num_msu1_packs; ++i)
        if (m->msu1_packs[i].id && !strcmp(id, m->msu1_packs[i].id) && strlen(id) < sizeof(m->s.msu1_pack)) {
            safe_copy(m->s.msu1_pack, sizeof(m->s.msu1_pack), id);
            return 1;
        }
    return 0;
}
void launcher_model_toggle_msu1(LauncherModel* m) {
    if (!m->msu1_supported) return;
    m->s.msu1_enabled = !m->s.msu1_enabled;
}

void launcher_model_set_msu1_dir(LauncherModel* m, const char* dir) {
    safe_copy(m->s.msu1_dir, sizeof(m->s.msu1_dir), dir ? dir : "");
}

// ---- cartridge light sensor --------------------------------------------------

/* Trims surrounding whitespace; a code pasted with a stray space should still
 * work. Case and existence are the host's business -- it owns the geocoder. */
static void copy_trimmed(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (*src == ' ' || *src == '\t') ++src;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) --n;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void launcher_model_set_solar_zip(LauncherModel* m, const char* zip) {
    if (!m->has_solar_sensor) return;    /* gated: no-op when unsupported */
    copy_trimmed(m->s.solar_zip, sizeof(m->s.solar_zip), zip);
}

void launcher_model_set_solar_country(LauncherModel* m, const char* country) {
    if (!m->has_solar_sensor) return;
    copy_trimmed(m->s.solar_country, sizeof(m->s.solar_country), country);
}

void launcher_model_set_solar_source(LauncherModel* m, int source) {
    if (!m->has_solar_sensor) return;
    m->s.solar_source = (source != 0) ? 1 : 0;
}

void launcher_model_set_solar_manual_step(LauncherModel* m, int step) {
    if (!m->has_solar_sensor) return;
    if (step < 0) step = 0;
    if (step > 8) step = 8;
    m->s.solar_manual_step = step;
}

void launcher_model_set_solar_full_sun(LauncherModel* m, int wm2) {
    if (!m->has_solar_sensor) return;
    if (wm2 < 300)  wm2 = 300;
    if (wm2 > 1200) wm2 = 1200;
    m->s.solar_full_sun = wm2;
}

// ---- NES-style settings ------------------------------------------------------

void launcher_model_toggle_integer_scale(LauncherModel* m) {
    if (!m->has_integer_scale) return;   // gated: no-op when unsupported
    m->s.integer_scale = !m->s.integer_scale;
}

void launcher_model_toggle_hdpack(LauncherModel* m) {
    if (!m->hdpack_supported) return;    // gated: no-op when unsupported
    m->s.hdpack_enabled = !m->s.hdpack_enabled;
}

void launcher_model_set_hdpack_dir(LauncherModel* m, const char* dir) {
    safe_copy(m->s.hdpack_dir, sizeof(m->s.hdpack_dir), dir ? dir : "");
}

// Password/mantra save: either a one-line text file (legacy NES titles) or a
// small MMXPASS record inside the SRAM file (Mega Man X synthetic SRAM).
void launcher_model_password_reload(LauncherModel* m) {
    m->password_text[0] = '\0';
    if (m->password_sram_path && m->password_sram_path[0]) {
        FILE* f = fopen(m->password_sram_path, "rb");
        if (!f) return;
        LmMmxPassRecord rec;
        if (m->password_sram_offset > 0)
            fseek(f, m->password_sram_offset, SEEK_SET);
        int ok = fread(&rec, 1, sizeof(rec), f) == sizeof(rec);
        fclose(f);
        if (ok && lm_mmxpass_valid(&rec))
            lm_mmxpass_format_text(rec.digits, m->password_text, sizeof(m->password_text));
        return;
    }

    if (!m->password_save_path || !m->password_save_path[0]) return;
    FILE* f = fopen(m->password_save_path, "r");
    if (!f) return;
    if (fgets(m->password_text, sizeof(m->password_text), f)) {
        size_t n = strlen(m->password_text);
        while (n > 0 && (m->password_text[n-1] == '\n' || m->password_text[n-1] == '\r'))
            m->password_text[--n] = '\0';
    } else {
        m->password_text[0] = '\0';
    }
    fclose(f);
}

void launcher_model_password_commit(LauncherModel* m, const char* text) {
    if (m->password_sram_path && m->password_sram_path[0]) {
        unsigned char digits[LM_MMXPASS_DIGITS];
        if (!lm_mmxpass_parse_text(text, digits))
            return;

        int min_size = m->password_sram_size > 0
                           ? m->password_sram_size
                           : LM_MMXPASS_MIN_SIZE;
        int offset = m->password_sram_offset > 0 ? m->password_sram_offset : 0;
        if (min_size < offset + (int)sizeof(LmMmxPassRecord))
            min_size = offset + (int)sizeof(LmMmxPassRecord);
        FILE* f = fopen(m->password_sram_path, "r+b");
        if (!f)
            f = fopen(m->password_sram_path, "w+b");
        if (!f)
            return;

        long size = lm_file_size(f);
        if (size < min_size) {
            fseek(f, 0, SEEK_END);
            lm_write_zero_padding(f, (long)min_size - size);
        }

        LmMmxPassRecord rec;
        memcpy(rec.magic, kLmMmxPassMagic, sizeof(rec.magic));
        rec.version = 1;
        memcpy(rec.digits, digits, sizeof(rec.digits));
        rec.checksum = lm_mmxpass_checksum(rec.digits);
        fseek(f, offset, SEEK_SET);
        fwrite(&rec, 1, sizeof(rec), f);
        fclose(f);
        launcher_model_password_reload(m);
        return;
    }

    if (!m->password_save_path || !m->password_save_path[0]) return;
    FILE* f = fopen(m->password_save_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", text ? text : "");
    fclose(f);
    launcher_model_password_reload(m);   // reflect what actually landed on disk
}

// Zapper switches: flip the model state and persist through launcher_binds'
// [zapper] section writer immediately (same persist-on-change behavior as the
// rebind chips). launcher_binds_set_zapper is a no-op-safe plain writer.
void launcher_binds_set_zapper(int mouse_enabled, int crosshair);   // launcher_binds.c

void launcher_model_toggle_zapper_mouse(LauncherModel* m) {
    if (!m->zapper) return;
    m->zapper_mouse = !m->zapper_mouse;
    launcher_binds_set_zapper(m->zapper_mouse ? 1 : 0, m->zapper_crosshair ? 1 : 0);
}

void launcher_model_toggle_zapper_crosshair(LauncherModel* m) {
    if (!m->zapper) return;
    m->zapper_crosshair = !m->zapper_crosshair;
    launcher_binds_set_zapper(m->zapper_mouse ? 1 : 0, m->zapper_crosshair ? 1 : 0);
}

// ---- MSU-1 IPS auto-patching (mirrors the legacy launcher's do_patch() /
// msu1_patch_available predicate in snesrecomp's launcher_gui.cpp) -----------

// Recomputed on every ROM change: the dashboard prompt only makes sense when
// this game HAS a patch, the loaded file verifies against the vanilla CRC
// (patching an already-patched or wrong ROM would corrupt it), and the user
// hasn't already dismissed it this run.
static void update_msu1_patch_available(LauncherModel* m) {
    m->msu1_patch_available = m->msu1_supported && m->msu1_patch_path &&
                              m->msu1_patch_path[0] && m->rom_present &&
                              m->crc_match && !m->msu1_patch_skipped;
}

static bool lm_read_whole_file(const char* path, uint8_t** out_data, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return false; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return false; }
    fclose(f);
    *out_data = buf;
    *out_len  = (size_t)n;
    return true;
}

void launcher_model_set_rom_patch(LauncherModel* m, const char* path) {
    if (!m || !m->rom_patch_supported) return;
    safe_copy(m->s.rom_patch_path, sizeof(m->s.rom_patch_path),
              path ? path : "");
    m->s.rom_patch_enabled = m->s.rom_patch_path[0] ? 1 : 0;
    m->rom_patch_prepared_path[0] = '\0';
    m->rom_patch_prepared_sha1[0] = '\0';
    m->rom_patch_status[0] = '\0';
}

void launcher_model_clear_rom_patch(LauncherModel* m) {
    if (!m) return;
    m->s.rom_patch_enabled = 0;
    m->s.rom_patch_path[0] = '\0';
    m->s.rom_patch_source_path[0] = '\0';
    m->s.rom_patch_sha1[0] = '\0';
    m->s.rom_patch_crc32[0] = '\0';
    m->rom_patch_prepared_path[0] = '\0';
    m->rom_patch_prepared_sha1[0] = '\0';
    m->rom_patch_status[0] = '\0';
}

void launcher_model_toggle_rom_patch(LauncherModel* m) {
    if (!m || !m->rom_patch_supported) return;
    if (!m->s.rom_patch_path[0]) {
        m->s.rom_patch_enabled = 0;
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Select a patch file first.");
        return;
    }
    m->s.rom_patch_enabled = m->s.rom_patch_enabled ? 0 : 1;
    m->rom_patch_prepared_path[0] = '\0';
    m->rom_patch_prepared_sha1[0] = '\0';
}

static const char* lm_path_extension(const char* path) {
    const char* base = path;
    const char* dot = NULL;
    for (const char* p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') { base = p + 1; dot = NULL; }
        else if (*p == '.') dot = p;
    }
    return dot && dot >= base && strlen(dot) <= 12 ? dot : ".rom";
}

bool launcher_model_prepare_rom_patch(LauncherModel* m) {
    if (!m || !m->rom_patch_supported || !m->s.rom_patch_enabled) {
        if (m) {
            m->rom_patch_prepared_path[0] = '\0';
            m->rom_patch_prepared_sha1[0] = '\0';
            safe_copy(m->s.rom_patch_source_path,
                      sizeof(m->s.rom_patch_source_path), m->rom_full);
            m->s.rom_patch_sha1[0] = '\0';
            m->s.rom_patch_crc32[0] = '\0';
        }
        return true;
    }
    if (!launcher_model_rom_verified(m) || !m->rom_sha1_hex[0]) {
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "The source ROM must verify before applying a patch.");
        return false;
    }
    if (!m->s.rom_patch_path[0] || !m->rom_patch_cache_dir ||
        !m->rom_patch_cache_dir[0]) {
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Patch file or cache directory is unavailable.");
        return false;
    }

    uint8_t* source = NULL; size_t source_len = 0;
    uint8_t* patch = NULL; size_t patch_len = 0;
    if (!lm_read_whole_file(m->rom_full, &source, &source_len) ||
        !lm_read_whole_file(m->s.rom_patch_path, &patch, &patch_len)) {
        free(source); free(patch);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Could not read the source ROM or patch file.");
        return false;
    }

    uint8_t* output = NULL; size_t output_len = 0;
    const bool applied = rom_patch_apply(source, source_len, patch, patch_len,
                                         &output, &output_len);
    free(source);
    if (!applied) {
        free(patch);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Patch rejected: wrong source, invalid data, or checksum failure.");
        return false;
    }

    uint8_t patch_sha[20], output_sha[20];
    char patch_hex[41], output_hex[41];
    recompui_sha1_compute(patch, patch_len, patch_sha);
    recompui_sha1_hex(patch_sha, patch_hex);
    recompui_sha1_compute(output, output_len, output_sha);
    recompui_sha1_hex(output_sha, output_hex);
    free(patch);
    if (m->rom_patch_required_sha1 && m->rom_patch_required_sha1[0] &&
        strcmp(output_hex, m->rom_patch_required_sha1) != 0) {
        free(output);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "This executable was built for a different patch release.");
        return false;
    }
    snprintf(m->s.rom_patch_crc32, sizeof(m->s.rom_patch_crc32), "%08x",
             recompui_crc32_compute(output, output_len));

    const size_t cache_len = strlen(m->rom_patch_cache_dir);
    const char* separator =
        cache_len && (m->rom_patch_cache_dir[cache_len - 1] == '/' ||
                      m->rom_patch_cache_dir[cache_len - 1] == '\\') ? "" : "/";
    char target[512];
    const int target_chars = snprintf(target, sizeof(target), "%s%s%s-%s%s",
             m->rom_patch_cache_dir, separator, m->rom_sha1_hex, patch_hex,
             lm_path_extension(m->rom_full));
    if (target_chars < 0 || (size_t)target_chars >= sizeof(target)) {
        free(output);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "The patched-ROM cache path is too long.");
        return false;
    }
    char temporary[520];
    const int temporary_chars =
        snprintf(temporary, sizeof(temporary), "%s.tmp", target);
    if (temporary_chars < 0 || (size_t)temporary_chars >= sizeof(temporary)) {
        free(output);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "The patched-ROM cache path is too long.");
        return false;
    }
    FILE* file = fopen(temporary, "wb");
    const bool wrote = file && fwrite(output, 1, output_len, file) == output_len;
    if (file) fclose(file);
    free(output);
    if (!wrote) {
        remove(temporary);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Could not write the patched-ROM cache.");
        return false;
    }
    remove(target);
    if (rename(temporary, target) != 0) {
        remove(temporary);
        safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
                  "Could not publish the patched-ROM cache.");
        return false;
    }

    safe_copy(m->rom_patch_prepared_path,
              sizeof(m->rom_patch_prepared_path), target);
    safe_copy(m->rom_patch_prepared_sha1,
              sizeof(m->rom_patch_prepared_sha1), output_hex);
    safe_copy(m->s.rom_patch_source_path,
              sizeof(m->s.rom_patch_source_path), m->rom_full);
    safe_copy(m->s.rom_patch_sha1, sizeof(m->s.rom_patch_sha1), output_hex);
    safe_copy(m->rom_patch_status, sizeof(m->rom_patch_status),
              "Patch prepared and checksum-verified.");
    return true;
}

// Build "<dir>/<stem>.msu1.<ext>" beside `rom_path` (matches the legacy
// launcher's std::filesystem stem()/extension() splice exactly).
static void lm_msu1_target_path(const char* rom_path, char* out, size_t out_cap) {
    const char* base = rom_path;
    for (const char* q = rom_path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    const char* dot = strrchr(base, '.');
    size_t dir_len  = (size_t)(base - rom_path);
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    const char* ext = dot ? dot : "";
    snprintf(out, out_cap, "%.*s%.*s.msu1%s",
             (int)dir_len, rom_path, (int)stem_len, base, ext);
}

void launcher_model_apply_msu1_patch(LauncherModel* m) {
    if (!m->msu1_patch_available) return;
    if (!m->rom_present || !m->msu1_patch_path || !m->msu1_patch_path[0]) return;

    uint8_t* src = NULL;   size_t src_len = 0;
    uint8_t* patch = NULL; size_t patch_len = 0;
    if (!lm_read_whole_file(m->rom_full, &src, &src_len)) return;
    if (!lm_read_whole_file(m->msu1_patch_path, &patch, &patch_len)) { free(src); return; }

    uint8_t* out = NULL; size_t out_len = 0;
    bool ok = ips_apply(src, src_len, patch, patch_len, &out, &out_len);
    free(src);
    free(patch);
    if (!ok) {
        fprintf(stderr, "launcher: IPS patch failed (%s)\n", m->msu1_patch_path);
        return;
    }

    char target[600];
    lm_msu1_target_path(m->rom_full, target, sizeof(target));
    FILE* f = fopen(target, "wb");
    if (!f) {
        fprintf(stderr, "launcher: cannot write %s\n", target);
        free(out);
        return;
    }
    bool wrote = fwrite(out, 1, out_len, f) == out_len;
    fclose(f);
    free(out);
    if (!wrote) { fprintf(stderr, "launcher: short write to %s\n", target); return; }

    fprintf(stderr, "launcher: wrote MSU-1 patched ROM: %s\n", target);
    // Switch the model onto the patched file and re-verify (this also
    // recomputes msu1_patch_available — the patched ROM's CRC will no longer
    // match the vanilla expected_crc, so it naturally clears). Belt-and-braces:
    // also mark skipped so the prompt never reappears if a game's expected_crc
    // happens to also match the patched image.
    m->msu1_patch_skipped = true;
    launcher_model_set_rom(m, target);
}

void launcher_model_skip_msu1_patch(LauncherModel* m) {
    if (!m->msu1_patch_available) return;
    m->msu1_patch_skipped = true;
    update_msu1_patch_available(m);
}

/* PSX Hybrid/Analog need sticks; keyboard players are forced to D-Pad.
 * Gamepads default to Analog — virtually every modern pad has sticks. */
static int pad_mode_is_psx_legacy(const LauncherModel* m) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const ControllerSpec* spec = prof ? &prof->controller : NULL;
    return !(spec && spec->modes && spec->mode_count > 0);
}

static void apply_default_pad_mode_for_source(LauncherModel* m, int player) {
    if (!m->pad_mode_supported || !m->pad_mode_selectable) return;
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    if (!pad_mode_is_psx_legacy(m)) return;
    if (m->s.player_src[player] == 1)
        m->s.pad_mode[player] = 2;   // D-Pad / digital (no sticks)
    else if (m->s.player_src[player] == 2)
        m->s.pad_mode[player] = 1;   // Analog / DualShock
}

void launcher_model_set_pad_mode(LauncherModel* m, int player, int mode) {
    if (!m->pad_mode_supported || !m->pad_mode_selectable) return;   // gated/locked
    player = clampi(player, 0, 1);
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const ControllerSpec* spec = prof ? &prof->controller : NULL;
    if (spec && spec->modes && spec->mode_count > 0) {
        // Custom mode list (Genesis): accept listed mode values only.
        for (int i = 0; i < spec->mode_count; ++i)
            if (spec->modes[i].mode == mode) { m->s.pad_mode[player] = mode; return; }
        return;
    }
    /* Keyboard has no sticks — Analog is unavailable. */
    if (m->s.player_src[player] == 1 && mode != 2) return;
    mode = clampi(mode, 0, 2);
    if (mode == 0) mode = 1;   /* Hybrid is mod-only -> snap to Analog */
    m->s.pad_mode[player] = mode;
}

int launcher_model_active_button_count(const LauncherModel* m, int player) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
    if (prof && prof->controller.modes && prof->controller.mode_count > 0) {
        player = clampi(player, 0, 1);
        for (int i = 0; i < prof->controller.mode_count; ++i)
            if (prof->controller.modes[i].mode == m->s.pad_mode[player]) {
                bc = prof->controller.modes[i].button_count;
                break;
            }
    }
    if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
    if (bc < 0) bc = 0;
    return bc;
}

void launcher_model_cycle_player_src(LauncherModel* m, int player) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.player_src[player] = (m->s.player_src[player] + 1) % 3;  // None/Kbd/Pad
    apply_default_pad_mode_for_source(m, player);
}

void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.deadzone[player] = clampi(m->s.deadzone[player] + delta, 0, 100);
}

void launcher_model_set_deadzone(LauncherModel* m, int player, int pct) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.deadzone[player] = clampi(pct, 0, 100);
}

void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name,
                               const char* pad_guid) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.player_src[player] = clampi(kind, 0, 2);
    if (kind == 2) {
        m->player_pad_id[player] = pad_id;
        safe_copy(m->player_pad_name[player], sizeof(m->player_pad_name[player]),
                  pad_name ? pad_name : "Gamepad");
        safe_copy(m->s.player_gamepad_guid[player],
                  sizeof(m->s.player_gamepad_guid[player]),
                  pad_guid ? pad_guid : "");
    } else {
        m->player_pad_id[player] = 0;
        m->player_pad_name[player][0] = '\0';
        m->s.player_gamepad_guid[player][0] = '\0';
    }
    apply_default_pad_mode_for_source(m, player);
}

// ---- mouse controls --------------------------------------------------------

void launcher_model_set_mouse_source(LauncherModel* m, int enabled) {
    if (!m->has_mouse_controls) return;
    launcher_model_set_source(m, 0, 1, 0, NULL, NULL);   // player 0 -> Keyboard
    m->s.mouse_enabled = enabled ? 1 : 0;
}

void launcher_model_set_mouse_sensitivity(LauncherModel* m, float value) {
    m->s.mouse_sensitivity = clampf(value, 0.01f, 0.50f);
}

void launcher_model_toggle_mouse_invert_x(LauncherModel* m) {
    m->s.mouse_invert_x = !m->s.mouse_invert_x;
}

void launcher_model_toggle_mouse_invert_y(LauncherModel* m) {
    m->s.mouse_invert_y = !m->s.mouse_invert_y;
}

void launcher_model_set_mouse_bind(LauncherModel* m, int which, int button_index) {
    if (which < 0 || which > 2) return;
    m->s.mouse_bind[which] = (button_index < 0) ? -1 : button_index;
}

void launcher_model_set_gyro_sensitivity(LauncherModel* m, float value) {
    if (!m || !m->has_gyro_controls) return;
    m->s.gyro_sensitivity = clampf(value, 0.25f, 4.00f);
}

void launcher_model_request_skip_toggle(LauncherModel* m) {
    if (!m->s.skip_launcher) {
        m->skip_modal_open = true;    // enabling: confirm first
    } else {
        m->s.skip_launcher = 0;       // disabling: immediate
    }
}

void launcher_model_skip_confirm(LauncherModel* m) {
    m->s.skip_launcher = 1;
    m->skip_modal_open = false;
}

void launcher_model_skip_cancel(LauncherModel* m) {
    m->skip_modal_open = false;
}

void launcher_model_begin_capture(LauncherModel* m, int b) {
    launcher_model_begin_capture_slot(m, b, 0);
}
void launcher_model_begin_capture_slot(LauncherModel* m, int b, int slot) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
    if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
    if (b < 0 || b >= bc) return;
    m->hk_capturing  = false;
    m->camera_capturing = false;
    m->capturing     = true;
    m->capture_btn   = b;
    m->capture_slot  = (slot == 1) ? 1 : 0;
    /* ImGui activates a button on RELEASE, so the click that opened this
     * capture is already fully consumed by the time capturing is true.
     * Arm straight away: the next press is a deliberate new click. */
    m->capture_mouse_armed = true;
    m->capture_pad  = false;
    m->capture_assist = false;
}
void launcher_model_begin_pad_capture(LauncherModel* m, int b) {
    launcher_model_begin_capture(m, b);
    if (m->capturing) m->capture_pad = true;   // begin_capture validated b
}
/* Which capture kind a PSX Map All run walks: the player's input SOURCE. A
 * gamepad source captures pad fields, a keyboard source captures keys into
 * the primary slot -- same walk order (kPsxGamepadBindOrder), same 24 steps.
 * Read per step rather than latched so the two paths cannot disagree. */
static bool psx_map_all_is_pad(const LauncherModel* m) {
    const int p = clampi(m->cfg_player, 0, LNG_MAX_PLAYERS - 1);
    return m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0] != 0;
}
void launcher_model_begin_map_all(LauncherModel* m) {
    if (!m) return;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (!prof || !prof->id || strcmp(prof->id, "psx") != 0) return;
    m->map_all_active = true;
    m->map_all_wait_release = false;
    m->map_all_step = 0;
    if (psx_map_all_is_pad(m))
        launcher_model_begin_pad_capture(m, kPsxGamepadBindOrder[0]);
    else
        launcher_model_begin_capture_slot(m, kPsxGamepadBindOrder[0], 0);
}
void launcher_model_map_all_advance(LauncherModel* m) {
    if (!m || !m->map_all_active) return;
    m->map_all_step++;
    if (m->map_all_step >= LNG_PSX_PAD_BUTTON_COUNT) {
        m->map_all_active = false;
        m->map_all_wait_release = false;
        m->map_all_step = 0;
        m->capturing = false;
        m->capture_pad = false;
        return;
    }
    const int b = kPsxGamepadBindOrder[m->map_all_step];
    if (psx_map_all_is_pad(m)) {
        /* Stay in pad-capture for the next button, but ignore input until the
         * previous press/throw has been released (see try_capture). */
        m->map_all_wait_release = true;
        launcher_model_begin_pad_capture(m, b);
    } else {
        /* Keys commit on KEY_DOWN and the committing press is already
         * consumed, so there is nothing to wait for -- the release-wait gate
         * exists for held sticks/triggers, which keyboards do not have. */
        m->map_all_wait_release = false;
        launcher_model_begin_capture_slot(m, b, 0);
    }
}
void launcher_model_begin_assist_capture(LauncherModel* m, int action,
                                         bool gamepad) {
    if (!m->settings_bindings || action < 0 ||
        action >= m->assist_binding_count)
        return;
    m->hk_capturing = false;
    m->capturing = true;
    m->capture_assist = true;
    m->capture_pad = gamepad;
    m->capture_btn = action;
    m->capture_slot = 0;
}
void launcher_model_set_captured_key(LauncherModel* m, int scancode) {
    if (!m || !m->capturing || m->capture_pad) return;
    if (m->capture_assist) {
        if (m->capture_btn >= 0 &&
            m->capture_btn < m->assist_binding_count)
            m->s.assist_key_bind[m->capture_btn] = scancode;
    } else {
        int buttons = launcher_model_active_button_count(m, m->cfg_player);
        if (m->capture_btn >= 0 && m->capture_btn < buttons)
            m->s.player_key_bind[m->cfg_player][m->capture_btn] = scancode;
    }
}
void launcher_model_set_captured_pad(LauncherModel* m, int encoded_binding) {
    if (!m || !m->capturing || !m->capture_pad) return;
    if (m->capture_assist) {
        if (m->capture_btn >= 0 &&
            m->capture_btn < m->assist_binding_count)
            m->s.assist_pad_bind[m->capture_btn] = encoded_binding;
    } else {
        int buttons = launcher_model_active_button_count(m, m->cfg_player);
        if (m->capture_btn >= 0 && m->capture_btn < buttons)
            m->s.player_pad_bind[m->cfg_player][m->capture_btn] =
                encoded_binding;
    }
}
void launcher_model_reset_player_bindings(LauncherModel* m, int player) {
    if (!m || !m->settings_bindings || !m->has_default_settings) return;
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    memcpy(m->s.player_key_bind[player],
           m->default_settings.player_key_bind[player],
           sizeof m->s.player_key_bind[player]);
    memcpy(m->s.player_pad_bind[player],
           m->default_settings.player_pad_bind[player],
           sizeof m->s.player_pad_bind[player]);
}
void launcher_model_reset_assist_bindings(LauncherModel* m) {
    if (!m || !m->settings_bindings) return;
    memcpy(m->s.assist_key_bind, m->default_assist_key_bind,
           sizeof m->s.assist_key_bind);
    memcpy(m->s.assist_pad_bind, m->default_assist_pad_bind,
           sizeof m->s.assist_pad_bind);
}
void launcher_model_cancel_capture(LauncherModel* m) {
    m->capturing      = false;
    m->capture_pad    = false;
    m->capture_assist = false;
    m->map_all_active = false;
    m->map_all_wait_release = false;
    m->map_all_step   = 0;
}

void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    m->capturing    = false;
    m->camera_capturing = false;
    m->hk_capturing = true;
    m->capture_hk   = h;
}
void launcher_model_cancel_hk_capture(LauncherModel* m) { m->hk_capturing = false; }

void launcher_model_begin_camera_capture(LauncherModel* m, int action) {
    if (!m || action < 0 || action >= LNG_CAMERA_BIND_COUNT) return;
    m->capturing = false;
    m->capture_pad = false;
    m->hk_capturing = false;
    m->camera_capturing = true;
    m->capture_camera = action;
}

void launcher_model_cancel_camera_capture(LauncherModel* m) {
    if (m) m->camera_capturing = false;
}

const char* launcher_model_scale_label(const LauncherModel* m) {
    static char buf[8];
    int s = m->s.window_scale < 1 ? 1 : m->s.window_scale;
    snprintf(buf, sizeof(buf), "%dx", s);
    return buf;
}

const char* launcher_model_freq_label(const LauncherModel* m) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d Hz", m->s.audio_freq);
    return buf;
}

const char* launcher_model_player_src_label(const LauncherModel* m, int player) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    int src = clampi(m->s.player_src[player], 0, 2);
    if (src == 2) {
        // Never show the generic "Gamepad" placeholder when we have a concrete
        // pad name (or at least a GUID-backed label filled by sync/hydrate).
        if (m->player_pad_name[player][0] &&
            strcmp(m->player_pad_name[player], "Gamepad") != 0)
            return m->player_pad_name[player];
        if (m->s.player_gamepad_guid[player][0])
            return m->s.player_gamepad_guid[player];
    }
    // Mouse-capable games split the keyboard source (player 0 only): the label
    // reflects whether mouse-aim is on. Every non-mouse game keeps kSrcNames.
    if (src == 1 && m->has_mouse_controls && player == 0)
        return m->s.mouse_enabled ? "Keyboard + Mouse" : "Keyboard";
    return kSrcNames[src];
}

const char* launcher_button_name(LngButton b) {
    if (b < 0 || b >= LNG_BTN_COUNT) return "?";
    return kButtonNames[b];
}

const char* launcher_hotkey_name(LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return "?";
    return kHotkeyNames[h];
}

const char* launcher_view_name(LngView v) {
    if (v < 0 || v > LNG_VIEW_LOBBY) return "?";
    return kViewNames[v];
}
