// On-device tests for lib/settings: A/B storage on LittleFS, fallback and recovery.
// Uses its own paths under /test so the device's real settings are never touched.
// Run: pio test -e pico -f embedded/test_settings_store

#include <Arduino.h>
#include <hardware/watchdog.h>
#include <LittleFS.h>
#include <unity.h>
#include <settings.h>

static const char* const PATH_A = "test/settings_a.json";
static const char* const PATH_B = "test/settings_b.json";

static void removeTestFiles() {
    LittleFS.remove(PATH_A);
    LittleFS.remove(PATH_B);
}

static void writeRaw(const char* path, const char* content) {
    File f = LittleFS.open(path, "w");
    TEST_ASSERT_TRUE_MESSAGE((bool)f, "cannot open test file for writing");
    f.print(content);
    f.close();
}

/// Fill s with valid static settings in place (no Settings copy on the stack)
static void fillStaticSettings(Settings& s, const char* ip) {
    settingsDefaults(s);
    s.dhcpEnabled = false;
    strcpy(s.staticIP, ip);
    strcpy(s.subnetMask, "255.255.255.0");
    strcpy(s.gateway, "192.168.50.1");
}

static Settings staticSettings(const char* ip) {
    Settings s;
    fillStaticSettings(s, ip);
    return s;
}

void setUp() {
    removeTestFiles();
}

void tearDown() {
    removeTestFiles();
}

static void test_empty_store_loads_defaults() {
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    TEST_ASSERT_EQUAL(SettingsSlot::None, store.load(s));
    TEST_ASSERT_TRUE(s.dhcpEnabled);
    TEST_ASSERT_EQUAL_STRING("public", s.community);
}

static void test_first_save_goes_to_slot_a_and_survives_reload() {
    SettingsStore store(PATH_A, PATH_B);
    Settings s = staticSettings("192.168.50.10");
    store.load(s);
    s = staticSettings("192.168.50.10");
    TEST_ASSERT_TRUE(store.save(s));
    TEST_ASSERT_EQUAL(SettingsSlot::A, store.activeSlot());
    TEST_ASSERT_EQUAL_UINT32(1, s.sequence);

    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::A, fresh.load(loaded));
    TEST_ASSERT_TRUE(settingsEqual(s, loaded));
}

static void test_saves_alternate_slots_and_newest_wins() {
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    store.load(s);

    s = staticSettings("192.168.50.11");
    TEST_ASSERT_TRUE(store.save(s));
    s = staticSettings("192.168.50.12");
    TEST_ASSERT_TRUE(store.save(s));
    TEST_ASSERT_EQUAL(SettingsSlot::B, store.activeSlot());
    TEST_ASSERT_EQUAL_UINT32(2, s.sequence);
    s = staticSettings("192.168.50.13");
    TEST_ASSERT_TRUE(store.save(s));
    TEST_ASSERT_EQUAL(SettingsSlot::A, store.activeSlot());

    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::A, fresh.load(loaded));
    TEST_ASSERT_EQUAL_STRING("192.168.50.13", loaded.staticIP);
    TEST_ASSERT_EQUAL_UINT32(3, loaded.sequence);
}

static void test_corrupt_newest_copy_falls_back_to_previous() {
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    store.load(s);
    s = staticSettings("192.168.50.21");
    TEST_ASSERT_TRUE(store.save(s));   // A, sequence 1
    s = staticSettings("192.168.50.22");
    TEST_ASSERT_TRUE(store.save(s));   // B, sequence 2

    // Simulate a power cut halfway through writing B
    writeRaw(PATH_B, "{\"community\":\"public\",\"dhcpEn");

    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::A, fresh.load(loaded));
    TEST_ASSERT_EQUAL_STRING("192.168.50.21", loaded.staticIP);

    // The next save must overwrite the corrupt copy, not the good one
    loaded = staticSettings("192.168.50.23");
    TEST_ASSERT_TRUE(fresh.save(loaded));
    TEST_ASSERT_EQUAL(SettingsSlot::B, fresh.activeSlot());
    TEST_ASSERT_EQUAL_UINT32(2, loaded.sequence);
}

