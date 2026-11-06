// On-device tests for src/fw_update_service.cpp: flash writes to the slot that
// is not running, boot record copies, and the upload path's stack use.
// Writes only the other slot (restored to erased) and the two boot record
// sectors (saved and restored around each test that writes them).
// Run: pio test -e pico -f embedded/test_fw_flash
//
// test_build_src is off for the pico environment, so the service and the
// two small files it needs are compiled into this test directly.

#include <Arduino.h>
#include <hardware/watchdog.h>
#include <unity.h>
#include <boot_decision.h>
#include <boot_record.h>
#include <fw_image.h>
#include <fw_layout.h>
#include <trial_monitor.h>
#include <update_receiver.h>
#include <watchdog_logic.h>
#include <web_app.h>

#include "../../../src/boot_info.cpp"
#include "../../../src/fw_update_service.cpp"
#include "../../../src/stack_watch.cpp"
#include "../../native/test_fw_receiver/golden_upd.h"

// The HIL tests require more than this many bytes of core 0 stack never used (test_web.py, test_dhcp6.py)
static const size_t STACK_HEADROOM_MIN = 1024;

static uint8_t savedRecords[2][FW_SECTOR_SIZE];  // static: far too big for the stack
static uint8_t sectorData[FW_SECTOR_SIZE];

static void saveRecordSectors() {
    for (int i = 0; i < 2; i++) memcpy(savedRecords[i], xip(FW_BOOT_RECORD_OFFSET[i]), FW_SECTOR_SIZE);
}

static void restoreRecordSectors() {
    for (int i = 0; i < 2; i++) {
        flashErase(FW_BOOT_RECORD_OFFSET[i]);
        flashProgram(FW_BOOT_RECORD_OFFSET[i], savedRecords[i], FW_SECTOR_SIZE);
        TEST_ASSERT_EQUAL_MEMORY(savedRecords[i], xip(FW_BOOT_RECORD_OFFSET[i]), FW_SECTOR_SIZE);
    }
    loadRecord();
}

static bool allErased(uint32_t flashOffset) {
    const uint8_t* p = xip(flashOffset);
    for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

static uint32_t recordSequence(int copy, bool& valid) {
    BootRecord r;
    valid = bootRecordDecode(xip(FW_BOOT_RECORD_OFFSET[copy]), BOOT_RECORD_SIZE, r);
    return r.sequence;
}

void setUp() {}
void tearDown() {}

static void test_slot_writer_erases_and_writes_inactive_sector() {
    FlashSlotWriter w(OTHER_SLOT);
    const uint32_t offsets[] = {FW_SECTOR_SIZE, FW_SLOT_SIZE - FW_SECTOR_SIZE};  // second and last sector
    for (uint32_t off : offsets) {
        for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) sectorData[i] = (uint8_t)(i * 7 + off / FW_SECTOR_SIZE);
        TEST_ASSERT_TRUE(w.eraseSector(off));
        TEST_ASSERT_TRUE(allErased(FW_SLOT_OFFSET[OTHER_SLOT] + off));
        TEST_ASSERT_TRUE(w.writeSector(off, sectorData));
        TEST_ASSERT_EQUAL_MEMORY(sectorData, xip(FW_SLOT_OFFSET[OTHER_SLOT] + off), FW_SECTOR_SIZE);
        TEST_ASSERT_TRUE(w.eraseSector(off));  // restore: erased
        TEST_ASSERT_TRUE(allErased(FW_SLOT_OFFSET[OTHER_SLOT] + off));
    }
    TEST_ASSERT_TRUE(w.touched());
}

static void test_slot_writer_refuses_running_slot() {
    // The last sector of the running slot: past the image, so a broken check would not hurt the test run
    const uint32_t off = FW_SLOT_SIZE - FW_SECTOR_SIZE;
    uint8_t before[16];
    memcpy(before, xip(FW_SLOT_OFFSET[RUNNING_SLOT] + off), sizeof(before));
    memset(sectorData, 0x5A, sizeof(sectorData));
    FlashSlotWriter w(RUNNING_SLOT);
    TEST_ASSERT_FALSE(w.eraseSector(off));
    TEST_ASSERT_FALSE(w.writeSector(off, sectorData));
    TEST_ASSERT_FALSE(w.eraseSector(0));
    TEST_ASSERT_FALSE(w.touched());
    TEST_ASSERT_EQUAL_MEMORY(before, xip(FW_SLOT_OFFSET[RUNNING_SLOT] + off), sizeof(before));
    // The boot program agrees which slot runs
    TEST_ASSERT_TRUE(bootInfoValid());
    TEST_ASSERT_EQUAL_UINT8(RUNNING_SLOT, bootInfoSlot());
}

