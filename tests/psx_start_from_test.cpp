// The PlayStation lister of "Start from" (consoles/psx/psx_start_from.h): which
// save states and replays it finds in a product's saves folder, how it marks
// them, and which start-up variables carry the choice to the host.
//
// The test builds its own product folder: a codegen hash header, save states
// and replays with the headers psxrecomp writes, made of filler bytes. No game
// data is in it.
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

struct Tag { uint32_t tag; std::vector<unsigned char> payload; };

static Tag text_tag(uint32_t tag, const std::string& text) {
    return { tag, std::vector<unsigned char>(text.begin(), text.end()) };
}

// A PSXRTI3 replay: header, tagged entries, then the frame records.
static std::vector<unsigned char> replay_bytes(uint32_t frames, const std::vector<Tag>& tags,
                                               const char* magic = "PSXRTI3") {
    std::vector<unsigned char> ext;
    for (const Tag& t : tags) {
        put32(ext, t.tag); put32(ext, (uint32_t)t.payload.size());
        ext.insert(ext.end(), t.payload.begin(), t.payload.end());
        while (ext.size() & 3) ext.push_back(0);
    }
    std::vector<unsigned char> b(8, 0);
    std::memcpy(b.data(), magic, std::strlen(magic));
    put32(b, 3); put32(b, 12); put32(b, frames); put32(b, 0); put32(b, (uint32_t)ext.size());
    b.insert(b.end(), ext.begin(), ext.end());
    b.resize(b.size() + 64, 0);   // the records are not read by the lister
    return b;
}

static Tag power_on_tag() { Tag t = { 0x00000306u, {} }; put32(t.payload, 0); return t; }
static Tag anchor_tag()   { return { 0x00000301u, std::vector<unsigned char>(4096 + 2, 0xAB) }; }
static Tag product_tag(uint32_t codegen) {
    char lines[160];
    std::snprintf(lines, sizeof(lines), "exe_sha256=00ff\ncodegen=%08x\nbios_crc32=0badf00d\nrenderer=opengl\n",
                  (unsigned)codegen);
    return text_tag(0x80000308u, lines);
}
static Tag picture_tag(int w, int h, unsigned char blue) {
    Tag t = { 0x80000304u, {} };
    t.payload.push_back((unsigned char)w); t.payload.push_back(0);
    t.payload.push_back((unsigned char)h); t.payload.push_back(0);
    for (int i = 0; i < w * h; ++i) {
        t.payload.push_back(blue); t.payload.push_back(0x55); t.payload.push_back(0x66); t.payload.push_back(0xFF);
    }
    return t;
}

