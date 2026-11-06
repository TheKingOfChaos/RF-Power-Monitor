// Host unit tests for the streaming update file receiver (lib/fw_update).
// The .upd fixture follows tools/fw_release.py build_upd(): 64 byte header
// (magic, format, version[16], offA, offB, lenA, lenB, 0xFF), blob A, blob B.
// Run: pio test -e native -f native/test_fw_receiver

#include <string.h>

#include <vector>

#include <unity.h>
#include <fw_image.h>
#include <monocypher-ed25519.h>
#include <update_receiver.h>

void setUp() {}
void tearDown() {}

namespace {

typedef std::vector<uint8_t> Bytes;

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

void trustedKeyId(uint8_t id[8]) {
    uint8_t secret[64], pub[32];
    makeKey(1, secret, pub);
    fwKeyId(pub, id);
}

/// Signed blob: 256 byte slot header + image of imageLen bytes (pattern from salt).
Bytes buildBlob(uint8_t seedByte, uint32_t slotNo, uint32_t imageLen, uint8_t salt) {
    uint8_t secret[64], pub[32];
    makeKey(seedByte, secret, pub);
    Bytes b(FW_SLOT_HEADER_SIZE + imageLen, 0xFF);
    putLe32(&b[64], FW_MAGIC);
    putLe32(&b[68], FW_HEADER_VERSION);
    putLe32(&b[72], slotNo);
    putLe32(&b[76], imageLen);
    memset(&b[80], 0, 16);
    memcpy(&b[80], "3.4.0", 5);
    fwKeyId(pub, &b[96]);
    for (uint32_t i = 0; i < imageLen; i++) b[FW_SLOT_HEADER_SIZE + i] = (uint8_t)(i * 7 + salt);
    crypto_ed25519_sign(&b[0], secret, &b[64], FW_SLOT_HEADER_SIZE - 64 + imageLen);
    return b;
}

/// Like tools/fw_release.py build_upd().
Bytes buildUpd(const Bytes& a, const Bytes& b) {
    Bytes f(64, 0xFF);
    putLe32(&f[0], 0x50554652u);
    putLe32(&f[4], 1);
    memset(&f[8], 0, 16);
    memcpy(&f[8], "3.4.0", 5);
    putLe32(&f[24], 64);
    putLe32(&f[28], 64 + (uint32_t)a.size());
    putLe32(&f[32], (uint32_t)a.size());
    putLe32(&f[36], (uint32_t)b.size());
    f.insert(f.end(), a.begin(), a.end());
    f.insert(f.end(), b.begin(), b.end());
    return f;
}

/// Flash of one slot. Records every operation; writes AND into the flash like NOR.
struct FakeSlot : SlotWriter {
    Bytes flash;
    std::vector<uint32_t> erases, writes;
    int failAt = -1;  // fail the Nth operation (erase or write), counting from 0
    int ops = 0;
    bool wroteOutside = false;
    FakeSlot() : flash(FW_SLOT_SIZE, 0xFF) {}
    bool eraseSector(uint32_t off) override {
        if (ops++ == failAt) return false;
        if (off % FW_SECTOR_SIZE || off + FW_SECTOR_SIZE > FW_SLOT_SIZE) {
            wroteOutside = true;
            return true;
        }
        erases.push_back(off);
        memset(&flash[off], 0xFF, FW_SECTOR_SIZE);
        return true;
    }
    bool writeSector(uint32_t off, const uint8_t* d) override {
        if (ops++ == failAt) return false;
        if (off % FW_SECTOR_SIZE || off + FW_SECTOR_SIZE > FW_SLOT_SIZE) {
            wroteOutside = true;
            return true;
        }
        writes.push_back(off);
        for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) flash[off + i] &= d[i];
        return true;
    }
};

UpdateReceiver rx;  // two 4 KB buffers: static, like the firmware

UpdateStatus feedAll(const Bytes& d, size_t chunk) {
    UpdateStatus s = UpdateStatus::InProgress;
    for (size_t i = 0; i < d.size() && s == UpdateStatus::InProgress; i += chunk) {
        size_t n = d.size() - i < chunk ? d.size() - i : chunk;
        s = rx.feed(&d[i], n);
    }
    return s;
}