static void test_slot_writer_refuses_offsets_outside_or_inside_a_sector() {
    FlashSlotWriter w(OTHER_SLOT);
    memset(sectorData, 0x5A, sizeof(sectorData));
    // FW_SLOT_SIZE past slot B is the unused sector before LittleFS; past slot A it is slot B
    TEST_ASSERT_FALSE(w.eraseSector(FW_SLOT_SIZE));
    TEST_ASSERT_FALSE(w.writeSector(FW_SLOT_SIZE, sectorData));
    TEST_ASSERT_FALSE(w.eraseSector(0xFFFFF000u));
    TEST_ASSERT_FALSE(w.eraseSector(FW_SECTOR_SIZE + 256));
    TEST_ASSERT_FALSE(w.writeSector(FW_SECTOR_SIZE + 256, sectorData));
    TEST_ASSERT_FALSE(w.touched());
}

static void test_slot_writer_refuses_source_data_in_flash() {
    FlashSlotWriter w(OTHER_SLOT);
    // Programming reads the source while XIP is off: a pointer into flash would crash
    TEST_ASSERT_FALSE(w.writeSector(FW_SECTOR_SIZE, GOLDEN_UPD));
    TEST_ASSERT_FALSE(w.writeSector(FW_SECTOR_SIZE, nullptr));
    TEST_ASSERT_FALSE(w.touched());
}