static const LauncherStartEntry* find(const LauncherStartEntry* list, int n, int kind, const char* label) {
    for (int i = 0; i < n; ++i)
        if (list[i].kind == kind && std::strcmp(list[i].label, label) == 0) return &list[i];
    return nullptr;
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
           "the product's codegen hash is read from its header");
    write_text(dir / "overlay_codegen_hash.h", "#define PSX_OVERLAY_CODEGEN_HASHES 1\n");
    expect(psx_start_codegen_hash(dir.string().c_str(), &hash) == 0, "a header without the hash gives none");
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
    // last, a picture that is cut short.
    write_file(saves / "scph1001" / "state_80010000_slot05.pst", state_bytes(kMine, 0x12345678u), 1);
    write_file(saves / "scph1001" / "state_80010000_slot07.pst", {}, 1);
    write_file(saves / "scph1001" / "state_80010000_slot12.pst", state_bytes(kMine), 1);
    write_file(saves / "scph1001" / "state_80010000_slot03.thumb", { 'P', 'S', 'T', 'H', 128, 0, 0, 0, 96, 0, 0, 0, 1 }, 1);
    // Counted, not listed: another program of the set, and the other BIOS.
    write_file(saves / "scph1001" / "state_80020000_slot00.pst", state_bytes(kMine), 5);
    write_file(saves / "openbios" / "state_80010000_slot00.pst", state_bytes(kMine), 6);

    // Replays: a power-on one of this build, an older named one of another
    // build with a picture, a copy a session left unfinished, a file that is
    // no replay, and an F11 slot beside the states.
    write_file(saves / "replays" / "Game-boot-20261007T101500Z.psxrpl",
               replay_bytes(3600, { power_on_tag(), product_tag(kMine) }), 1);
    write_file(saves / "replays" / "Old.psxrpl",
               replay_bytes(7325, { anchor_tag(), text_tag(0x80000305u, "My run"), picture_tag(2, 2, 0x99),
                                    product_tag(kOther) }), 8);
    write_file(saves / "replays" / "Crash-boot-20261001T000000Z.partial.psxrpl",
               replay_bytes(1800, { power_on_tag() }), 9);
    write_file(saves / "replays" / "Broken.psxrpl", replay_bytes(100, {}, "NOTRPLY"), 1);
    write_file(saves / "replays" / "notes.txt", { 'x' }, 1);
    write_file(saves / "scph1001" / "replay_80010000_slot02.psxrpl",
               replay_bytes(600, { anchor_tag(), product_tag(kMine) }), 4);
    write_file(saves / "openbios" / "replay_80010000_slot01.psxrpl",
               replay_bytes(600, { anchor_tag() }), 4);

    LauncherStartEntry list[LNG_START_MAX];
    LauncherStartNotes notes;
    const int n = psx_start_scan(saves.string().c_str(), product.string().c_str(), list, LNG_START_MAX, &notes);
    expect_int(n, 7, "three save states and four replays are listed");
    expect_int(notes.build_known, 1, "the build is known");
    expect_int(notes.states_elsewhere, 2, "two save states are of another program or the other BIOS");
    expect_int(notes.states_unlisted, 0, "no save state is left out for room");
    if (n != 7) return;

    expect(list[0].kind == LNG_START_STATE && list[1].kind == LNG_START_STATE &&
           list[2].kind == LNG_START_STATE, "the save states come first");
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

    expect(list[3].kind == LNG_START_REPLAY && list[6].kind == LNG_START_REPLAY, "the replays follow");
    expect(std::strcmp(list[3].label, "Game-boot-20261007T101500Z") == 0, "newest first: the power-on replay, by its file name");
    expect(list[3].power_on == 1 && list[3].frames == 3600 && list[3].slot == -1 && !list[3].partial,
           "it starts at power-on and has 3,600 frames");
    expect_int(list[3].build, LNG_START_BUILD_SAME, "this build recorded it");
    expect(list[3].thumb_path[0] == '\0', "a power-on replay has no picture");
    const LauncherStartEntry* slot = find(list, n, LNG_START_REPLAY, "Replay 2");
    expect(slot && slot->slot == 1 && !slot->power_on && slot->frames == 600,
           "the F11 slot is listed as Replay 2, from a save state");
    const LauncherStartEntry* old = find(list, n, LNG_START_REPLAY, "My run");
    expect(old != nullptr, "a replay with a name is listed under it");
    if (old) {
        expect_int(old->build, LNG_START_BUILD_OTHER, "another build recorded it");
        expect(old->thumb_w == 2 && old->thumb_h == 2 && byte_at(old->thumb_path, old->thumb_offset) == 0x99,
               "its picture is found behind the save state it carries");
        expect(old->frames == 7325 && !old->power_on, "with its length");
    }
    const LauncherStartEntry* part = find(list, n, LNG_START_REPLAY, "Crash-boot-20261001T000000Z.partial");
    expect(part && part->partial == 1, "the unfinished copy is marked");
    if (part) expect_int(part->build, LNG_START_BUILD_UNKNOWN, "a replay without product lines has no build mark");
    expect(find(list, n, LNG_START_REPLAY, "Broken") == nullptr, "a file that is no replay is not listed");
    expect(find(list, n, LNG_START_REPLAY, "Replay 1") == nullptr, "a replay slot of the other BIOS is not listed");

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

