// Host unit tests for firmware slot headers and Ed25519 verification (lib/fw_image).
// Run: pio test -e native -f native/test_fw_image

#include <stdio.h>
#include <string.h>

#include <vector>

#include <unity.h>
#include <fw_image.h>
#include <fw_sha512.h>
#include <monocypher-ed25519.h>

void setUp() {}
void tearDown() {}

namespace {

void putLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void makeKey(uint8_t seedByte, uint8_t secret[64], uint8_t pub[32]) {
    uint8_t seed[32];
    memset(seed, seedByte, sizeof(seed));
    crypto_ed25519_key_pair(secret, pub, seed);
}

/// Header fields without the signature, written into slot[64..104).
void writeHeader(std::vector<uint8_t>& slot, uint32_t slotNo, uint32_t length,
                 const char* version, const uint8_t pub[32]) {
    memset(slot.data(), 0xFF, slot.size());
    putLe32(&slot[64], FW_MAGIC);
    putLe32(&slot[68], FW_HEADER_VERSION);
    putLe32(&slot[72], slotNo);
    putLe32(&slot[76], length);
    memset(&slot[80], 0, 16);
    memcpy(&slot[80], version, strlen(version));
    fwKeyId(pub, &slot[96]);
}

/// A full slot image (FW_SLOT_SIZE bytes) signed with the given seed byte.
std::vector<uint8_t> buildSlot(uint8_t seedByte, uint32_t slotNo, uint32_t length) {
    uint8_t secret[64], pub[32];
    makeKey(seedByte, secret, pub);
    std::vector<uint8_t> slot(FW_SLOT_SIZE);
    writeHeader(slot, slotNo, length, "3.3.0", pub);
    for (uint32_t i = 0; i < length; i++) slot[FW_SLOT_HEADER_SIZE + i] = (uint8_t)(i * 7 + 3);
    crypto_ed25519_sign(&slot[0], secret, &slot[64], FW_SLOT_HEADER_SIZE - 64 + length);
    return slot;
}

void trustedKey(uint8_t pub[32]) {
    uint8_t secret[64];
    makeKey(1, secret, pub);
}

void hexToBytes(const char* hex, uint8_t* out) {
    auto nib = [](char c) { return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (size_t i = 0; hex[2 * i]; i++) out[i] = (uint8_t)((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
}

}  // namespace

static void test_ed25519_matches_rfc8032_test1() {
    uint8_t seed[32], expectPub[32], expectSig[64];
    hexToBytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    hexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", expectPub);
    hexToBytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
               "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
               expectSig);

    uint8_t secret[64], pub[32], sig[64];
    crypto_ed25519_key_pair(secret, pub, seed);
    TEST_ASSERT_EQUAL_MEMORY(expectPub, pub, 32);
    crypto_ed25519_sign(sig, secret, nullptr, 0);
    TEST_ASSERT_EQUAL_MEMORY(expectSig, sig, 64);
    TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_check(expectSig, expectPub, nullptr, 0));
}

static void test_verify_accepts_signed_slot() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 0, 1000);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Ok, (uint8_t)slotVerify(slot.data(), 0, pub, h));
    TEST_ASSERT_EQUAL_STRING("3.3.0", h.version);
    TEST_ASSERT_EQUAL_UINT32(1000, h.imageLength);
    TEST_ASSERT_EQUAL_UINT32(0, h.slot);
}

static void test_verify_rejects_flipped_image_byte() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 0, 1000);
    slot[256 + 500] ^= 0x01;
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadSignature, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_verify_rejects_flipped_header_byte() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 0, 1000);
    slot[80] ^= 0x01;  // "3.3.0" -> "2.3.0", still a valid version string
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadSignature, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_verify_rejects_flipped_signature_byte() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 0, 1000);
    slot[10] ^= 0x80;
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadSignature, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_verify_rejects_other_key() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(2, 0, 1000);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::WrongKey, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_verify_rejects_wrong_slot() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 1, 1000);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::WrongSlot, (uint8_t)slotVerify(slot.data(), 0, pub, h));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Ok, (uint8_t)slotVerify(slot.data(), 1, pub, h));
}

