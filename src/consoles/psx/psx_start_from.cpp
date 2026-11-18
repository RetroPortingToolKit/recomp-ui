// psx_start_from.cpp - see psx_start_from.h.

#include "psx_start_from.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;

uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Size and modified time of a file with content; false for anything else.
bool file_info(const std::string& path, int64_t* size, int64_t* when) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) return false;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
#endif
    if (st.st_size <= 0) return false;
    if (size) *size = (int64_t)st.st_size;
    if (when) *when = (int64_t)st.st_mtime;
    return true;
}

// The files of one folder, by name. An absent folder has none.
std::vector<std::string> files_of(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code is_ec;
        if (it->is_regular_file(is_ec) && !is_ec) names.push_back(it->path().filename().string());
    }
    return names;
}

// "state_<entry pc>[_disc<N>]_slot<NN>.pst", as savestate.c names a slot's file.
bool parse_state_name(const std::string& name, uint32_t* entry, int* disc, int* slot) {
    unsigned pc = 0;
    int d = 0, s = -1, used = 0;
    if (std::sscanf(name.c_str(), "state_%8X_disc%d_slot%2d.pst%n", &pc, &d, &s, &used) == 3 &&
        used == (int)name.size() && d >= 1) {
        *entry = pc; *disc = d; *slot = s;
        return true;
    }
    d = 0; s = -1; used = 0;
    if (std::sscanf(name.c_str(), "state_%8X_slot%2d.pst%n", &pc, &s, &used) == 2 &&
        used == (int)name.size()) {
        *entry = pc; *disc = 0; *slot = s;
        return true;
    }
    return false;
}

struct StateFile {
    std::string bios;      // the folder it is in
    uint32_t    entry = 0; // the program it belongs to
    int         disc = 0, slot = -1;
    int64_t     when = 0;
    std::string path;
    uint32_t    codegen = 0;
};

const uint32_t kStateMagic = 0x50535842u;   // "PSXB", boot_state.h
const size_t   kStateHeader = 36;           // nine 32-bit fields

// The codegen hash in a save state's header; false when it is no save state.
bool read_state_codegen(const std::string& path, uint32_t* codegen) {
    unsigned char head[kStateHeader];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const size_t got = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (got != sizeof(head) || le32(head) != kStateMagic) return false;
    *codegen = le32(head + 16);
    return true;
}

// A save state's picture: "PSTH", width, height, then the pixels.
void find_state_thumb(const std::string& state_path, LauncherStartEntry* e) {
    std::string path = state_path.substr(0, state_path.size() - 4) + ".thumb";
    unsigned char head[12];
    int64_t size = 0;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    const size_t got = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (got != sizeof(head) || std::memcmp(head, "PSTH", 4) != 0) return;
    const uint32_t w = le32(head + 4), h = le32(head + 8);
    if (w < 1 || h < 1 || w > 512 || h > 512) return;
    if (!file_info(path, &size, nullptr) || size < (int64_t)(12 + (uint64_t)w * h * 4)) return;
    std::snprintf(e->thumb_path, sizeof(e->thumb_path), "%s", path.c_str());
    e->thumb_offset = 12;
    e->thumb_w = (int)w;
    e->thumb_h = (int)h;
}

