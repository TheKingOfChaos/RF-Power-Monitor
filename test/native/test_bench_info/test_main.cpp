// Host unit tests for lib/bench_info: the device data block the bench tool reads (tools/bench_device.py).
// Run: pio test -e native -f native/test_bench_info

#include <unity.h>
#include <string.h>
#include <bench_info.h>

void setUp() {}
void tearDown() {}

static const char EXPECTED[] =
    "BENCHINFO BEGIN\r\n"
    "hostname=RF-Power-A1B2C3\r\n"
    "mac=2C:CF:67:A1:B2:C3\r\n"
    "serial=E6614103E7A1B2C3\r\n"
    "firmware=3.5.0\r\n"
    "fwkey=0123456789abcdef\r\n"
    "password=k7mq2xr9tp\r\n"
    "locked=1\r\n"
    "lost=0\r\n"
    "BENCHINFO END\r\n";

static BenchInfo sample() {
    BenchInfo info{};
    info.hostname = "RF-Power-A1B2C3";
    const uint8_t mac[6] = {0x2C, 0xCF, 0x67, 0xA1, 0xB2, 0xC3};
    memcpy(info.mac, mac, sizeof(mac));
    info.serial = "E6614103E7A1B2C3";
    info.firmware = "3.5.0";
    info.fwKey = "0123456789abcdef";
    info.password = "k7mq2xr9tp";
    info.locked = true;
    info.passwordLost = false;
    return info;
}

static void test_format_bench_info_exact_block() {
    char out[BENCH_INFO_MAX];
    size_t n = formatBenchInfo(sample(), out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(EXPECTED, out);
    TEST_ASSERT_EQUAL(strlen(EXPECTED), n);
}

static void test_format_bench_info_flags_print_as_digits() {
    BenchInfo info = sample();
    info.locked = false;
    info.passwordLost = true;
    char out[BENCH_INFO_MAX];
    TEST_ASSERT_TRUE(formatBenchInfo(info, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nlocked=0\r\nlost=1\r\n"));
}

static void test_format_bench_info_too_small_returns_zero() {
    size_t len = strlen(EXPECTED);
    char out[BENCH_INFO_MAX];
    memset(out, 'x', sizeof(out));
    TEST_ASSERT_EQUAL(0, formatBenchInfo(sample(), out, len));  // no room for the NUL
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
    TEST_ASSERT_EQUAL(len, formatBenchInfo(sample(), out, len + 1));
}

static void test_format_bench_info_zero_size_writes_nothing() {
    char out[1] = {'x'};
    TEST_ASSERT_EQUAL(0, formatBenchInfo(sample(), out, 0));
    TEST_ASSERT_EQUAL_CHAR('x', out[0]);
}

static void test_format_bench_info_null_fields_print_empty() {
    BenchInfo info = sample();
    info.serial = nullptr;
    info.password = nullptr;
    char out[BENCH_INFO_MAX];
    TEST_ASSERT_TRUE(formatBenchInfo(info, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\nserial=\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\npassword=\r\n"));
}

static void test_format_bench_info_longest_hostname_fits() {
    BenchInfo info = sample();
    info.hostname = "abcdefghijklmnopqrstuvwxyz012345";  // 32, HOSTNAME_MAX_LENGTH
    info.firmware = "3.5.0-rc.12345678";                   // 16, the slot header's version field
    char out[BENCH_INFO_MAX];
    TEST_ASSERT_TRUE(formatBenchInfo(info, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "hostname=abcdefghijklmnopqrstuvwxyz012345\r\n"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_format_bench_info_exact_block);
    RUN_TEST(test_format_bench_info_flags_print_as_digits);
    RUN_TEST(test_format_bench_info_too_small_returns_zero);
    RUN_TEST(test_format_bench_info_zero_size_writes_nothing);
    RUN_TEST(test_format_bench_info_null_fields_print_empty);
    RUN_TEST(test_format_bench_info_longest_hostname_fits);
    return UNITY_END();
}