static void test_many_replays(const fs::path& root) {
    const fs::path saves = root / "many" / "saves";
    for (int i = 0; i < PSX_START_MAX_REPLAYS + 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "r%02d.psxrpl", i);
        write_file(saves / "replays" / name, replay_bytes(60, { power_on_tag() }), i + 1);
    }
    LauncherStartEntry list[LNG_START_MAX];
    LauncherStartNotes notes;
    const int n = psx_start_scan(saves.string().c_str(), nullptr, list, LNG_START_MAX, &notes);
    expect_int(n, PSX_START_MAX_REPLAYS, "of thirty replays the newest twenty-four are listed");
    expect_int(notes.replays_unlisted, 6, "and six are counted as left out");
    expect(n > 0 && std::strcmp(list[0].label, "r00") == 0, "the newest is first");
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
    expect_int(launcher_model_start_count(m), 7, "the model holds the product's seven entries");
    expect_int(launcher_model_start_selected(m), -1, "and starts on Power on");

    PsxStartVar vars[2];
    psx_start_from_vars(m, 0, vars);
    expect(std::strcmp(vars[0].name, "PSX_LOAD_SLOT") == 0 && std::strcmp(vars[1].name, "PSX_REPLAY_FILE") == 0,
           "the two variables are the ones the host reads");
    expect(!vars[0].value[0] && !vars[1].value[0], "Power on sets neither");

    launcher_model_select_start(m, 0);   // Slot 4
    psx_start_from_vars(m, 0, vars);
    expect(std::strcmp(vars[0].value, "3") == 0 && !vars[1].value[0], "Slot 4 is PSX_LOAD_SLOT=3");
    psx_start_from_vars(m, 1, vars);
    expect(!vars[0].value[0] && !vars[1].value[0], "a netplay launch sets neither");

    launcher_model_select_start(m, 3);   // the power-on replay
    psx_start_from_vars(m, 0, vars);
    expect(!vars[0].value[0] && std::strstr(vars[1].value, "Game-boot-20261007T101500Z.psxrpl") != nullptr,
           "a replay is PSX_REPLAY_FILE with its file");

    // In the process: only what the launcher set itself is taken away again.
#ifdef _WIN32
    _putenv_s("PSX_REPLAY_FILE", "set-from-outside.psxrpl");
    _putenv_s("PSX_LOAD_SLOT", "");
#else
    setenv("PSX_REPLAY_FILE", "set-from-outside.psxrpl", 1);
    unsetenv("PSX_LOAD_SLOT");
#endif
    launcher_model_select_start(m, -1);
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_REPLAY_FILE") && std::strcmp(std::getenv("PSX_REPLAY_FILE"), "set-from-outside.psxrpl") == 0,
           "Power on leaves a variable alone that was set from outside");
    expect(std::getenv("PSX_LOAD_SLOT") == nullptr, "and sets no slot");

    launcher_model_select_start(m, 1);   // Slot 1
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_LOAD_SLOT") && std::strcmp(std::getenv("PSX_LOAD_SLOT"), "0") == 0,
           "Slot 1 puts PSX_LOAD_SLOT=0 into the process");
    launcher_model_select_start(m, 3);
    psx_start_from_apply(m, 0);
    expect(std::getenv("PSX_LOAD_SLOT") == nullptr, "choosing a replay next takes the slot away");
    expect(std::getenv("PSX_REPLAY_FILE") && std::strstr(std::getenv("PSX_REPLAY_FILE"), "Game-boot") != nullptr,
           "and puts the replay's file in");
    psx_start_from_apply(m, 1);
    expect(std::getenv("PSX_REPLAY_FILE") == nullptr && std::getenv("PSX_LOAD_SLOT") == nullptr,
           "a netplay launch takes both away");
    std::free(m);
}

int main(int argc, char** argv) {
    const fs::path root = fs::path(argc > 1 ? argv[1] : ".") / "psx-start-from-fixture";
    std::error_code ec;
    fs::remove_all(root, ec);
    test_codegen_hash(root);
    test_scan(root);
    test_many_replays(root);
    test_hand_over(root);
    fs::remove_all(root, ec);
    if (fails) { std::fprintf(stderr, "psx_start_from_test: %d failure(s)\n", fails); return 1; }
    std::printf("psx_start_from_test: all checks passed\n");
    return 0;
}
