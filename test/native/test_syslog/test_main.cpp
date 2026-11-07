// Host unit tests for lib/syslog: server settings codec and input checks.
// Run: pio test -e native -f native/test_syslog

#include <string.h>

#include <string>

#include <unity.h>
#include <log_settings.h>

void setUp() {}
void tearDown() {}

// ---- settings codec ----

static LogSettings typedSettings() {
    LogSettings s;
    logSettingsDefaults(s);
    s.ntpMode = ServerMode::Typed;
    strcpy(s.ntpServer, "pool.ntp.org");
    s.syslogMode = ServerMode::Typed;
    strcpy(s.syslogServer, "[2001:db8::5]:5514");
    s.sequence = 7;
    return s;
}

static void test_log_settings_defaults_are_dhcp() {
    LogSettings s;
    memset(&s, 0x5A, sizeof(s));
    logSettingsDefaults(s);
    TEST_ASSERT_TRUE(s.ntpMode == ServerMode::Dhcp);
    TEST_ASSERT_TRUE(s.syslogMode == ServerMode::Dhcp);
    TEST_ASSERT_EQUAL_STRING("", s.ntpServer);
    TEST_ASSERT_EQUAL_STRING("", s.syslogServer);
    TEST_ASSERT_EQUAL_UINT32(LOG_SETTINGS_VERSION, s.version);
}

static void test_log_settings_json_round_trip() {
    LogSettings s = typedSettings();
    char json[LOG_SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, logSettingsToJson(s, json, sizeof(json)));
    LogSettings back;
    TEST_ASSERT_TRUE(logSettingsFromJson(json, back));
    TEST_ASSERT_TRUE(logSettingsEqual(s, back));
    TEST_ASSERT_EQUAL_UINT32(7, back.sequence);

    LogSettings none;
    logSettingsDefaults(none);
    none.ntpMode = ServerMode::None;
    none.syslogMode = ServerMode::None;
    TEST_ASSERT_GREATER_THAN(0, logSettingsToJson(none, json, sizeof(json)));
    TEST_ASSERT_TRUE(logSettingsFromJson(json, back));
    TEST_ASSERT_TRUE(back.ntpMode == ServerMode::None);
    TEST_ASSERT_TRUE(back.syslogMode == ServerMode::None);
}

static void test_log_settings_equal_compares_modes_and_servers() {
    LogSettings a = typedSettings(), b = typedSettings();
    b.sequence = 99;
    TEST_ASSERT_TRUE(logSettingsEqual(a, b));
    b.syslogMode = ServerMode::None;
    TEST_ASSERT_FALSE(logSettingsEqual(a, b));
    b = typedSettings();
    strcpy(b.ntpServer, "other.ntp.org");
    TEST_ASSERT_FALSE(logSettingsEqual(a, b));
}

static void test_log_settings_longest_values_fit_the_file() {
    LogSettings s = typedSettings();
    memset(s.ntpServer, 'a', LOG_HOST_MAX);  // 63-character name
    s.ntpServer[LOG_HOST_MAX] = '\0';
    strcpy(s.syslogServer, "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535");
    s.sequence = 0xFFFFFFFFu;
    char json[LOG_SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, logSettingsToJson(s, json, sizeof(json)));
    LogSettings back;
    TEST_ASSERT_TRUE(logSettingsFromJson(json, back));
    TEST_ASSERT_TRUE(logSettingsEqual(s, back));
}

static void test_log_settings_to_json_too_small_returns_zero() {
    LogSettings s = typedSettings();
    char json[20];
    TEST_ASSERT_EQUAL(0, logSettingsToJson(s, json, sizeof(json)));
    TEST_ASSERT_EQUAL(0, logSettingsToJson(s, nullptr, 0));
}

static void test_log_settings_rejects_missing_field() {
    const char* docs[] = {
        "{\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"dhcp\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"dhcp\"}",
    };
    for (const char* d : docs) {
        LogSettings s;
        TEST_ASSERT_FALSE_MESSAGE(logSettingsFromJson(d, s), d);
        TEST_ASSERT_TRUE(s.ntpMode == ServerMode::Dhcp);  // defaults after a rejection
    }
}