static void test_verify_rejects_length_over_slot() {
    uint8_t pub[32];
    trustedKey(pub);
    std::vector<uint8_t> slot(FW_SLOT_SIZE);
    writeHeader(slot, 0, FW_IMAGE_MAX + 1, "3.3.0", pub);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::TooLong, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_verify_accepts_length_exactly_max() {
    uint8_t pub[32];
    trustedKey(pub);
    auto slot = buildSlot(1, 0, FW_IMAGE_MAX);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Ok, (uint8_t)slotVerify(slot.data(), 0, pub, h));
    TEST_ASSERT_EQUAL_UINT32(FW_IMAGE_MAX, h.imageLength);
}

static void test_verify_reports_erased_slot_as_empty() {
    uint8_t pub[32];
    trustedKey(pub);
    std::vector<uint8_t> slot(FW_SLOT_SIZE, 0xFF);
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Empty, (uint8_t)slotVerify(slot.data(), 0, pub, h));
}

static void test_parse_accepts_valid_header_and_fills_fields() {
    uint8_t pub[32], id[8];
    trustedKey(pub);
    fwKeyId(pub, id);
    auto slot = buildSlot(1, 0, 1000);
    SlotHeader h;
    memset(&h, 0xAA, sizeof(h));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Ok, (uint8_t)slotHeaderParse(slot.data(), 256, 0, id, h));
    TEST_ASSERT_EQUAL_STRING("3.3.0", h.version);
    TEST_ASSERT_EQUAL_MEMORY(id, h.keyId, 8);
}

static void test_parse_rejects_bad_magic() {
    uint8_t pub[32], id[8];
    trustedKey(pub);
    fwKeyId(pub, id);
    auto slot = buildSlot(1, 0, 1000);
    slot[64] ^= 0x01;
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadMagic, (uint8_t)slotHeaderParse(slot.data(), 256, 0, id, h));
}

static void test_parse_rejects_other_header_version() {
    uint8_t pub[32], id[8];
    trustedKey(pub);
    fwKeyId(pub, id);
    auto slot = buildSlot(1, 0, 1000);
    slot[68] = 2;  // header version 2: a layout this code does not know
    SlotHeader h;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadMagic, (uint8_t)slotHeaderParse(slot.data(), 256, 0, id, h));
    slot[68] = 0;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadMagic, (uint8_t)slotHeaderParse(slot.data(), 256, 0, id, h));
}

static void test_parse_rejects_unterminated_or_nonprintable_version() {
    uint8_t pub[32], id[8];
    trustedKey(pub);
    fwKeyId(pub, id);
    SlotHeader h;

    auto unterminated = buildSlot(1, 0, 1000);
    memset(&unterminated[80], 'x', 16);  // no NUL in the field
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadVersion,
                            (uint8_t)slotHeaderParse(unterminated.data(), 256, 0, id, h));

    auto control = buildSlot(1, 0, 1000);
    control[82] = 0x07;  // control character inside the string
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadVersion,
                            (uint8_t)slotHeaderParse(control.data(), 256, 0, id, h));

    auto junkAfterNul = buildSlot(1, 0, 1000);
    junkAfterNul[90] = 'z';  // text after the terminator
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadVersion,
                            (uint8_t)slotHeaderParse(junkAfterNul.data(), 256, 0, id, h));

    auto empty = buildSlot(1, 0, 1000);
    memset(&empty[80], 0, 16);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::BadVersion,
                            (uint8_t)slotHeaderParse(empty.data(), 256, 0, id, h));
}

