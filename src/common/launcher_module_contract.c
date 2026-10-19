#include "launcher_module_contract.h"
#include <stdio.h>
#include <string.h>

RecompLauncherV1Header launcher_v1_header(uint32_t bytes) {
    RecompLauncherV1Header h = {bytes, RECOMP_LAUNCHER_V1_MAJOR, RECOMP_LAUNCHER_V1_MINOR, 0};
    return h;
}

uint32_t launcher_v1_architecture(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return RECOMP_LAUNCHER_V1_ARM64;
#elif defined(__x86_64__) || defined(_M_X64)
    return RECOMP_LAUNCHER_V1_X64;
#elif defined(__i386__) || defined(_M_IX86)
    return RECOMP_LAUNCHER_V1_X86;
#else
    return 0;
#endif
}

uint32_t launcher_v1_byte_order(void) {
    const uint32_t value = 1;
    return *(const uint8_t*)&value ? RECOMP_LAUNCHER_V1_LITTLE_ENDIAN : RECOMP_LAUNCHER_V1_BIG_ENDIAN;
}

uint32_t launcher_v1_check_header(const RecompLauncherV1Header* h, uint32_t bytes) {
    if (!h || h->size != bytes || h->reserved) return RECOMP_LAUNCHER_V1_BAD_RECORD;
    if (h->major != RECOMP_LAUNCHER_V1_MAJOR || h->minor != RECOMP_LAUNCHER_V1_MINOR)
        return RECOMP_LAUNCHER_V1_BAD_VERSION;
    return RECOMP_LAUNCHER_V1_OK;
}

void launcher_v1_diagnostic(char* out, uint32_t cap, const char* message) {
    if (out && cap) snprintf(out, cap, "%s", message);
}

int launcher_v1_absolute_root(const char* root) {
    if (!root || !root[0]) return 0;
#if defined(_WIN32)
    return (root[1] == ':' && (root[2] == '/' || root[2] == '\\')) ||
        ((root[0] == '\\' || root[0] == '/') && root[1] == root[0] && root[2]);
#else
    return root[0] == '/';
#endif
}

/* FNV-1a is a deterministic layout identifier, not an artifact checksum.
 * SHA-256 manifests separately bind source, module, SDL and assets. Values
 * are encoded explicitly little-endian, independent of compiler byte order. */
static uint64_t hash_bytes(uint64_t hash, const char* text) {
    do { hash = (hash ^ (uint8_t)*text) * UINT64_C(1099511628211); } while (*text++);
    return hash;
}
static uint64_t hash_number(uint64_t hash, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        hash = (hash ^ (uint8_t)(value >> (8 * i))) * UINT64_C(1099511628211);
    }
    return hash;
}

