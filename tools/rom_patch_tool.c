#include "crc32.h"
#include "ips_patch.h"
#include "sha1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char* path, uint8_t** data, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    const long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    uint8_t* bytes = (uint8_t*)malloc((size_t)end ? (size_t)end : 1);
    if (!bytes) { fclose(file); return 0; }
    const int ok = fread(bytes, 1, (size_t)end, file) == (size_t)end;
    fclose(file);
    if (!ok) { free(bytes); return 0; }
    *data = bytes;
    *size = (size_t)end;
    return 1;
}

static const char* format_name(IpsPatchFormat format) {
    switch (format) {
        case IPS_PATCH_CLASSIC: return "IPS";
        case IPS_PATCH_IPS32: return "IPS32";
        case IPS_PATCH_BPS: return "BPS";
        default: return "unknown";
    }
}

static int byte_changed(const uint8_t* source, size_t source_size,
                        const uint8_t* target, size_t target_size,
                        size_t offset) {
    if (offset >= source_size || offset >= target_size)
        return offset < source_size || offset < target_size;
    return source[offset] != target[offset];
}

static void print_change_summary(const uint8_t* source, size_t source_size,
                                 const uint8_t* target, size_t target_size) {
    const size_t span = source_size > target_size ? source_size : target_size;
    size_t changed = 0, runs = 0, first = 0, last = 0;
    int in_run = 0;
    for (size_t i = 0; i < span; ++i) {
        const int different = byte_changed(source, source_size,
                                           target, target_size, i);
        if (different) {
            if (!changed) first = i;
            last = i;
            ++changed;
            if (!in_run) { ++runs; in_run = 1; }
        } else {
            in_run = 0;
        }
    }
    printf("Changed bytes: %zu in %zu contiguous runs\n", changed, runs);
    if (changed)
        printf("Changed range: 0x%zX-0x%zX\n", first, last);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <source-rom> <patch> <output-rom>\n",
                argc > 0 ? argv[0] : "rom-patch-tool");
        return 2;
    }

    uint8_t* source = NULL;
    uint8_t* patch = NULL;
    uint8_t* output = NULL;
    size_t source_size = 0, patch_size = 0, output_size = 0;
    if (!read_file(argv[1], &source, &source_size)) {
        fprintf(stderr, "Could not read source ROM: %s\n", argv[1]);
        return 1;
    }
    if (!read_file(argv[2], &patch, &patch_size)) {
        fprintf(stderr, "Could not read patch: %s\n", argv[2]);
        free(source);
        return 1;
    }

    const IpsPatchFormat format = ips_detect_format(patch, patch_size);
    printf("Format: %s\n", format_name(format));
    if (!rom_patch_apply(source, source_size, patch, patch_size,
                         &output, &output_size)) {
        fprintf(stderr, "Patch rejected (wrong source, invalid data, or checksum failure).\n");
        free(source);
        free(patch);
        return 1;
    }

    FILE* target = fopen(argv[3], "wb");
    const int wrote = target &&
        fwrite(output, 1, output_size, target) == output_size;
    if (target) fclose(target);
    if (!wrote) {
        fprintf(stderr, "Could not write output ROM: %s\n", argv[3]);
        free(source); free(patch); free(output);
        return 1;
    }

    uint8_t digest[20];
    char sha1[41];
    recompui_sha1_compute(output, output_size, digest);
    recompui_sha1_hex(digest, sha1);
    printf("Source size: %zu bytes\n", source_size);
    printf("Target size: %zu bytes\n", output_size);
    printf("Target CRC32: %08X\n", recompui_crc32_compute(output, output_size));
    printf("Target SHA1: %s\n", sha1);
    print_change_summary(source, source_size, output, output_size);

    free(source);
    free(patch);
    free(output);
    return 0;
}
