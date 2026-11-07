// Host unit tests for lib/syslog: server settings codec and input checks.
// Run: pio test -e native -f native/test_syslog

#include <stdio.h>
#include <string.h>

#include <string>

#include <unity.h>
#include <log_settings.h>
#include <syslog_message.h>

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

// ---- messages, queue, throttle ----

static void test_syslog_format_rfc5424_layout() {
    char buf[SYSLOG_PACKET_MAX];
    size_t n = syslogFormat(buf, sizeof(buf), SyslogSeverity::Warning, true, 1762540930123ull, "rf-power-abcd", "MAINS",
                            "mains power lost");
    TEST_ASSERT_EQUAL_STRING("<28>1 2025-11-07T18:42:10.123Z rf-power-abcd rfpower - MAINS - mains power lost", buf);
    TEST_ASSERT_EQUAL(strlen(buf), n);
    syslogFormat(buf, sizeof(buf), SyslogSeverity::Critical, true, 1762540930123ull, "h", "MAINS", "x");
    TEST_ASSERT_EQUAL_STRING("<26>1 2025-11-07T18:42:10.123Z h rfpower - MAINS - x", buf);
}

static void test_syslog_format_undated_uses_dash() {
    char buf[SYSLOG_PACKET_MAX];
    syslogFormat(buf, sizeof(buf), SyslogSeverity::Notice, false, 0, "", "", "boot");
    TEST_ASSERT_EQUAL_STRING("<29>1 - - rfpower - - - boot", buf);
}

static void test_syslog_format_cuts_long_text() {
    std::string text(300, 'x');
    char buf[SYSLOG_PACKET_MAX];
    size_t n = syslogFormat(buf, sizeof(buf), SyslogSeverity::Info, false, 0, "h", "WEB", text.c_str());
    TEST_ASSERT_GREATER_THAN(0, n);
    const char* body = strstr(buf, "WEB - ") + 6;
    TEST_ASSERT_EQUAL(SYSLOG_TEXT_MAX, strlen(body));
}

static void test_syslog_format_replaces_control_chars() {
    char buf[SYSLOG_PACKET_MAX];
    syslogFormat(buf, sizeof(buf), SyslogSeverity::Info, false, 0, "h", "WEB", "a\r\nb\tc\x7f");
    TEST_ASSERT_NOT_NULL(strstr(buf, " - a??b?c?"));
}

static void test_syslog_format_too_small_buffer_returns_zero() {
    char buf[20];
    TEST_ASSERT_EQUAL(0, syslogFormat(buf, sizeof(buf), SyslogSeverity::Info, true, 1762540930123ull, "host", "MAINS",
                                      "mains power lost"));
    TEST_ASSERT_EQUAL(0, syslogFormat(nullptr, 0, SyslogSeverity::Info, false, 0, "h", "M", "t"));
}

static SyslogEntry entry(uint32_t ms, const char* text) {
    SyslogEntry e;
    memset(&e, 0, sizeof(e));
    e.uptimeMs = ms;
    e.severity = SyslogSeverity::Notice;
    strcpy(e.msgId, "TEST");
    strcpy(e.text, text);
    return e;
}

static void test_queue_keeps_order() {
    static SyslogQueue q;
    q = SyslogQueue();
    SyslogEntry out;
    TEST_ASSERT_FALSE(q.peek(out));
    q.push(entry(1, "one"));
    q.push(entry(2, "two"));
    TEST_ASSERT_EQUAL(2, q.size());
    TEST_ASSERT_TRUE(q.peek(out));
    TEST_ASSERT_EQUAL_STRING("one", out.text);
    q.pop();
    TEST_ASSERT_TRUE(q.peek(out));
    TEST_ASSERT_EQUAL_STRING("two", out.text);
    q.pop();
    TEST_ASSERT_EQUAL(0, q.size());
    q.pop();  // popping an empty queue does nothing
    TEST_ASSERT_EQUAL(0, q.size());
}

static void test_queue_full_drops_oldest_and_counts() {
    static SyslogQueue q;
    q = SyslogQueue();
    char t[16];
    for (int i = 0; i < SyslogQueue::CAPACITY + 3; i++) {
        snprintf(t, sizeof(t), "e%d", i);
        q.push(entry((uint32_t)i, t));
    }
    TEST_ASSERT_EQUAL(SyslogQueue::CAPACITY, q.size());
    SyslogEntry out;
    q.peek(out);
    TEST_ASSERT_EQUAL_STRING("e3", out.text);
    TEST_ASSERT_EQUAL_UINT32(3, q.takeDropped());
    TEST_ASSERT_EQUAL_UINT32(0, q.takeDropped());
}