/// begin + feed + finish; returns the final status.
UpdateStatus receive(FakeSlot& w, const Bytes& upd, uint8_t slot, size_t chunk, uint32_t announced) {
    uint8_t id[8];
    trustedKeyId(id);
    rx.begin(slot, announced, id, w);
    UpdateStatus s = feedAll(upd, chunk);
    if (s == UpdateStatus::Failed) return s;
    return rx.finish();
}
UpdateStatus receive(FakeSlot& w, const Bytes& upd, uint8_t slot, size_t chunk) {
    return receive(w, upd, slot, chunk, (uint32_t)upd.size());
}

const size_t CHUNKS[3] = {1, 7, 4096};

bool slotEmpty(const FakeSlot& w) {
    uint8_t id[8];
    trustedKeyId(id);
    SlotHeader h;
    return slotHeaderParse(w.flash.data(), FW_SLOT_SIZE, 1, id, h) == SlotCheck::Empty;
}

}  // namespace

static void test_receiver_writes_target_blob_only() {
    Bytes a = buildBlob(1, 0, 5000, 1), b = buildBlob(1, 1, 9000, 2);
    Bytes upd = buildUpd(a, b);
    for (size_t chunk : CHUNKS) {
        FakeSlot w;
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)receive(w, upd, 1, chunk));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::None, (uint8_t)rx.error());
        TEST_ASSERT_EQUAL_STRING("3.4.0", rx.version());
        // slot B blob at offset 0, rest of the slot still erased
        TEST_ASSERT_EQUAL_MEMORY(b.data(), w.flash.data(), b.size());
        for (size_t i = b.size(); i < w.flash.size(); i++) {
            if (w.flash[i] != 0xFF) TEST_FAIL_MESSAGE("byte past the blob was written");
        }
        TEST_ASSERT_FALSE(w.wroteOutside);
        // 256 + 9000 = 9256 bytes = 3 sectors
        TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)w.writes.size());
        TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)w.erases.size());
    }
}

static void test_receiver_writes_slot_a_blob() {
    Bytes a = buildBlob(1, 0, 5000, 1), b = buildBlob(1, 1, 9000, 2);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)receive(w, buildUpd(a, b), 0, 4096));
    TEST_ASSERT_EQUAL_MEMORY(a.data(), w.flash.data(), a.size());
}

static void test_receiver_writes_first_sector_last() {
    Bytes upd = buildUpd(buildBlob(1, 0, 5000, 1), buildBlob(1, 1, 9000, 2));
    for (size_t chunk : CHUNKS) {
        FakeSlot w;
        uint8_t id[8];
        trustedKeyId(id);
        rx.begin(1, (uint32_t)upd.size(), id, w);
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());  // begin does no I/O
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::InProgress, (uint8_t)feedAll(upd, chunk));
        // all data in, nothing written to sector 0 yet, and the slot still reads Empty
        TEST_ASSERT_EQUAL_UINT32(0, w.erases[0]);
        for (uint32_t off : w.writes) TEST_ASSERT_NOT_EQUAL_UINT32(0, off);
        TEST_ASSERT_TRUE(slotEmpty(w));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)rx.finish());
        TEST_ASSERT_EQUAL_UINT32(0, w.writes.back());
        TEST_ASSERT_FALSE(slotEmpty(w));
    }
}

static void test_receiver_rejects_bad_magic() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    upd[0] ^= 1;
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadMagic, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_unknown_format() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    upd[4] = 2;
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadFormat, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_blob_past_content_length() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[36], 556 + 100000);  // slot B blob claims far more than the file holds
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_offset_overflow() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[28], 0xFFFFFF00u);  // offset + length wraps in 32 bit arithmetic
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_overlapping_blobs() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[28], 64 + 100);  // slot B starts inside slot A
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_blob_overlapping_file_header() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[24], 10);  // slot A blob starts inside the 64 byte header
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_blob_over_slot_size() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[36], FW_SLOT_SIZE + 1);
    FakeSlot w;
    // announce a file big enough that only the slot size limit can reject it
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed,
                            (uint8_t)receive(w, upd, 1, 7, 64 + 556 + FW_SLOT_SIZE + 1));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::TooLarge, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_target_blob_shorter_than_slot_header() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    putLe32(&upd[36], 255);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::SlotMissing, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_blob_for_wrong_slot() {
    // the file's slot B blob was signed for slot A
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 0, 300, 2));
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::WrongSlot, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_rejects_other_key_before_erasing() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(2, 1, 300, 2));
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.writes.size());
}

