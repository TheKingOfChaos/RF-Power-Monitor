// Host unit tests for lib/fw_update fw_records: which boot record the firmware
// writes after a fallback, a confirmed or given-up trial, an upload or a switch.
// Run: pio test -e native -f native/test_fw_records

#include <string.h>
#include <unity.h>
#include <fw_records.h>

void setUp() {}
void tearDown() {}

/// A record as an upload leaves it: slot `active` kept, `trial` on trial
static BootRecord record(uint8_t active, uint8_t trial) {
    BootRecord r = {};
    r.sequence = 7;
    r.active = active;
    r.trial = trial;
    r.lastResult = FwResult::None;
    strcpy(r.lastVersion, "3.4.0");
    return r;
}

static void test_fallback_from_trial_slot_is_rolled_back_crash() {
    BootRecord rec = record(0, 1);  // A kept, B on trial; B crashed 3 times, A runs
    BootRecord r = fwRecordAfterFallback(true, rec, 0, 1, "3.4.0");
    TEST_ASSERT_EQUAL_UINT8(0, r.active);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, r.trial);
    TEST_ASSERT_EQUAL(FwResult::RolledBackCrash, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.4.0", r.lastVersion);
    TEST_ASSERT_TRUE(r.trapPending);
}

static void test_fallback_from_kept_slot_is_fallback_crash() {
    BootRecord rec = record(1, FW_NO_SLOT);  // B kept; B crashed 3 times, A runs
    BootRecord r = fwRecordAfterFallback(true, rec, 0, 1, "3.3.1");
    TEST_ASSERT_EQUAL_UINT8(0, r.active);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, r.trial);
    TEST_ASSERT_EQUAL(FwResult::FallbackCrash, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.3.1", r.lastVersion);
    TEST_ASSERT_TRUE(r.trapPending);
}

