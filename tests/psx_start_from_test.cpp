// The PlayStation lister of "Start from" (consoles/psx/psx_start_from.h): which
// save states and replays it finds in a product's saves folder, how it marks
// them, and the start-up variables that carry the choice to the host.
//
// The test builds its own product folder: a codegen hash header, save states
// with the header psxrecomp writes and replays in its PSXRTI3 layout, made of
// filler bytes. No game data is in it.
#include "psx_start_from.h"

#include "launcher_profile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void launcher_binds_set_zapper(int a, int b);
extern "C" void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

namespace fs = std::filesystem;

static int fails;

static void expect(bool cond, const std::string& what) {
    if (cond) { std::printf("ok: %s\n", what.c_str()); return; }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++fails;
}

static void expect_int(long long got, long long want, const std::string& what) {
    if (got == want) { std::printf("ok: %s\n", what.c_str()); return; }
    std::fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", what.c_str(), got, want);
    ++fails;
}

static void put32(std::vector<unsigned char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((unsigned char)(v >> (8 * i)));
}

static void write_file(const fs::path& p, const std::vector<unsigned char>& bytes, int hours_old) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    f.close();
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(hours_old));
}

static const uint32_t kMine = 0x25fd1f54u, kOther = 0x11112222u;

// A save state as boot_state.h lays its header out, with filler behind it.
static std::vector<unsigned char> state_bytes(uint32_t codegen, uint32_t magic = 0x50535842u) {
    std::vector<unsigned char> b;
    put32(b, magic); put32(b, 15); put32(b, 0xB105B105u); put32(b, 0x80010000u);
    put32(b, codegen); put32(b, 26); put32(b, 3); put32(b, 0); put32(b, 0);
    b.resize(600, 0xEE);
    return b;
}

// "PSTH", width, height, pixels (blue, green, red, alpha).
static std::vector<unsigned char> thumb_bytes(int w, int h, unsigned char blue) {
    std::vector<unsigned char> b = { 'P', 'S', 'T', 'H' };
    put32(b, (uint32_t)w); put32(b, (uint32_t)h);
    for (int i = 0; i < w * h; ++i) { b.push_back(blue); b.push_back(0x22); b.push_back(0x33); b.push_back(0xFF); }
    return b;
}

// One PSXRTI3 entry: tag, length, payload, padding to four bytes.
static void entry(std::vector<unsigned char>& b, uint32_t tag, const std::vector<unsigned char>& payload) {
    put32(b, tag); put32(b, (uint32_t)payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
    while (b.size() % 4) b.push_back(0);
}

static std::vector<unsigned char> text(const std::string& s) {
    return std::vector<unsigned char>(s.begin(), s.end());
}

static std::vector<unsigned char> u32s(std::initializer_list<uint32_t> v) {
    std::vector<unsigned char> b;
    for (uint32_t x : v) put32(b, x);
    return b;
}

struct ReplayShape {
    bool     power_on = true;   // else an anchor state
    bool     anchor_too = false;
    uint32_t codegen = kMine;
    bool     product = true;
    std::string name;
    int      thumb_w = 0, thumb_h = 0;
    uint32_t extra_tag = 0;     // one more empty entry
    uint32_t version = 3, record_size = 12, flags = 0;
    int      size_off = 0;      // bytes added to (or cut from) the end
    uint32_t frames = 75;
};

// A replay as runtime/include/replay_file.h lays it out: the header, the
// entries, then one 12-byte record per frame, the records filler.
static std::vector<unsigned char> replay_bytes(const ReplayShape& s) {
    std::vector<unsigned char> ext;
    if (s.power_on) { entry(ext, 0x00000306u, u32s({ 0 })); entry(ext, 0x00000307u, u32s({ 0 })); }
    if (!s.power_on || s.anchor_too) entry(ext, 0x00000301u, std::vector<unsigned char>(64, 0xAB));
    entry(ext, 0x00000302u, text("cd_speed=2\n"));
    entry(ext, 0x00000303u, u32s({ 0 }));
    if (!s.name.empty()) entry(ext, 0x80000305u, text(s.name));
    if (s.thumb_w) {
        std::vector<unsigned char> t = { (unsigned char)s.thumb_w, 0, (unsigned char)s.thumb_h, 0 };
        for (int i = 0; i < s.thumb_w * s.thumb_h; ++i) { t.push_back(0x5A); t.push_back(0x22); t.push_back(0x33); t.push_back(0xFF); }
        entry(ext, 0x80000304u, t);
    }
    if (s.product) {
        char line[64];
        std::snprintf(line, sizeof(line), "exe_sha256=00\ncodegen=%08x\nplatform=test\n", (unsigned)s.codegen);
        entry(ext, 0x80000308u, text(line));
    }
    if (s.extra_tag) entry(ext, s.extra_tag, {});
    std::vector<unsigned char> b;
    put32(b, 0x52585350u); put32(b, 0x00334954u);   // "PSXRTI3", then a zero byte
    put32(b, s.version); put32(b, s.record_size); put32(b, s.frames); put32(b, s.flags);
    put32(b, (uint32_t)ext.size());
    b.insert(b.end(), ext.begin(), ext.end());
    b.resize(b.size() + (size_t)s.frames * 12, 0xFF);
    if (s.size_off > 0) b.resize(b.size() + (size_t)s.size_off, 0);
    if (s.size_off < 0) b.resize(b.size() - (size_t)(-s.size_off));
    return b;
}

static unsigned char byte_at(const char* path, uint32_t offset) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(offset);
    return (unsigned char)f.get();
}