static void test_receiver_rejects_image_length_mismatch() {
    // slot header says 400 bytes of image but the blob holds 300
    Bytes b = buildBlob(1, 1, 300, 2);
    putLe32(&b[76], 400);
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), b);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::BadHeader, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_truncated_body_fails() {
    Bytes upd = buildUpd(buildBlob(1, 0, 5000, 1), buildBlob(1, 1, 9000, 2));
    upd.resize(upd.size() - 100);
    for (size_t chunk : CHUNKS) {
        FakeSlot w;
        uint8_t id[8];
        trustedKeyId(id);
        // the announced length is the full file, the body stops short
        rx.begin(1, (uint32_t)upd.size() + 100, id, w);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::InProgress, (uint8_t)feedAll(upd, chunk));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.finish());
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::Truncated, (uint8_t)rx.error());
        TEST_ASSERT_EQUAL_UINT32(0, w.erases[0]);  // sector 0 erased ...
        for (uint32_t off : w.writes) TEST_ASSERT_NOT_EQUAL_UINT32(0, off);  // ... never written
        TEST_ASSERT_TRUE(slotEmpty(w));
    }
}

static void test_receiver_extra_bytes_fail() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    FakeSlot w;
    uint8_t id[8];
    trustedKeyId(id);
    rx.begin(1, (uint32_t)upd.size(), id, w);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::InProgress, (uint8_t)feedAll(upd, 4096));
    uint8_t extra = 0;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.feed(&extra, 1));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::TooLong, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.finish());
    TEST_ASSERT_TRUE(slotEmpty(w));
}

static void test_receiver_write_failure_stops() {
    Bytes upd = buildUpd(buildBlob(1, 0, 5000, 1), buildBlob(1, 1, 20000, 2));
    // operations: erase 0, then (erase, write) per sector 1..n; fail each in turn
    for (int failAt = 0; failAt < 5; failAt++) {
        FakeSlot w;
        w.failAt = failAt;
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 4096));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::WriteFailed, (uint8_t)rx.error());
        int opsSoFar = w.ops;
        uint8_t extra = 0;
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.feed(&extra, 1));
        TEST_ASSERT_EQUAL_INT(opsSoFar, w.ops);  // no more flash I/O after the failure
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.finish());
        TEST_ASSERT_EQUAL_INT(opsSoFar, w.ops);
        TEST_ASSERT_TRUE(slotEmpty(w));
    }
}

static void test_receiver_sector_zero_write_failure_in_finish() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    FakeSlot w;
    w.failAt = 1;  // erase 0 is op 0, the final write of sector 0 is op 1
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::WriteFailed, (uint8_t)rx.error());
}

static void test_receiver_header_split_across_feeds() {
    Bytes upd = buildUpd(buildBlob(1, 0, 5000, 1), buildBlob(1, 1, 9000, 2));
    // splits inside the file header, inside the slot header, at the 256 byte boundary
    size_t splits[] = {1, 63, 64, 65, 64 + 5256 + 255, 64 + 5256 + 256, 64 + 5256 + 257};
    for (size_t s : splits) {
        FakeSlot w;
        uint8_t id[8];
        trustedKeyId(id);
        rx.begin(1, (uint32_t)upd.size(), id, w);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::InProgress, (uint8_t)rx.feed(upd.data(), s));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::InProgress,
                                (uint8_t)rx.feed(upd.data() + s, upd.size() - s));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)rx.finish());
        TEST_ASSERT_EQUAL_MEMORY(&upd[64 + 5256], w.flash.data(), 9256);
    }
}

static void test_receiver_empty_body_fails() {
    FakeSlot w;
    uint8_t id[8];
    trustedKeyId(id);
    rx.begin(1, 0, id, w);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.finish());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::Truncated, (uint8_t)rx.error());
    rx.begin(1, 1000, id, w);  // announced but nothing arrives
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)rx.finish());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::Truncated, (uint8_t)rx.error());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)w.erases.size());
}

