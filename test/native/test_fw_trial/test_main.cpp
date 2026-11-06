// Host unit tests for the trial monitor (lib/fw_update): decides when a firmware
// on trial is confirmed (a valid request arrived) or given up (unreachable).
// Run: pio test -e native -f native/test_fw_trial

#include <unity.h>
#include <trial_monitor.h>

void setUp() {}
void tearDown() {}

static void test_trial_confirms_after_valid_request() {
    TrialMonitor m;
    m.begin(1000);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(2000, true));
    m.noteValidRequest();
    TEST_ASSERT_EQUAL(TrialVerdict::Confirmed, m.poll(2010, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Confirmed, m.poll(FW_TRIAL_LIMIT_MS * 2, false));  // stays confirmed
}

static void test_trial_unreachable_after_10_min_with_network() {
    TrialMonitor m;
    m.begin(0);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(0, true));  // the network is up from the start
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(1, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(FW_TRIAL_REACH_MS - 1, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Unreachable, m.poll(FW_TRIAL_REACH_MS, true));
}

static void test_trial_interval_before_network_came_up_does_not_count() {
    // Down at t=0, up at the next poll: the interval between them is not network time
    TrialMonitor m;
    m.begin(0);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(0, false));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(FW_TRIAL_REACH_MS, true));
    // From here on it counts
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(FW_TRIAL_REACH_MS + FW_TRIAL_REACH_MS - 1, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Unreachable, m.poll(FW_TRIAL_REACH_MS + FW_TRIAL_REACH_MS, true));
}

static void test_trial_interval_after_network_went_down_does_not_count() {
    TrialMonitor m;
    m.begin(0);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(0, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(1000, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(1001, false));  // down: counts the 1 ms before it
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(1001 + FW_TRIAL_REACH_MS, false));
}

static void test_trial_time_without_network_does_not_count() {
    TrialMonitor m;
    m.begin(0);
    // 20 minutes without network: well past the reach time, still pending
    for (uint32_t t = 60000; t <= 1200000; t += 60000) {
        TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(t, false));
    }
    // Network arrives: the reach time counts from here
    uint32_t up = 1200000;
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(up, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(up + FW_TRIAL_REACH_MS - 1, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Unreachable, m.poll(up + FW_TRIAL_REACH_MS, true));
}

static void test_trial_hard_limit_60_min_without_network() {
    TrialMonitor m;
    m.begin(5000);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(5000 + FW_TRIAL_LIMIT_MS - 1, false));
    TEST_ASSERT_EQUAL(TrialVerdict::Unreachable, m.poll(5000 + FW_TRIAL_LIMIT_MS, false));
}

static void test_trial_survives_millis_wraparound() {
    TrialMonitor m;
    uint32_t start = 0xFFFFFFFFu - 100000u;  // 100 s before the wrap
    m.begin(start);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(start, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(start + 50000u, true));
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, m.poll(start + FW_TRIAL_REACH_MS - 1, true));  // wrapped past 0
    TEST_ASSERT_EQUAL(TrialVerdict::Unreachable, m.poll(start + FW_TRIAL_REACH_MS, true));

    TrialMonitor n;
    n.begin(start);
    TEST_ASSERT_EQUAL(TrialVerdict::Pending, n.poll(start + 200000u, false));  // across the wrap, no network
    n.noteValidRequest();
    TEST_ASSERT_EQUAL(TrialVerdict::Confirmed, n.poll(start + 200001u, true));
}

static void test_trial_request_before_network_still_confirms() {
    TrialMonitor m;
    m.begin(0);
    m.noteValidRequest();  // e.g. a request that arrived before the first poll saw the network
    TEST_ASSERT_EQUAL(TrialVerdict::Confirmed, m.poll(100, false));
}

static void test_crash_count_kept_on_trial_after_healthy_time() {
    // A trial firmware that hangs every ~6 min must still reach the crash limit
    TEST_ASSERT_FALSE(fwShouldClearCrashCount(true, false, FW_HEALTHY_MS));
    TEST_ASSERT_FALSE(fwShouldClearCrashCount(true, false, FW_HEALTHY_MS * 10));
}

static void test_crash_count_cleared_when_trial_confirmed() {
    TEST_ASSERT_TRUE(fwShouldClearCrashCount(true, true, 0));
    TEST_ASSERT_TRUE(fwShouldClearCrashCount(false, true, 0));
}

static void test_crash_count_cleared_after_healthy_time_when_kept() {
    TEST_ASSERT_FALSE(fwShouldClearCrashCount(false, false, 0));
    TEST_ASSERT_FALSE(fwShouldClearCrashCount(false, false, FW_HEALTHY_MS - 1));
    TEST_ASSERT_TRUE(fwShouldClearCrashCount(false, false, FW_HEALTHY_MS));
    TEST_ASSERT_TRUE(fwShouldClearCrashCount(false, false, 0xFFFFFFFFu));
}

static void test_net_ready_needs_link_and_ipv4_or_routable_ipv6() {
    TEST_ASSERT_TRUE(fwTrialNetReady(true, true, false));
    TEST_ASSERT_TRUE(fwTrialNetReady(true, false, true));
    TEST_ASSERT_TRUE(fwTrialNetReady(true, true, true));
}

static void test_net_not_ready_with_link_local_only() {
    // Link up but DHCP and SLAAC failed: only link-local, which is always there
    TEST_ASSERT_FALSE(fwTrialNetReady(true, false, false));
}

static void test_net_not_ready_without_link() {
    // A static IPv4 address stays configured when the cable is out
    TEST_ASSERT_FALSE(fwTrialNetReady(false, true, true));
    TEST_ASSERT_FALSE(fwTrialNetReady(false, false, false));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_trial_confirms_after_valid_request);
    RUN_TEST(test_trial_unreachable_after_10_min_with_network);
    RUN_TEST(test_trial_interval_before_network_came_up_does_not_count);
    RUN_TEST(test_trial_interval_after_network_went_down_does_not_count);
    RUN_TEST(test_trial_time_without_network_does_not_count);
    RUN_TEST(test_trial_hard_limit_60_min_without_network);
    RUN_TEST(test_trial_survives_millis_wraparound);
    RUN_TEST(test_trial_request_before_network_still_confirms);
    RUN_TEST(test_crash_count_kept_on_trial_after_healthy_time);
    RUN_TEST(test_crash_count_cleared_when_trial_confirmed);
    RUN_TEST(test_crash_count_cleared_after_healthy_time_when_kept);
    RUN_TEST(test_net_ready_needs_link_and_ipv4_or_routable_ipv6);
    RUN_TEST(test_net_not_ready_with_link_local_only);
    RUN_TEST(test_net_not_ready_without_link);
    return UNITY_END();
}
