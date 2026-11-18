// psx_start_from.cpp - see psx_start_from.h.

#include "psx_start_from.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;

// Paths cross the C interface as UTF-8 (SDL hands them over that way), which
// on Windows is not the ANSI code page fs::path(std::string) and fopen assume.
// Everything below keeps them as fs::path and converts at the edges.
fs::path from_utf8(const std::string& s) { return fs::u8path(s); }

std::string to_utf8(const fs::path& p) {
    const auto u = p.u8string();   // std::string before C++20, std::u8string after
    return std::string(u.begin(), u.end());
}

uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Size and modified time of a file with content; false for anything else.
bool file_info(const fs::path& path, int64_t* size, int64_t* when) {
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(path.c_str(), &st) != 0) return false;
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
std::vector<fs::path> files_of(const fs::path& dir) {
    std::vector<fs::path> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code is_ec;
        if (it->is_regular_file(is_ec) && !is_ec) names.push_back(it->path().filename());
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
    fs::path    path;
    uint32_t    codegen = 0;
};

const uint32_t kStateMagic = 0x50535842u;   // "PSXB", boot_state.h
const size_t   kStateHeader = 36;           // nine 32-bit fields

// The codegen hash in a save state's header; false when it is no save state.
bool read_state_codegen(const fs::path& path, uint32_t* codegen) {
    unsigned char head[kStateHeader];
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(head), (std::streamsize)sizeof(head));
    const size_t got = (size_t)f.gcount();
    if (got != sizeof(head) || le32(head) != kStateMagic) return false;
    *codegen = le32(head + 16);
    return true;
}

// A save state's picture: "PSTH", width, height, then the pixels.
void find_state_thumb(const fs::path& state_path, LauncherStartEntry* e) {
    fs::path path = state_path;
    path.replace_extension(".thumb");
    unsigned char head[12];
    int64_t size = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    f.read(reinterpret_cast<char*>(head), (std::streamsize)sizeof(head));
    const size_t got = (size_t)f.gcount();
    f.close();
    if (got != sizeof(head) || std::memcmp(head, "PSTH", 4) != 0) return;
    const uint32_t w = le32(head + 4), h = le32(head + 8);
    if (w < 1 || h < 1 || w > 512 || h > 512) return;
    if (!file_info(path, &size, nullptr) || size < (int64_t)(12 + (uint64_t)w * h * 4)) return;
    std::snprintf(e->thumb_path, sizeof(e->thumb_path), "%s", to_utf8(path).c_str());
    e->thumb_offset = 12;
    e->thumb_w = (int)w;
    e->thumb_h = (int)h;
}

bool newer(const LauncherStartEntry& a, const LauncherStartEntry& b) {
    if (a.when != b.when) return a.when > b.when;
    return std::strcmp(a.path, b.path) < 0;   // a fixed order for equal times
}

bool g_set_by_us;   // psx_start_from_apply set the variable in this process

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
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(text, (std::streamsize)(sizeof(text) - 1));
    const size_t got = (size_t)f.gcount();
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
    const fs::path dir = from_utf8(product_dir);
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
        const fs::path dir = from_utf8(save_root) / bios;
        for (const fs::path& file : files_of(dir)) {
            StateFile sf;
            sf.bios = bios;
            sf.path = dir / file;
            if (!parse_state_name(to_utf8(file), &sf.entry, &sf.disc, &sf.slot)) continue;
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
        e.build = !known ? LNG_START_BUILD_UNKNOWN
                         : (mine == sf.codegen ? LNG_START_BUILD_SAME : LNG_START_BUILD_OTHER);
        std::snprintf(e.label, sizeof(e.label), "Slot %d", sf.slot + 1);   // as the save-state menu counts
        std::snprintf(e.path, sizeof(e.path), "%s", to_utf8(sf.path).c_str());
        find_state_thumb(sf.path, &e);
        list.push_back(e);
    }
    std::sort(list.begin(), list.end(), newer);
    if ((int)list.size() > PSX_START_MAX_STATES) {
        n.states_unlisted = (int)list.size() - PSX_START_MAX_STATES;
        list.resize(PSX_START_MAX_STATES);
    }

    if (notes) *notes = n;
    const int count = (int)list.size() < cap ? (int)list.size() : cap;
    for (int i = 0; i < count; ++i) out[i] = list[(size_t)i];
    return count;
}

extern "C" void psx_start_from_load(LauncherModel* m, const char* product_dir, const char* save_root) {
    if (!launcher_model_start_from_available(m)) return;
    const std::string product = product_dir ? product_dir : "";
    const std::string root = save_root ? std::string(save_root)
                                       : to_utf8(from_utf8(product) / "saves");
    static LauncherStartEntry entries[LNG_START_MAX];
    LauncherStartNotes notes = {};
    const int count = psx_start_scan(root.c_str(), product.c_str(), entries, LNG_START_MAX, &notes);
    launcher_model_set_start_entries(m, entries, count, &notes);
}

extern "C" void psx_start_from_vars(const LauncherModel* m, int netplay_launch, PsxStartVar* out) {
    out->name = "PSX_LOAD_SLOT";
    out->value[0] = '\0';
    if (netplay_launch) return;
    int slot = -1;
    if (launcher_model_start_handover(m, &slot) == LNG_START_STATE)
        std::snprintf(out->value, sizeof(out->value), "%d", slot);
}

extern "C" void psx_start_from_apply(const LauncherModel* m, int netplay_launch) {
    PsxStartVar var;
    psx_start_from_vars(m, netplay_launch, &var);
    if (var.value[0]) {
        put_env(var.name, var.value);
        g_set_by_us = true;
    } else if (g_set_by_us) {
        put_env(var.name, nullptr);
        g_set_by_us = false;
    }
}