static void test_receiver_exact_slot_size_blob_ok() {
    Bytes b = buildBlob(1, 1, FW_IMAGE_MAX, 9);
    TEST_ASSERT_EQUAL_UINT32(FW_SLOT_SIZE, (uint32_t)b.size());
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), b);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)receive(w, upd, 1, 4096));
    TEST_ASSERT_EQUAL_MEMORY(b.data(), w.flash.data(), b.size());
    TEST_ASSERT_FALSE(w.wroteOutside);
    TEST_ASSERT_EQUAL_UINT32(FW_SLOT_SIZE / FW_SECTOR_SIZE, (uint32_t)w.writes.size());
}

static void test_receiver_pads_last_sector_with_ff() {
    Bytes b = buildBlob(1, 1, 4096, 5);  // 4352 bytes: sector 1 holds 256 bytes
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), b);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)receive(w, upd, 1, 7));
    TEST_ASSERT_EQUAL_MEMORY(b.data(), w.flash.data(), b.size());
    for (size_t i = b.size(); i < 8192; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, w.flash[i]);
}

static void test_receiver_blob_of_one_sector_only() {
    Bytes b = buildBlob(1, 1, 300, 5);
    FakeSlot w;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done,
                            (uint8_t)receive(w, buildUpd(buildBlob(1, 0, 300, 1), b), 1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)w.writes.size());
    TEST_ASSERT_EQUAL_MEMORY(b.data(), w.flash.data(), b.size());
}

static void test_receiver_reusable_after_failure() {
    Bytes upd = buildUpd(buildBlob(1, 0, 300, 1), buildBlob(1, 1, 300, 2));
    Bytes bad = upd;
    bad[0] ^= 1;
    FakeSlot w1, w2;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Failed, (uint8_t)receive(w1, bad, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateStatus::Done, (uint8_t)receive(w2, upd, 1, 7));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UpdateError::None, (uint8_t)rx.error());
}

static void test_receiver_error_names() {
    TEST_ASSERT_EQUAL_STRING("bad magic", updateErrorName(UpdateError::BadMagic));
    TEST_ASSERT_EQUAL_STRING("truncated", updateErrorName(UpdateError::Truncated));
    TEST_ASSERT_EQUAL_STRING("none", updateErrorName(UpdateError::None));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_receiver_writes_target_blob_only);
    RUN_TEST(test_receiver_writes_slot_a_blob);
    RUN_TEST(test_receiver_writes_first_sector_last);
    RUN_TEST(test_receiver_rejects_bad_magic);
    RUN_TEST(test_receiver_rejects_unknown_format);
    RUN_TEST(test_receiver_rejects_blob_past_content_length);
    RUN_TEST(test_receiver_rejects_offset_overflow);
    RUN_TEST(test_receiver_rejects_overlapping_blobs);
    RUN_TEST(test_receiver_rejects_blob_overlapping_file_header);
    RUN_TEST(test_receiver_rejects_blob_over_slot_size);
    RUN_TEST(test_receiver_rejects_target_blob_shorter_than_slot_header);
    RUN_TEST(test_receiver_rejects_blob_for_wrong_slot);
    RUN_TEST(test_receiver_rejects_other_key_before_erasing);
    RUN_TEST(test_receiver_rejects_image_length_mismatch);
    RUN_TEST(test_receiver_truncated_body_fails);
    RUN_TEST(test_receiver_extra_bytes_fail);
    RUN_TEST(test_receiver_write_failure_stops);
    RUN_TEST(test_receiver_sector_zero_write_failure_in_finish);
    RUN_TEST(test_receiver_header_split_across_feeds);
    RUN_TEST(test_receiver_empty_body_fails);
    RUN_TEST(test_receiver_exact_slot_size_blob_ok);
    RUN_TEST(test_receiver_pads_last_sector_with_ff);
    RUN_TEST(test_receiver_blob_of_one_sector_only);
    RUN_TEST(test_receiver_reusable_after_failure);
    RUN_TEST(test_receiver_error_names);
    return UNITY_END();
}