static void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static void test_codegen_hash(const fs::path& root) {
    const fs::path dir = root / "hash";
    uint32_t hash = 0;
    write_text(dir / "overlay_codegen_hash.h",
               "/* AUTO-GENERATED: PSX_OVERLAY_CODEGEN_HASH is the hash. */\n#pragma once\n"
               "#define PSX_OVERLAY_CODEGEN_HASH 0x25fd1f54u\n");
    expect(psx_start_codegen_hash(dir.string().c_str(), &hash) == 1 && hash == kMine,
           "the codegen hash is read from a header beside the exe");
    write_text(dir / "overlay_toolchain" / "include" / "overlay_codegen_hash.h",
               "#define PSX_OVERLAY_CODEGEN_HASH 0x11112222u\n");
    expect(psx_start_codegen_hash(dir.string().c_str(), &hash) == 1 && hash == kOther,
           "the packaged overlay toolchain's header comes first");
    const fs::path bare = root / "bare";
    write_text(bare / "overlay_codegen_hash.h", "#define PSX_OVERLAY_CODEGEN_HASHES 1\n");
    expect(psx_start_codegen_hash(bare.string().c_str(), &hash) == 0, "a header without the hash gives none");
    expect(psx_start_codegen_hash((root / "nowhere").string().c_str(), &hash) == 0, "no header: no hash");
    expect(psx_start_codegen_hash(nullptr, &hash) == 0, "no folder: no hash");
}