RecompLauncherV1Layout launcher_v1_layout(void) {
    RecompLauncherV1Layout result = {0};
    uint64_t hash = UINT64_C(14695981039346656037);
#define RL_WITNESS_STRUCT(type) \
    hash = hash_bytes(hash, #type); \
    hash = hash_number(hash, sizeof(type)); \
    hash = hash_number(hash, _Alignof(type));
#define RL_WITNESS_FIELD(type, member, signature) \
    hash = hash_bytes(hash, #type "." #member); \
    hash = hash_bytes(hash, signature); \
    hash = hash_number(hash, offsetof(type, member)); \
    hash = hash_number(hash, sizeof(((type*)0)->member));
#include "launcher_module_legacy_witness.inc"
    result.records = hash;
    /* Group identifiers include their nested records, not just pointer sizes. */
    RL_WITNESS_STRUCT(RecompLauncherCSettings)
    result.settings = hash;
    RL_WITNESS_STRUCT(RecompLauncherCGameInfo)
    RL_WITNESS_STRUCT(RecompLauncherCModProvider)
    RL_WITNESS_STRUCT(RecompLauncherCNetplayCallbacks)
    result.callbacks = hash;
    hash = UINT64_C(14695981039346656037);
#define RL_WITNESS_SIGNATURE(signature) hash = hash_bytes(hash, signature);
#include "launcher_module_boundary_witness.inc"
    result.boundary = hash;
#undef RL_WITNESS_SIGNATURE
#undef RL_WITNESS_FIELD
#undef RL_WITNESS_STRUCT
    result.settings_bytes = (uint32_t)sizeof(RecompLauncherCSettings);
    result.game_bytes = (uint32_t)sizeof(RecompLauncherCGameInfo);
    result.mods_bytes = (uint32_t)sizeof(RecompLauncherCModProvider);
    result.netplay_bytes = (uint32_t)sizeof(RecompLauncherCNetplayCallbacks);
    return result;
}

static uint32_t refuse(uint32_t status, char* out, uint32_t cap, const char* message) {
    launcher_v1_diagnostic(out, cap, message);
    return status;
}

uint32_t launcher_v1_validate_query(const RecompLauncherV1Query* q,
                                  const RecompLauncherV1SDL* local_sdl,
                                  uint64_t supported, char* out, uint32_t cap) {
    uint32_t status = launcher_v1_check_header(q ? &q->header : NULL, sizeof(*q));
    if (status) {
        if (out && cap) snprintf(out, cap, "Launcher interface mismatch: found major %u, minor %u, %u bytes; need major 1, minor 0, %u bytes. Restore a matching v1 module.",
            q ? q->header.major : 0, q ? q->header.minor : 0, q ? q->header.size : 0, (unsigned)sizeof(*q));
        return status;
    }
    if (!launcher_v1_architecture() || q->architecture != launcher_v1_architecture() ||
        q->pointer_bits != sizeof(void*) * 8 || q->byte_order != launcher_v1_byte_order() ||
        q->calling_convention != RECOMP_LAUNCHER_V1_CDECL)
        return refuse(RECOMP_LAUNCHER_V1_BAD_ARCHITECTURE, out, cap, "Launcher architecture or calling convention differs from this host; restore its matching module.");
    const RecompLauncherV1Layout local = launcher_v1_layout();
    if (q->layout.settings != local.settings || q->layout.callbacks != local.callbacks ||
        q->layout.records != local.records || q->layout.boundary != local.boundary ||
        q->layout.settings_bytes != local.settings_bytes || q->layout.game_bytes != local.game_bytes ||
        q->layout.mods_bytes != local.mods_bytes || q->layout.netplay_bytes != local.netplay_bytes)
        return refuse(RECOMP_LAUNCHER_V1_BAD_LAYOUT, out, cap, "Launcher settings/callback layout differs from the host; restore its matching v1 module.");
    if ((q->required & ~q->offered) || (q->required & ~supported))
        return refuse(RECOMP_LAUNCHER_V1_MISSING_CAPABILITY, out, cap, "Launcher lacks a capability required by the host; restore a matching module.");
    if (launcher_v1_check_header(&q->sdl.header, sizeof(q->sdl)) ||
        launcher_v1_check_header(local_sdl ? &local_sdl->header : NULL, sizeof(*local_sdl)) ||
        q->sdl.reserved || local_sdl->reserved || !local_sdl->version ||
        !local_sdl->get_version_address || !local_sdl->get_joystick_address ||
        q->sdl.version != local_sdl->version ||
        q->sdl.get_version_address != local_sdl->get_version_address ||
        q->sdl.get_joystick_address != local_sdl->get_joystick_address ||
        memcmp(q->sdl.sha256, local_sdl->sha256, sizeof(local_sdl->sha256)) ||
        !memchr(q->sdl.provider_path, 0, sizeof(q->sdl.provider_path)) ||
        !memchr(local_sdl->provider_path, 0, sizeof(local_sdl->provider_path)) ||
        !local_sdl->provider_path[0] || strcmp(q->sdl.provider_path, local_sdl->provider_path))
        return refuse(RECOMP_LAUNCHER_V1_BAD_SDL, out, cap, "Launcher and host do not use the same shared SDL3 provider; restore the matching module and SDL library.");
    return RECOMP_LAUNCHER_V1_OK;
}

uint32_t launcher_v1_validate_run(const RecompLauncherV1Run* run,
                                const RecompLauncherV1Result* result,
                                const RecompLauncherV1SDL* local_sdl,
                                uint64_t supported, char* out, uint32_t cap) {
    uint32_t status = launcher_v1_check_header(run ? &run->header : NULL, sizeof(*run));
    if (status) return refuse(status, out, cap, "Launcher run record has the wrong size/version; no settings or files were changed.");
    status = launcher_v1_validate_query(run->contract, local_sdl, supported, out, cap);
    if (status) return status;
    if (launcher_v1_check_header(result ? &result->header : NULL, sizeof(*result)) ||
        launcher_v1_check_header(run->settings ? &run->settings->header : NULL, sizeof(*run->settings)) ||
        launcher_v1_check_header(run->game ? &run->game->header : NULL, sizeof(*run->game)) ||
        (run->mods && launcher_v1_check_header(&run->mods->header, sizeof(*run->mods))) ||
        (run->netplay && launcher_v1_check_header(&run->netplay->header, sizeof(*run->netplay))) ||
        (run->starts && launcher_v1_check_header(&run->starts->header, sizeof(*run->starts))))
        return refuse(RECOMP_LAUNCHER_V1_BAD_RECORD, out, cap, "Launcher settings, game, provider or result record has the wrong size/version; no callbacks ran.");
    if (run->game->value.mods || run->game->value.netplay)
        return refuse(RECOMP_LAUNCHER_V1_BAD_RECORD, out, cap, "Launcher requires sized mod/netplay providers, not raw GameInfo provider pointers.");
    if (!launcher_v1_absolute_root(run->product_root) || !launcher_v1_absolute_root(run->save_root) ||
        !launcher_v1_absolute_root(run->assets_root) || !result->selected_disc || result->selected_disc_capacity < 512 ||
        !result->relaunch_path || result->relaunch_path_capacity < 512 || !result->replay_path || result->replay_path_capacity < 512 ||
        result->reserved || (run->starts && (!run->starts->count || !run->starts->get)))
        return refuse(RECOMP_LAUNCHER_V1_BAD_RECORD, out, cap, "Launcher needs explicit absolute roots, caller-owned result buffers and complete start-list callbacks.");
    uint64_t used = RECOMP_LAUNCHER_V1_SETTINGS | RECOMP_LAUNCHER_V1_INPUT | RECOMP_LAUNCHER_V1_DISC_BIOS_CARD;
    if (run->mods) used |= RECOMP_LAUNCHER_V1_MODS;
    if (run->netplay || run->game->value.netplay_supported) used |= RECOMP_LAUNCHER_V1_NETPLAY;
    if (run->game->value.setup_wizard_supported) used |= RECOMP_LAUNCHER_V1_SETUP_PROGRESS;
    if (run->starts) used |= RECOMP_LAUNCHER_V1_START_CHOICE;
    if (run->game->value.in_session) used |= RECOMP_LAUNCHER_V1_IN_SESSION;
    if ((used & ~supported) || (used & ~run->contract->offered))
        return refuse(RECOMP_LAUNCHER_V1_MISSING_CAPABILITY, out, cap, "Launcher run uses a capability that the host/module did not negotiate.");
    if (run->game->value.netplay_supported && !run->netplay)
        return refuse(RECOMP_LAUNCHER_V1_BAD_RECORD, out, cap, "Netplay was enabled without its sized provider; no callbacks ran.");
    return RECOMP_LAUNCHER_V1_OK;
}
