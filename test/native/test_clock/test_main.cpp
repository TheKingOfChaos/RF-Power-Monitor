// Host unit tests for lib/clock: calendar time, Danish local time, DateAndTime.
// Run: pio test -e native -f native/test_clock

#include <string.h>

#include <unity.h>
#include <clock.h>

void setUp() {}
void tearDown() {}

static void assertCivil(uint64_t t, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    CivilTime c = civilFromEpoch(t);
    TEST_ASSERT_EQUAL_UINT16(y, c.year);
    TEST_ASSERT_EQUAL_UINT8(mo, c.month);
    TEST_ASSERT_EQUAL_UINT8(d, c.day);
    TEST_ASSERT_EQUAL_UINT8(h, c.hour);
    TEST_ASSERT_EQUAL_UINT8(mi, c.minute);
    TEST_ASSERT_EQUAL_UINT8(s, c.second);
}

static void test_civil_from_epoch_known_dates() {
    assertCivil(0, 1970, 1, 1, 0, 0, 0);
    assertCivil(951782400ull, 2000, 2, 29, 0, 0, 0);  // leap day of a century leap year
    assertCivil(1772236799ull, 2026, 2, 27, 23, 59, 59);
    assertCivil(4102444800ull, 2100, 1, 1, 0, 0, 0);
}

static void test_summer_time_starts_last_sunday_march_0100_utc() {
    const uint64_t start = 1774746000ull;  // 2026-03-29 01:00:00 UTC
    TEST_ASSERT_FALSE(euSummerTime(start - 1));
    TEST_ASSERT_TRUE(euSummerTime(start));
}

static void test_summer_time_ends_last_sunday_october_0100_utc() {
    const uint64_t end = 1792890000ull;  // 2026-10-25 01:00:00 UTC
    TEST_ASSERT_TRUE(euSummerTime(end - 1));
    TEST_ASSERT_FALSE(euSummerTime(end));
}

static void test_local_time_around_the_switch_hours() {
    bool summer;
    CivilTime c = danishLocal(1774746000ull - 1, summer);  // 01:59:59 CET
    TEST_ASSERT_FALSE(summer);
    TEST_ASSERT_EQUAL_UINT8(1, c.hour);
    c = danishLocal(1774746000ull, summer);  // 03:00:00 CEST
    TEST_ASSERT_TRUE(summer);
    TEST_ASSERT_EQUAL_UINT8(3, c.hour);
    c = danishLocal(1792890000ull - 1, summer);  // 02:59:59 CEST
    TEST_ASSERT_EQUAL_UINT8(2, c.hour);
    c = danishLocal(1792890000ull, summer);  // 02:00:00 CET again
    TEST_ASSERT_FALSE(summer);
    TEST_ASSERT_EQUAL_UINT8(2, c.hour);
}

static void test_local_time_crosses_midnight_and_new_year() {
    bool summer;
    CivilTime c = danishLocal(4102444800ull - 1800, summer);  // 2099-12-31 23:30 UTC
    TEST_ASSERT_EQUAL_UINT16(2100, c.year);
    TEST_ASSERT_EQUAL_UINT8(1, c.month);
    TEST_ASSERT_EQUAL_UINT8(1, c.day);
    TEST_ASSERT_EQUAL_UINT8(0, c.hour);
    TEST_ASSERT_EQUAL_UINT8(30, c.minute);
}

static void test_format_local_time_winter_and_summer() {
    char buf[32];
    TEST_ASSERT_EQUAL(23, formatLocalTime(1762540930ull, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2025-11-07 19:42:10 CET", buf);
    TEST_ASSERT_EQUAL(24, formatLocalTime(1783870930ull, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2026-07-12 17:42:10 CEST", buf);
}

static void test_format_local_time_too_small_buffer_returns_zero() {
    char buf[23];  // one short for "2025-11-07 19:42:10 CET" and its NUL
    TEST_ASSERT_EQUAL(0, formatLocalTime(1762540930ull, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, formatLocalTime(1762540930ull, nullptr, 0));
}

static void test_format_utc_rfc5424_has_millis_and_z() {
    char buf[32];
    TEST_ASSERT_EQUAL(24, formatUtcRfc5424(1762540930123ull, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2025-11-07T18:42:10.123Z", buf);
    TEST_ASSERT_EQUAL(24, formatUtcRfc5424(1762540930007ull, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2025-11-07T18:42:10.007Z", buf);
    char small[24];
    TEST_ASSERT_EQUAL(0, formatUtcRfc5424(1762540930123ull, small, sizeof(small)));
}

static void test_date_and_time_bytes() {
    uint8_t d[11];
    dateAndTime(1762540930ull, d);  // 2025-11-07 19:42:10 CET
    const uint8_t winter[11] = {0x07, 0xE9, 11, 7, 19, 42, 10, 0, '+', 1, 0};
    TEST_ASSERT_EQUAL_MEMORY(winter, d, 11);
    dateAndTime(1783870930ull, d);  // 2026-07-12 17:42:10 CEST
    const uint8_t summer[11] = {0x07, 0xEA, 7, 12, 17, 42, 10, 0, '+', 2, 0};
    TEST_ASSERT_EQUAL_MEMORY(summer, d, 11);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_civil_from_epoch_known_dates);
    RUN_TEST(test_summer_time_starts_last_sunday_march_0100_utc);
    RUN_TEST(test_summer_time_ends_last_sunday_october_0100_utc);
    RUN_TEST(test_local_time_around_the_switch_hours);
    RUN_TEST(test_local_time_crosses_midnight_and_new_year);
    RUN_TEST(test_format_local_time_winter_and_summer);
    RUN_TEST(test_format_local_time_too_small_buffer_returns_zero);
    RUN_TEST(test_format_utc_rfc5424_has_millis_and_z);
    RUN_TEST(test_date_and_time_bytes);
    return UNITY_END();
}