// "<serial>-boot-<YYYYMMDD>-<HHMMSS>Z[-<n>].psxrpl", as the recorder names a
// replay from power-on: the serial with every byte but a letter or a digit
// made '_', the UTC time, and n 1..99 when that name was taken.
bool parse_replay_name(const std::string& name) {
    static const char kExt[] = ".psxrpl", kBoot[] = "-boot-";
    const size_t ext = sizeof(kExt) - 1;
    if (name.size() <= ext || name.compare(name.size() - ext, ext, kExt) != 0) return false;
    const std::string stem = name.substr(0, name.size() - ext);
    const size_t at = stem.find(kBoot);
    if (at == 0 || at == std::string::npos) return false;
    for (size_t i = 0; i < at; ++i) {
        const char c = stem[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'))
            return false;
    }
    const std::string t = stem.substr(at + sizeof(kBoot) - 1);   // YYYYMMDD-HHMMSSZ[-n]
    if (t.size() < 16 || t[8] != '-' || t[15] != 'Z') return false;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && (t[i] < '0' || t[i] > '9')) return false;
    if (t.size() == 16) return true;
    const std::string n = t.substr(16);
    if (n.size() < 2 || n.size() > 3 || n[0] != '-' || n[1] == '0') return false;
    for (size_t i = 1; i < n.size(); ++i)
        if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

struct ReplayInfo {
    uint32_t    frames = 0;
    bool        power_on = false;
    bool        has_codegen = false;
    uint32_t    codegen = 0;
    std::string name;
    uint32_t    thumb_offset = 0;
    int         thumb_w = 0, thumb_h = 0;
};

// The PSXRTI3 tags of runtime/include/replay_file.h this lister knows.
const uint32_t kTagSkippable = 0x80000000u;
const uint32_t kTagAnchor = 0x00000301u, kTagSettings = 0x00000302u, kTagDigests = 0x00000303u;
const uint32_t kTagPowerOn = 0x00000306u, kTagCards = 0x00000307u;
const uint32_t kTagThumb = 0x80000304u, kTagName = 0x80000305u, kTagProduct = 0x80000308u;

// An entry the host's reader refuses: a tag that changes playback and that it
// does not know, or an unknown identity or marker tag (0x800001xx).
bool refused_tag(uint32_t tag) {
    if (!(tag & kTagSkippable))
        return tag != kTagAnchor && tag != kTagSettings && tag != kTagDigests &&
               tag != kTagPowerOn && tag != kTagCards;
    if ((tag & 0xFFFFFF00u) == 0x80000100u)
        return !((tag >= 0x80000101u && tag <= 0x80000105u) || tag == 0x80000110u || tag == 0x80000111u);
    return false;
}

// A replay of the kind the host plays, and what the list shows of it: the
// frame count, where it starts, its name, its picture and the codegen hash of
// the build that recorded it. The large entries (the anchor state, the memory
// cards) are stepped over, and the records are not read.
bool read_replay(const std::string& path, ReplayInfo* r) {
    static const unsigned char kMagic[8] = { 'P', 'S', 'X', 'R', 'T', 'I', '3', 0 };
    unsigned char head[28];
    int64_t size = 0;
    if (!file_info(path, &size, nullptr)) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head) &&
              std::memcmp(head, kMagic, sizeof(kMagic)) == 0 &&
              le32(head + 8) == 3 && le32(head + 12) == 12 && le32(head + 20) == 0;
    const uint32_t frames = ok ? le32(head + 16) : 0;
    const uint32_t ext = ok ? le32(head + 24) : 0;
    ok = ok && frames >= 1 && frames <= 1000000u && (ext & 3u) == 0 && ext <= (16u << 20) &&
         size == (int64_t)28 + ext + (int64_t)frames * 12;
    uint32_t pos = 28, starts = 0;
    const uint32_t end = 28 + ext;
    while (ok && pos < end) {
        unsigned char th[8];
        if (end - pos < 8 || std::fseek(f, (long)pos, SEEK_SET) != 0 || std::fread(th, 1, 8, f) != 8) {
            ok = false;
            break;
        }
        const uint32_t tag = le32(th), len = le32(th + 4);
        if (len > end - pos - 8 || ((len + 3u) & ~3u) > end - pos - 8 || refused_tag(tag)) {
            ok = false;
            break;
        }
        if (tag == kTagAnchor || tag == kTagPowerOn) {
            ++starts;
            r->power_on = tag == kTagPowerOn;
        } else if (tag == kTagName && len >= 1 && len <= 96) {
            char text[97];
            if (std::fread(text, 1, len, f) == len) {
                text[len] = '\0';
                bool plain = std::strlen(text) == len;
                for (uint32_t i = 0; plain && i < len; ++i)
                    plain = (unsigned char)text[i] >= 0x20 && text[i] != 0x7F;
                if (plain) r->name = text;
            }
        } else if (tag == kTagProduct) {
            char text[1025];
            const size_t want = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
            const size_t got = std::fread(text, 1, want, f);
            text[got] = '\0';
            for (const char* line = std::strstr(text, "codegen="); line; line = std::strstr(line + 1, "codegen="))
                if (line == text || line[-1] == '\n') {
                    r->codegen = (uint32_t)std::strtoul(line + 8, nullptr, 16);
                    r->has_codegen = true;
                    break;
                }
        } else if (tag == kTagThumb && len >= 4) {
            unsigned char wh[4];
            if (std::fread(wh, 1, 4, f) == 4) {
                const uint32_t w = (uint32_t)wh[0] | ((uint32_t)wh[1] << 8);
                const uint32_t h = (uint32_t)wh[2] | ((uint32_t)wh[3] << 8);
                if (w >= 1 && h >= 1 && w <= 256 && h <= 256 && len == 4 + w * h * 4) {
                    r->thumb_offset = pos + 12;
                    r->thumb_w = (int)w;
                    r->thumb_h = (int)h;
                }
            }
        }
        pos += 8 + ((len + 3u) & ~3u);
    }
    std::fclose(f);
    ok = ok && starts == 1;   // exactly one of an anchor state and power-on
    if (ok) r->frames = frames;
    return ok;
}

