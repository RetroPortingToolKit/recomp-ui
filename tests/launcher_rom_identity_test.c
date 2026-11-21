/* A refused cartridge ROM must say what it is and what the build needs:
 * a catalogued revision by name, an unknown dump by its own SHA-256 (with the
 * host's header description), an archive as an archive. */
#include "launcher_model.h"
#include "launcher_system.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static void require(int ok, const char* label) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", label); exit(1); }
}

static void require_has(const char* text, const char* part, const char* label) {
    if (!strstr(text, part)) {
        fprintf(stderr, "FAIL: %s\n  missing: %s\n  in: %s\n", label, part, text);
        exit(1);
    }
}

static void write_bytes(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    require(f != NULL, "create fixture");
    require(fwrite(data, 1, n, f) == n, "write fixture");
    require(fclose(f) == 0, "close fixture");
}

static void sha256_hex(const void* data, size_t n, char out[65]) {
    uint8_t d[32];
    recompui_sha256_compute((const uint8_t*)data, n, d);
    for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", d[i]);
}

static int describe_calls;
static int describe(void* ctx, const uint8_t* image, size_t len,
                    char* out, size_t cap) {
    (void)ctx;
    ++describe_calls;
    snprintf(out, cap, "Header says \"%.*s\".", (int)(len < 5 ? len : 5),
             (const char*)image);
    return 1;
}