static void test_both_copies_corrupt_loads_defaults() {
    writeRaw(PATH_A, "garbage");
    writeRaw(PATH_B, "");
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    TEST_ASSERT_EQUAL(SettingsSlot::None, store.load(s));
    TEST_ASSERT_TRUE(s.dhcpEnabled);
}

static void test_oversized_file_is_ignored() {
    String big = "{\"community\":\"";
    for (size_t i = 0; i < SETTINGS_JSON_MAX + 100; i++) big += 'x';
    big += "\"}";
    writeRaw(PATH_A, big.c_str());
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    TEST_ASSERT_EQUAL(SettingsSlot::None, store.load(s));
}

static void test_legacy_file_without_sequence_is_loaded_and_superseded() {
    writeRaw(PATH_A,
             "{\"community\":\"legacy\",\"dhcpEnabled\":true,\"staticIP\":\"192.168.1.100\","
             "\"subnetMask\":\"255.255.255.0\",\"gateway\":\"192.168.1.1\",\"version\":1}");
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    TEST_ASSERT_EQUAL(SettingsSlot::A, store.load(s));
    TEST_ASSERT_EQUAL_STRING("legacy", s.community);

    TEST_ASSERT_TRUE(store.save(s));
    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::B, fresh.load(loaded));
    TEST_ASSERT_EQUAL_STRING("legacy", loaded.community);
}

static void test_saving_defaults_is_newer_than_stored_copy() {
    // Factory reset saves settingsDefaults(), whose sequence is 0. It must still win.
    SettingsStore store(PATH_A, PATH_B);
    Settings s;
    store.load(s);
    for (int i = 0; i < 3; i++) {
        s = staticSettings("192.168.50.30");
        TEST_ASSERT_TRUE(store.save(s));
    }
    settingsDefaults(s);
    TEST_ASSERT_TRUE(store.save(s));

    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    fresh.load(loaded);
    TEST_ASSERT_TRUE(loaded.dhcpEnabled);
    TEST_ASSERT_EQUAL_UINT32(4, loaded.sequence);
}

static void test_maximum_size_settings_survive_save_and_reload() {
    // All text fields at maximum length with characters JSON escapes: the largest file possible
    Settings s = staticSettings("192.168.50.40");
    memset(s.community, '"', COMMUNITY_MAX_LENGTH);
    s.community[COMMUNITY_MAX_LENGTH] = '\0';
    memset(s.sysName, '\\', SYS_TEXT_MAX_LENGTH);
    s.sysName[SYS_TEXT_MAX_LENGTH] = '\0';
    memset(s.sysContact, '"', SYS_TEXT_MAX_LENGTH);
    s.sysContact[SYS_TEXT_MAX_LENGTH] = '\0';
    memset(s.sysLocation, 'L', SYS_TEXT_MAX_LENGTH);
    s.sysLocation[SYS_TEXT_MAX_LENGTH] = '\0';

    SettingsStore store(PATH_A, PATH_B);
    Settings tmp;
    store.load(tmp);
    TEST_ASSERT_TRUE(store.save(s));

    File f = LittleFS.open(PATH_A, "r");
    size_t size = f.size();
    f.close();
    TEST_ASSERT_GREATER_THAN(256, size);   // bigger than the old 256-byte limit
    TEST_ASSERT_LESS_THAN(SETTINGS_JSON_MAX, size);

    SettingsStore fresh(PATH_A, PATH_B);
    Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::A, fresh.load(loaded));
    TEST_ASSERT_TRUE(settingsEqual(s, loaded));
}