static void test_record_write_alternates_copies_and_increments_sequence() {
    saveRecordSectors();
    flashErase(FW_BOOT_RECORD_OFFSET[0]);
    flashErase(FW_BOOT_RECORD_OFFSET[1]);
    loadRecord();
    TEST_ASSERT_FALSE(haveRecord);

    bool valid0, valid1;
    BootRecord r = settledRecord(RUNNING_SLOT, FwResult::Installed, "3.3.0", true);
    TEST_ASSERT_TRUE(writeRecord(r));  // no valid copy: copy 0, sequence 1
    TEST_ASSERT_EQUAL_UINT32(1, recordSequence(0, valid0));
    TEST_ASSERT_TRUE(valid0);
    recordSequence(1, valid1);
    TEST_ASSERT_FALSE(valid1);

    TEST_ASSERT_TRUE(writeRecord(r));  // the copy that is not the newest: 1
    TEST_ASSERT_EQUAL_UINT32(1, recordSequence(0, valid0));
    TEST_ASSERT_EQUAL_UINT32(2, recordSequence(1, valid1));
    TEST_ASSERT_TRUE(valid1);

    r.lastResult = FwResult::RolledBackCrash;
    TEST_ASSERT_TRUE(writeRecord(r));  // back to 0
    TEST_ASSERT_EQUAL_UINT32(3, recordSequence(0, valid0));
    TEST_ASSERT_EQUAL_UINT32(2, recordSequence(1, valid1));

    // What the boot program reads: the newest copy, as written
    loadRecord();
    TEST_ASSERT_EQUAL_INT(0, recordCopy);
    TEST_ASSERT_EQUAL_UINT32(3, record.sequence);
    TEST_ASSERT_EQUAL(FwResult::RolledBackCrash, record.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.3.0", record.lastVersion);
    restoreRecordSectors();
}

static bool uploadOk = false;
static char uploadVersion[17];
static const char* uploadError = nullptr;

/// The upload path as webPoll() runs it: begin, 512-byte feeds, finish (Ed25519 check from flash)
static void __attribute__((noinline)) uploadGolden() {
    const char* error = nullptr;
    uploadOk = fwUpdateUploadBegin(sizeof(GOLDEN_UPD), error);
    if (!uploadOk) {
        uploadError = error;
        return;
    }
    static uint8_t chunk[512];  // web_service.cpp feeds from a static 512-byte buffer
    for (size_t pos = 0; pos < sizeof(GOLDEN_UPD) && uploadOk; pos += sizeof(chunk)) {
        size_t n = sizeof(GOLDEN_UPD) - pos < sizeof(chunk) ? sizeof(GOLDEN_UPD) - pos : sizeof(chunk);
        memcpy(chunk, GOLDEN_UPD + pos, n);
        uploadOk = fwUpdateUploadFeed(chunk, n);
    }
    uploadOk = uploadOk && fwUpdateUploadFinish(uploadVersion, uploadError);
}

/// Erase what an upload wrote to the other slot (the golden blob spans two sectors)
static void eraseOtherSlotStart() {
    flashErase(FW_SLOT_OFFSET[OTHER_SLOT]);
    flashErase(FW_SLOT_OFFSET[OTHER_SLOT] + FW_SECTOR_SIZE);
}

static void test_upload_path_stack_headroom() {
    saveRecordSectors();
    flashErase(FW_BOOT_RECORD_OFFSET[0]);  // no record: an upload may start
    flashErase(FW_BOOT_RECORD_OFFSET[1]);
    loadRecord();

    stackWatchBegin();  // paints the stack below this frame
    uploadGolden();
    size_t neverUsed = stackWatchNeverUsed();
    char msg[96];
    snprintf(msg, sizeof(msg), "upload + verify: %u bytes of core 0 stack never used (%s)", (unsigned)neverUsed,
             uploadError ? uploadError : "ok");
    Serial.println(msg);
    TEST_ASSERT_TRUE_MESSAGE(uploadOk, msg);
    TEST_ASSERT_EQUAL_STRING("3.4.0", uploadVersion);
    // Verified and recorded: the other slot boots next, on trial
    TEST_ASSERT_TRUE(haveRecord);
    TEST_ASSERT_EQUAL_UINT8(OTHER_SLOT, record.trial);
    TEST_ASSERT_EQUAL_UINT8(RUNNING_SLOT, record.active);
    SlotHeader h;
    TEST_ASSERT_EQUAL(SlotCheck::Ok, slotVerify(xip(FW_SLOT_OFFSET[OTHER_SLOT]), OTHER_SLOT, FW_PUBLIC_KEY, h));
    TEST_ASSERT_GREATER_THAN_MESSAGE(STACK_HEADROOM_MIN, neverUsed, msg);

    eraseOtherSlotStart();
    restoreRecordSectors();
    otherState = OtherSlot::Unchecked;
}

static void test_rejected_upload_leaves_other_slot_empty_and_no_record() {
    saveRecordSectors();
    flashErase(FW_BOOT_RECORD_OFFSET[0]);
    flashErase(FW_BOOT_RECORD_OFFSET[1]);
    loadRecord();

    // Flip one image byte of the other slot's blob: the header passes, the signature does not
    static uint8_t bad[sizeof(GOLDEN_UPD)];
    memcpy(bad, GOLDEN_UPD, sizeof(bad));
    uint32_t blobOffset = (uint32_t)bad[24 + 4 * OTHER_SLOT] | (uint32_t)bad[25 + 4 * OTHER_SLOT] << 8 |
                          (uint32_t)bad[26 + 4 * OTHER_SLOT] << 16 | (uint32_t)bad[27 + 4 * OTHER_SLOT] << 24;
    bad[blobOffset + FW_SLOT_HEADER_SIZE + 10] ^= 0x01;

    const char* error = nullptr;
    char version[17];
    TEST_ASSERT_TRUE(fwUpdateUploadBegin(sizeof(bad), error));
    TEST_ASSERT_TRUE(fwUpdateUploadFeed(bad, sizeof(bad)));
    TEST_ASSERT_FALSE(fwUpdateUploadFinish(version, error));
    TEST_ASSERT_EQUAL_STRING("bad signature", error);
    SlotHeader h;
    TEST_ASSERT_EQUAL(SlotCheck::Empty, slotVerify(xip(FW_SLOT_OFFSET[OTHER_SLOT]), OTHER_SLOT, FW_PUBLIC_KEY, h));
    TEST_ASSERT_FALSE(haveRecord);  // nothing switches
    WebFirmware fw = {};
    fwUpdateStatus(fw);
    TEST_ASSERT_EQUAL_STRING("rejected: bad signature", fw.lastResult);
    TEST_ASSERT_EQUAL_STRING("empty", fw.otherProblem);
    TEST_ASSERT_FALSE(fw.uploading);

    eraseOtherSlotStart();
    restoreRecordSectors();
    otherState = OtherSlot::Unchecked;
    uploadResultText[0] = '\0';
}

void setup() {
    // The boot program starts every image with the watchdog armed (~8.3 s);
    // these tests run longer than that in setup() and do not feed it
    watchdog_disable();
    bootInfoBegin();
    Serial.begin(115200);
    uint32_t start = millis();
    while (!Serial && millis() - start < 10000) delay(10);
    delay(500);
    fwUpdateBegin();

    UNITY_BEGIN();
    RUN_TEST(test_slot_writer_erases_and_writes_inactive_sector);
    RUN_TEST(test_slot_writer_refuses_running_slot);
    RUN_TEST(test_slot_writer_refuses_offsets_outside_or_inside_a_sector);
    RUN_TEST(test_slot_writer_refuses_source_data_in_flash);
    RUN_TEST(test_record_write_alternates_copies_and_increments_sequence);
    RUN_TEST(test_upload_path_stack_headroom);
    RUN_TEST(test_rejected_upload_leaves_other_slot_empty_and_no_record);
    UNITY_END();
}

void loop() {}