static void test_log_settings_rejects_bad_values() {
    const char* docs[] = {
        "{\"version\":2,\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"auto\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"typed\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"typed\",\"ntp\":\"a..b\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"typed\",\"syslog\":\"host.name\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"typed\",\"syslog\":\"1.2.3.4:0\"}",
        "{\"version\":1,\"sequence\":-1,\"ntpMode\":\"dhcp\",\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "{\"version\":1,\"sequence\":1,\"ntpMode\":5,\"ntp\":\"\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}",
        "[1,2,3]",
        "",
    };
    for (const char* d : docs) {
        LogSettings s;
        TEST_ASSERT_FALSE_MESSAGE(logSettingsFromJson(d, s), d);
    }
    LogSettings s;
    TEST_ASSERT_FALSE(logSettingsFromJson(nullptr, s));
}

static void test_log_settings_rejects_truncated_json() {
    LogSettings s = typedSettings();
    char json[LOG_SETTINGS_JSON_MAX];
    size_t len = logSettingsToJson(s, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN(0, len);
    for (size_t cut = 0; cut < len; cut++) {
        std::string prefix(json, cut);
        LogSettings back;
        TEST_ASSERT_FALSE_MESSAGE(logSettingsFromJson(prefix.c_str(), back), prefix.c_str());
    }
}

static void test_log_settings_rejects_overlong_server() {
    std::string name(LOG_HOST_MAX + 1, 'a');
    std::string doc = "{\"version\":1,\"sequence\":1,\"ntpMode\":\"typed\",\"ntp\":\"" + name +
                      "\",\"syslogMode\":\"dhcp\",\"syslog\":\"\"}";
    LogSettings s;
    TEST_ASSERT_FALSE(logSettingsFromJson(doc.c_str(), s));
}

// ---- input checks ----

static void assertTarget(const char* text, bool ipv6, uint16_t port, const uint8_t* addr, size_t addrLen) {
    SyslogTarget t;
    TEST_ASSERT_TRUE_MESSAGE(parseSyslogTarget(text, t), text);
    TEST_ASSERT_EQUAL_MESSAGE(ipv6, t.ipv6, text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(port, t.port, text);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(addr, t.addr, addrLen, text);
}

static void test_parse_syslog_target_forms() {
    const uint8_t v4[4] = {10, 132, 0, 45};
    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5};
    assertTarget("10.132.0.45", false, 514, v4, 4);
    assertTarget("10.132.0.45:5514", false, 5514, v4, 4);
    assertTarget("10.132.0.45:1", false, 1, v4, 4);
    assertTarget("10.132.0.45:65535", false, 65535, v4, 4);
    assertTarget("2001:db8::5", true, 514, v6, 16);
    assertTarget("[2001:db8::5]", true, 514, v6, 16);
    assertTarget("[2001:db8::5]:6514", true, 6514, v6, 16);
}

static void test_parse_syslog_target_rejects_bad_input() {
    const char* bad[] = {"", "10.132.0.45:0", "10.132.0.45:65536", "10.132.0.45:", "10.132.0.45:5x",
                         "[2001:db8::5", "[2001:db8::5]:", "[2001:db8::5]x", "[10.0.0.1]", "host.name",
                         "10.132.0.256", "10.132.0.45:99999999999"};
    for (const char* b : bad) {
        SyslogTarget t;
        TEST_ASSERT_FALSE_MESSAGE(parseSyslogTarget(b, t), b);
    }
    SyslogTarget t;
    TEST_ASSERT_FALSE(parseSyslogTarget(nullptr, t));
}

static void test_ntp_server_text_accepts_names_and_addresses() {
    const char* good[] = {"pool.ntp.org", "ntp", "dk.pool.ntp.org", "a-b.c1", "10.0.0.1", "2001:db8::1",
                          "time.example."};
    for (const char* g : good) TEST_ASSERT_TRUE_MESSAGE(ntpServerTextIsValid(g), g);
    std::string longest(LOG_HOST_MAX, 'a');
    TEST_ASSERT_TRUE(ntpServerTextIsValid(longest.c_str()));
}

static void test_ntp_server_text_rejects_bad_names() {
    const char* bad[] = {"", "-a.b", "a-.b", "a..b", ".a", "a b", "a_b", "ntp:123", "[::1]"};
    for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(ntpServerTextIsValid(b), b);
    std::string tooLong(LOG_HOST_MAX + 1, 'a');
    TEST_ASSERT_FALSE(ntpServerTextIsValid(tooLong.c_str()));
    TEST_ASSERT_FALSE(ntpServerTextIsValid(nullptr));
}

static void test_classify_server_input() {
    TEST_ASSERT_TRUE(classifyServerInput("") == ServerInput::Keep);
    TEST_ASSERT_TRUE(classifyServerInput("dhcp") == ServerInput::Dhcp);
    TEST_ASSERT_TRUE(classifyServerInput("DHCP") == ServerInput::Dhcp);
    TEST_ASSERT_TRUE(classifyServerInput("none") == ServerInput::None);
    TEST_ASSERT_TRUE(classifyServerInput("-") == ServerInput::None);
    TEST_ASSERT_TRUE(classifyServerInput("pool.ntp.org") == ServerInput::Value);
    TEST_ASSERT_TRUE(classifyServerInput(nullptr) == ServerInput::Invalid);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_log_settings_defaults_are_dhcp);
    RUN_TEST(test_log_settings_json_round_trip);
    RUN_TEST(test_log_settings_equal_compares_modes_and_servers);
    RUN_TEST(test_log_settings_longest_values_fit_the_file);
    RUN_TEST(test_log_settings_to_json_too_small_returns_zero);
    RUN_TEST(test_log_settings_rejects_missing_field);
    RUN_TEST(test_log_settings_rejects_bad_values);
    RUN_TEST(test_log_settings_rejects_truncated_json);
    RUN_TEST(test_log_settings_rejects_overlong_server);
    RUN_TEST(test_parse_syslog_target_forms);
    RUN_TEST(test_parse_syslog_target_rejects_bad_input);
    RUN_TEST(test_ntp_server_text_accepts_names_and_addresses);
    RUN_TEST(test_ntp_server_text_rejects_bad_names);
    RUN_TEST(test_classify_server_input);
    return UNITY_END();
}
