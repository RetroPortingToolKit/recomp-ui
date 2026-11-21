// ips_patch.c — classic IPS patch format implementation. See ips_patch.h.

#include "ips_patch.h"
#include "crc32.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Grow `*buf` (currently `*cap` bytes) so it holds at least `need` bytes,
// doubling capacity as needed (patches are small; ROMs are a few MB at most,
// so this never loops more than ~20 times). Returns false on OOM, in which
// case `*buf`/`*cap` are left as they were (still valid, still owned).
static bool ips_grow(uint8_t** buf, size_t* cap, size_t need) {
    if (need <= *cap) return true;
    size_t ncap = *cap ? *cap : 4096;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    uint8_t* nb = (uint8_t*)realloc(*buf, ncap);
    if (!nb) return false;
    *buf = nb;
    *cap = ncap;
    return true;
}

IpsPatchFormat ips_detect_format(const uint8_t* patch, size_t patch_len) {
    if (!patch || patch_len < 4) return IPS_PATCH_UNKNOWN;
    if (patch_len >= 5 && memcmp(patch, "PATCH", 5) == 0) return IPS_PATCH_CLASSIC;
    if (patch_len >= 5 && memcmp(patch, "IPS32", 5) == 0) return IPS_PATCH_IPS32;
    if (memcmp(patch, "BPS1", 4) == 0) return IPS_PATCH_BPS;
    return IPS_PATCH_UNKNOWN;
}

static size_t ips_read_be(const uint8_t* p, size_t width) {
    size_t value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | p[i];
    return value;
}

bool ips_apply(const uint8_t* src, size_t src_len,
               const uint8_t* patch, size_t patch_len,
               uint8_t** out_data, size_t* out_len) {
    if (!out_data || !out_len || (src_len && !src)) return false;
    const IpsPatchFormat format = ips_detect_format(patch, patch_len);
    if (format != IPS_PATCH_CLASSIC && format != IPS_PATCH_IPS32)
        return false;
    const size_t offset_width = format == IPS_PATCH_IPS32 ? 4 : 3;
    const size_t footer_width = format == IPS_PATCH_IPS32 ? 4 : 3;
    const char* footer = format == IPS_PATCH_IPS32 ? "EEOF" : "EOF";
    if (patch_len < 5 + footer_width) return false;

    size_t cap = src_len ? src_len : 4096;
    uint8_t* out = (uint8_t*)malloc(cap);
    if (!out) return false;
    size_t len = src_len;
    if (src && src_len) memcpy(out, src, src_len);

    size_t i = 5;
    while (i + footer_width <= patch_len) {
        if (memcmp(patch + i, footer, footer_width) == 0) {
            i += footer_width;
            *out_data = out;
            *out_len  = len;
            return true;
        }

        if (i + offset_width > patch_len) { free(out); return false; }
        const size_t off = ips_read_be(patch + i, offset_width);
        i += offset_width;

        if (i + 2 > patch_len) { free(out); return false; }
        // 16-bit big-endian length. len==0 introduces an RLE record instead.
        size_t rec_len = ((size_t)patch[i] << 8) | patch[i + 1];
        i += 2;

        if (rec_len == 0) {
            if (i + 3 > patch_len) { free(out); return false; }
            size_t run = ((size_t)patch[i] << 8) | patch[i + 1];
            i += 2;
            uint8_t val = patch[i++];
            if (run > SIZE_MAX - off) { free(out); return false; }
            size_t need = off + run;
            if (!ips_grow(&out, &cap, need)) { free(out); return false; }
            if (need > len) { memset(out + len, 0, need - len); len = need; }
            memset(out + off, val, run);
        } else {
            if (i + rec_len > patch_len) { free(out); return false; }
            if (rec_len > SIZE_MAX - off) { free(out); return false; }
            size_t need = off + rec_len;
            if (!ips_grow(&out, &cap, need)) { free(out); return false; }
            if (need > len) { memset(out + len, 0, need - len); len = need; }
            memcpy(out + off, patch + i, rec_len);
            i += rec_len;
        }
    }

    // No EOF marker, but every record parsed cleanly — matches the reference
    // launcher_gui.cpp behavior (still a successful patch).
    *out_data = out;
    *out_len  = len;
    return true;
}

