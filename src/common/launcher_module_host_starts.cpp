#include "launcher_module_host_starts.h"
#include "launcher_module_contract.h"
#include "consoles/psx/psx_start_from.h"
#include <cstdio>
#include <cstring>

namespace {
struct Snapshot {
    LauncherStartEntry entries[LNG_START_MAX];
    LauncherStartNotes notes;
    int count;
};
Snapshot snapshot;

int32_t RECOMP_LAUNCHER_V2_CALL count(void* ctx, RecompLauncherV2StartNotes* out) {
    const Snapshot& s = *static_cast<const Snapshot*>(ctx);
    if (!out || launcher_v2_check_header(&out->header, sizeof(*out))) return -1;
    out->build_known = s.notes.build_known;
    out->states_elsewhere = s.notes.states_elsewhere;
    out->states_unlisted = s.notes.states_unlisted;
    out->replays_unlisted = s.notes.replays_unlisted;
    return s.count;
}

int32_t RECOMP_LAUNCHER_V2_CALL get(void* ctx, uint32_t index, RecompLauncherV2StartEntry* out) {
    const Snapshot& s = *static_cast<const Snapshot*>(ctx);
    if (!out || launcher_v2_check_header(&out->header, sizeof(*out)) || index >= static_cast<uint32_t>(s.count)) return 0;
    const LauncherStartEntry& e = s.entries[index];
    RecompLauncherV2StartEntry result = {};
    result.header = launcher_v2_header(sizeof(result));
    result.kind = static_cast<uint32_t>(e.kind);
    result.slot = e.slot; result.disc = e.disc;
    result.build = static_cast<uint32_t>(e.build);
    result.power_on = static_cast<uint32_t>(e.power_on);
    result.partial = static_cast<uint32_t>(e.partial);
    result.frames = e.frames; result.when = e.when;
    std::memcpy(result.label, e.label, sizeof(result.label));
    if (e.kind == LNG_START_REPLAY) std::memcpy(result.replay_path, e.path, sizeof(result.replay_path));
    if (e.thumb_path[0] && e.thumb_w > 0 && e.thumb_h > 0 && e.thumb_w <= 2048 && e.thumb_h <= 2048) {
        result.thumbnail_format = RECOMP_LAUNCHER_V2_THUMB_BGRA8;
        result.thumbnail_width = static_cast<uint32_t>(e.thumb_w);
        result.thumbnail_height = static_cast<uint32_t>(e.thumb_h);
    }
    *out = result;
    return 1;
}

uint32_t RECOMP_LAUNCHER_V2_CALL thumbnail(void* ctx, uint32_t index, uint8_t* pixels, uint32_t capacity) {
    const Snapshot& s = *static_cast<const Snapshot*>(ctx);
    if (index >= static_cast<uint32_t>(s.count)) return 0;
    const LauncherStartEntry& e = s.entries[index];
    if (!e.thumb_path[0] || e.thumb_w <= 0 || e.thumb_h <= 0 || e.thumb_w > 2048 || e.thumb_h > 2048) return 0;
    const uint32_t bytes = static_cast<uint32_t>(e.thumb_w) * static_cast<uint32_t>(e.thumb_h) * 4;
    if (!pixels || capacity < bytes) return bytes;
    FILE* file = std::fopen(e.thumb_path, "rb");
    if (!file) return 0;
    const bool complete = std::fseek(file, static_cast<long>(e.thumb_offset), SEEK_SET) == 0 &&
                          std::fread(pixels, 1, bytes, file) == bytes && !std::ferror(file);
    std::fclose(file);
    return complete ? bytes : 0;
}
}

extern "C" const RecompLauncherV2StartProvider* launcher_v2_host_starts(const char* product, const char* saves) {
    launcher_v2_host_starts_clear();
    snapshot.count = psx_start_scan(saves, product, snapshot.entries, LNG_START_MAX, &snapshot.notes);
    static RecompLauncherV2StartProvider provider = {};
    provider.header = launcher_v2_header(sizeof(provider));
    provider.ctx = &snapshot;
    provider.count = count; provider.get = get; provider.thumbnail = thumbnail;
    return &provider;
}

extern "C" void launcher_v2_host_starts_clear(void) { std::memset(&snapshot, 0, sizeof(snapshot)); }

