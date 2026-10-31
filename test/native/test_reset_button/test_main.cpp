// Host unit tests for the factory reset button logic (lib/power_monitor/reset_button).
// Run: pio test -e native -f native/test_reset_button

#include <unity.h>
#include <reset_button.h>

void setUp() {}
void tearDown() {}

static const uint32_t HOLD = ResetButton::DEFAULT_HOLD_MS;
static const uint32_t DEB = ResetButton::DEBOUNCE_MS;

/// Feed a constant level every millisecond from `from` to `to` inclusive; return the first non-None event
static ResetButtonEvent feed(ResetButton& b, bool pressed, uint32_t from, uint32_t to, uint32_t* at = nullptr) {
    ResetButtonEvent first = ResetButtonEvent::None;
    for (uint32_t t = from; t != to + 1; t++) {
        ResetButtonEvent e = b.update(pressed, t);
        if (e != ResetButtonEvent::None && first == ResetButtonEvent::None) {
            first = e;
            if (at) *at = t;
        }
    }
    return first;
}

static void test_idle_button_does_nothing_and_led_is_off() {
    ResetButton b;
    b.begin(false, 0);
    TEST_ASSERT_EQUAL(ResetButtonEvent::None, feed(b, false, 1, 60000));
    TEST_ASSERT_FALSE(b.pressed());
    TEST_ASSERT_FALSE(b.ledOn(30000));
}

static void test_hold_for_ten_seconds_triggers_once() {
    ResetButton b;
    b.begin(false, 0);
    uint32_t pressedAt = 0, triggeredAt = 0;
    TEST_ASSERT_EQUAL(ResetButtonEvent::Pressed, feed(b, true, 1000, 1000 + DEB, &pressedAt));
    TEST_ASSERT_EQUAL(ResetButtonEvent::Triggered, feed(b, true, 1000 + DEB + 1, 1000 + HOLD + 500, &triggeredAt));
    // Hold time counts from the debounced press
    TEST_ASSERT_EQUAL_UINT32(pressedAt + HOLD, triggeredAt);
    TEST_ASSERT_TRUE(b.triggered());
    // Keeps holding: no second trigger
    TEST_ASSERT_EQUAL(ResetButtonEvent::None, feed(b, true, 1000 + HOLD + 501, 1000 + 3 * HOLD));
}