static void test_throttle_allows_five_per_minute_then_counts() {
    SyslogThrottle t(5, 60000);
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(t.allow(1000 + i));
    TEST_ASSERT_FALSE(t.allow(2000));
    TEST_ASSERT_FALSE(t.allow(60999));
    TEST_ASSERT_EQUAL_UINT32(2, t.takeSuppressed());
    TEST_ASSERT_EQUAL_UINT32(0, t.takeSuppressed());
    TEST_ASSERT_TRUE(t.allow(61000));  // a new window
}

static void test_throttle_window_survives_millis_wrap() {
    SyslogThrottle t(1, 60000);
    TEST_ASSERT_TRUE(t.allow(0xFFFFFF00u));
    TEST_ASSERT_FALSE(t.allow(0x00000100u));  // 512 ms later, across the wrap
    TEST_ASSERT_TRUE(t.allow(0xFFFFFF00u + 60000u));
}

static void test_event_epoch_dated_from_uptime_not_sync_moment() {
    // Event at uptime 5 s; the clock is set at uptime 65 s to E: the event happened at E - 60 s
    const uint64_t E = 1762540930000ull;
    TEST_ASSERT_EQUAL_UINT64(E - 60000, syslogEventEpochMs(5000, 65000, E));
    TEST_ASSERT_EQUAL_UINT64(E, syslogEventEpochMs(65000, 65000, E));
}

static void test_event_epoch_survives_millis_wrap() {
    const uint64_t E = 1762540930000ull;
    TEST_ASSERT_EQUAL_UINT64(E - 0x200, syslogEventEpochMs(0xFFFFFF00u, 0x00000100u, E));
}

// ---- applying typed input (serial menu, web page) ----

static void test_apply_input_empty_keeps_values() {
    LogSettings s = typedSettings();
    const char* error = nullptr;
    TEST_ASSERT_TRUE(logSettingsApplyInput(s, "", "", error));
    TEST_ASSERT_TRUE(logSettingsEqual(s, typedSettings()));
}

static void test_apply_input_sets_modes() {
    LogSettings s = typedSettings();
    const char* error = nullptr;
    TEST_ASSERT_TRUE(logSettingsApplyInput(s, "dhcp", "none", error));
    TEST_ASSERT_TRUE(s.ntpMode == ServerMode::Dhcp);
    TEST_ASSERT_TRUE(s.syslogMode == ServerMode::None);
    TEST_ASSERT_EQUAL_STRING("", s.ntpServer);
    TEST_ASSERT_EQUAL_STRING("", s.syslogServer);
    TEST_ASSERT_TRUE(logSettingsApplyInput(s, "10.0.0.1", "10.132.0.45:5514", error));
    TEST_ASSERT_TRUE(s.ntpMode == ServerMode::Typed);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", s.ntpServer);
    TEST_ASSERT_TRUE(s.syslogMode == ServerMode::Typed);
    TEST_ASSERT_EQUAL_STRING("10.132.0.45:5514", s.syslogServer);
}

static void test_apply_input_bad_ntp_changes_nothing() {
    LogSettings s = typedSettings();
    const char* error = nullptr;
    TEST_ASSERT_FALSE(logSettingsApplyInput(s, "a..b", "none", error));
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_TRUE(logSettingsEqual(s, typedSettings()));
}

static void test_apply_input_bad_syslog_changes_nothing() {
    LogSettings s = typedSettings();
    const char* error = nullptr;
    TEST_ASSERT_FALSE(logSettingsApplyInput(s, "dhcp", "1.2.3.4:0", error));
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_TRUE(logSettingsEqual(s, typedSettings()));
    TEST_ASSERT_FALSE(logSettingsApplyInput(s, "dhcp", "syslog.example.com", error));  // names: NTP only
    TEST_ASSERT_FALSE(logSettingsApplyInput(s, nullptr, "none", error));
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
    RUN_TEST(test_apply_input_empty_keeps_values);
    RUN_TEST(test_apply_input_sets_modes);
    RUN_TEST(test_apply_input_bad_ntp_changes_nothing);
    RUN_TEST(test_apply_input_bad_syslog_changes_nothing);
    RUN_TEST(test_syslog_format_rfc5424_layout);
    RUN_TEST(test_syslog_format_undated_uses_dash);
    RUN_TEST(test_syslog_format_cuts_long_text);
    RUN_TEST(test_syslog_format_replaces_control_chars);
    RUN_TEST(test_syslog_format_too_small_buffer_returns_zero);
    RUN_TEST(test_queue_keeps_order);
    RUN_TEST(test_queue_full_drops_oldest_and_counts);
    RUN_TEST(test_throttle_allows_five_per_minute_then_counts);
    RUN_TEST(test_throttle_window_survives_millis_wrap);
    RUN_TEST(test_event_epoch_dated_from_uptime_not_sync_moment);
    RUN_TEST(test_event_epoch_survives_millis_wrap);
    return UNITY_END();
}
