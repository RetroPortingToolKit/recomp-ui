#include "ips_patch.h"
#include "crc32.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static void write_le32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static int test_bps_checksums_and_actions(void) {
    const uint8_t source[] = {'a','b','c','d','e'};
    const uint8_t expected[] = {'a','b','X','Y','e'};
    uint8_t patch[24] = {
        'B','P','S','1',
        0x85, 0x85, 0x80,
        0x84,
        0x85, 'X', 'Y',
        0x80,
    };
    write_le32(patch + 12, recompui_crc32_compute(source, sizeof(source)));
    write_le32(patch + 16, recompui_crc32_compute(expected, sizeof(expected)));
    write_le32(patch + 20, recompui_crc32_compute(patch, 20));

    uint8_t* output = NULL;
    size_t output_len = 0;
    const int ok = rom_patch_apply(source, sizeof(source), patch, sizeof(patch),
                                   &output, &output_len);
    const int pass =
        expect(ips_detect_format(patch, sizeof(patch)) == IPS_PATCH_BPS,
               "BPS format detection") &&
        expect(ok, "BPS applies with valid checksums") &&
        expect(output_len == sizeof(expected), "BPS target length") &&
        expect(memcmp(output, expected, sizeof(expected)) == 0,
               "BPS SourceRead and TargetRead bytes");
    free(output);

    const uint8_t wrong_source[] = {'a','b','c','d','f'};
    output = NULL;
    output_len = 0;
    return pass && expect(!rom_patch_apply(
        wrong_source, sizeof(wrong_source), patch, sizeof(patch),
        &output, &output_len), "BPS rejects the wrong source ROM");
}
static int test_bps_relative_copy_actions(void) {
    const uint8_t source[] = {'a','b','c','d','e','f'};
    const uint8_t expected[] = {'c','d','c','d','c','d'};
    uint8_t patch[23] = {
        'B','P','S','1',
        0x86, 0x86, 0x80,
        0x86, 0x84,
        0x8F, 0x80,
    };
    write_le32(patch + 11, recompui_crc32_compute(source, sizeof(source)));
    write_le32(patch + 15, recompui_crc32_compute(expected, sizeof(expected)));
    write_le32(patch + 19, recompui_crc32_compute(patch, 19));

    uint8_t* output = NULL;
    size_t output_len = 0;
    const int ok = rom_patch_apply(source, sizeof(source), patch, sizeof(patch),
                                   &output, &output_len);
    const int pass =
        expect(ok, "BPS relative-copy patch applies") &&
        expect(output_len == sizeof(expected), "BPS relative-copy length") &&
        expect(memcmp(output, expected, sizeof(expected)) == 0,
               "BPS SourceCopy and overlapping TargetCopy bytes");
    free(output);

    patch[19] ^= 1u;
    output = NULL;
    output_len = 0;
    return pass && expect(!rom_patch_apply(
        source, sizeof(source), patch, sizeof(patch), &output, &output_len),
        "BPS rejects a corrupt patch checksum");
}


static int test_classic_literal_and_rle(void) {
    const uint8_t source[] = {0x10, 0x20, 0x30, 0x40};
    const uint8_t patch[] = {
        'P','A','T','C','H',
        0x00,0x00,0x01, 0x00,0x02, 0xAA,0xBB,
        0x00,0x00,0x04, 0x00,0x00, 0x00,0x03,0x7F,
        'E','O','F'
    };
    const uint8_t expected[] = {0x10,0xAA,0xBB,0x40,0x7F,0x7F,0x7F};
    uint8_t* output = NULL;
    size_t output_len = 0;
    const int ok = rom_patch_apply(source, sizeof(source), patch, sizeof(patch),
                                   &output, &output_len);
    const int pass =
        expect(ips_detect_format(patch, sizeof(patch)) == IPS_PATCH_CLASSIC,
               "classic IPS format detection") &&
        expect(ok, "classic IPS applies") &&
        expect(output_len == sizeof(expected), "classic IPS output length") &&
        expect(memcmp(output, expected, sizeof(expected)) == 0,
               "classic IPS literal and RLE bytes");
    free(output);
    return pass;
}

static int test_ips32_high_offset(void) {
    const uint8_t patch[] = {
        'I','P','S','3','2',
        0x01,0x00,0x00,0x10, 0x00,0x02, 0x42,0x43,
        'E','E','O','F'
    };
    uint8_t* output = NULL;
    size_t output_len = 0;
    const int ok = rom_patch_apply(NULL, 0, patch, sizeof(patch),
                                   &output, &output_len);
    const int pass =
        expect(ips_detect_format(patch, sizeof(patch)) == IPS_PATCH_IPS32,
               "IPS32 format detection") &&
        expect(ok, "IPS32 applies") &&
        expect(output_len == 0x01000012u, "IPS32 output extends above 16 MiB") &&
        expect(output[0x01000010u] == 0x42 && output[0x01000011u] == 0x43,
               "IPS32 writes bytes above 16 MiB");
    free(output);
    return pass;
}

static int test_rejects_truncated_record(void) {
    const uint8_t patch[] = {
        'P','A','T','C','H', 0x00,0x00,0x01, 0x00,0x02, 0xAA
    };
    uint8_t* output = NULL;
    size_t output_len = 0;
    return expect(!rom_patch_apply(NULL, 0, patch, sizeof(patch),
                                   &output, &output_len),
                  "truncated record is rejected");
}

int main(void) {
    if (!test_bps_checksums_and_actions()) return 1;
    if (!test_bps_relative_copy_actions()) return 1;
    if (!test_classic_literal_and_rle()) return 1;
    if (!test_ips32_high_offset()) return 1;
    if (!test_rejects_truncated_record()) return 1;
    puts("PASS: BPS, IPS, and IPS32 parser tests");
    return 0;
}