static void test_legacy_files_are_removed_and_settings_kept() {
    // Files written by older firmware; the real settings copies must survive
    writeRaw("config.bak", "{}");
    writeRaw("config.schema.json", "{}");
    writeRaw("data/active_file", "data/settings_a.json");
    bool hadA = LittleFS.exists(SETTINGS_FILE_A);
    bool hadB = LittleFS.exists(SETTINGS_FILE_B);

    TEST_ASSERT_EQUAL_UINT8(3, removeLegacyFiles());
    TEST_ASSERT_FALSE(LittleFS.exists("config.bak"));
    TEST_ASSERT_FALSE(LittleFS.exists("config.schema.json"));
    TEST_ASSERT_FALSE(LittleFS.exists("data/active_file"));
    TEST_ASSERT_EQUAL(hadA, LittleFS.exists(SETTINGS_FILE_A));
    TEST_ASSERT_EQUAL(hadB, LittleFS.exists(SETTINGS_FILE_B));

    TEST_ASSERT_EQUAL_UINT8(0, removeLegacyFiles());  // nothing left, nothing to do
}

// ---- stack usage ----
// Core 0 has a 4 KB stack shared with USB, lwIP and W5500 interrupts. Saving
// from the serial menu happens several calls deep, so save/load must stay small.

static const size_t STACK_PROBE_SPAN = 3072;  // bytes below the caller that are painted and checked
static const size_t SAVE_LOAD_STACK_BUDGET = 1536;
static const uint8_t STACK_PAINT = 0xA5;

static void saveAndLoadOnce() {
    static SettingsStore store(PATH_A, PATH_B);
    static Settings s;
    store.load(s);
    fillStaticSettings(s, "192.168.50.60");  // in place: measure save/load, not a test temporary
    memset(s.sysLocation, 'S', SYS_TEXT_MAX_LENGTH);
    s.sysLocation[SYS_TEXT_MAX_LENGTH] = '\0';
    TEST_ASSERT_TRUE(store.save(s));
    static SettingsStore fresh(PATH_A, PATH_B);
    static Settings loaded;
    TEST_ASSERT_EQUAL(SettingsSlot::A, fresh.load(loaded));
}

/**
 * @brief Bytes of stack fn() uses below this function's frame (high-water mark)
 *
 * Paints the free stack with a pattern, runs fn, and finds the lowest byte
 * that was overwritten.
 */
static size_t __attribute__((noinline)) measureStackUse(void (*fn)()) {
    volatile uint8_t* top = (volatile uint8_t*)__builtin_frame_address(0) - 64;  // margin for this frame
    volatile uint8_t* bottom = top - STACK_PROBE_SPAN;
    for (volatile uint8_t* p = bottom; p < top; p++) *p = STACK_PAINT;
    fn();
    volatile uint8_t* p = bottom;
    while (p < top && *p == STACK_PAINT) p++;
    return (size_t)(top - p);
}

static void test_save_and_load_use_little_stack() {
    if ((size_t)rp2040.getFreeStack() < STACK_PROBE_SPAN + 512) {
        TEST_IGNORE_MESSAGE("not enough free stack to probe");
    }
    size_t used = measureStackUse(saveAndLoadOnce);
    char msg[64];
    snprintf(msg, sizeof(msg), "save+load used %u bytes of stack", (unsigned)used);
    Serial.println(msg);
    TEST_ASSERT_LESS_THAN_MESSAGE(STACK_PROBE_SPAN, used, "probe span exhausted, usage unknown");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(SAVE_LOAD_STACK_BUDGET, used, msg);
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
    if (!LittleFS.begin()) {
        TEST_FAIL_MESSAGE("LittleFS mount failed");
    }
    LittleFS.mkdir("test");

    RUN_TEST(test_empty_store_loads_defaults);
    RUN_TEST(test_first_save_goes_to_slot_a_and_survives_reload);
    RUN_TEST(test_saves_alternate_slots_and_newest_wins);
    RUN_TEST(test_corrupt_newest_copy_falls_back_to_previous);
    RUN_TEST(test_both_copies_corrupt_loads_defaults);
    RUN_TEST(test_oversized_file_is_ignored);
    RUN_TEST(test_legacy_file_without_sequence_is_loaded_and_superseded);
    RUN_TEST(test_saving_defaults_is_newer_than_stored_copy);
    RUN_TEST(test_maximum_size_settings_survive_save_and_reload);
    RUN_TEST(test_save_and_load_use_little_stack);
    RUN_TEST(test_legacy_files_are_removed_and_settings_kept);

    LittleFS.rmdir("test");
    UNITY_END();
}

void loop() {}
