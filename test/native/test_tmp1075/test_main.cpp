// Host unit tests for the TMP1075 conversion and sensor state (lib/tmp1075).
// Run: pio test -e native -f native/test_tmp1075

#include <unity.h>
#include <tmp1075.h>

void setUp() {}
void tearDown() {}

// ---- tmp1075ToDeciC: 12-bit left-aligned two's complement, 0.0625 C per bit ----

static void test_to_deci_c_reads_values_captured_from_the_carrier() {
    // Read from the TMP1075 on the carrier board with the I2C test sketch
    TEST_ASSERT_EQUAL_INT32(271, tmp1075ToDeciC(0x1B20));  // 27.125 C
    TEST_ASSERT_EQUAL_INT32(271, tmp1075ToDeciC(0x1B10));  // 27.0625 C
}

static void test_to_deci_c_zero() {
    TEST_ASSERT_EQUAL_INT32(0, tmp1075ToDeciC(0x0000));
}

static void test_to_deci_c_rounds_half_up_for_positive_values() {
    TEST_ASSERT_EQUAL_INT32(1, tmp1075ToDeciC(0x0010));    // 0.0625 C -> 0.1
    TEST_ASSERT_EQUAL_INT32(5, tmp1075ToDeciC(0x0080));    // 0.5 C
    TEST_ASSERT_EQUAL_INT32(256, tmp1075ToDeciC(0x1990));  // 25.5625 C -> 25.6
}

static void test_to_deci_c_negative_values() {
    TEST_ASSERT_EQUAL_INT32(-1, tmp1075ToDeciC(0xFFF0));   // -0.0625 C -> -0.1
    TEST_ASSERT_EQUAL_INT32(-250, tmp1075ToDeciC(0xE700)); // -25 C (datasheet table)
    TEST_ASSERT_EQUAL_INT32(-256, tmp1075ToDeciC(0xE670)); // -25.5625 C -> -25.6
}

static void test_to_deci_c_range_limits() {
    TEST_ASSERT_EQUAL_INT32(1279, tmp1075ToDeciC(0x7FF0));  // 127.9375 C, largest value
    TEST_ASSERT_EQUAL_INT32(-1280, tmp1075ToDeciC(0x8000)); // -128 C, smallest value
    TEST_ASSERT_EQUAL_INT32(1250, tmp1075ToDeciC(0x7D00));  // 125 C (datasheet table)
    TEST_ASSERT_EQUAL_INT32(-400, tmp1075ToDeciC(0xD800));  // -40 C (datasheet table)
}

// ---- TempSensorState ----

static void test_state_is_invalid_before_the_first_reading() {
    TempSensorState s;
    TEST_ASSERT_FALSE(s.valid());
    TEST_ASSERT_EQUAL_UINT32(0, s.readErrors());
}

static void test_state_is_valid_after_one_good_reading() {
    TempSensorState s;
    s.reading(true, 271);
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_INT32(271, s.deciC());
}

static void test_state_keeps_the_last_value_through_two_failed_reads() {
    TempSensorState s;
    s.reading(true, 271);
    s.reading(false, 0);
    s.reading(false, 0);
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_INT32(271, s.deciC());
    TEST_ASSERT_EQUAL_UINT32(2, s.readErrors());
}

static void test_state_is_invalid_after_three_failed_reads_in_a_row() {
    TempSensorState s;
    s.reading(true, 271);
    for (uint8_t i = 0; i < TempSensorState::MAX_FAILURES; i++) s.reading(false, 0);
    TEST_ASSERT_FALSE(s.valid());
    TEST_ASSERT_EQUAL_UINT32(TempSensorState::MAX_FAILURES, s.readErrors());
}

static void test_state_good_reading_resets_the_failure_run() {
    TempSensorState s;
    s.reading(true, 200);
    s.reading(false, 0);
    s.reading(false, 0);
    s.reading(true, 210);  // run of failures restarts here
    s.reading(false, 0);
    s.reading(false, 0);
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_INT32(210, s.deciC());
    TEST_ASSERT_EQUAL_UINT32(4, s.readErrors());
}

static void test_state_recovers_when_the_sensor_answers_again() {
    TempSensorState s;
    for (uint8_t i = 0; i < 10; i++) s.reading(false, 0);
    TEST_ASSERT_FALSE(s.valid());
    s.reading(true, -55);
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_INT32(-55, s.deciC());
}

static void test_state_never_read_sensor_stays_invalid_after_failures() {
    TempSensorState s;
    s.reading(false, 0);
    TEST_ASSERT_FALSE(s.valid());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_to_deci_c_reads_values_captured_from_the_carrier);
    RUN_TEST(test_to_deci_c_zero);
    RUN_TEST(test_to_deci_c_rounds_half_up_for_positive_values);
    RUN_TEST(test_to_deci_c_negative_values);
    RUN_TEST(test_to_deci_c_range_limits);
    RUN_TEST(test_state_is_invalid_before_the_first_reading);
    RUN_TEST(test_state_is_valid_after_one_good_reading);
    RUN_TEST(test_state_keeps_the_last_value_through_two_failed_reads);
    RUN_TEST(test_state_is_invalid_after_three_failed_reads_in_a_row);
    RUN_TEST(test_state_good_reading_resets_the_failure_run);
    RUN_TEST(test_state_recovers_when_the_sensor_answers_again);
    RUN_TEST(test_state_never_read_sensor_stays_invalid_after_failures);
    return UNITY_END();
}
