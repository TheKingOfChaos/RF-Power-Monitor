// SHA-512 (FIPS 180-4) for the slot signature check, written for the
// Cortex-M0+: the boot program hashes the whole image on every start, and
// Monocypher's portable SHA-512 took ~2.5 us per byte there (1.07 s for a
// 425 KB image). Same result as crypto_sha512; eight rounds are unrolled so
// the working variables are not moved each round, the sigma functions work on
// 32-bit halves, and whole blocks are read as words straight from (XIP) flash.
// Freestanding safe: no heap, no exceptions.
#pragma once
#include <stddef.h>
#include <stdint.h>

/// Running SHA-512 state (~216 bytes)
struct FwSha512 {
    uint64_t h[8];
    uint8_t block[128];  // bytes of the current, not yet full block
    size_t blockLen;
    uint64_t total;  // message bytes so far (images are far below 2^61)
};

/// Start a new hash
void fwSha512Init(FwSha512& ctx);
/// Add `len` message bytes; `data` may be unaligned and may be nullptr when len is 0
void fwSha512Update(FwSha512& ctx, const uint8_t* data, size_t len);
/// Finish: write the 64-byte digest. Start again with fwSha512Init
void fwSha512Final(FwSha512& ctx, uint8_t out[64]);
