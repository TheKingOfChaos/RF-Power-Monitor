// Host unit tests for the one-minute processor load (lib/cpu_load): CpuLoad for core 1, which sleeps
// in delay(1), and LoopLoad for core 0, which never sleeps.
// Run: pio test -e native -f native/test_cpu_load

#include <unity.h>
#include <cpu_load.h>

void setUp() {}
void tearDown() {}

static const uint32_t SECOND = 1000000;

/// seconds of time from nowUs in 1 ms steps, busy for busyPercent of each step
static uint32_t run(CpuLoad& load, uint32_t nowUs, uint32_t seconds, uint32_t busyPercent) {
    for (uint32_t ms = 0; ms < seconds * 1000; ms++) {
        nowUs += 1000;
        load.add(nowUs, busyPercent * 10);
    }
    return nowUs;
}

/// seconds of core 0 loop passes of passUs each, from nowUs
static uint32_t loop(LoopLoad& load, uint32_t nowUs, uint32_t seconds, uint32_t passUs) {
    for (uint32_t t = 0; t < seconds * SECOND; t += passUs) {
        nowUs += passUs;
        load.pass(nowUs);
    }
    return nowUs;
}

// ---- CpuLoad ----

static void test_cpu_load_is_zero_before_one_second() {
    CpuLoad load;
    TEST_ASSERT_EQUAL_UINT8(0, load.percent());
    uint32_t now = 0;
    for (int ms = 0; ms < 999; ms++) {
        now += 1000;
        load.add(now, 1000);  // fully busy, but no second completed yet
    }
    TEST_ASSERT_EQUAL_UINT8(0, load.percent());
}

static void test_cpu_load_half_busy_reads_50() {
    CpuLoad load;
    run(load, 0, 3, 50);
    TEST_ASSERT_EQUAL_UINT8(50, load.percent());
}

static void test_cpu_load_averages_over_sixty_seconds() {
    CpuLoad load;
    uint32_t now = run(load, 0, 30, 100);
    now = run(load, now, 30, 0);
    TEST_ASSERT_UINT8_WITHIN(1, 50, load.percent());
    run(load, now, 60, 0);  // the busy half has left the minute
    TEST_ASSERT_EQUAL_UINT8(0, load.percent());
}

static void test_cpu_load_never_exceeds_100() {
    CpuLoad load;
    uint32_t now = 0;
    for (int ms = 0; ms < 5000; ms++) {
        now += 1000;
        load.add(now, 5000);  // more busy time than elapsed (a pass that straddled seconds)
    }
    TEST_ASSERT_EQUAL_UINT8(100, load.percent());
}

static void test_cpu_load_survives_timer_wrap() {
    CpuLoad load;
    uint32_t now = run(load, 0xFFF00000u, 10, 25);  // time_us_32 wraps after ~1 s
    TEST_ASSERT_LESS_THAN_UINT32(0xFFF00000u, now);
    TEST_ASSERT_EQUAL_UINT8(25, load.percent());
}

// ---- LoopLoad ----

static void test_loop_load_idle_loop_reads_zero() {
    LoopLoad load;
    loop(load, 0, 5, 40);
    TEST_ASSERT_EQUAL_UINT8(0, load.percent());
}

static void test_loop_load_busy_passes_raise_load() {
    LoopLoad load;
    uint32_t now = loop(load, 0, 2, 40);   // idle baseline: 40 us per pass
    now = loop(load, now, 0, 40);
    // One second: 0.5 s of idle 40 us passes, then one pass of 500 ms
    for (uint32_t t = 0; t < SECOND / 2; t += 40) {
        now += 40;
        load.pass(now);
    }
    now += 500000;
    load.pass(now);
    now = loop(load, now, 1, 40);  // closes the busy second
    // 3 completed seconds of 4: idle, idle, ~50 %, idle -> average ~12-13
    uint8_t p = load.percent();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(10, p);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(17, p);
}

static void test_loop_load_long_passes_read_high() {
    LoopLoad load;
    uint32_t now = loop(load, 0, 3, 40);
    // 10 s where every pass takes 400 us instead of 40: 90 % busy
    now = loop(load, now, 10, 400);
    uint8_t p = load.percent();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(65, p);  // 10 of 13 s at ~90 %
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(72, p);
}

static void test_loop_load_baseline_follows_the_last_minute() {
    LoopLoad load;
    uint32_t now = loop(load, 0, 1, 10);  // a short pass early on (before the network started)
    now = loop(load, now, 61, 40);        // then 40 us is the idle pass
    now = loop(load, now, 60, 40);        // a minute with the 10 us baseline gone
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(1, load.percent());
}

static void test_loop_load_survives_timer_wrap() {
    LoopLoad load;
    uint32_t now = loop(load, 0xFFF00000u, 5, 40);
    TEST_ASSERT_LESS_THAN_UINT32(0xFFF00000u, now);
    TEST_ASSERT_EQUAL_UINT8(0, load.percent());
    loop(load, now, 5, 400);
    TEST_ASSERT_GREATER_THAN_UINT8(20, load.percent());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_cpu_load_is_zero_before_one_second);
    RUN_TEST(test_cpu_load_half_busy_reads_50);
    RUN_TEST(test_cpu_load_averages_over_sixty_seconds);
    RUN_TEST(test_cpu_load_never_exceeds_100);
    RUN_TEST(test_cpu_load_survives_timer_wrap);
    RUN_TEST(test_loop_load_idle_loop_reads_zero);
    RUN_TEST(test_loop_load_busy_passes_raise_load);
    RUN_TEST(test_loop_load_long_passes_read_high);
    RUN_TEST(test_loop_load_baseline_follows_the_last_minute);
    RUN_TEST(test_loop_load_survives_timer_wrap);
    return UNITY_END();
}
