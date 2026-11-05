// Host unit tests for lib/watchdog_logic: core 1 heartbeat check and the restart record.
// Run: pio test -e native -f native/test_watchdog_logic

#include <unity.h>
#include <string.h>
#include <watchdog_logic.h>
#include <boot_decision.h>

void setUp() {}
void tearDown() {}

static const uint32_t STALE_MS = 1000;

// ---- heartbeat ----

static void test_heartbeat_alive_right_after_begin() {
    CoreHeartbeatCheck h(STALE_MS);
    h.begin(7, 5000);
    TEST_ASSERT_TRUE(h.alive(7, 5000));
}

static void test_heartbeat_stays_alive_while_the_counter_moves() {
    CoreHeartbeatCheck h(STALE_MS);
    h.begin(0, 0);
    for (uint32_t t = 1; t <= 10000; t++) TEST_ASSERT_TRUE(h.alive(t / 500, t));  // moves every 500 ms
}

static void test_heartbeat_stale_counter_is_hung_after_exactly_stale_ms() {
    CoreHeartbeatCheck h(STALE_MS);
    h.begin(3, 2000);
    TEST_ASSERT_TRUE(h.alive(3, 2000 + STALE_MS));
    TEST_ASSERT_FALSE(h.alive(3, 2000 + STALE_MS + 1));
}

static void test_heartbeat_recovers_when_the_counter_moves_again() {
    CoreHeartbeatCheck h(STALE_MS);
    h.begin(3, 0);
    TEST_ASSERT_FALSE(h.alive(3, 5000));
    TEST_ASSERT_TRUE(h.alive(4, 5001));
}

static void test_heartbeat_handles_counter_and_millis_wrap() {
    CoreHeartbeatCheck h(STALE_MS);
    h.begin(0xFFFFFFFFu, 0xFFFFFF00u);
    TEST_ASSERT_TRUE(h.alive(0, 0x00000100u));  // both wrapped, counter moved
    TEST_ASSERT_TRUE(h.alive(0, 0x00000100u + STALE_MS));
    TEST_ASSERT_FALSE(h.alive(0, 0x00000100u + STALE_MS + 1));
}

// ---- restart record ----

static RestartRecord garbage() {
    RestartRecord r;
    memset(&r, 0xA5, sizeof(r));
    return r;
}

static void test_record_starts_at_zero_after_power_on() {
    RestartRecord r = garbage();
    TEST_ASSERT_EQUAL_UINT32(0, restartRecordUpdate(r, RestartCause::PowerOn));
}

static void test_record_counts_watchdog_restarts() {
    RestartRecord r = garbage();
    restartRecordUpdate(r, RestartCause::PowerOn);
    TEST_ASSERT_EQUAL_UINT32(1, restartRecordUpdate(r, RestartCause::Watchdog));
    TEST_ASSERT_EQUAL_UINT32(1, restartRecordUpdate(r, RestartCause::Reboot));  // a normal reboot keeps the count
    TEST_ASSERT_EQUAL_UINT32(1, restartRecordUpdate(r, RestartCause::ResetPin));
    TEST_ASSERT_EQUAL_UINT32(2, restartRecordUpdate(r, RestartCause::Watchdog));
}

static void test_record_power_on_clears_the_count() {
    RestartRecord r = garbage();
    restartRecordUpdate(r, RestartCause::PowerOn);
    restartRecordUpdate(r, RestartCause::Watchdog);
    TEST_ASSERT_EQUAL_UINT32(0, restartRecordUpdate(r, RestartCause::PowerOn));
}

static void test_record_with_random_ram_is_not_trusted() {
    RestartRecord r = garbage();
    TEST_ASSERT_EQUAL_UINT32(0, restartRecordUpdate(r, RestartCause::Reboot));
    r = garbage();
    TEST_ASSERT_EQUAL_UINT32(1, restartRecordUpdate(r, RestartCause::Watchdog));  // this restart, nothing before
}

static void test_record_with_a_corrupted_count_is_not_trusted() {
    RestartRecord r = garbage();
    restartRecordUpdate(r, RestartCause::PowerOn);
    restartRecordUpdate(r, RestartCause::Watchdog);
    restartRecordUpdate(r, RestartCause::Watchdog);
    r.watchdogRestarts ^= 0x10;  // one bit flipped, check no longer matches
    TEST_ASSERT_EQUAL_UINT32(0, restartRecordUpdate(r, RestartCause::Reboot));
}

static void test_record_count_saturates() {
    RestartRecord r = garbage();
    restartRecordUpdate(r, RestartCause::PowerOn);
    // The check is the count xor a constant: forge a valid record at the maximum
    uint32_t key = r.check ^ r.watchdogRestarts;
    r.watchdogRestarts = 0xFFFFFFFFu;
    r.check = key ^ 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, restartRecordUpdate(r, RestartCause::Watchdog));
}

static void test_every_restart_cause_has_a_name() {
    for (uint8_t c = 0; c <= (uint8_t)RestartCause::Watchdog; c++) {
        TEST_ASSERT_TRUE(strcmp("?", restartCauseName((RestartCause)c)) != 0);
    }
    TEST_ASSERT_EQUAL_STRING("watchdog", restartCauseName(RestartCause::Watchdog));
}

static void test_boot_info_maps_each_reset_kind_to_a_restart_cause() {
    const ResetKind kinds[] = {ResetKind::PowerOn, ResetKind::ResetPin, ResetKind::Planned, ResetKind::Crash};
    const RestartCause want[] = {RestartCause::PowerOn, RestartCause::ResetPin, RestartCause::Reboot,
                                 RestartCause::Watchdog};
    for (int i = 0; i < 4; i++) {
        RestartCause c = RestartCause::Unknown;
        TEST_ASSERT_TRUE(restartCauseFromBootInfo(bootInfoEncode(0, BootReason::Normal, kinds[i], FW_NO_SLOT), c));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)want[i], (uint8_t)c);
    }
}

static void test_boot_info_without_magic_gives_no_cause() {
    RestartCause c = RestartCause::PowerOn;
    TEST_ASSERT_FALSE(restartCauseFromBootInfo(0, c));
    TEST_ASSERT_FALSE(restartCauseFromBootInfo(0xFFFFFFFFu, c));
    TEST_ASSERT_FALSE(restartCauseFromBootInfo(0xB1000000u | (9u << 12), c));  // reset kind 9
    TEST_ASSERT_EQUAL_UINT8((uint8_t)RestartCause::PowerOn, (uint8_t)c);       // left alone
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_heartbeat_alive_right_after_begin);
    RUN_TEST(test_heartbeat_stays_alive_while_the_counter_moves);
    RUN_TEST(test_heartbeat_stale_counter_is_hung_after_exactly_stale_ms);
    RUN_TEST(test_heartbeat_recovers_when_the_counter_moves_again);
    RUN_TEST(test_heartbeat_handles_counter_and_millis_wrap);
    RUN_TEST(test_record_starts_at_zero_after_power_on);
    RUN_TEST(test_record_counts_watchdog_restarts);
    RUN_TEST(test_record_power_on_clears_the_count);
    RUN_TEST(test_record_with_random_ram_is_not_trusted);
    RUN_TEST(test_record_with_a_corrupted_count_is_not_trusted);
    RUN_TEST(test_record_count_saturates);
    RUN_TEST(test_every_restart_cause_has_a_name);
    RUN_TEST(test_boot_info_maps_each_reset_kind_to_a_restart_cause);
    RUN_TEST(test_boot_info_without_magic_gives_no_cause);
    return UNITY_END();
}
