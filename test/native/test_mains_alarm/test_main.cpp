// Host unit tests for lib/power_monitor: the alarm raised once mains has been lost for 60 s.
// Run: pio test -e native -f native/test_mains_alarm

#include <unity.h>
#include <mains_alarm.h>

void setUp() {}
void tearDown() {}

static void test_alarm_not_raised_before_60_s() {
    MainsAlarm a;
    a.update(false, 1, 1000, 60999);
    TEST_ASSERT_FALSE(a.raised());
    TEST_ASSERT_FALSE(a.trapPending());
}

static void test_alarm_raised_at_60_s() {
    MainsAlarm a;
    a.update(false, 1, 1000, 30000);
    a.update(false, 1, 1000, 61000);
    TEST_ASSERT_TRUE(a.raised());
    TEST_ASSERT_TRUE(a.trapPending());
}

static void test_restore_clears_alarm_and_pending_trap() {
    MainsAlarm a;
    a.update(false, 1, 1000, 61000);
    a.update(true, 2, 70000, 70000);
    TEST_ASSERT_FALSE(a.raised());
    TEST_ASSERT_FALSE(a.trapPending());
}

static void test_restore_at_the_mark_raises_nothing() {
    MainsAlarm a;
    a.update(false, 1, 1000, 30000);
    a.update(true, 2, 60500, 61000);
    TEST_ASSERT_FALSE(a.raised());
    TEST_ASSERT_FALSE(a.trapPending());
}

static void test_quick_restore_and_loss_restarts_the_count() {
    MainsAlarm a;
    a.update(false, 1, 0, 61000);
    a.trapSent();
    // Restored and lost again between two updates: still lost, but a new outage from 70000
    a.update(false, 3, 70000, 71000);
    TEST_ASSERT_FALSE(a.raised());
    TEST_ASSERT_FALSE(a.trapPending());
    a.update(false, 3, 70000, 130000);
    TEST_ASSERT_TRUE(a.raised());
    TEST_ASSERT_TRUE(a.trapPending());
}

static void test_boot_without_mains_counts_from_boot() {
    MainsAlarm a;
    a.update(false, 0, 0, 59999);
    TEST_ASSERT_FALSE(a.raised());
    a.update(false, 0, 0, 60000);
    TEST_ASSERT_TRUE(a.raised());
}

static void test_mains_present_never_raises() {
    MainsAlarm a;
    a.update(true, 0, 0, 10000000);
    a.update(true, 2, 5000, 20000000);
    TEST_ASSERT_FALSE(a.raised());
    TEST_ASSERT_FALSE(a.trapPending());
}

static void test_pending_trap_survives_until_sent_once() {
    MainsAlarm a;
    a.update(false, 1, 1000, 61000);
    a.update(false, 1, 1000, 500000);
    TEST_ASSERT_TRUE(a.trapPending());
    a.trapSent();
    TEST_ASSERT_FALSE(a.trapPending());
    a.update(false, 1, 1000, 900000);
    TEST_ASSERT_FALSE(a.trapPending());
    TEST_ASSERT_TRUE(a.raised());
}

static void test_new_outage_after_restore_sends_a_new_trap() {
    MainsAlarm a;
    a.update(false, 1, 1000, 61000);
    a.trapSent();
    a.update(true, 2, 70000, 70000);
    a.update(false, 3, 80000, 80000);
    TEST_ASSERT_FALSE(a.trapPending());
    a.update(false, 3, 80000, 140000);
    TEST_ASSERT_TRUE(a.raised());
    TEST_ASSERT_TRUE(a.trapPending());
}

static void test_alarm_stays_raised_across_millis_wrap() {
    MainsAlarm a;
    const uint32_t lost = 0xFFFF0000u;
    a.update(false, 1, lost, lost + 60000u);
    TEST_ASSERT_TRUE(a.raised());
    a.update(false, 1, lost, lost + 0x80000000u);
    TEST_ASSERT_TRUE(a.raised());
    a.update(false, 1, lost, lost + 0xC0000000u);
    a.update(false, 1, lost, lost + 1000u);  // 49.7 days + 1 s on: the elapsed time looks like 1 s
    TEST_ASSERT_TRUE(a.raised());
}

static void test_lost_timing_handles_millis_wrap() {
    MainsAlarm a;
    a.update(false, 1, 0xFFFFFF00u, 0x0000E95Fu);
    TEST_ASSERT_FALSE(a.raised());
    a.update(false, 1, 0xFFFFFF00u, 0x0000E960u);
    TEST_ASSERT_TRUE(a.raised());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_alarm_not_raised_before_60_s);
    RUN_TEST(test_alarm_raised_at_60_s);
    RUN_TEST(test_restore_clears_alarm_and_pending_trap);
    RUN_TEST(test_restore_at_the_mark_raises_nothing);
    RUN_TEST(test_quick_restore_and_loss_restarts_the_count);
    RUN_TEST(test_boot_without_mains_counts_from_boot);
    RUN_TEST(test_mains_present_never_raises);
    RUN_TEST(test_pending_trap_survives_until_sent_once);
    RUN_TEST(test_new_outage_after_restore_sends_a_new_trap);
    RUN_TEST(test_alarm_stays_raised_across_millis_wrap);
    RUN_TEST(test_lost_timing_handles_millis_wrap);
    return UNITY_END();
}