int build_mark(bool known, uint32_t mine, bool has, uint32_t theirs) {
    if (!known || !has) return LNG_START_BUILD_UNKNOWN;
    return mine == theirs ? LNG_START_BUILD_SAME : LNG_START_BUILD_OTHER;
}

bool newer(const LauncherStartEntry& a, const LauncherStartEntry& b) {
    if (a.when != b.when) return a.when > b.when;
    return std::strcmp(a.path, b.path) < 0;   // a fixed order for equal times
}

bool g_set_by_us[PSX_START_VARS];   // psx_start_from_apply set the variable in this process

void put_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   // an empty value removes it
#else
    if (value) setenv(name, value, 1);
    else       unsetenv(name);
#endif
}

// The hash in one overlay_codegen_hash.h; false when it is absent or holds none.
bool codegen_hash_in(const fs::path& path, uint32_t* hash) {
    char text[2048];
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    const size_t got = std::fread(text, 1, sizeof(text) - 1, f);
    std::fclose(f);
    text[got] = '\0';
    static const char kName[] = "PSX_OVERLAY_CODEGEN_HASH";
    for (const char* at = std::strstr(text, kName); at; at = std::strstr(at + 1, kName)) {
        const char* p = at + sizeof(kName) - 1;
        if (*p != ' ' && *p != '\t') continue;   // the name inside a longer word
        char* stop = nullptr;
        const unsigned long v = std::strtoul(p, &stop, 16);
        if (stop != p && stop - p >= 3) {
            *hash = (uint32_t)v;
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" int psx_start_codegen_hash(const char* product_dir, uint32_t* hash) {
    if (!product_dir || !hash) return 0;
    // A packaged product carries the header in its overlay toolchain.
    const fs::path dir(product_dir);
    if (codegen_hash_in(dir / "overlay_toolchain" / "include" / "overlay_codegen_hash.h", hash)) return 1;
    return codegen_hash_in(dir / "overlay_codegen_hash.h", hash) ? 1 : 0;
}

extern "C" int psx_start_scan(const char* save_root, const char* product_dir,
                              LauncherStartEntry* out, int cap, LauncherStartNotes* notes) {
    LauncherStartNotes n = {};
    uint32_t mine = 0;
    const bool known = psx_start_codegen_hash(product_dir, &mine) != 0;
    n.build_known = known ? 1 : 0;
    if (notes) *notes = n;
    if (!save_root || !save_root[0] || !out || cap <= 0) return 0;

    static const char* const kBios[] = { "scph1001", "openbios" };
    std::vector<StateFile> states;
    for (const char* bios : kBios) {
        const std::string dir = (fs::path(save_root) / bios).string();
        for (const std::string& name : files_of(dir)) {
            StateFile sf;
            sf.bios = bios;
            sf.path = (fs::path(dir) / name).string();
            if (!parse_state_name(name, &sf.entry, &sf.disc, &sf.slot)) continue;
            if (sf.slot < 0 || sf.slot > 11) continue;
            if (!file_info(sf.path, nullptr, &sf.when)) continue;
            if (!read_state_codegen(sf.path, &sf.codegen)) continue;
            states.push_back(sf);
        }
    }

    // The BIOS folder and the program in use: those of the newest save state.
    const StateFile* newest = nullptr;
    for (const StateFile& sf : states)
        if (!newest || sf.when > newest->when) newest = &sf;

    std::vector<LauncherStartEntry> list;
    for (const StateFile& sf : states) {
        if (sf.bios != newest->bios || sf.entry != newest->entry) { ++n.states_elsewhere; continue; }
        LauncherStartEntry e = {};
        e.kind = LNG_START_STATE;
        e.slot = sf.slot;
        e.disc = sf.disc;
        e.when = sf.when;
        e.build = build_mark(known, mine, true, sf.codegen);
        std::snprintf(e.label, sizeof(e.label), "Slot %d", sf.slot + 1);   // as the save-state menu counts
        std::snprintf(e.path, sizeof(e.path), "%s", sf.path.c_str());
        find_state_thumb(sf.path, &e);
        list.push_back(e);
    }
    std::sort(list.begin(), list.end(), newer);
    if ((int)list.size() > PSX_START_MAX_STATES) {
        n.states_unlisted = (int)list.size() - PSX_START_MAX_STATES;
        list.resize(PSX_START_MAX_STATES);
    }

    // Replays from power-on, as the recorder names them, handed over by
    // their absolute path.
    std::vector<LauncherStartEntry> replays;
    std::error_code abs_ec;
    fs::path replay_dir = fs::absolute(fs::path(save_root) / "replays", abs_ec);
    if (abs_ec) replay_dir = fs::path(save_root) / "replays";
    for (const std::string& name : files_of(replay_dir.string())) {
        if (!parse_replay_name(name)) continue;
        const std::string path = (replay_dir / name).string();
        ReplayInfo info;
        LauncherStartEntry e = {};
        if (path.size() >= sizeof(e.path) || !file_info(path, nullptr, &e.when) || !read_replay(path, &info))
            continue;
        e.kind = LNG_START_REPLAY;
        e.slot = -1;
        e.frames = info.frames;
        e.power_on = info.power_on ? 1 : 0;
        e.build = build_mark(known, mine, info.has_codegen, info.codegen);
        std::snprintf(e.label, sizeof(e.label), "%s",
                      !info.name.empty() ? info.name.c_str()
                                         : info.power_on ? "Replay from power-on" : "Replay");
        std::snprintf(e.path, sizeof(e.path), "%s", path.c_str());
        if (info.thumb_offset) {
            std::snprintf(e.thumb_path, sizeof(e.thumb_path), "%s", path.c_str());
            e.thumb_offset = info.thumb_offset;
            e.thumb_w = info.thumb_w;
            e.thumb_h = info.thumb_h;
        }
        replays.push_back(e);
    }
    std::sort(replays.begin(), replays.end(), newer);
    if ((int)replays.size() > PSX_START_MAX_REPLAYS) {
        n.replays_unlisted = (int)replays.size() - PSX_START_MAX_REPLAYS;
        replays.resize(PSX_START_MAX_REPLAYS);
    }
    list.insert(list.end(), replays.begin(), replays.end());

    if (notes) *notes = n;
    const int count = (int)list.size() < cap ? (int)list.size() : cap;
    for (int i = 0; i < count; ++i) out[i] = list[(size_t)i];
    return count;
}

extern "C" void psx_start_from_load(LauncherModel* m, const char* product_dir, const char* save_root) {
    if (!launcher_model_start_from_available(m)) return;
    const std::string product = product_dir ? product_dir : "";
    const std::string root = save_root ? std::string(save_root)
                                       : (fs::path(product) / "saves").string();
    static LauncherStartEntry entries[LNG_START_MAX];
    LauncherStartNotes notes = {};
    const int count = psx_start_scan(root.c_str(), product.c_str(), entries, LNG_START_MAX, &notes);
    launcher_model_set_start_entries(m, entries, count, &notes);
}

extern "C" void psx_start_from_vars(const LauncherModel* m, int netplay_launch,
                                    PsxStartVar out[PSX_START_VARS]) {
    out[0].name = "PSX_LOAD_SLOT";
    out[0].value[0] = '\0';
    out[1].name = "PSX_REPLAY_FILE";
    out[1].value[0] = '\0';
    if (netplay_launch) return;
    int slot = -1;
    const char* path = nullptr;
    switch (launcher_model_start_handover(m, &slot, &path)) {
        case LNG_START_STATE:
            std::snprintf(out[0].value, sizeof(out[0].value), "%d", slot);
            break;
        case LNG_START_REPLAY:
            if (std::strlen(path) < sizeof(out[1].value))
                std::snprintf(out[1].value, sizeof(out[1].value), "%s", path);
            break;
        default:
            break;
    }
}

extern "C" void psx_start_from_apply(const LauncherModel* m, int netplay_launch) {
    PsxStartVar vars[PSX_START_VARS];
    psx_start_from_vars(m, netplay_launch, vars);
    for (int i = 0; i < PSX_START_VARS; ++i) {
        if (vars[i].value[0]) {
            put_env(vars[i].name, vars[i].value);
            g_set_by_us[i] = true;
        } else if (g_set_by_us[i]) {
            put_env(vars[i].name, nullptr);
            g_set_by_us[i] = false;
        }
    }
}
