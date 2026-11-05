// Host unit tests for the boot record and boot decision (lib/fw_update).
// Run: pio test -e native -f native/test_fw_boot

#include <string.h>

#include <unity.h>
#include <boot_decision.h>
#include <boot_record.h>

void setUp() {}
void tearDown() {}

namespace {

BootRecord makeRecord(uint32_t seq, uint8_t active, uint8_t trial) {
    BootRecord r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.active = active;
    r.trial = trial;
    r.lastResult = FwResult::None;
    r.trapPending = false;
    return r;
}

const uint32_t WD_TIMEOUT = 1;  // watchdog reason: timeout
const uint32_t WD_FORCE = 2;    // watchdog reason: forced reboot
const uint32_t NON_REBOOT = 0x6ab73121u;
const uint32_t HAD_RUN = 1u << 16;
const bool BOTH_OK[2] = {true, true};

uint32_t crashSlot(uint8_t slot, uint8_t count) { return 0xC7A50000u | ((uint32_t)slot << 8) | count; }

// One boot after the previous decision's scratch value
BootDecision step(const BootRecord& r, ResetKind k, uint32_t scratch, const bool ok[2] = BOTH_OK) {
    return decideBoot(true, r, ok, k, scratch);
}

}  // namespace

// ---- CRC and record ----
void test_crc32_check_value() {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, fwCrc32((const uint8_t*)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0u, fwCrc32(nullptr, 0));
}

void test_record_roundtrip() {
    BootRecord r = makeRecord(0xFFFFFFF0u, 1, FW_NO_SLOT);
    r.lastResult = FwResult::RolledBackCrash;
    r.trapPending = true;
    strcpy(r.lastVersion, "1.2.3-rc1");
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(r, buf);
    BootRecord o;
    TEST_ASSERT_TRUE(bootRecordDecode(buf, sizeof(buf), o));
    TEST_ASSERT_EQUAL_HEX32(r.sequence, o.sequence);
    TEST_ASSERT_EQUAL_UINT8(1, o.active);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, o.trial);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FwResult::RolledBackCrash, (uint8_t)o.lastResult);
    TEST_ASSERT_TRUE(o.trapPending);
    TEST_ASSERT_EQUAL_STRING("1.2.3-rc1", o.lastVersion);
}

void test_record_roundtrip_full_length_version_and_trial() {
    BootRecord r = makeRecord(7, 0, 1);
    memcpy(r.lastVersion, "0123456789abcdef", 17);  // 16 chars + NUL
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(r, buf);
    BootRecord o;
    TEST_ASSERT_TRUE(bootRecordDecode(buf, sizeof(buf), o));
    TEST_ASSERT_EQUAL_UINT8(1, o.trial);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", o.lastVersion);
}

void test_encode_starts_with_magic() {
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(makeRecord(1, 0, FW_NO_SLOT), buf);
    TEST_ASSERT_EQUAL_HEX8(0x52, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x42, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x46, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x52, buf[3]);
}

void test_decode_rejects_bad_crc() {
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(makeRecord(1, 0, FW_NO_SLOT), buf);
    for (size_t i = 0; i < BOOT_RECORD_SIZE; i++) {
        uint8_t c[BOOT_RECORD_SIZE];
        memcpy(c, buf, sizeof(c));
        c[i] ^= 0x01;
        BootRecord o;
        TEST_ASSERT_FALSE_MESSAGE(bootRecordDecode(c, sizeof(c), o), "flipped bit accepted");
    }
}

void test_decode_rejects_truncated() {
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(makeRecord(1, 0, FW_NO_SLOT), buf);
    BootRecord o;
    TEST_ASSERT_FALSE(bootRecordDecode(buf, 0, o));
    TEST_ASSERT_FALSE(bootRecordDecode(buf, 63, o));
    TEST_ASSERT_FALSE(bootRecordDecode(nullptr, 64, o));
}

void test_decode_rejects_trial_equal_active() {
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(makeRecord(1, 1, 1), buf);  // encode does not validate; decode must
    BootRecord o;
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
}

void test_decode_rejects_invalid_slots_and_enums() {
    BootRecord o;
    uint8_t buf[BOOT_RECORD_SIZE];
    bootRecordEncode(makeRecord(1, FW_NO_SLOT, FW_NO_SLOT), buf);
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));  // no active slot
    bootRecordEncode(makeRecord(1, 2, FW_NO_SLOT), buf);
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
    bootRecordEncode(makeRecord(1, 0, 2), buf);
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
    BootRecord r = makeRecord(1, 0, FW_NO_SLOT);
    r.lastResult = (FwResult)5;
    bootRecordEncode(r, buf);
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
}