static void test_parse_rejects_truncated_header() {
    uint8_t pub[32], id[8];
    trustedKey(pub);
    fwKeyId(pub, id);
    auto slot = buildSlot(1, 0, 1000);
    SlotHeader h;
    const size_t lens[] = {0, 63, 255};
    for (size_t len : lens) {
        TEST_ASSERT_NOT_EQUAL_UINT8((uint8_t)SlotCheck::Ok,
                                    (uint8_t)slotHeaderParse(slot.data(), len, 0, id, h));
    }
    uint8_t erased[100];
    memset(erased, 0xFF, sizeof(erased));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SlotCheck::Empty, (uint8_t)slotHeaderParse(erased, 100, 0, id, h));
}

static void test_fingerprint_is_16_lowercase_hex() {
    uint8_t pub[32];
    trustedKey(pub);
    char fp[17];
    memset(fp, 'Z', sizeof(fp));
    fwKeyFingerprint(pub, fp);
    TEST_ASSERT_EQUAL_size_t(16, strlen(fp));
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_TRUE((fp[i] >= '0' && fp[i] <= '9') || (fp[i] >= 'a' && fp[i] <= 'f'));
    }
    uint8_t id[8];
    fwKeyId(pub, id);
    char expect[17];
    snprintf(expect, sizeof(expect), "%02x%02x%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2],
             id[3], id[4], id[5], id[6], id[7]);
    TEST_ASSERT_EQUAL_STRING(expect, fp);
}

static void test_key_id_is_sha512_prefix() {
    // SHA-512("abc") starts ddaf35a193617aba; a 32 byte key is hashed, so check
    // the first 8 bytes of a known SHA-512 via the library directly.
    uint8_t pub[32], hash[64], id[8];
    memset(pub, 0x42, sizeof(pub));
    crypto_sha512(hash, pub, 32);
    fwKeyId(pub, id);
    TEST_ASSERT_EQUAL_MEMORY(hash, id, 8);
    uint8_t abc[64];
    crypto_sha512(abc, (const uint8_t*)"abc", 3);
    uint8_t expect[8];
    hexToBytes("ddaf35a193617aba", expect);  // FIPS 180-2 test vector
    TEST_ASSERT_EQUAL_MEMORY(expect, abc, 8);
}

// ---- fw_sha512: the boot program's SHA-512 ----

