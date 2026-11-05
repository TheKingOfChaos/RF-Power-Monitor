// On-device tests for lib/mains_input: GPIO27 sampling on core 1 and hand-over to core 0.
// Works with or without the mains circuit connected; it checks that the state
// published by core 1 tracks the pin, not what the pin level is.
// Run: pio test -e pico -f embedded/test_mains_input

#include <Arduino.h>
#include <hardware/watchdog.h>
#include <unity.h>
#include <mains_input.h>
#include "pins.h"

// Core 1: same sampler as the firmware
void setup1() {
    mainsBegin();
}

void loop1() {
    mainsSample();
    delay(1);
}

void setUp() {}
void tearDown() {}

static void test_core1_publishes_a_snapshot() {
    MainsSnapshot s;
    uint32_t start = millis();
    while (!mainsRead(s) && millis() - start < 500) delay(1);
    TEST_ASSERT_TRUE_MESSAGE(mainsRead(s), "core 1 never published a reading");
}

static void test_core1_samples_at_about_1khz() {
    MainsSnapshot a, b;
    TEST_ASSERT_TRUE(mainsRead(a));
    delay(1000);
    TEST_ASSERT_TRUE(mainsRead(b));
    uint32_t rate = b.samples - a.samples;
    Serial.printf("Sample rate: %lu per second\n", (unsigned long)rate);
    // Detection within 10 ms needs at least ~1 sample per ms
    TEST_ASSERT_UINT32_WITHIN(150, 950, rate);
}

static void test_snapshots_are_consistent_while_core1_writes() {
    // Hammer the reader; samples must never go backwards and fields must be sane
    MainsSnapshot prev, cur;
    TEST_ASSERT_TRUE(mainsRead(prev));
    for (int i = 0; i < 200000; i++) {
        TEST_ASSERT_TRUE(mainsRead(cur));
        TEST_ASSERT_TRUE_MESSAGE(cur.samples >= prev.samples, "sample count went backwards");
        TEST_ASSERT_TRUE_MESSAGE(cur.changes >= prev.changes, "change count went backwards");
        prev = cur;
    }
}

static void test_state_matches_a_stable_pin() {
    // Find a moment where the raw pin has been stable for longer than the debounce time
    uint32_t deadline = millis() + 2000;
    while (millis() < deadline) {
        bool level = digitalRead(POWER_PIN) == HIGH;
        bool stable = true;
        for (uint32_t t = 0; t < 4 * MAINS_DEBOUNCE_MS && stable; t++) {
            delay(1);
            stable = (digitalRead(POWER_PIN) == HIGH) == level;
        }
        if (!stable) continue;
        MainsSnapshot s;
        TEST_ASSERT_TRUE(mainsRead(s));
        TEST_ASSERT_EQUAL_MESSAGE(level, s.present, "debounced state does not follow a stable pin");
        Serial.printf("Mains input: %s\n", level ? "HIGH (present)" : "LOW (lost)");
        return;
    }
    TEST_IGNORE_MESSAGE("GPIO27 never stable for 20 ms (AC ripple?), see raw toggle count");
}

static void test_report_raw_signal_quality() {
    // Diagnostic only: how often the raw pin toggles. A clean DC signal gives 0;
    // an unfiltered optocoupler on AC gives ~100 per second (50 Hz).
    uint32_t toggles = 0, high = 0;
    bool last = digitalRead(POWER_PIN) == HIGH;
    for (int i = 0; i < 10000; i++) {   // 1 s at 100 us
        bool level = digitalRead(POWER_PIN) == HIGH;
        if (level != last) toggles++;
        if (level) high++;
        last = level;
        delayMicroseconds(100);
    }
    Serial.printf("GPIO27 raw: %lu toggles/s, HIGH %lu%% of the time\n",
                  (unsigned long)toggles, (unsigned long)(high / 100));
    TEST_PASS();
}

void setup() {
    // The boot program starts every image with the watchdog armed (~8.3 s);
    // these tests run longer than that in setup() and do not feed it
    watchdog_disable();
    Serial.begin(115200);
    uint32_t start = millis();
    while (!Serial && millis() - start < 10000) delay(10);
    delay(500);

    UNITY_BEGIN();
    RUN_TEST(test_core1_publishes_a_snapshot);
    RUN_TEST(test_core1_samples_at_about_1khz);
    RUN_TEST(test_snapshots_are_consistent_while_core1_writes);
    RUN_TEST(test_state_matches_a_stable_pin);
    RUN_TEST(test_report_raw_signal_quality);
    UNITY_END();
}

void loop() {}
