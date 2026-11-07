// On-device tests for lib/fw_image fw_sha512: the boot program's SHA-512, as
// the M0+ build runs it, against Monocypher over flash, and its speed.
// Prints the time per byte and the fixed Ed25519 check cost (docs/TESTING.md).
// Run: pio test -e pico -f embedded/test_fw_sha512

#include <Arduino.h>
#include <unity.h>
#include <fw_sha512.h>
#include <monocypher-ed25519.h>
#include <monocypher.h>

static const uint8_t* const FLASH_XIP = reinterpret_cast<const uint8_t*>(0x10000000u);
static const size_t SPEED_BYTES = 256 * 1024;  // the firmware's own flash: real code, XIP reads

void setUp() {}
void tearDown() {}

static void test_sha512_matches_monocypher_over_flash() {
    // The C code is host tested; this is the same code as the M0+ compiler built it
    static const size_t LENGTHS[] = {0, 111, 112, 128, 4097, 65536};
    for (size_t len : LENGTHS) {
        uint8_t want[64], got[64];
        crypto_sha512(want, FLASH_XIP + 3, len);  // unaligned start: the byte path
        FwSha512 ctx;
        fwSha512Init(ctx);
        fwSha512Update(ctx, FLASH_XIP + 3, len);
        fwSha512Final(ctx, got);
        TEST_ASSERT_EQUAL_MEMORY(want, got, 64);
        crypto_sha512(want, FLASH_XIP + 256, len);  // aligned: whole blocks as words
        fwSha512Init(ctx);
        fwSha512Update(ctx, FLASH_XIP + 192, 64);  // R || A, then the image from an aligned address
        fwSha512Update(ctx, FLASH_XIP + 256, len);
        fwSha512Final(ctx, got);
        uint8_t want2[64];
        crypto_sha512_ctx m;
        crypto_sha512_init(&m);
        crypto_sha512_update(&m, FLASH_XIP + 192, 64);
        crypto_sha512_update(&m, FLASH_XIP + 256, len);
        crypto_sha512_final(&m, want2);
        TEST_ASSERT_EQUAL_MEMORY(want2, got, 64);
    }
}

static void test_sha512_is_faster_than_monocypher() {
    uint8_t d[64];
    uint32_t t0 = micros();
    crypto_sha512(d, FLASH_XIP, SPEED_BYTES);
    uint32_t mono = micros() - t0;
    t0 = micros();
    FwSha512 ctx;
    fwSha512Init(ctx);
    fwSha512Update(ctx, FLASH_XIP, SPEED_BYTES);
    fwSha512Final(ctx, d);
    uint32_t ours = micros() - t0;
    char msg[96];
    snprintf(msg, sizeof(msg), "SHA-512 over %u KB: Monocypher %lu us (%lu ns/byte), fw_sha512 %lu us (%lu ns/byte)",
             (unsigned)(SPEED_BYTES / 1024), (unsigned long)mono, (unsigned long)(mono * 1000ull / SPEED_BYTES),
             (unsigned long)ours, (unsigned long)(ours * 1000ull / SPEED_BYTES));
    TEST_MESSAGE(msg);
    TEST_ASSERT_LESS_THAN_UINT32(mono, ours);
}

static void test_ed25519_fixed_cost_is_reported() {
    // The part of the verification that does not grow with the image
    uint8_t seed[32], secret[64], pub[32], sig[64];
    memset(seed, 7, sizeof(seed));
    crypto_ed25519_key_pair(secret, pub, seed);
    crypto_ed25519_sign(sig, secret, FLASH_XIP, 64);
    uint8_t hash[64], hRam[32];
    crypto_sha512_ctx m;
    crypto_sha512_init(&m);
    crypto_sha512_update(&m, sig, 32);
    crypto_sha512_update(&m, pub, 32);
    crypto_sha512_update(&m, FLASH_XIP, 64);
    crypto_sha512_final(&m, hash);
    crypto_eddsa_reduce(hRam, hash);
    uint32_t t0 = micros();
    int r = crypto_eddsa_check_equation(sig, pub, hRam);
    uint32_t us = micros() - t0;
    char msg[64];
    snprintf(msg, sizeof(msg), "Ed25519 check equation: %lu us", (unsigned long)us);
    TEST_MESSAGE(msg);
    TEST_ASSERT_EQUAL(0, r);
}

void setup() {
    delay(2000);  // let the host open the serial port
    UNITY_BEGIN();
    RUN_TEST(test_sha512_matches_monocypher_over_flash);
    RUN_TEST(test_sha512_is_faster_than_monocypher);
    RUN_TEST(test_ed25519_fixed_cost_is_reported);
    UNITY_END();
}

void loop() {}
