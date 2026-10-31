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

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// "<prefix>_<entry pc>[_disc<N>]_slot<NN><ext>", as savestate.c and
// replay_session.c name a slot's file.
bool parse_slot_name(const std::string& name, const char* prefix, const char* ext,
                     uint32_t* entry, int* disc, int* slot) {
    char with_disc[64], plain[64];
    std::snprintf(with_disc, sizeof(with_disc), "%s_%%8X_disc%%d_slot%%2d%s%%n", prefix, ext);
    std::snprintf(plain, sizeof(plain), "%s_%%8X_slot%%2d%s%%n", prefix, ext);
    unsigned pc = 0;
    int d = 0, s = -1, used = 0;
    if (std::sscanf(name.c_str(), with_disc, &pc, &d, &s, &used) == 3 &&
        used == (int)name.size() && d >= 1) {
        *entry = pc; *disc = d; *slot = s;
        return true;
    }
    d = 0; s = -1; used = 0;
    if (std::sscanf(name.c_str(), plain, &pc, &s, &used) == 2 && used == (int)name.size()) {
        *entry = pc; *disc = 0; *slot = s;
        return true;
    }
    return false;
}

struct SlotFile {
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

struct ReplayInfo {
    uint32_t    frames = 0;
    bool        power_on = false;
    bool        has_codegen = false;
    uint32_t    codegen = 0;
    std::string name;
    uint32_t    thumb_offset = 0;
    int         thumb_w = 0, thumb_h = 0;
};

// The header and the entries of a PSXRTI3 replay that the list shows. The
// large entries (the anchor state, the memory cards) are stepped over.
bool read_replay(const std::string& path, ReplayInfo* r) {
    static const unsigned char kMagic[8] = { 'P', 'S', 'X', 'R', 'T', 'I', '3', 0 };
    unsigned char head[28];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head) &&
              std::memcmp(head, kMagic, sizeof(kMagic)) == 0 && le32(head + 8) == 3;
    const uint32_t frames = ok ? le32(head + 16) : 0;
    const uint32_t ext = ok ? le32(head + 24) : 0;
    ok = ok && frames >= 1 && frames <= 1000000u && (ext & 3u) == 0 && ext <= (16u << 20);
    uint32_t pos = 28;
    const uint32_t end = 28 + ext;
    while (ok && pos + 8 <= end) {
        unsigned char th[8];
        if (std::fseek(f, (long)pos, SEEK_SET) != 0 || std::fread(th, 1, 8, f) != 8) { ok = false; break; }
        const uint32_t tag = le32(th), len = le32(th + 4);
        const uint32_t padded = (len + 3u) & ~3u;
        if (len > end || padded > end - pos - 8) { ok = false; break; }
        if (tag == 0x80000305u || tag == 0x80000308u) {      // name; product lines
            char text[1024];
            const size_t want = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
            const size_t got = std::fread(text, 1, want, f);
            text[got] = '\0';
            if (tag == 0x80000305u) {
                r->name = text;
            } else if (const char* line = std::strstr(text, "codegen=")) {
                if (line == text || line[-1] == '\n') {
                    r->codegen = (uint32_t)std::strtoul(line + 8, nullptr, 16);
                    r->has_codegen = true;
                }
            }
        } else if (tag == 0x00000306u) {                     // starts at power-on
            r->power_on = true;
        } else if (tag == 0x80000304u && len >= 4) {         // picture
            unsigned char wh[4];
            if (std::fread(wh, 1, 4, f) == 4) {
                const uint32_t w = (uint32_t)wh[0] | ((uint32_t)wh[1] << 8);
                const uint32_t h = (uint32_t)wh[2] | ((uint32_t)wh[3] << 8);
                if (w >= 1 && h >= 1 && w <= 512 && h <= 512 && len >= 4 + w * h * 4) {
                    r->thumb_offset = pos + 12;
                    r->thumb_w = (int)w;
                    r->thumb_h = (int)h;
                }
            }
        }
        pos += 8 + padded;
    }
    std::fclose(f);
    if (ok) r->frames = frames;
    return ok;
}

// Text for a list row: printable ASCII and UTF-8 bytes, nothing else.
void set_label(LauncherStartEntry* e, const std::string& text) {
    size_t n = 0;
    for (const char ch : text) {
        if (n + 1 >= sizeof(e->label)) break;
        const unsigned char u = (unsigned char)ch;
        e->label[n++] = u < 0x20 || u == 0x7F ? ' ' : ch;
    }
    e->label[n] = '\0';
}

int build_mark(bool known, uint32_t mine, bool has, uint32_t theirs) {
    if (!known || !has) return LNG_START_BUILD_UNKNOWN;
    return mine == theirs ? LNG_START_BUILD_SAME : LNG_START_BUILD_OTHER;
}

bool newer(const LauncherStartEntry& a, const LauncherStartEntry& b) {
    if (a.when != b.when) return a.when > b.when;
    return std::strcmp(a.path, b.path) < 0;   // a fixed order for equal times
}

#if !defined(RECOMP_LAUNCHER_V2_HOST_SCANNER)
bool g_set_by_us[2];   // psx_start_from_apply set the variable in this process

void put_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   // an empty value removes it
#else
    if (value) setenv(name, value, 1);
    else       unsetenv(name);