static void test_scan(const fs::path& root) {
    const fs::path product = root / "product";
    const fs::path saves = product / "saves";
    write_text(product / "overlay_codegen_hash.h", "#define PSX_OVERLAY_CODEGEN_HASH 0x25fd1f54u\n");

    // Save states of this program under the retail BIOS: newest first is
    // slot 4 (other build), slot 1 with a picture, then disc 2's slot 2.
    write_file(saves / "scph1001" / "state_80010000_slot03.pst", state_bytes(kOther), 1);
    write_file(saves / "scph1001" / "state_80010000_slot00.pst", state_bytes(kMine), 2);
    write_file(saves / "scph1001" / "state_80010000_slot00.thumb", thumb_bytes(128, 96, 0x77), 2);
    write_file(saves / "scph1001" / "state_80010000_disc2_slot01.pst", state_bytes(kMine), 3);
    // Not listed: a file that is no save state, an empty one, a slot past the
    // last, a picture that is cut short, and files that are no save state.
    write_file(saves / "scph1001" / "state_80010000_slot05.pst", state_bytes(kMine, 0x12345678u), 1);
    write_file(saves / "scph1001" / "state_80010000_slot07.pst", {}, 1);
    write_file(saves / "scph1001" / "state_80010000_slot12.pst", state_bytes(kMine), 1);
    write_file(saves / "scph1001" / "state_80010000_slot03.thumb", { 'P', 'S', 'T', 'H', 128, 0, 0, 0, 96, 0, 0, 0, 1 }, 1);
    write_file(saves / "scph1001" / "replay_80010000_slot02.psxrpl", state_bytes(kMine), 1);
    write_file(saves / "replays" / "run.psxrpl", state_bytes(kMine), 1);

    // Replays, as the recorder names them: from power-on (newest), and a
    // second one of the same second, anchored, with a name and a picture,
    // recorded by another build.
    const fs::path rp = saves / "replays";
    write_file(rp / "TEST_00001-boot-20261118-101500Z.psxrpl", replay_bytes(ReplayShape()), 1);
    {
        ReplayShape s;
        s.power_on = false; s.codegen = kOther; s.name = "Fixture run"; s.thumb_w = 8; s.thumb_h = 6;
        write_file(rp / "TEST_00001-boot-20261118-101500Z-1.psxrpl", replay_bytes(s), 2);
    }
    {
        ReplayShape s;
        s.product = false;   // no codegen line: no mark
        write_file(rp / "game-boot-20261117-090000Z.psxrpl", replay_bytes(s), 4);
    }
    // Not listed: names the recorder does not write, and files the host
    // would refuse to play.
    write_file(rp / "fixture.psxrpl", replay_bytes(ReplayShape()), 1);
    write_file(rp / "TEST_00001-boot-20261118-1015Z.psxrpl", replay_bytes(ReplayShape()), 1);
    write_file(rp / "TEST_00001-boot-20261118-101500Z-0.psxrpl", replay_bytes(ReplayShape()), 1);
    write_file(rp / "TEST-00001-boot-20261118-101500Z.psxrpl", replay_bytes(ReplayShape()), 1);
    write_file(rp / "TEST_00001-boot-20261118-101500Z.psxrpl.tmp", replay_bytes(ReplayShape()), 1);
    {
        ReplayShape s;
        s.version = 2;
        write_file(rp / "TEST_00001-boot-20261118-101501Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.record_size = 8;
        write_file(rp / "TEST_00001-boot-20261118-101502Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.flags = 1;
        write_file(rp / "TEST_00001-boot-20261118-101503Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.size_off = -12;
        write_file(rp / "TEST_00001-boot-20261118-101504Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.size_off = 4;
        write_file(rp / "TEST_00001-boot-20261118-101505Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.extra_tag = 0x00000309u;   // changes playback; unknown to the host
        write_file(rp / "TEST_00001-boot-20261118-101506Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.extra_tag = 0x80000120u;   // an identity tag the host does not know
        write_file(rp / "TEST_00001-boot-20261118-101507Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.anchor_too = true;         // two starts
        write_file(rp / "TEST_00001-boot-20261118-101508Z.psxrpl", replay_bytes(s), 1);
        s = ReplayShape(); s.frames = 0;
        write_file(rp / "TEST_00001-boot-20261118-101509Z.psxrpl", replay_bytes(s), 1);
        write_file(rp / "TEST_00001-boot-20261118-101510Z.psxrpl", state_bytes(kMine), 1);
        write_file(rp / "TEST_00001-boot-20261118-101511Z.psxrpl", {}, 1);
    }
    // Kept: an entry the host skips.
    {
        ReplayShape s;
        s.extra_tag = 0x80000399u;
        write_file(rp / "TEST_00001-boot-20261116-080000Z.psxrpl", replay_bytes(s), 6);
    }
    // Counted, not listed: another program of the set, and the other BIOS.
    write_file(saves / "scph1001" / "state_80020000_slot00.pst", state_bytes(kMine), 5);
    write_file(saves / "openbios" / "state_80010000_slot00.pst", state_bytes(kMine), 6);

    LauncherStartEntry list[LNG_START_MAX];
    LauncherStartNotes notes;
    const int n = psx_start_scan(saves.string().c_str(), product.string().c_str(), list, LNG_START_MAX, &notes);
    expect_int(n, 7, "three save states and four replays are listed");
    expect_int(notes.build_known, 1, "the build is known");
    expect_int(notes.states_elsewhere, 2, "two save states are of another program or the other BIOS");
    expect_int(notes.states_unlisted, 0, "no save state is left out for room");
    expect_int(notes.replays_unlisted, 0, "no replay is left out for room");
    if (n != 7) return;

    expect(list[0].kind == LNG_START_STATE && list[1].kind == LNG_START_STATE &&
           list[2].kind == LNG_START_STATE, "every entry is a save state");
    expect(std::strcmp(list[0].label, "Slot 4") == 0 && list[0].slot == 3, "newest first: slot 4, as the game counts it");
    expect_int(list[0].build, LNG_START_BUILD_OTHER, "it is marked as another build's");
    expect(list[0].thumb_path[0] == '\0', "a picture cut short is no picture");
    expect(std::strcmp(list[1].label, "Slot 1") == 0 && list[1].slot == 0, "then slot 1");
    expect_int(list[1].build, LNG_START_BUILD_SAME, "which this build made");
    expect(list[1].thumb_w == 128 && list[1].thumb_h == 96 && list[1].thumb_offset == 12,
           "with its picture, 128 by 96");
    expect(byte_at(list[1].thumb_path, list[1].thumb_offset) == 0x77, "the picture's first byte is where the entry says");
    expect(list[2].slot == 1 && list[2].disc == 2, "then disc 2's slot 2");
    expect(list[0].disc == 0 && list[1].disc == 0, "a state without a disc in its name names none");
    expect(list[0].when > list[1].when && list[1].when > list[2].when, "the times fall");

    // Then the replays, newest first, each handed over by its absolute path.
    bool replays = true;
    for (int i = 3; i < 7; ++i)
        replays = replays && list[i].kind == LNG_START_REPLAY && list[i].slot == -1 &&
                  list[i].disc == 0 && fs::path(list[i].path).is_absolute();
    expect(replays, "after the save states come the replays, with absolute paths, no slot and no disc");
    const auto leaf = [&](int i) { return fs::path(list[i].path).filename().string(); };
    expect(leaf(3) == "TEST_00001-boot-20261118-101500Z.psxrpl", "the newest replay first");
    expect(std::strcmp(list[3].label, "Replay from power-on") == 0 && list[3].power_on == 1,
           "a replay without a name is called by where it starts");
    expect_int(list[3].frames, 75, "with its frame count");
    expect_int(list[3].build, LNG_START_BUILD_SAME, "which this build recorded");
    expect(list[3].thumb_path[0] == '\0', "and no picture");
    expect(leaf(4) == "TEST_00001-boot-20261118-101500Z-1.psxrpl", "then the second name of that second");
    expect(std::strcmp(list[4].label, "Fixture run") == 0 && list[4].power_on == 0,
           "called by its name, starting at its save state");
    expect_int(list[4].build, LNG_START_BUILD_OTHER, "and marked as another build's");
    expect(list[4].thumb_w == 8 && list[4].thumb_h == 6 && std::strcmp(list[4].thumb_path, list[4].path) == 0,
           "its picture, 8 by 6, is in the replay itself");
    expect(byte_at(list[4].thumb_path, list[4].thumb_offset) == 0x5A, "the picture's first byte is where the entry says");
    expect(leaf(5) == "game-boot-20261117-090000Z.psxrpl", "a replay of a disc without a serial");
    expect_int(list[5].build, LNG_START_BUILD_UNKNOWN, "without a codegen line it carries no mark");
    expect(leaf(6) == "TEST_00001-boot-20261116-080000Z.psxrpl", "an entry the host skips does not hide a replay");
    expect(list[3].when > list[4].when && list[4].when > list[5].when && list[5].when > list[6].when,
           "the replays' times fall");

    // Without the product's header nothing is marked.
    const int m = psx_start_scan(saves.string().c_str(), (root / "nowhere").string().c_str(), list,
                                 LNG_START_MAX, &notes);
    expect(m == 7 && notes.build_known == 0, "no hash header: the same list, build not known");
    bool none = true;
    for (int i = 0; i < m; ++i) none = none && list[i].build == LNG_START_BUILD_UNKNOWN;
    expect(none, "and no entry carries a mark");

    // A short list keeps the newest.
    expect_int(psx_start_scan(saves.string().c_str(), product.string().c_str(), list, 2, &notes), 2,
               "a list with room for two holds two");
    expect(list[0].slot == 3 && list[1].slot == 0, "the two newest");

    expect_int(psx_start_scan((root / "nowhere").string().c_str(), product.string().c_str(), list,
                              LNG_START_MAX, &notes), 0, "no saves folder: nothing listed");
    expect_int(psx_start_scan(nullptr, nullptr, list, LNG_START_MAX, &notes), 0, "no folder named: nothing listed");
}

static void test_many_states(const fs::path& root) {
    const fs::path saves = root / "many" / "saves";
    // Twelve slots on each of three discs: thirty-six states of one program.
    for (int i = 0; i < 36; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "state_80010000_disc%d_slot%02d.pst", i / 12 + 1, i % 12);
        write_file(saves / "scph1001" / name, state_bytes(kMine), i + 1);
    }
    LauncherStartEntry list[LNG_START_MAX];
    LauncherStartNotes notes;
    const int n = psx_start_scan(saves.string().c_str(), nullptr, list, LNG_START_MAX, &notes);
    expect_int(n, PSX_START_MAX_STATES, "of thirty-six save states the newest twenty-four are listed");
    expect_int(notes.states_unlisted, 36 - PSX_START_MAX_STATES, "and the rest are counted as left out");
    expect(n > 0 && list[0].disc == 1 && list[0].slot == 0, "the newest is first");

    // Thirty replays besides: the newest twenty-four, and the whole list fits.
    for (int i = 0; i < 30; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "TEST_00001-boot-20261118-1000%02dZ.psxrpl", i);
        write_file(saves / "replays" / name, replay_bytes(ReplayShape()), i + 1);
    }
    const int r = psx_start_scan(saves.string().c_str(), nullptr, list, LNG_START_MAX, &notes);
    expect_int(r, PSX_START_MAX_STATES + PSX_START_MAX_REPLAYS, "twenty-four save states and twenty-four replays");
    expect_int(notes.replays_unlisted, 30 - PSX_START_MAX_REPLAYS, "the older replays are counted as left out");
    expect(r == LNG_START_MAX && list[PSX_START_MAX_STATES].kind == LNG_START_REPLAY &&
           fs::path(list[PSX_START_MAX_STATES].path).filename() == "TEST_00001-boot-20261118-100000Z.psxrpl",
           "the newest replay leads the replays, and the list is full");
}

static void test_hand_over(const fs::path& root) {
    RecompLauncherCGameInfo game;
    RecompLauncherCSettings io;
    std::memset(&game, 0, sizeof(game));
    std::memset(&io, 0, sizeof(io));
    launcher_profile_apply("psx", &game);
    game.name = "Start From Fixture";
    LauncherModel* m = (LauncherModel*)std::calloc(1, sizeof(LauncherModel));
    launcher_model_init(m, &io, &game, nullptr);
    const fs::path product = root / "product";
    psx_start_from_load(m, product.string().c_str(), nullptr);
    expect_int(launcher_model_start_count(m), 7, "the model holds the product's three save states and four replays");
    expect_int(launcher_model_start_selected(m), -1, "and starts on Power on");

    PsxStartVar vars[PSX_START_VARS];
    psx_start_from_vars(m, 0, vars);
    expect(std::strcmp(vars[0].name, "PSX_LOAD_SLOT") == 0 && std::strcmp(vars[1].name, "PSX_REPLAY_FILE") == 0,
           "the variables are the ones the host reads");
    expect(!vars[0].value[0] && !vars[1].value[0], "Power on sets neither");

    launcher_model_select_start(m, 0);   // Slot 4
    psx_start_from_vars(m, 0, vars);
    expect(std::strcmp(vars[0].value, "3") == 0 && !vars[1].value[0], "Slot 4 is PSX_LOAD_SLOT=3, and no replay");
    psx_start_from_vars(m, 1, vars);
    expect(!vars[0].value[0] && !vars[1].value[0], "a netplay launch sets neither");

    const LauncherStartEntry* rpl = launcher_model_start_entry(m, 3);
    launcher_model_select_start(m, 3);   // the newest replay
    psx_start_from_vars(m, 0, vars);
    expect(rpl && std::strcmp(vars[1].value, rpl->path) == 0 && fs::path(vars[1].value).is_absolute(),
           "a replay is PSX_REPLAY_FILE=<its absolute path>");
    expect(!vars[0].value[0], "with no slot beside it");
    psx_start_from_vars(m, 1, vars);
    expect(!vars[1].value[0], "a netplay launch plays no replay");

    // In the process: only what the launcher set itself is taken away again.
#ifdef _WIN32
    _putenv_s("PSX_LOAD_SLOT", "5");
#else
    setenv("PSX_LOAD_SLOT", "5", 1);
#endif
    launcher_model_select_start(m, -1);
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_LOAD_SLOT") && std::strcmp(std::getenv("PSX_LOAD_SLOT"), "5") == 0,
           "Power on leaves a slot alone that was set from outside");

    launcher_model_select_start(m, 1);   // Slot 1
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_LOAD_SLOT") && std::strcmp(std::getenv("PSX_LOAD_SLOT"), "0") == 0,
           "Slot 1 puts PSX_LOAD_SLOT=0 into the process");
    launcher_model_select_start(m, -1);
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_LOAD_SLOT") == nullptr, "Power on next takes the slot away");
    launcher_model_select_start(m, 0);
    psx_start_from_apply(m, 0);
    psx_start_from_apply(m, 1);
    expect(std::getenv("PSX_LOAD_SLOT") == nullptr, "a netplay launch takes it away");

    // The replay variable, by the same rules, and never beside a slot of ours.
#ifdef _WIN32
    _putenv_s("PSX_REPLAY_FILE", "outside.psxrpl");
#else
    setenv("PSX_REPLAY_FILE", "outside.psxrpl", 1);
#endif
    launcher_model_select_start(m, -1);
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_REPLAY_FILE") && std::strcmp(std::getenv("PSX_REPLAY_FILE"), "outside.psxrpl") == 0,
           "Power on leaves a replay alone that was set from outside");
    launcher_model_select_start(m, 0);   // Slot 4
    psx_start_from_apply(m, 0);
    launcher_model_select_start(m, 4);   // the named replay
    psx_start_from_apply(m, 0);
    rpl = launcher_model_start_entry(m, 4);
    expect(rpl && std::getenv("PSX_REPLAY_FILE") && std::strcmp(std::getenv("PSX_REPLAY_FILE"), rpl->path) == 0,
           "a replay puts PSX_REPLAY_FILE into the process");
    expect(std::getenv("PSX_LOAD_SLOT") == nullptr, "and takes away the slot it set before");
    launcher_model_select_start(m, 1);   // Slot 1
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_REPLAY_FILE") == nullptr, "a save state next takes the replay away");
    expect(std::getenv("PSX_LOAD_SLOT") && std::strcmp(std::getenv("PSX_LOAD_SLOT"), "0") == 0,
           "and sets its slot");
    launcher_model_select_start(m, 3);
    psx_start_from_apply(m, 0);
    psx_start_from_apply(m, 1);
    expect(std::getenv("PSX_REPLAY_FILE") == nullptr && std::getenv("PSX_LOAD_SLOT") == nullptr,
           "a netplay launch takes both away");
    std::free(m);
}

// SDL hands the launcher UTF-8 paths. An install folder with non-ASCII
// characters (here u-umlaut, e-acute and two kana) must list its save states
// and their pictures, and the hash header must be found, on Windows too.
static std::string utf8_of(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

static void test_unicode_paths(const fs::path& root) {
    const fs::path product = root / fs::u8path("pr\xC3\xB6" "duct caf\xC3\xA9 \xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0");
    const fs::path saves = product / "saves";
    write_text(product / "overlay_codegen_hash.h", "#define PSX_OVERLAY_CODEGEN_HASH 0x25fd1f54u\n");
    write_file(saves / "scph1001" / "state_80010000_slot00.pst", state_bytes(kMine), 2);
    write_file(saves / "scph1001" / "state_80010000_slot00.thumb", thumb_bytes(128, 96, 0x77), 2);
    write_file(saves / "scph1001" / "state_80010000_slot01.pst", state_bytes(kOther), 1);

    const std::string product_u8 = utf8_of(product), saves_u8 = utf8_of(saves);
    expect(product_u8.find("pr\xC3\xB6" "duct") != std::string::npos,
           "the fixture folder name is non-ASCII");
    uint32_t hash = 0;
    expect(psx_start_codegen_hash(product_u8.c_str(), &hash) == 1 && hash == kMine,
           "the codegen hash is found under a non-ASCII folder");

    LauncherStartEntry list[LNG_START_MAX];
    LauncherStartNotes notes;
    const int n = psx_start_scan(saves_u8.c_str(), product_u8.c_str(), list, LNG_START_MAX, &notes);
    expect_int(n, 2, "save states are listed under a non-ASCII folder");
    if (n != 2) return;
    expect_int(notes.build_known, 1, "the build is known from the non-ASCII product folder");
    expect(std::strcmp(list[0].label, "Slot 2") == 0 && std::strcmp(list[1].label, "Slot 1") == 0,
           "the states are the ones written there, newest first");
    expect(fs::u8path(list[1].path) == saves / "scph1001" / "state_80010000_slot00.pst",
           "an entry's path comes back as the same UTF-8 path");
    expect(list[1].thumb_w == 128 && list[1].thumb_h == 96 &&
           fs::exists(fs::u8path(list[1].thumb_path)),
           "the picture beside the state is found");
    expect(list[0].thumb_w == 0, "a state with no picture has none");

    // With no save root named, the lister looks in "<product>/saves".
    LauncherModel* m = (LauncherModel*)std::calloc(1, sizeof(LauncherModel));
    if (!m) { expect(false, "out of memory"); return; }
    RecompLauncherCGameInfo game;
    RecompLauncherCSettings io;
    std::memset(&game, 0, sizeof(game));
    std::memset(&io, 0, sizeof(io));
    launcher_profile_apply("psx", &game);
    game.name = "Unicode Fixture";
    launcher_model_init(m, &io, &game, nullptr);
    psx_start_from_load(m, product_u8.c_str(), nullptr);
    expect_int(launcher_model_start_count(m), 2, "the model is filled from a non-ASCII product folder");
    std::free(m);
}

int main(int argc, char** argv) {
    const fs::path root = fs::path(argc > 1 ? argv[1] : ".") / "psx-start-from-fixture";
    std::error_code ec;
    fs::remove_all(root, ec);
    test_codegen_hash(root);
    test_scan(root);
    test_many_states(root);
    test_hand_over(root);
    test_unicode_paths(root);
    fs::remove_all(root, ec);
    if (fails) { std::fprintf(stderr, "psx_start_from_test: %d failure(s)\n", fails); return 1; }
    std::printf("psx_start_from_test: all checks passed\n");
    return 0;
}
