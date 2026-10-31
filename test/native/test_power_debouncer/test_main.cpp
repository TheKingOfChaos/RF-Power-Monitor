// Host unit tests for lib/power_monitor: debouncing of the mains detection input.
// Run: pio test -e native -f native/test_power_debouncer

#include <unity.h>
#include <power_debouncer.h>

void setUp() {}
void tearDown() {}

static const uint32_t DEBOUNCE_MS = 5;

/// Feed a constant level once per millisecond from `from` to `to` (inclusive)
static void hold(PowerDebouncer& d, bool level, uint32_t from, uint32_t to) {
    for (uint32_t t = from; t != to + 1; t++) d.update(level, t);
}

static void test_begin_sets_initial_state_without_counting_a_change() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 100);
    TEST_ASSERT_TRUE(d.state());
    TEST_ASSERT_EQUAL_UINT32(0, d.changes());

    PowerDebouncer off(DEBOUNCE_MS);
    off.begin(false, 100);
    TEST_ASSERT_FALSE(off.state());
}

static void test_stable_input_never_changes_state() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    hold(d, true, 1, 1000);
    TEST_ASSERT_TRUE(d.state());
    TEST_ASSERT_EQUAL_UINT32(0, d.changes());
}

static void test_change_is_accepted_after_debounce_time() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    // Input drops at t=10. Not yet accepted before 10 + DEBOUNCE_MS
    for (uint32_t t = 10; t < 10 + DEBOUNCE_MS; t++) {
        TEST_ASSERT_FALSE(d.update(false, t));
        TEST_ASSERT_TRUE(d.state());
    }
    TEST_ASSERT_TRUE(d.update(false, 10 + DEBOUNCE_MS));
    TEST_ASSERT_FALSE(d.state());
    TEST_ASSERT_EQUAL_UINT32(1, d.changes());
    TEST_ASSERT_EQUAL_UINT32(10 + DEBOUNCE_MS, d.lastChangeMs());
}

static void test_detection_meets_10ms_requirement() {
    // .clinerules: the firmware's share of mains detection (GPIO27 change to detection) is 10 ms
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    uint32_t detectedAt = 0;
    for (uint32_t t = 50; t < 100; t++) {
        if (d.update(false, t)) { detectedAt = t; break; }
    }
    TEST_ASSERT_NOT_EQUAL(0, detectedAt);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(10, detectedAt - 50);
}

static void test_glitch_shorter_than_debounce_is_ignored() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    hold(d, false, 10, 10 + DEBOUNCE_MS - 2);  // Too short
    hold(d, true, 10 + DEBOUNCE_MS - 1, 100);
    TEST_ASSERT_TRUE(d.state());
    TEST_ASSERT_EQUAL_UINT32(0, d.changes());
}

static void test_bouncing_restarts_the_debounce_timer() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    // Contact bounce: low, high, low ... each shorter than the debounce time
    for (uint32_t t = 10; t < 40; t += 2) {
        d.update(false, t);
        d.update(true, t + 1);
    }
    TEST_ASSERT_TRUE(d.state());
    // Then it settles low and must be accepted DEBOUNCE_MS after settling
    hold(d, false, 40, 40 + DEBOUNCE_MS - 1);
    TEST_ASSERT_TRUE(d.state());
    d.update(false, 40 + DEBOUNCE_MS);
    TEST_ASSERT_FALSE(d.state());
    TEST_ASSERT_EQUAL_UINT32(1, d.changes());
}

static void test_counts_every_accepted_change() {
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    hold(d, false, 1, 20);
    hold(d, true, 21, 40);
    hold(d, false, 41, 60);
    TEST_ASSERT_FALSE(d.state());
    TEST_ASSERT_EQUAL_UINT32(3, d.changes());
}

static void test_works_across_millis_wraparound() {
    PowerDebouncer d(DEBOUNCE_MS);
    const uint32_t start = 0xFFFFFFFFu - 2;
    d.begin(true, start - 10);
    uint32_t t = start;
    bool changed = false;
    for (int i = 0; i <= (int)DEBOUNCE_MS; i++, t++) changed = d.update(false, t);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_FALSE(d.state());
    TEST_ASSERT_EQUAL_UINT32(start + DEBOUNCE_MS, d.lastChangeMs());
}

static void test_zero_debounce_follows_input_immediately() {
    PowerDebouncer d(0);
    d.begin(false, 0);
    TEST_ASSERT_TRUE(d.update(true, 1));
    TEST_ASSERT_TRUE(d.state());
}

static void test_sparse_samples_use_elapsed_time() {
    // If sampling is delayed, a level held across the gap still counts
    PowerDebouncer d(DEBOUNCE_MS);
    d.begin(true, 0);
    TEST_ASSERT_FALSE(d.update(false, 100));
    TEST_ASSERT_TRUE(d.update(false, 100 + DEBOUNCE_MS + 20));
    TEST_ASSERT_FALSE(d.state());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_sets_initial_state_without_counting_a_change);
    RUN_TEST(test_stable_input_never_changes_state);
    RUN_TEST(test_change_is_accepted_after_debounce_time);
    RUN_TEST(test_detection_meets_10ms_requirement);
    RUN_TEST(test_glitch_shorter_than_debounce_is_ignored);
    RUN_TEST(test_bouncing_restarts_the_debounce_timer);
    RUN_TEST(test_counts_every_accepted_change);
    RUN_TEST(test_works_across_millis_wraparound);
    RUN_TEST(test_zero_debounce_follows_input_immediately);
    RUN_TEST(test_sparse_samples_use_elapsed_time);
    return UNITY_END();
}
