#include "fw_sha512.h"

#include <string.h>

namespace {

const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

const uint64_t H0[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

// The sigma functions on 32-bit halves: a 64-bit rotate by n < 32 is
// hi' = hi >> n | lo << (32 - n), lo' = lo >> n | hi << (32 - n), and by
// n >= 32 the same with the halves swapped and n - 32. The M0+ has no 64-bit
// shifts, so this is what the compiler would have to build anyway, minus the
// generic shift code it emits for u64.

inline uint32_t hi32(uint64_t x) { return (uint32_t)(x >> 32); }
inline uint32_t lo32(uint64_t x) { return (uint32_t)x; }
inline uint64_t join(uint32_t hi, uint32_t lo) { return ((uint64_t)hi << 32) | lo; }

/// Σ1(e) = rotr 14 ^ rotr 18 ^ rotr 41
inline uint64_t bigSigma1(uint64_t x) {
    uint32_t h = hi32(x), l = lo32(x);
    return join((h >> 14 | l << 18) ^ (h >> 18 | l << 14) ^ (l >> 9 | h << 23),
                (l >> 14 | h << 18) ^ (l >> 18 | h << 14) ^ (h >> 9 | l << 23));
}

/// Σ0(a) = rotr 28 ^ rotr 34 ^ rotr 39
inline uint64_t bigSigma0(uint64_t x) {
    uint32_t h = hi32(x), l = lo32(x);
    return join((h >> 28 | l << 4) ^ (l >> 2 | h << 30) ^ (l >> 7 | h << 25),
                (l >> 28 | h << 4) ^ (h >> 2 | l << 30) ^ (h >> 7 | l << 25));
}

/// σ0(w) = rotr 1 ^ rotr 8 ^ shr 7
inline uint64_t smallSigma0(uint64_t x) {
    uint32_t h = hi32(x), l = lo32(x);
    return join((h >> 1 | l << 31) ^ (h >> 8 | l << 24) ^ (h >> 7),
                (l >> 1 | h << 31) ^ (l >> 8 | h << 24) ^ (l >> 7 | h << 25));
}

/// σ1(w) = rotr 19 ^ rotr 61 ^ shr 6
inline uint64_t smallSigma1(uint64_t x) {
    uint32_t h = hi32(x), l = lo32(x);
    return join((h >> 19 | l << 13) ^ (l >> 29 | h << 3) ^ (h >> 6),
                (l >> 19 | h << 13) ^ (h >> 29 | l << 3) ^ (l >> 6 | h << 26));
}

inline uint64_t ch(uint64_t e, uint64_t f, uint64_t g) { return g ^ (e & (f ^ g)); }
inline uint64_t maj(uint64_t a, uint64_t b, uint64_t c) { return (a & b) | (c & (a | b)); }

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/// Load one 128-byte block as 16 big-endian words. From a word-aligned
/// address (the slot image in flash) with word reads: a byte at a time costs
/// four XIP reads per word
void loadBlock(uint64_t w[16], const uint8_t* p) {
    if (((uintptr_t)p & 3) == 0) {
        const uint32_t* q = static_cast<const uint32_t*>(static_cast<const void*>(p));
        for (int i = 0; i < 16; i++) {
            w[i] = join(__builtin_bswap32(q[2 * i]), __builtin_bswap32(q[2 * i + 1]));
        }
    } else {
        for (int i = 0; i < 16; i++) w[i] = join(be32(p + 8 * i), be32(p + 8 * i + 4));
    }
}

// One round: a..h are the working variables in their roles for this round;
// instead of moving them, the next round names them in the next order
#define FW_SHA512_ROUND(a, b, c, d, e, f, g, h, k, wi)                \
    do {                                                              \
        uint64_t t1 = h + bigSigma1(e) + ch(e, f, g) + (k) + (wi);    \
        d += t1;                                                      \
        h = t1 + bigSigma0(a) + maj(a, b, c);                         \
    } while (0)

/// Message schedule: w[i & 15] becomes W[i] for i >= 16
#define FW_SHA512_W(i) \
    (w[(i) & 15] += smallSigma1(w[((i) - 2) & 15]) + w[((i) - 7) & 15] + smallSigma0(w[((i) - 15) & 15]))

/// Compress one block (w: its 16 words, overwritten by the schedule)
void compress(uint64_t state[8], uint64_t w[16]) {
    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 16; i += 8) {
        FW_SHA512_ROUND(a, b, c, d, e, f, g, h, K[i + 0], w[i + 0]);
        FW_SHA512_ROUND(h, a, b, c, d, e, f, g, K[i + 1], w[i + 1]);
        FW_SHA512_ROUND(g, h, a, b, c, d, e, f, K[i + 2], w[i + 2]);
        FW_SHA512_ROUND(f, g, h, a, b, c, d, e, K[i + 3], w[i + 3]);
        FW_SHA512_ROUND(e, f, g, h, a, b, c, d, K[i + 4], w[i + 4]);
        FW_SHA512_ROUND(d, e, f, g, h, a, b, c, K[i + 5], w[i + 5]);
        FW_SHA512_ROUND(c, d, e, f, g, h, a, b, K[i + 6], w[i + 6]);
        FW_SHA512_ROUND(b, c, d, e, f, g, h, a, K[i + 7], w[i + 7]);
    }
    for (int i = 16; i < 80; i += 8) {
        FW_SHA512_ROUND(a, b, c, d, e, f, g, h, K[i + 0], FW_SHA512_W(i + 0));
        FW_SHA512_ROUND(h, a, b, c, d, e, f, g, K[i + 1], FW_SHA512_W(i + 1));
        FW_SHA512_ROUND(g, h, a, b, c, d, e, f, K[i + 2], FW_SHA512_W(i + 2));
        FW_SHA512_ROUND(f, g, h, a, b, c, d, e, K[i + 3], FW_SHA512_W(i + 3));
        FW_SHA512_ROUND(e, f, g, h, a, b, c, d, K[i + 4], FW_SHA512_W(i + 4));
        FW_SHA512_ROUND(d, e, f, g, h, a, b, c, K[i + 5], FW_SHA512_W(i + 5));
        FW_SHA512_ROUND(c, d, e, f, g, h, a, b, K[i + 6], FW_SHA512_W(i + 6));
        FW_SHA512_ROUND(b, c, d, e, f, g, h, a, K[i + 7], FW_SHA512_W(i + 7));
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

#undef FW_SHA512_ROUND
#undef FW_SHA512_W

void compressBytes(uint64_t state[8], const uint8_t* block) {
    uint64_t w[16];
    loadBlock(w, block);
    compress(state, w);
}

}  // namespace

void fwSha512Init(FwSha512& ctx) {
    memcpy(ctx.h, H0, sizeof(ctx.h));
    ctx.blockLen = 0;
    ctx.total = 0;
}

void fwSha512Update(FwSha512& ctx, const uint8_t* data, size_t len) {
    if (len == 0) return;
    ctx.total += len;
    if (ctx.blockLen > 0) {
        size_t take = sizeof(ctx.block) - ctx.blockLen;
        if (take > len) take = len;
        memcpy(ctx.block + ctx.blockLen, data, take);
        ctx.blockLen += take;
        data += take;
        len -= take;
        if (ctx.blockLen < sizeof(ctx.block)) return;
        compressBytes(ctx.h, ctx.block);
        ctx.blockLen = 0;
    }
    for (; len >= sizeof(ctx.block); data += sizeof(ctx.block), len -= sizeof(ctx.block)) {
        compressBytes(ctx.h, data);  // whole blocks straight from the caller's memory
    }
    if (len > 0) {
        memcpy(ctx.block, data, len);
        ctx.blockLen = len;
    }
}

void fwSha512Final(FwSha512& ctx, uint8_t out[64]) {
    const uint64_t bits = ctx.total << 3;
    ctx.block[ctx.blockLen++] = 0x80;
    if (ctx.blockLen > sizeof(ctx.block) - 16) {
        memset(ctx.block + ctx.blockLen, 0, sizeof(ctx.block) - ctx.blockLen);
        compressBytes(ctx.h, ctx.block);
        ctx.blockLen = 0;
    }
    // 128-bit big-endian length; the upper 64 bits are 0 for any message this hashes
    memset(ctx.block + ctx.blockLen, 0, sizeof(ctx.block) - 8 - ctx.blockLen);
    for (int i = 0; i < 8; i++) ctx.block[sizeof(ctx.block) - 1 - i] = (uint8_t)(bits >> (8 * i));
    compressBytes(ctx.h, ctx.block);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) out[8 * i + j] = (uint8_t)(ctx.h[i] >> (56 - 8 * j));
    }
}