#endif
}
#endif

} // namespace

extern "C" int psx_start_codegen_hash(const char* product_dir, uint32_t* hash) {
    if (!product_dir || !hash) return 0;
    const std::string path = (fs::path(product_dir) / "overlay_codegen_hash.h").string();
    char text[2048];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
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
            return 1;
        }
    }
    return 0;
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
    std::vector<SlotFile> states, slot_replays;
    for (const char* bios : kBios) {
        const std::string dir = (fs::path(save_root) / bios).string();
        for (const std::string& name : files_of(dir)) {
            SlotFile sf;
            sf.bios = bios;
            sf.path = (fs::path(dir) / name).string();
            if (parse_slot_name(name, "state", ".pst", &sf.entry, &sf.disc, &sf.slot)) {
                if (sf.slot < 0 || sf.slot > 11) continue;
                if (!file_info(sf.path, nullptr, &sf.when)) continue;
                if (!read_state_codegen(sf.path, &sf.codegen)) continue;
                states.push_back(sf);
            } else if (parse_slot_name(name, "replay", ".psxrpl", &sf.entry, &sf.disc, &sf.slot)) {
                if (sf.slot < 1 || sf.slot > 12) continue;
                if (!file_info(sf.path, nullptr, &sf.when)) continue;
                slot_replays.push_back(sf);
            }
        }
    }

    // The BIOS folder and the program in use: those of the newest save state,
    // or of the newest replay slot while there is no save state.
    const std::vector<SlotFile>& lead = !states.empty() ? states : slot_replays;
    const SlotFile* newest = nullptr;
    for (const SlotFile& sf : lead)
        if (!newest || sf.when > newest->when) newest = &sf;
    auto in_use = [&](const SlotFile& sf) {
        return newest && sf.bios == newest->bios && sf.entry == newest->entry;
    };

    std::vector<LauncherStartEntry> list;
    for (const SlotFile& sf : states) {
        if (!in_use(sf)) { ++n.states_elsewhere; continue; }
        LauncherStartEntry e = {};
        e.kind = LNG_START_STATE;
        e.slot = sf.slot;
        e.disc = sf.disc;
        e.when = sf.when;
        e.build = build_mark(known, mine, true, sf.codegen);
        char label[48];
        std::snprintf(label, sizeof(label), "Slot %d", sf.slot + 1);   // as the F7 menu counts
        set_label(&e, label);
        std::snprintf(e.path, sizeof(e.path), "%s", sf.path.c_str());
        find_state_thumb(sf.path, &e);
        list.push_back(e);
    }
    std::sort(list.begin(), list.end(), newer);
    if ((int)list.size() > PSX_START_MAX_STATES) {
        n.states_unlisted = (int)list.size() - PSX_START_MAX_STATES;
        list.resize(PSX_START_MAX_STATES);
    }

    std::vector<LauncherStartEntry> replays;
    auto add_replay = [&](const std::string& path, const std::string& fallback_name, int slot, int disc) {
        ReplayInfo info;
        int64_t when = 0;
        if (!file_info(path, nullptr, &when) || !read_replay(path, &info)) return;
        LauncherStartEntry e = {};
        e.kind = LNG_START_REPLAY;
        e.slot = slot;
        e.disc = disc;
        e.when = when;
        e.frames = info.frames;
        e.power_on = info.power_on ? 1 : 0;
        e.partial = ends_with(path, ".partial.psxrpl") ? 1 : 0;
        e.build = build_mark(known, mine, info.has_codegen, info.codegen);
        set_label(&e, info.name.empty() ? fallback_name : info.name);
        std::snprintf(e.path, sizeof(e.path), "%s", path.c_str());
        if (info.thumb_offset) {
            std::snprintf(e.thumb_path, sizeof(e.thumb_path), "%s", path.c_str());
            e.thumb_offset = info.thumb_offset;
            e.thumb_w = info.thumb_w;
            e.thumb_h = info.thumb_h;
        }
        replays.push_back(e);
    };
    const std::string replay_dir = (fs::path(save_root) / "replays").string();
    for (const std::string& name : files_of(replay_dir)) {
        if (!ends_with(name, ".psxrpl")) continue;
        add_replay((fs::path(replay_dir) / name).string(), name.substr(0, name.size() - 7), -1, 0);
    }
    for (const SlotFile& sf : slot_replays) {
        if (!in_use(sf)) continue;
        char name[48];
        std::snprintf(name, sizeof(name), "Replay %d", sf.slot);   // as the F7 menu counts
        add_replay(sf.path, name, sf.slot - 1, sf.disc);
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

#if !defined(RECOMP_LAUNCHER_V2_HOST_SCANNER)
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

extern "C" void psx_start_from_vars(const LauncherModel* m, int netplay_launch, PsxStartVar out[2]) {
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
    PsxStartVar vars[2];
    psx_start_from_vars(m, netplay_launch, vars);
    for (int i = 0; i < 2; ++i) {
        if (vars[i].value[0]) {
            put_env(vars[i].name, vars[i].value);
            g_set_by_us[i] = true;
        } else if (g_set_by_us[i]) {
            put_env(vars[i].name, nullptr);
            g_set_by_us[i] = false;
        }
    }
}
#endif