void test_decode_rejects_unprintable_or_gapped_version() {
    BootRecord o;
    uint8_t buf[BOOT_RECORD_SIZE];
    BootRecord r = makeRecord(1, 0, FW_NO_SLOT);
    strcpy(r.lastVersion, "1.0");
    r.lastVersion[1] = '\n';
    bootRecordEncode(r, buf);
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
    // data after the terminator (the encoder stops at the NUL, so patch the bytes and re-seal the CRC)
    bootRecordEncode(makeRecord(1, 0, FW_NO_SLOT), buf);
    buf[12] = '1';
    buf[18] = 'x';
    uint32_t crc = fwCrc32(buf, 60);
    for (int i = 0; i < 4; i++) buf[60 + i] = (uint8_t)(crc >> (8 * i));
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
    buf[18] = 0;  // control: same patch without the stray byte is valid
    crc = fwCrc32(buf, 60);
    for (int i = 0; i < 4; i++) buf[60 + i] = (uint8_t)(crc >> (8 * i));
    TEST_ASSERT_TRUE(bootRecordDecode(buf, sizeof(buf), o));
}

void test_decode_rejects_erased_sector() {
    uint8_t buf[BOOT_RECORD_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    BootRecord o;
    TEST_ASSERT_FALSE(bootRecordDecode(buf, sizeof(buf), o));
}

void test_newest_picks_higher_sequence() {
    BootRecord a = makeRecord(5, 0, FW_NO_SLOT), b = makeRecord(6, 1, FW_NO_SLOT);
    TEST_ASSERT_EQUAL_INT(1, bootRecordNewest(true, a, true, b));
    TEST_ASSERT_EQUAL_INT(0, bootRecordNewest(true, b, true, a));
}

void test_newest_ignores_copy_with_bad_crc() {
    BootRecord a = makeRecord(5, 0, FW_NO_SLOT), b = makeRecord(6, 1, FW_NO_SLOT);
    TEST_ASSERT_EQUAL_INT(0, bootRecordNewest(true, a, false, b));
    TEST_ASSERT_EQUAL_INT(1, bootRecordNewest(false, a, true, b));
}

void test_newest_handles_sequence_wraparound() {
    BootRecord a = makeRecord(0xFFFFFFFFu, 0, FW_NO_SLOT), b = makeRecord(0, 1, FW_NO_SLOT);
    TEST_ASSERT_EQUAL_INT(1, bootRecordNewest(true, a, true, b));
    TEST_ASSERT_EQUAL_INT(0, bootRecordNewest(true, b, true, a));
}

void test_newest_none_valid_returns_minus_one() {
    BootRecord a = makeRecord(5, 0, FW_NO_SLOT);
    TEST_ASSERT_EQUAL_INT(-1, bootRecordNewest(false, a, false, a));
}

// ---- reset classification ----
void test_classify_watchdog_timeout_is_crash() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::Crash, (uint8_t)classifyReset(WD_TIMEOUT, NON_REBOOT, 0));
}

void test_classify_reboot_is_planned() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::Planned, (uint8_t)classifyReset(WD_FORCE, 0xb007c0d3u, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::Planned, (uint8_t)classifyReset(WD_FORCE, 0, 0));
}

void test_classify_power_on() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::PowerOn, (uint8_t)classifyReset(0, 0, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::PowerOn, (uint8_t)classifyReset(0, NON_REBOOT, 1u));
}

void test_classify_run_pin() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::ResetPin, (uint8_t)classifyReset(0, 0, HAD_RUN));
}

// ---- boot decision ----
void test_decide_boots_active_when_no_trial() {
    BootDecision d = step(makeRecord(1, 1, FW_NO_SLOT), ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Normal, (uint8_t)d.reason);
}

void test_decide_boots_trial_slot() {
    BootDecision d = step(makeRecord(1, 0, 1), ResetKind::Planned, 0);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Trial, (uint8_t)d.reason);
}

void test_decide_without_record_boots_slot_a() {
    BootRecord none = makeRecord(0, 0, FW_NO_SLOT);
    BootDecision d = decideBoot(false, none, BOTH_OK, ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::NoRecord, (uint8_t)d.reason);
}