static void sha512Hex(const uint8_t* data, size_t len, char out[129]) {
    FwSha512 ctx;
    fwSha512Init(ctx);
    fwSha512Update(ctx, data, len);
    uint8_t d[64];
    fwSha512Final(ctx, d);
    for (size_t i = 0; i < 64; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void test_sha512_fips_vectors() {
    // FIPS 180-2 / NIST example vectors: empty, one block, two blocks
    char hex[129];
    sha512Hex(nullptr, 0, hex);
    TEST_ASSERT_EQUAL_STRING(
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", hex);
    sha512Hex((const uint8_t*)"abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING(
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", hex);
    const char* two = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                      "klmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512Hex((const uint8_t*)two, strlen(two), hex);
    TEST_ASSERT_EQUAL_STRING(
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909", hex);
}

static void test_sha512_million_a() {
    std::vector<uint8_t> a(1000000, 'a');
    char hex[129];
    sha512Hex(a.data(), a.size(), hex);
    TEST_ASSERT_EQUAL_STRING(
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
        "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b", hex);
}

static void test_sha512_every_length_around_block_edges_matches_monocypher() {
    // 0..400 bytes covers empty, the 111/112 padding edge, full blocks and partial blocks
    std::vector<uint8_t> msg(400);
    for (size_t i = 0; i < msg.size(); i++) msg[i] = (uint8_t)(i * 167 + 13);
    for (size_t len = 0; len <= msg.size(); len++) {
        uint8_t want[64], got[64];
        crypto_sha512(want, msg.data(), len);
        FwSha512 ctx;
        fwSha512Init(ctx);
        fwSha512Update(ctx, msg.data(), len);
        fwSha512Final(ctx, got);
        char what[32];
        snprintf(what, sizeof(what), "length %u", (unsigned)len);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, got, 64, what);
    }
}

static void test_sha512_split_and_unaligned_updates_match_one_shot() {
    // The verification feeds R, A, then the image; any split and any alignment gives the same hash
    std::vector<uint8_t> buf(3 + 1000);
    uint8_t* msg = buf.data() + 3;  // not word aligned
    for (size_t i = 0; i < 1000; i++) msg[i] = (uint8_t)(i ^ (i >> 3));
    uint8_t want[64];
    crypto_sha512(want, msg, 1000);
    const size_t splits[] = {1, 7, 8, 63, 64, 65, 127, 128, 129, 255, 999};
    for (size_t s : splits) {
        for (size_t t = s; t <= 1000; t += 211) {
            FwSha512 ctx;
            fwSha512Init(ctx);
            fwSha512Update(ctx, msg, s);
            fwSha512Update(ctx, nullptr, 0);
            fwSha512Update(ctx, msg + s, t - s);
            fwSha512Update(ctx, msg + t, 1000 - t);
            uint8_t got[64];
            fwSha512Final(ctx, got);
            char what[40];
            snprintf(what, sizeof(what), "split %u/%u", (unsigned)s, (unsigned)t);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, got, 64, what);
        }
    }
}

static void test_sha512_init_restarts_after_final() {
    FwSha512 ctx;
    fwSha512Init(ctx);
    fwSha512Update(ctx, (const uint8_t*)"something else", 14);
    uint8_t d[64];
    fwSha512Final(ctx, d);
    fwSha512Init(ctx);
    fwSha512Update(ctx, (const uint8_t*)"abc", 3);
    fwSha512Final(ctx, d);
    uint8_t want[64];
    crypto_sha512(want, (const uint8_t*)"abc", 3);
    TEST_ASSERT_EQUAL_MEMORY(want, d, 64);
}

static void test_check_names() {
    TEST_ASSERT_EQUAL_STRING("bad signature", slotCheckName(SlotCheck::BadSignature));
    TEST_ASSERT_EQUAL_STRING("ok", slotCheckName(SlotCheck::Ok));
    TEST_ASSERT_EQUAL_STRING("empty", slotCheckName(SlotCheck::Empty));
    TEST_ASSERT_EQUAL_STRING("wrong key", slotCheckName(SlotCheck::WrongKey));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ed25519_matches_rfc8032_test1);
    RUN_TEST(test_verify_accepts_signed_slot);
    RUN_TEST(test_verify_rejects_flipped_image_byte);
    RUN_TEST(test_verify_rejects_flipped_header_byte);
    RUN_TEST(test_verify_rejects_flipped_signature_byte);
    RUN_TEST(test_verify_rejects_other_key);
    RUN_TEST(test_verify_rejects_wrong_slot);
    RUN_TEST(test_verify_rejects_length_over_slot);
    RUN_TEST(test_verify_accepts_length_exactly_max);
    RUN_TEST(test_verify_reports_erased_slot_as_empty);
    RUN_TEST(test_parse_accepts_valid_header_and_fills_fields);
    RUN_TEST(test_parse_rejects_bad_magic);
    RUN_TEST(test_parse_rejects_other_header_version);
    RUN_TEST(test_parse_rejects_unterminated_or_nonprintable_version);
    RUN_TEST(test_parse_rejects_truncated_header);
    RUN_TEST(test_fingerprint_is_16_lowercase_hex);
    RUN_TEST(test_key_id_is_sha512_prefix);
    RUN_TEST(test_check_names);
    RUN_TEST(test_sha512_fips_vectors);
    RUN_TEST(test_sha512_million_a);
    RUN_TEST(test_sha512_every_length_around_block_edges_matches_monocypher);
    RUN_TEST(test_sha512_split_and_unaligned_updates_match_one_shot);
    RUN_TEST(test_sha512_init_restarts_after_final);
    return UNITY_END();
}