static uint32_t bps_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool bps_read_number(const uint8_t* patch, size_t limit,
                            size_t* cursor, uint64_t* out) {
    uint64_t value = 0;
    uint64_t shift = 1;
    for (;;) {
        if (*cursor >= limit) return false;
        const uint8_t x = patch[(*cursor)++];
        const uint64_t digit = x & 0x7fu;
        if (digit && shift > (UINT64_MAX - value) / digit) return false;
        value += digit * shift;
        if (x & 0x80u) {
            *out = value;
            return true;
        }
        if (shift > UINT64_MAX / 128u) return false;
        shift <<= 7;
        if (value > UINT64_MAX - shift) return false;
        value += shift;
    }
}

static bool bps_move_relative(int64_t* relative, uint64_t encoded) {
    const uint64_t magnitude = encoded >> 1;
    if (magnitude > (uint64_t)INT64_MAX) return false;
    const int64_t amount = (int64_t)magnitude;
    if (encoded & 1u) {
        if (*relative < INT64_MIN + amount) return false;
        *relative -= amount;
    } else {
        if (*relative > INT64_MAX - amount) return false;
        *relative += amount;
    }
    return true;
}

bool bps_apply(const uint8_t* src, size_t src_len,
               const uint8_t* patch, size_t patch_len,
               uint8_t** out_data, size_t* out_len) {
    if (!out_data || !out_len || (src_len && !src) || !patch ||
        patch_len < 4 + 3 + 12 || memcmp(patch, "BPS1", 4) != 0)
        return false;

    const size_t command_end = patch_len - 12;
    const uint32_t expected_source_crc = bps_read_le32(patch + command_end);
    const uint32_t expected_target_crc = bps_read_le32(patch + command_end + 4);
    const uint32_t expected_patch_crc = bps_read_le32(patch + command_end + 8);
    if (recompui_crc32_compute(patch, patch_len - 4) != expected_patch_crc ||
        recompui_crc32_compute(src, src_len) != expected_source_crc)
        return false;

    size_t cursor = 4;
    uint64_t source_size = 0, target_size = 0, metadata_size = 0;
    if (!bps_read_number(patch, command_end, &cursor, &source_size) ||
        !bps_read_number(patch, command_end, &cursor, &target_size) ||
        !bps_read_number(patch, command_end, &cursor, &metadata_size) ||
        source_size != src_len || target_size > SIZE_MAX ||
        metadata_size > command_end - cursor)
        return false;
    cursor += (size_t)metadata_size;

    const size_t target_len = (size_t)target_size;
    uint8_t* target = (uint8_t*)malloc(target_len ? target_len : 1);
    if (!target) return false;

    size_t output = 0;
    int64_t source_relative = 0;
    int64_t target_relative = 0;
    while (output < target_len) {
        uint64_t command = 0;
        if (!bps_read_number(patch, command_end, &cursor, &command)) {
            free(target);
            return false;
        }
        const unsigned action = (unsigned)(command & 3u);
        const uint64_t encoded_length = (command >> 2) + 1;
        if (encoded_length > target_len - output) {
            free(target);
            return false;
        }
        const size_t length = (size_t)encoded_length;

        if (action == 0) {
            if (output > src_len || length > src_len - output) {
                free(target);
                return false;
            }
            memcpy(target + output, src + output, length);
            output += length;
        } else if (action == 1) {
            if (cursor > command_end || length > command_end - cursor) {
                free(target);
                return false;
            }
            memcpy(target + output, patch + cursor, length);
            cursor += length;
            output += length;
        } else {
            uint64_t delta = 0;
            int64_t* relative = action == 2 ? &source_relative
                                            : &target_relative;
            if (!bps_read_number(patch, command_end, &cursor, &delta) ||
                !bps_move_relative(relative, delta)) {
                free(target);
                return false;
            }
            for (size_t i = 0; i < length; ++i) {
                if (*relative < 0) {
                    free(target);
                    return false;
                }
                const uint64_t index = (uint64_t)*relative;
                if ((action == 2 && index >= src_len) ||
                    (action == 3 && index >= output)) {
                    free(target);
                    return false;
                }
                target[output++] = action == 2 ? src[(size_t)index]
                                               : target[(size_t)index];
                ++*relative;
            }
        }
    }

    if (cursor != command_end ||
        recompui_crc32_compute(target, target_len) != expected_target_crc) {
        free(target);
        return false;
    }
    *out_data = target;
    *out_len = target_len;
    return true;
}

bool rom_patch_apply(const uint8_t* src, size_t src_len,
                     const uint8_t* patch, size_t patch_len,
                     uint8_t** out_data, size_t* out_len) {
    const IpsPatchFormat format = ips_detect_format(patch, patch_len);
    if (format == IPS_PATCH_BPS)
        return bps_apply(src, src_len, patch, patch_len, out_data, out_len);
    return ips_apply(src, src_len, patch, patch_len, out_data, out_len);
}