static void test_trigger_is_within_spec_of_the_physical_press() {
    // Requirement: 10 s hold; the debounce may add at most DEBOUNCE_MS
    ResetButton b;
    b.begin(false, 0);
    uint32_t trig = 0;
    for (uint32_t t = 500; t < 500 + HOLD + 100; t++) {
        if (b.update(true, t) == ResetButtonEvent::Triggered) {
            trig = t;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(500 + HOLD, trig);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(500 + HOLD + DEB, trig);
}

static void test_short_press_releases_without_reset() {
    ResetButton b;
    b.begin(false, 0);
    feed(b, true, 100, 100 + 3000);
    TEST_ASSERT_EQUAL(ResetButtonEvent::Released, feed(b, false, 3101, 3101 + DEB + 5));
    TEST_ASSERT_FALSE(b.triggered());
}

static void test_release_just_before_hold_time_does_not_trigger() {
    ResetButton b;
    b.begin(false, 0);
    uint32_t pressedAt = 0;
    feed(b, true, 0, DEB, &pressedAt);
    TEST_ASSERT_EQUAL(ResetButtonEvent::None, feed(b, true, DEB + 1, pressedAt + HOLD - 1));
    feed(b, false, pressedAt + HOLD, pressedAt + HOLD + 100);
    TEST_ASSERT_FALSE(b.triggered());
}

static void test_contact_bounce_does_not_restart_or_trigger() {
    ResetButton b;
    b.begin(false, 0);
    // 5 ms glitches are filtered, so they never count as a press
    for (uint32_t t = 0; t < 2000; t += 100) {
        feed(b, true, t, t + 5);
        feed(b, false, t + 6, t + 99);
    }
    TEST_ASSERT_FALSE(b.pressed());
    // Bounce at the start of a real press: hold time still counts from the debounced press
    feed(b, true, 3000, 3003);
    feed(b, false, 3004, 3006);
    uint32_t pressedAt = 0, trig = 0;
    feed(b, true, 3007, 3007 + DEB, &pressedAt);
    feed(b, true, 3008 + DEB, 3007 + DEB + HOLD + 10, &trig);
    TEST_ASSERT_EQUAL_UINT32(pressedAt + HOLD, trig);
}

static void test_button_held_at_boot_must_be_released_first() {
    // Avoids a reset loop if the button is stuck or held while powering up
    ResetButton b;
    b.begin(true, 0);
    TEST_ASSERT_EQUAL(ResetButtonEvent::None, feed(b, true, 1, 3 * HOLD));
    TEST_ASSERT_FALSE(b.triggered());
    feed(b, false, 3 * HOLD + 1, 3 * HOLD + 100);
    TEST_ASSERT_EQUAL(ResetButtonEvent::Pressed, feed(b, true, 3 * HOLD + 101, 3 * HOLD + 200));
    TEST_ASSERT_EQUAL(ResetButtonEvent::Triggered, feed(b, true, 3 * HOLD + 201, 4 * HOLD + 300));
}

static void test_held_ms_counts_from_debounced_press() {
    ResetButton b;
    b.begin(false, 0);
    uint32_t pressedAt = 0;
    feed(b, true, 1000, 1000 + DEB, &pressedAt);
    TEST_ASSERT_EQUAL_UINT32(0, b.heldMs(pressedAt));
    feed(b, true, pressedAt + 1, pressedAt + 4000);
    TEST_ASSERT_EQUAL_UINT32(4000, b.heldMs(pressedAt + 4000));
    feed(b, false, pressedAt + 4001, pressedAt + 4100);
    TEST_ASSERT_EQUAL_UINT32(0, b.heldMs(pressedAt + 4100));
}

// The serial log reports this at the Released event, when heldMs() is already 0
static void test_released_press_reports_how_long_it_lasted() {
    ResetButton b;
    b.begin(false, 0);
    feed(b, true, 1000, 3999);
    uint32_t releasedAt = 0;
    TEST_ASSERT_EQUAL(ResetButtonEvent::Released, feed(b, false, 4000, 4100, &releasedAt));
    TEST_ASSERT_EQUAL_UINT32(0, b.heldMs(releasedAt));
    TEST_ASSERT_EQUAL_UINT32(3000, b.lastPressMs());
}

static void test_last_press_ms_is_zero_before_any_release() {
    ResetButton b;
    b.begin(false, 0);
    TEST_ASSERT_EQUAL_UINT32(0, b.lastPressMs());
    feed(b, true, 0, 2000);
    TEST_ASSERT_EQUAL_UINT32(0, b.lastPressMs());
}

static void test_led_blinks_slowly_while_held_and_fast_after_trigger() {
    ResetButton b;
    b.begin(false, 0);
    uint32_t pressedAt = 0;
    feed(b, true, 0, DEB, &pressedAt);
    TEST_ASSERT_TRUE(b.ledOn(pressedAt));
    TEST_ASSERT_FALSE(b.ledOn(pressedAt + ResetButton::HOLD_BLINK_MS));
    TEST_ASSERT_TRUE(b.ledOn(pressedAt + 2 * ResetButton::HOLD_BLINK_MS));

    uint32_t trig = 0;
    feed(b, true, pressedAt + 1, pressedAt + HOLD + 5, &trig);
    TEST_ASSERT_TRUE(b.ledOn(trig));
    TEST_ASSERT_FALSE(b.ledOn(trig + ResetButton::TRIGGERED_BLINK_MS));
    TEST_ASSERT_TRUE(b.ledOn(trig + 2 * ResetButton::TRIGGERED_BLINK_MS));
    // Keeps blinking fast after release, until the reboot
    feed(b, false, trig + 1, trig + 200);
    TEST_ASSERT_TRUE(b.ledOn(trig + 4 * ResetButton::TRIGGERED_BLINK_MS));
}

static void test_works_across_millis_wraparound() {
    ResetButton b;
    const uint32_t start = 0xFFFFFFFFu - 5000;
    b.begin(false, start - 10);
    uint32_t pressedAt = 0, trig = 0;
    feed(b, true, start, start + DEB, &pressedAt);
    feed(b, true, pressedAt + 1, pressedAt + HOLD + 5, &trig);
    TEST_ASSERT_EQUAL_UINT32(pressedAt + HOLD, trig);
}

static void test_short_hold_time_for_testing() {
    ResetButton b(500);
    b.begin(false, 0);
    uint32_t pressedAt = 0, trig = 0;
    feed(b, true, 0, DEB, &pressedAt);
    feed(b, true, DEB + 1, 2000, &trig);
    TEST_ASSERT_EQUAL_UINT32(pressedAt + 500, trig);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_button_does_nothing_and_led_is_off);
    RUN_TEST(test_hold_for_ten_seconds_triggers_once);
    RUN_TEST(test_trigger_is_within_spec_of_the_physical_press);
    RUN_TEST(test_short_press_releases_without_reset);
    RUN_TEST(test_release_just_before_hold_time_does_not_trigger);
    RUN_TEST(test_released_press_reports_how_long_it_lasted);
    RUN_TEST(test_last_press_ms_is_zero_before_any_release);
    RUN_TEST(test_contact_bounce_does_not_restart_or_trigger);
    RUN_TEST(test_button_held_at_boot_must_be_released_first);
    RUN_TEST(test_held_ms_counts_from_debounced_press);
    RUN_TEST(test_led_blinks_slowly_while_held_and_fast_after_trigger);
    RUN_TEST(test_works_across_millis_wraparound);
    RUN_TEST(test_short_hold_time_for_testing);
    return UNITY_END();
}