static void test_fallback_without_record_is_fallback_crash() {
    BootRecord none = {};
    none.trial = 0;  // garbage: must be ignored without a record
    BootRecord r = fwRecordAfterFallback(false, none, 1, 0, "");
    TEST_ASSERT_EQUAL_UINT8(1, r.active);
    TEST_ASSERT_EQUAL(FwResult::FallbackCrash, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("", r.lastVersion);
    TEST_ASSERT_TRUE(r.trapPending);
}

static void test_fallback_with_unknown_failed_slot_is_fallback_crash() {
    BootRecord rec = record(0, FW_NO_SLOT);
    BootRecord r = fwRecordAfterFallback(true, rec, 0, FW_NO_SLOT, nullptr);
    TEST_ASSERT_EQUAL(FwResult::FallbackCrash, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("", r.lastVersion);
}

static void test_fallback_record_is_valid_for_the_boot_program() {
    BootRecord r = fwRecordAfterFallback(true, record(0, 1), 0, 1, "3.4.0");
    uint8_t enc[BOOT_RECORD_SIZE];
    bootRecordEncode(r, enc);
    BootRecord back;
    TEST_ASSERT_TRUE(bootRecordDecode(enc, sizeof(enc), back));
}

static void test_confirmed_keeps_running_slot_with_trap() {
    BootRecord r = fwRecordConfirmed(1, "3.4.0");
    TEST_ASSERT_EQUAL_UINT8(1, r.active);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, r.trial);
    TEST_ASSERT_EQUAL(FwResult::Installed, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.4.0", r.lastVersion);
    TEST_ASSERT_TRUE(r.trapPending);
}

static void test_unreachable_goes_back_to_other_slot_with_trap() {
    BootRecord r = fwRecordUnreachable(1, "3.4.0");
    TEST_ASSERT_EQUAL_UINT8(0, r.active);
    TEST_ASSERT_EQUAL_UINT8(FW_NO_SLOT, r.trial);
    TEST_ASSERT_EQUAL(FwResult::RolledBackUnreachable, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.4.0", r.lastVersion);
    TEST_ASSERT_TRUE(r.trapPending);
    TEST_ASSERT_EQUAL_UINT8(1, fwRecordUnreachable(0, "x").active);
}

static void test_trial_of_other_slot_keeps_running_slot_active() {
    BootRecord r = fwRecordTrialOfOther(0, "3.5.0");
    TEST_ASSERT_EQUAL_UINT8(0, r.active);
    TEST_ASSERT_EQUAL_UINT8(1, r.trial);
    TEST_ASSERT_EQUAL(FwResult::None, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.5.0", r.lastVersion);
    TEST_ASSERT_FALSE(r.trapPending);
    BootRecord b = fwRecordTrialOfOther(1, "3.5.0");
    TEST_ASSERT_EQUAL_UINT8(1, b.active);
    TEST_ASSERT_EQUAL_UINT8(0, b.trial);
}

static void test_long_version_is_cut_and_terminated() {
    BootRecord r = fwRecordConfirmed(0, "0123456789abcdefXYZ");
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", r.lastVersion);
    TEST_ASSERT_EQUAL_CHAR('\0', r.lastVersion[16]);
}

static void test_trap_sent_clears_only_the_trap_flag() {
    BootRecord rec = fwRecordConfirmed(1, "3.4.0");
    rec.sequence = 9;
    BootRecord r = fwRecordTrapSent(rec);
    TEST_ASSERT_FALSE(r.trapPending);
    TEST_ASSERT_EQUAL_UINT32(9, r.sequence);
    TEST_ASSERT_EQUAL_UINT8(1, r.active);
    TEST_ASSERT_EQUAL(FwResult::Installed, r.lastResult);
    TEST_ASSERT_EQUAL_STRING("3.4.0", r.lastVersion);
}

static void test_settled_without_record_or_with_running_slot_active() {
    BootRecord none = {};
    TEST_ASSERT_TRUE(fwRecordSettled(false, none, 0));
    TEST_ASSERT_TRUE(fwRecordSettled(true, record(0, FW_NO_SLOT), 0));
}

static void test_not_settled_while_trial_pending_or_other_slot_active() {
    TEST_ASSERT_FALSE(fwRecordSettled(true, record(0, 1), 0));           // upload done, restart pending
    TEST_ASSERT_FALSE(fwRecordSettled(true, record(1, FW_NO_SLOT), 0));  // fallback not rewritten
}

static void test_crash_limit_is_crash_reset_with_count_zero() {
    TEST_ASSERT_TRUE(fwFallbackWasCrashLimit(ResetKind::Crash, 0));
    TEST_ASSERT_TRUE(fwFallbackWasCrashLimit(ResetKind::Crash, 0xC7A50000u));  // slot A, count 0
    TEST_ASSERT_TRUE(fwFallbackWasCrashLimit(ResetKind::Crash, 0xC7A50100u));  // slot B, count 0
}

static void test_failed_check_is_any_other_reset_or_a_count() {
    TEST_ASSERT_FALSE(fwFallbackWasCrashLimit(ResetKind::Crash, 0xC7A50101u));
    TEST_ASSERT_FALSE(fwFallbackWasCrashLimit(ResetKind::Planned, 0));
    TEST_ASSERT_FALSE(fwFallbackWasCrashLimit(ResetKind::PowerOn, 0));
    TEST_ASSERT_FALSE(fwFallbackWasCrashLimit(ResetKind::ResetPin, 0));
}

static void test_forget_other_after_crash_rollback() {
    // B on trial crashed 3 times, A runs again and has written the rollback record
    BootRecord r = fwRecordAfterFallback(true, record(0, 1), 0, 1, "3.4.0");
    TEST_ASSERT_TRUE(fwRecordForgetOther(true, r, 0, false));
}

static void test_forget_other_after_unreachable_rollback() {
    // B on trial was not reached and wrote the record that makes A active again; A runs
    BootRecord r = fwRecordUnreachable(1, "3.4.0");
    TEST_ASSERT_TRUE(fwRecordForgetOther(true, r, 0, false));
}

static void test_forget_other_not_when_already_erased() {
    // Erased at an earlier start: no second erase at every boot
    BootRecord r = fwRecordUnreachable(1, "3.4.0");
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, r, 0, true));
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, fwRecordAfterFallback(true, record(0, 1), 0, 1, "3.4.0"), 0, true));
}