int main(int argc, char** argv) {
    require(argc == 2, "test directory argument");
    char good[1024], headered[1024], old_rev[1024], unknown[1024],
         zip[1024], missing[1024], empty[1024];
    snprintf(good, sizeof(good), "%s/good.sfc", argv[1]);
    snprintf(headered, sizeof(headered), "%s/headered.smc", argv[1]);
    snprintf(old_rev, sizeof(old_rev), "%s/old.sfc", argv[1]);
    snprintf(unknown, sizeof(unknown), "%s/unknown.sfc", argv[1]);
    snprintf(zip, sizeof(zip), "%s/rom.zip", argv[1]);
    snprintf(missing, sizeof(missing), "%s/does-not-exist.sfc", argv[1]);
    snprintf(empty, sizeof(empty), "%s/empty.sfc", argv[1]);

    /* 1024-byte bodies so a 512-byte copier header is detected by size. */
    static uint8_t body_good[1024], body_old[1024], body_unknown[1024];
    static uint8_t with_header[1536];
    memset(body_good, 0x11, sizeof(body_good));
    memcpy(body_good, "GOOD!", 5);
    memset(body_old, 0x22, sizeof(body_old));
    memcpy(body_old, "OLDV1", 5);
    memset(body_unknown, 0x33, sizeof(body_unknown));
    memcpy(body_unknown, "HACKD", 5);
    memset(with_header, 0, 512);
    memcpy(with_header + 512, body_good, sizeof(body_good));
    const uint8_t zip_bytes[] = {'P', 'K', 3, 4, 20, 0, 0, 0, 8, 0};
    write_bytes(good, body_good, sizeof(body_good));
    write_bytes(headered, with_header, sizeof(with_header));
    write_bytes(old_rev, body_old, sizeof(body_old));
    write_bytes(unknown, body_unknown, sizeof(body_unknown));
    write_bytes(zip, zip_bytes, sizeof(zip_bytes));
    write_bytes(empty, "", 0);
    remove(missing);

    char good_sha[65], old_sha[65], unknown_sha[65];
    sha256_hex(body_good, sizeof(body_good), good_sha);
    sha256_hex(body_old, sizeof(body_old), old_sha);
    sha256_hex(body_unknown, sizeof(body_unknown), unknown_sha);
    /* Uppercase catalog digests must still match. */
    char old_sha_upper[65];
    for (int i = 0; i < 65; ++i)
        old_sha_upper[i] = (old_sha[i] >= 'a' && old_sha[i] <= 'f')
                               ? (char)(old_sha[i] - 32) : old_sha[i];

    const RecompLauncherCRomRevision revisions[] = {
        {"Test Game (USA) (Rev 1)", good_sha, NULL, NULL, 1, NULL},
        {"Test Game (USA) (Rev 0)", old_sha_upper, NULL, NULL, 0,
         "Rev 0 crashes on stage 3."},
        /* Listed after the supported entry with the same digest: the
         * supported one must win. */
        {"Duplicate unsupported label", good_sha, NULL, NULL, 0, NULL},
    };

    RecompLauncherCSettings settings = {0};
    RecompLauncherCGameInfo game = {0};
    game.name = "Test Game";
    game.rom_revisions = revisions;
    game.num_rom_revisions = 3;
    game.rom_help = "Headered and headerless dumps both work.";
    game.rom_describe = describe;
    LauncherModel* m = calloc(1, sizeof(*m));
    require(m != NULL, "allocate model");
    launcher_model_init(m, &settings, &game, "");
    char headline[64], report[2048];

    /* Supported revision verifies with no legacy fingerprint at all. */
    launcher_model_set_rom(m, good);
    require(m->rom_verdict == LNG_ROM_VERIFIED, "supported revision verifies");
    require(launcher_model_rom_verified(m), "verified accessor");
    require(launcher_model_can_launch(m), "verified ROM launches");
    require(strcmp(m->rom_identified, "Test Game (USA) (Rev 1)") == 0,
            "supported entry outranks a duplicate unsupported entry");
    require(m->rom_detail[0] == '\0', "verified ROM has no complaint");
    launcher_model_rom_headline(m, headline, sizeof(headline));
    require(strcmp(headline, "ROM verified") == 0, "verified headline");

    /* A copier header is stripped before fingerprinting. */
    launcher_model_set_rom(m, headered);
    require(m->rom_verdict == LNG_ROM_VERIFIED, "headered dump verifies");
    require(strcmp(m->rom_sha256_hex, good_sha) == 0, "digest covers the body");

    /* Known but unsupported revision: named, with the requirement. */
    launcher_model_set_rom(m, old_rev);
    require(m->rom_verdict == LNG_ROM_WRONG, "old revision is wrong, not unknown");
    require(!launcher_model_can_launch(m), "wrong revision cannot launch");
    require_has(m->rom_detail, "This is Test Game (USA) (Rev 0).", "names the revision");
    require_has(m->rom_detail, "Test Game needs Test Game (USA) (Rev 1)", "names the requirement");
    require_has(m->rom_detail, good_sha, "gives the required SHA-256");
    require_has(m->rom_detail, "Rev 0 crashes on stage 3.", "carries the note");
    require_has(m->rom_detail, "Headered and headerless", "carries rom_help");
    launcher_model_rom_headline(m, headline, sizeof(headline));
    require(strcmp(headline, "Wrong ROM") == 0, "wrong headline");
    launcher_model_rom_report(m, report, sizeof(report));
    require_has(report, old_sha, "report gives the file's SHA-256");
    require_has(report, "Identified as: Test Game (USA) (Rev 0)", "report names it");
    require_has(report, "Required: Test Game (USA) (Rev 1)", "report gives the requirement");

    /* Unknown dump: host description, its own digest, the requirement. */
    describe_calls = 0;
    launcher_model_set_rom(m, unknown);
    require(m->rom_verdict == LNG_ROM_UNRECOGNIZED, "unknown dump");
    require(describe_calls == 1, "host describes an unknown dump");
    require_has(m->rom_detail, "This file matches no known dump. Header says \"HACKD\".",
                "host description");
    launcher_model_rom_report(m, report, sizeof(report));
    require_has(report, unknown_sha, "report gives the file's SHA-256");
    require_has(m->rom_detail, good_sha, "gives the required SHA-256");
    require(!launcher_model_can_launch(m), "unknown dump cannot launch");

    /* Archive. */
    describe_calls = 0;
    launcher_model_set_rom(m, zip);
    require(m->rom_verdict == LNG_ROM_ARCHIVE, "zip is an archive");
    require(describe_calls == 0, "archives are not described as ROMs");
    require_has(m->rom_detail, "ZIP archive", "names the archive kind");
    launcher_model_rom_headline(m, headline, sizeof(headline));
    require(strcmp(headline, "Archive, not a ROM") == 0, "archive headline");

    /* A verified ROM that happens to start with archive magic stays verified. */
    {
        static uint8_t gz_body[1024];
        char gz[1024], gz_sha[65];
        memset(gz_body, 0x66, sizeof(gz_body));
        gz_body[0] = 0x1F; gz_body[1] = 0x8B;
        snprintf(gz, sizeof(gz), "%s/gzip-magic.sfc", argv[1]);
        write_bytes(gz, gz_body, sizeof(gz_body));
        sha256_hex(gz_body, sizeof(gz_body), gz_sha);
        const RecompLauncherCRomRevision gz_rev[] = {
            {"Magic Game", gz_sha, NULL, NULL, 1, NULL},
        };
        RecompLauncherCGameInfo gz_game = {0};
        gz_game.name = "Magic Game";
        gz_game.rom_revisions = gz_rev;
        gz_game.num_rom_revisions = 1;
        LauncherModel* g = calloc(1, sizeof(*g));
        require(g != NULL, "allocate model");
        launcher_model_init(g, &settings, &gz_game, "");
        launcher_model_set_rom(g, gz);
        require(g->rom_verdict == LNG_ROM_VERIFIED, "digest wins over archive magic");
        free(g);
    }

    /* Unreadable. */
    launcher_model_set_rom(m, missing);
    require(m->rom_verdict == LNG_ROM_UNREADABLE, "missing file is unreadable");
    require_has(m->rom_detail, "could not be opened", "missing file explained");
    launcher_model_set_rom(m, empty);
    require(m->rom_verdict == LNG_ROM_UNREADABLE, "empty file is unreadable");
    require_has(m->rom_detail, "empty", "empty file explained");

    /* Legacy fingerprint only: the requirement still names the digest. */
    {
        static const uint8_t want[1][32] = {{0}};
        RecompLauncherCGameInfo legacy = {0};
        legacy.name = "Legacy Game";
        legacy.known_sha256 = want;
        legacy.num_known_sha256 = 1;
        launcher_model_init(m, &settings, &legacy, "");
        launcher_model_set_rom(m, unknown);
        require(m->rom_verdict == LNG_ROM_UNRECOGNIZED, "legacy mismatch");
        require_has(m->rom_detail,
                    "the ROM it was built from (SHA-256 "
                    "0000000000000000000000000000000000000000000000000000000000000000)",
                    "legacy requirement digest");
    }

    /* No fingerprint: readable ROMs launch unverified. */
    {
        RecompLauncherCGameInfo open_game = {0};
        open_game.name = "Open Game";
        launcher_model_init(m, &settings, &open_game, "");
        launcher_model_set_rom(m, unknown);
        require(m->rom_verdict == LNG_ROM_UNCHECKED, "no fingerprint: unchecked");
        require(m->rom_detail[0] == '\0', "unchecked has no complaint");
        require(launcher_model_can_launch(m), "unchecked ROM launches");
    }

    free(m);
    puts("launcher rom identity: ok");
    return 0;
}