void test_decide_without_record_and_bad_a_boots_b() {
    BootRecord none = makeRecord(0, 0, FW_NO_SLOT);
    const bool ok[2] = {false, true};
    BootDecision d = decideBoot(false, none, ok, ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
}

void test_decide_third_crash_falls_back_on_trial() {
    BootRecord r = makeRecord(1, 0, 1);
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(0, 0), d.crashScratch);
}

void test_decide_third_crash_falls_back_when_kept() {
    BootRecord r = makeRecord(1, 1, FW_NO_SLOT);
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
}

void test_decide_two_crashes_do_not_fall_back() {
    BootRecord r = makeRecord(1, 0, 1);
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Trial, (uint8_t)d.reason);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(1, 2), d.crashScratch);
}

void test_decide_power_on_resets_count() {
    BootRecord r = makeRecord(1, 0, 1);
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    d = step(r, ResetKind::PowerOn, d.crashScratch);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(1, 0), d.crashScratch);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
}

void test_decide_planned_reboot_resets_count() {
    BootRecord r = makeRecord(1, 0, 1);
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    d = step(r, ResetKind::Planned, d.crashScratch);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    d = step(r, ResetKind::ResetPin, d.crashScratch);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(1, 0), d.crashScratch);
}

void test_decide_never_picks_invalid_slot() {
    BootRecord r = makeRecord(1, 0, 1);
    const bool ok[2] = {true, false};  // trial slot 1 fails verification
    BootDecision d = step(r, ResetKind::PowerOn, 0, ok);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
    // crash limit reached but the other slot is invalid: stay on the target
    BootRecord k = makeRecord(1, 1, FW_NO_SLOT);
    const bool ok1[2] = {false, true};
    d = step(k, ResetKind::Crash, crashSlot(1, 2), ok1);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Normal, (uint8_t)d.reason);
}

void test_decide_nothing_bootable_returns_no_slot() {
    const bool none[2] = {false, false};
    BootDecision d = step(makeRecord(1, 0, 1), ResetKind::PowerOn, 0, none);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, d.slot);
    BootRecord nr = makeRecord(0, 0, FW_NO_SLOT);
    d = decideBoot(false, nr, none, ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, d.slot);
}

void test_decide_count_for_other_slot_restarts_at_one() {
    BootRecord r = makeRecord(1, 0, FW_NO_SLOT);
    BootDecision d = step(r, ResetKind::Crash, crashSlot(1, 2));
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(0, 1), d.crashScratch);
    // garbage scratch means count 0
    d = step(r, ResetKind::Crash, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(0, 1), d.crashScratch);
}

void test_decide_out_of_range_record_treated_as_no_record() {
    BootRecord bad = makeRecord(1, 7, FW_NO_SLOT);
    BootDecision d = step(bad, ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::NoRecord, (uint8_t)d.reason);
    bad = makeRecord(1, 0, 9);
    d = step(bad, ResetKind::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::NoRecord, (uint8_t)d.reason);
    const bool ok[2] = {false, true};
    d = step(makeRecord(1, 7, FW_NO_SLOT), ResetKind::PowerOn, 0, ok);
    TEST_ASSERT_EQUAL_UINT8(1, d.slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
}

void test_decide_failed_trial_then_fallback_slot_crashes_stays_on_it() {
    BootRecord r = makeRecord(1, 0, 1);
    const bool ok[2] = {true, false};  // trial slot 1 does not verify
    BootDecision d = step(r, ResetKind::PowerOn, 0, ok);
    for (int i = 0; i < 5; i++) {
        d = step(r, ResetKind::Crash, d.crashScratch, ok);
        TEST_ASSERT_EQUAL_UINT8(0, d.slot);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)d.reason);
        TEST_ASSERT_EQUAL_HEX32(crashSlot(0, (uint8_t)(i + 1)), d.crashScratch);
    }
}

void test_decide_unchanged_record_after_fallback_returns_to_target() {
    BootRecord r = makeRecord(1, 0, 1);  // firmware has not rewritten the record
    BootDecision d = step(r, ResetKind::Crash, 0);
    d = step(r, ResetKind::Crash, d.crashScratch);
    d = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);  // fell back
    BootDecision c = step(r, ResetKind::Crash, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(1, 1), c.crashScratch);
    BootDecision p = step(r, ResetKind::Planned, d.crashScratch);
    TEST_ASSERT_EQUAL_UINT8(1, p.slot);
    TEST_ASSERT_EQUAL_HEX32(crashSlot(1, 0), p.crashScratch);
}