static void test_forget_other_not_on_the_failed_slot_itself() {
    // The unreachable trial has just written its record and not restarted yet (it runs slot B)
    BootRecord r = fwRecordUnreachable(1, "3.4.0");
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, r, 1, false));
}

static void test_forget_other_not_after_install_upload_or_fallback() {
    // Kept firmware: the other slot holds the previous release, the way back
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, fwRecordConfirmed(0, "3.4.0"), 0, false));
    // Uploaded, restart pending: the other slot is the new release
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, fwRecordTrialOfOther(0, "3.4.0"), 0, false));
    // The kept firmware crashed 3 times: the crashing slot stays, it is the only other bootable one
    TEST_ASSERT_FALSE(
        fwRecordForgetOther(true, fwRecordAfterFallback(true, record(1, FW_NO_SLOT), 0, 1, "3.3.1"), 0, false));
    BootRecord none = {};
    none.lastResult = FwResult::RolledBackCrash;  // garbage: ignored without a record
    TEST_ASSERT_FALSE(fwRecordForgetOther(false, none, 0, false));
}

static void test_forget_other_not_while_a_trial_is_set() {
    BootRecord r = fwRecordUnreachable(1, "3.4.0");
    r.trial = 1;  // not a record the firmware writes, but never erase a slot the record still boots
    TEST_ASSERT_FALSE(fwRecordForgetOther(true, r, 0, false));
}

static void test_forgotten_slot_is_never_fallen_back_to() {
    // After the rollback the failed slot is erased (does not verify): when the kept
    // firmware then reaches the crash limit, the boot program stays on it
    BootRecord r = fwRecordAfterFallback(true, record(0, 1), 0, 1, "3.4.0");
    TEST_ASSERT_TRUE(fwRecordForgetOther(true, r, 0, false));
    const bool ok[2] = {true, false};  // slot B erased
    uint32_t scratch = 0xC7A50000u | 2;  // slot A, 2 crashes so far
    BootDecision d = decideBoot(true, r, ok, ResetKind::Crash, scratch);
    TEST_ASSERT_EQUAL_UINT8(0, d.slot);
    TEST_ASSERT_EQUAL(BootReason::Normal, d.reason);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fallback_from_trial_slot_is_rolled_back_crash);
    RUN_TEST(test_fallback_from_kept_slot_is_fallback_crash);
    RUN_TEST(test_fallback_without_record_is_fallback_crash);
    RUN_TEST(test_fallback_with_unknown_failed_slot_is_fallback_crash);
    RUN_TEST(test_fallback_record_is_valid_for_the_boot_program);
    RUN_TEST(test_confirmed_keeps_running_slot_with_trap);
    RUN_TEST(test_unreachable_goes_back_to_other_slot_with_trap);
    RUN_TEST(test_trial_of_other_slot_keeps_running_slot_active);
    RUN_TEST(test_long_version_is_cut_and_terminated);
    RUN_TEST(test_trap_sent_clears_only_the_trap_flag);
    RUN_TEST(test_settled_without_record_or_with_running_slot_active);
    RUN_TEST(test_not_settled_while_trial_pending_or_other_slot_active);
    RUN_TEST(test_crash_limit_is_crash_reset_with_count_zero);
    RUN_TEST(test_failed_check_is_any_other_reset_or_a_count);
    RUN_TEST(test_forget_other_after_crash_rollback);
    RUN_TEST(test_forget_other_after_unreachable_rollback);
    RUN_TEST(test_forget_other_not_when_already_erased);
    RUN_TEST(test_forget_other_not_on_the_failed_slot_itself);
    RUN_TEST(test_forget_other_not_after_install_upload_or_fallback);
    RUN_TEST(test_forget_other_not_while_a_trial_is_set);
    RUN_TEST(test_forgotten_slot_is_never_fallen_back_to);
    return UNITY_END();
}
