// ips_patch.h — classic IPS patch format (PATCH...EOF, 24-bit offsets).
//
// Pure algorithm, no file I/O (mirrors the crc32.c/sha256.c "bundled engine
// helper" pattern) — ported from the legacy SNES launcher's ips_apply()
// (snesrecomp/runner/src/launcher/launcher_gui.cpp) so recomp-ui's MSU-1
// dashboard flow (launcher_model_apply_msu1_patch, launcher_model.c) can
// write a patched ROM from a vanilla ROM + a game-supplied .ips file.

#ifndef LAUNCHER_NG_IPS_PATCH_H
#define LAUNCHER_NG_IPS_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IpsPatchFormat {
    IPS_PATCH_UNKNOWN = 0,
    IPS_PATCH_CLASSIC,
    IPS_PATCH_IPS32,
    IPS_PATCH_BPS,
} IpsPatchFormat;

IpsPatchFormat ips_detect_format(const uint8_t* patch, size_t patch_len);

// Apply an IPS patch onto `src` (src_len bytes; may be NULL/0 for an empty
// source — a patch may still build a whole file from scratch via its
// records). On success, mallocs *out_data (*out_len bytes) — the CALLER
// frees it with free(). Returns false and leaves *out_data/*out_len
// untouched on a malformed patch (bad magic, truncated record, or overlong
// record that runs past the patch buffer).
//
// Supports both normal (literal-data) and RLE (run-length, len-field == 0)
// records. Classic IPS uses PATCH/EOF and 24-bit offsets; IPS32 uses
// IPS32/EEOF and 32-bit offsets, allowing changes beyond 16 MiB. A patch
// with no trailing end marker but otherwise well-formed
// records still succeeds (matches the reference implementation).
bool ips_apply(const uint8_t* src, size_t src_len,
               const uint8_t* patch, size_t patch_len,
               uint8_t** out_data, size_t* out_len);

// Apply a BPS1 patch and verify all three checksums embedded in it. A source
// size/checksum mismatch, malformed command stream, or target/patch checksum
// mismatch fails without publishing an output buffer.
bool bps_apply(const uint8_t* src, size_t src_len,
               const uint8_t* patch, size_t patch_len,
               uint8_t** out_data, size_t* out_len);

// Auto-detect and apply classic IPS, IPS32, or BPS1.
bool rom_patch_apply(const uint8_t* src, size_t src_len,
                     const uint8_t* patch, size_t patch_len,
                     uint8_t** out_data, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_IPS_PATCH_H