void test_boot_info_roundtrip() {
    uint32_t v = bootInfoEncode(1, BootReason::Fallback, ResetKind::Crash, 0);
    TEST_ASSERT_EQUAL_HEX8(0xB1, v >> 24);
    uint8_t slot, failed;
    BootReason reason;
    ResetKind reset;
    TEST_ASSERT_TRUE(bootInfoDecode(v, slot, reason, reset, failed));
    TEST_ASSERT_EQUAL_UINT8(1, slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BootReason::Fallback, (uint8_t)reason);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ResetKind::Crash, (uint8_t)reset);
    TEST_ASSERT_EQUAL_UINT8(0, failed);
    v = bootInfoEncode(0, BootReason::Normal, ResetKind::PowerOn, FW_NO_SLOT);
    TEST_ASSERT_TRUE(bootInfoDecode(v, slot, reason, reset, failed));
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, failed);
}

void test_boot_info_rejects_garbage() {
    uint8_t slot, failed;
    BootReason reason;
    ResetKind reset;
    TEST_ASSERT_FALSE(bootInfoDecode(0, slot, reason, reset, failed));
    TEST_ASSERT_FALSE(bootInfoDecode(0xFFFFFFFFu, slot, reason, reset, failed));
    TEST_ASSERT_FALSE(bootInfoDecode(0xB1000000u | (9u << 8), slot, reason, reset, failed));   // reason 9
    TEST_ASSERT_FALSE(bootInfoDecode(0xB1000000u | (9u << 12), slot, reason, reset, failed));  // reset 9
    TEST_ASSERT_FALSE(bootInfoDecode(0xB1000000u | 5u, slot, reason, reset, failed));          // slot 5
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_record_roundtrip);
    RUN_TEST(test_record_roundtrip_full_length_version_and_trial);
    RUN_TEST(test_encode_starts_with_magic);
    RUN_TEST(test_decode_rejects_bad_crc);
    RUN_TEST(test_decode_rejects_truncated);
    RUN_TEST(test_decode_rejects_trial_equal_active);
    RUN_TEST(test_decode_rejects_invalid_slots_and_enums);
    RUN_TEST(test_decode_rejects_unprintable_or_gapped_version);
    RUN_TEST(test_decode_rejects_erased_sector);
    RUN_TEST(test_newest_picks_higher_sequence);
    RUN_TEST(test_newest_ignores_copy_with_bad_crc);
    RUN_TEST(test_newest_handles_sequence_wraparound);
    RUN_TEST(test_newest_none_valid_returns_minus_one);
    RUN_TEST(test_classify_watchdog_timeout_is_crash);
    RUN_TEST(test_classify_reboot_is_planned);
    RUN_TEST(test_classify_power_on);
    RUN_TEST(test_classify_run_pin);
    RUN_TEST(test_decide_boots_active_when_no_trial);
    RUN_TEST(test_decide_boots_trial_slot);
    RUN_TEST(test_decide_without_record_boots_slot_a);
    RUN_TEST(test_decide_without_record_and_bad_a_boots_b);
    RUN_TEST(test_decide_third_crash_falls_back_on_trial);
    RUN_TEST(test_decide_third_crash_falls_back_when_kept);
    RUN_TEST(test_decide_two_crashes_do_not_fall_back);
    RUN_TEST(test_decide_power_on_resets_count);
    RUN_TEST(test_decide_planned_reboot_resets_count);
    RUN_TEST(test_decide_never_picks_invalid_slot);
    RUN_TEST(test_decide_nothing_bootable_returns_no_slot);
    RUN_TEST(test_decide_count_for_other_slot_restarts_at_one);
    RUN_TEST(test_decide_out_of_range_record_treated_as_no_record);
    RUN_TEST(test_decide_failed_trial_then_fallback_slot_crashes_stays_on_it);
    RUN_TEST(test_decide_unchanged_record_after_fallback_returns_to_target);
    RUN_TEST(test_boot_info_roundtrip);
    RUN_TEST(test_boot_info_rejects_garbage);
    return UNITY_END();
}
