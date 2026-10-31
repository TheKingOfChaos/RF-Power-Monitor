// Host unit tests for lib/settings_codec: JSON round trip, validation, A/B slot selection.
// Run: pio test -e native -f native/test_settings_codec

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <settings_codec.h>
#include <string>

// A well-formed stored web password: 32 hex salt, '$', 64 hex hash
static const char WEB_HASH_EXAMPLE[] =
    "000102030405060708090a0b0c0d0e0f$e26c162ef26c955d928e62311a429a4cf549b87f3f889489c46dd262e14c3356";

void setUp() {}
void tearDown() {}

// A v1 file as written by firmware before the "sequence" field existed
static const char* const LEGACY_V1_JSON =
    "{\"community\":\"public\",\"dhcpEnabled\":true,\"staticIP\":\"192.168.1.100\","
    "\"subnetMask\":\"255.255.255.0\",\"gateway\":\"192.168.1.1\",\"version\":1}";

static Settings staticSettings() {
    Settings s;
    settingsDefaults(s);
    s.dhcpEnabled = false;
    strcpy(s.staticIP, "10.132.0.200");
    strcpy(s.subnetMask, "255.255.252.0");
    strcpy(s.gateway, "10.132.0.1");
    strcpy(s.community, "secret");
    s.sequence = 42;
    return s;
}

// ---- defaults ----

static void test_defaults_are_dhcp_and_public() {
    Settings s;
    memset(&s, 0xAA, sizeof(s));
    settingsDefaults(s);
    TEST_ASSERT_TRUE(s.dhcpEnabled);
    TEST_ASSERT_EQUAL_STRING("public", s.community);
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_VERSION, s.version);
    TEST_ASSERT_EQUAL_UINT32(0, s.sequence);
}

static void test_defaults_survive_round_trip() {
    Settings in, out;
    settingsDefaults(in);
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_TRUE(settingsEqual(in, out));
}

// ---- serialization ----

static void test_round_trip_static_settings() {
    Settings in = staticSettings();
    Settings out;
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    TEST_ASSERT_EQUAL_UINT32(42, out.sequence);
}

static void test_max_length_fields_fit_in_buffer() {
    Settings s = staticSettings();
    memset(s.community, 'x', sizeof(s.community) - 1);
    s.community[sizeof(s.community) - 1] = '\0';
    strcpy(s.staticIP, "255.255.255.254");
    strcpy(s.subnetMask, "255.255.255.255");
    strcpy(s.gateway, "255.255.255.253");
    s.sequence = 0xFFFFFFFFu;
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(s, json, sizeof(json)));
}

static void test_to_json_fails_when_buffer_too_small() {
    Settings s;
    settingsDefaults(s);
    char json[16];
    TEST_ASSERT_EQUAL(0, settingsToJson(s, json, sizeof(json)));
    TEST_ASSERT_EQUAL(0, settingsToJson(s, nullptr, 0));
}

// ---- parsing and validation ----

static void test_legacy_v1_file_loads_with_sequence_zero() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_UINT32(0, s.sequence);
    TEST_ASSERT_TRUE(s.dhcpEnabled);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", s.staticIP);
}

static void test_rejects_empty_and_garbage() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::Empty, settingsFromJson("", s));
    TEST_ASSERT_EQUAL(SettingsParseError::Empty, settingsFromJson(nullptr, s));
    TEST_ASSERT_EQUAL(SettingsParseError::InvalidJson, settingsFromJson("{\"community\":", s));
    TEST_ASSERT_EQUAL(SettingsParseError::InvalidJson, settingsFromJson("[1,2,3]", s));
    TEST_ASSERT_EQUAL(SettingsParseError::InvalidJson, settingsFromJson("\xFF\xFF\xFF", s));
}

static void test_rejects_wrong_or_missing_version() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::WrongVersion,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":true,\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":2}", s));
    TEST_ASSERT_EQUAL(SettingsParseError::WrongVersion,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":true}", s));
}

static void test_rejects_missing_or_mistyped_field() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField,
                      settingsFromJson("{\"community\":\"public\",\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1}", s));
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":\"yes\",\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1}", s));
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":true,\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1,"
                                       "\"sequence\":-1}", s));
}

static void test_rejects_overlong_field_instead_of_truncating() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::FieldTooLong,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":true,"
                                       "\"staticIP\":\"1111.1111.1111.1111\",\"subnetMask\":\"255.0.0.0\","
                                       "\"gateway\":\"1.1.1.2\",\"version\":1}", s));
}

static void test_rejects_empty_or_unprintable_community() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::BadCommunity,
                      settingsFromJson("{\"community\":\"\",\"dhcpEnabled\":true,\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1}", s));
    TEST_ASSERT_EQUAL(SettingsParseError::BadCommunity,
                      settingsFromJson("{\"community\":\"ab\\ncd\",\"dhcpEnabled\":true,\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1}", s));
}

static void test_static_config_validated_only_when_dhcp_disabled() {
    Settings s;
    const char* badStatic =
        "{\"community\":\"public\",\"dhcpEnabled\":%s,\"staticIP\":\"10.0.0.5\","
        "\"subnetMask\":\"255.0.255.0\",\"gateway\":\"10.0.0.1\",\"version\":1}";
    char json[SETTINGS_JSON_MAX];
    snprintf(json, sizeof(json), badStatic, "true");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
    snprintf(json, sizeof(json), badStatic, "false");
    TEST_ASSERT_EQUAL(SettingsParseError::BadStaticConfig, settingsFromJson(json, s));
}

static void test_failed_parse_leaves_output_untouched() {
    Settings s = staticSettings();
    Settings before = s;
    TEST_ASSERT_NOT_EQUAL(SettingsParseError::None, settingsFromJson("{}", s));
    TEST_ASSERT_TRUE(settingsEqual(before, s));
}

static void test_ignores_unknown_fields() {
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::None,
                      settingsFromJson("{\"community\":\"public\",\"dhcpEnabled\":true,\"staticIP\":\"1.1.1.1\","
                                       "\"subnetMask\":\"255.0.0.0\",\"gateway\":\"1.1.1.2\",\"version\":1,"
                                       "\"future\":123}", s));
}

// ---- SNMP text fields (sysName, sysContact, sysLocation) ----

static void test_defaults_include_sys_text() {
    Settings s;
    memset(&s, 0xAA, sizeof(s));
    settingsDefaults(s);
    TEST_ASSERT_EQUAL_STRING("rf-ups-monitor", s.sysName);
    TEST_ASSERT_EQUAL_STRING("", s.sysContact);
    TEST_ASSERT_EQUAL_STRING("", s.sysLocation);
}

static void test_round_trip_sys_text_with_characters_json_must_escape() {
    Settings in = staticSettings();
    strcpy(in.sysName, "ups \"rack\" 4");
    strcpy(in.sysContact, "C:\\ops <ops@example.com>");
    strcpy(in.sysLocation, "Server room / 2nd floor");
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    Settings out;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_TRUE(settingsEqual(in, out));
}

static void test_worst_case_escaped_settings_fit_and_round_trip() {
    // Every text field at maximum length and made of characters JSON escapes to two bytes
    Settings in = staticSettings();
    memset(in.community, '"', COMMUNITY_MAX_LENGTH);
    in.community[COMMUNITY_MAX_LENGTH] = '\0';
    memset(in.sysName, '\\', SYS_TEXT_MAX_LENGTH);
    in.sysName[SYS_TEXT_MAX_LENGTH] = '\0';
    memset(in.sysContact, '"', SYS_TEXT_MAX_LENGTH);
    in.sysContact[SYS_TEXT_MAX_LENGTH] = '\0';
    memset(in.sysLocation, '\\', SYS_TEXT_MAX_LENGTH);
    in.sysLocation[SYS_TEXT_MAX_LENGTH] = '\0';
    strcpy(in.trapTarget, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffd");
    strcpy(in.staticIP, "255.255.255.254");
    strcpy(in.subnetMask, "255.255.255.254");
    strcpy(in.gateway, "255.255.255.255");
    strcpy(in.webPassword, WEB_HASH_EXAMPLE);
    strcpy(in.ipv6Address, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(in.ipv6Gateway, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:ffff");
    in.sequence = 0xFFFFFFFFu;

    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    Settings out;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_TRUE(settingsEqual(in, out));
}

static void test_file_without_sys_text_loads_with_defaults() {
    // Files written by firmware up to v3.1.0 have no sysName/sysContact/sysLocation
    Settings s;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_STRING("rf-ups-monitor", s.sysName);
    TEST_ASSERT_EQUAL_STRING("", s.sysContact);
    TEST_ASSERT_EQUAL_STRING("", s.sysLocation);
}

/// Legacy v1 document plus one extra field
static void withField(char* json, size_t size, const char* field) {
    snprintf(json, size,
             "{\"community\":\"public\",\"dhcpEnabled\":true,\"staticIP\":\"192.168.1.100\","
             "\"subnetMask\":\"255.255.255.0\",\"gateway\":\"192.168.1.1\",\"version\":1,%s}", field);
}

static void test_rejects_overlong_sys_text() {
    char json[SETTINGS_JSON_MAX], field[128];
    char text[SYS_TEXT_MAX_LENGTH + 2];
    memset(text, 'x', SYS_TEXT_MAX_LENGTH + 1);
    text[SYS_TEXT_MAX_LENGTH + 1] = '\0';
    Settings s;
    const char* names[] = {"sysName", "sysContact", "sysLocation"};
    for (const char* name : names) {
        snprintf(field, sizeof(field), "\"%s\":\"%s\"", name, text);
        withField(json, sizeof(json), field);
        TEST_ASSERT_EQUAL_MESSAGE(SettingsParseError::FieldTooLong, settingsFromJson(json, s), name);
    }
    text[SYS_TEXT_MAX_LENGTH] = '\0';  // exactly the maximum is fine
    snprintf(field, sizeof(field), "\"sysName\":\"%s\"", text);
    withField(json, sizeof(json), field);
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

static void test_web_password_defaults_to_empty_and_round_trips() {
    Settings s;
    settingsDefaults(s);
    TEST_ASSERT_EQUAL_STRING("", s.webPassword);  // empty = the device default password
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_STRING("", s.webPassword);

    Settings in = staticSettings(), out;
    strcpy(in.webPassword, WEB_HASH_EXAMPLE);
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_EQUAL_STRING(WEB_HASH_EXAMPLE, out.webPassword);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    out.webPassword[0] = '1';
    TEST_ASSERT_FALSE(settingsEqual(in, out));
}

static void test_web_enabled_and_dhcp_trap_target_defaults_and_round_trip() {
    Settings s;
    settingsDefaults(s);
    TEST_ASSERT_TRUE(s.webEnabled);     // web page on
    TEST_ASSERT_FALSE(s.trapFromDhcp);  // fixed trap destination
    // Files from older firmware have neither field
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_TRUE(s.webEnabled);
    TEST_ASSERT_FALSE(s.trapFromDhcp);

    Settings in = staticSettings(), out;
    in.webEnabled = false;
    in.trapFromDhcp = true;
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_FALSE(out.webEnabled);
    TEST_ASSERT_TRUE(out.trapFromDhcp);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    out.webEnabled = true;
    TEST_ASSERT_FALSE(settingsEqual(in, out));
    out.webEnabled = false;
    out.trapFromDhcp = false;
    TEST_ASSERT_FALSE(settingsEqual(in, out));
}

static void test_rejects_mistyped_web_enabled_and_trap_from_dhcp() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    const char* bad[] = {"\"webEnabled\":1", "\"webEnabled\":\"false\"", "\"trapFromDhcp\":0",
                         "\"trapFromDhcp\":null2"};
    for (const char* b : bad) {
        withField(json, sizeof(json), b);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(SettingsParseError::None, settingsFromJson(json, s), b);
    }
    withField(json, sizeof(json), "\"webEnabled\":false,\"trapFromDhcp\":true");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
    TEST_ASSERT_FALSE(s.webEnabled);
    TEST_ASSERT_TRUE(s.trapFromDhcp);
}

static void test_rejects_malformed_web_password_hash() {
    char json[SETTINGS_JSON_MAX], field[160];
    Settings s;
    std::string good = WEB_HASH_EXAMPLE;
    std::string upper = good;
    upper[50] = 'F';
    std::string noDollar = good;
    noDollar[32] = 'a';
    const std::string bad[] = {"plaintext", good.substr(1), good + "0", upper, noDollar, "$"};
    for (const std::string& b : bad) {
        snprintf(field, sizeof(field), "\"webPassword\":\"%s\"", b.c_str());
        withField(json, sizeof(json), field);
        SettingsParseError err = settingsFromJson(json, s);
        TEST_ASSERT_TRUE_MESSAGE(err == SettingsParseError::BadWebPassword || err == SettingsParseError::FieldTooLong,
                                 b.c_str());
    }
    withField(json, sizeof(json), "\"webPassword\":7");
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"webPassword\":\"\"");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

static void test_rejects_unprintable_or_mistyped_sys_text() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    withField(json, sizeof(json), "\"sysContact\":\"line1\\nline2\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadSysText, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"sysLocation\":\"caf\\u00e9\"");  // non-ASCII
    TEST_ASSERT_EQUAL(SettingsParseError::BadSysText, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"sysName\":42");
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField, settingsFromJson(json, s));
}

static void test_empty_sys_text_is_allowed() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    withField(json, sizeof(json), "\"sysName\":\"\",\"sysContact\":\"\",\"sysLocation\":\"\"");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
    TEST_ASSERT_EQUAL_STRING("", s.sysName);
}

static void test_settings_text_is_valid() {
    TEST_ASSERT_TRUE(settingsTextIsValid("public", COMMUNITY_MAX_LENGTH, false));
    TEST_ASSERT_TRUE(settingsTextIsValid(" ~!\"#$%&'()*+,-./0123456789:;<=>?@[\\]^_`{|}", 63, false));
    TEST_ASSERT_TRUE(settingsTextIsValid("", 63, true));
    TEST_ASSERT_FALSE(settingsTextIsValid("", 31, false));
    TEST_ASSERT_FALSE(settingsTextIsValid(nullptr, 31, true));
    TEST_ASSERT_FALSE(settingsTextIsValid("tab\there", 63, true));
    TEST_ASSERT_FALSE(settingsTextIsValid("del\x7f", 63, true));
    TEST_ASSERT_FALSE(settingsTextIsValid("\xc3\xa9", 63, true));
    TEST_ASSERT_TRUE(settingsTextIsValid("abc", 3, false));
    TEST_ASSERT_FALSE(settingsTextIsValid("abcd", 3, false));
}

// ---- trap destination ----

static void test_trap_target_defaults_to_off() {
    Settings s;
    settingsDefaults(s);
    TEST_ASSERT_EQUAL_STRING("", s.trapTarget);
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_STRING("", s.trapTarget);
}

static void test_trap_target_round_trip() {
    Settings in = staticSettings();
    strcpy(in.trapTarget, "10.10.6.65");
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    Settings out;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_EQUAL_STRING("10.10.6.65", out.trapTarget);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    Settings other = in;
    strcpy(other.trapTarget, "10.10.6.66");
    TEST_ASSERT_FALSE(settingsEqual(in, other));
}

static void test_rejects_invalid_trap_target() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    withField(json, sizeof(json), "\"trapTarget\":\"not-an-ip\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadTrapTarget, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"10.10.6.256\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadTrapTarget, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"255.255.255.255.1\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadTrapTarget, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"fd16:d833:4f0e:3ece:ffff:ffff:ffff:ffff0\"");  // 40 characters
    TEST_ASSERT_EQUAL(SettingsParseError::FieldTooLong, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"\"");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

static void test_ipv6_trap_target_round_trips() {
    Settings in = staticSettings();
    strcpy(in.trapTarget, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");  // longest text
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    Settings out;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe", out.trapTarget);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
}

static void test_rejects_unusable_ipv6_trap_target() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    const char* bad[] = {"\"trapTarget\":\"fe80::1\"", "\"trapTarget\":\"ff02::1\"", "\"trapTarget\":\"::1\"",
                         "\"trapTarget\":\"2001:db8::1/64\""};
    for (const char* field : bad) {
        withField(json, sizeof(json), field);
        TEST_ASSERT_EQUAL_MESSAGE(SettingsParseError::BadTrapTarget, settingsFromJson(json, s), field);
    }
}

static void test_ipv6_trap_target_needs_v2c() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    withField(json, sizeof(json), "\"trapTarget\":\"2001:db8::162\",\"trapVersion\":1");
    TEST_ASSERT_EQUAL(SettingsParseError::TrapIPv6NeedsV2c, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"2001:db8::162\",\"trapVersion\":2");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
    // While DHCP option 224 is used, the kept fixed address is not sent to
    withField(json, sizeof(json), "\"trapTarget\":\"2001:db8::162\",\"trapVersion\":1,\"trapFromDhcp\":true");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"trapTarget\":\"10.132.0.45\",\"trapVersion\":1");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

static void test_trap_version_fits_target() {
    TEST_ASSERT_TRUE(trapVersionFitsTarget("", false, TRAP_VERSION_V1));
    TEST_ASSERT_TRUE(trapVersionFitsTarget("10.132.0.45", false, TRAP_VERSION_V1));
    TEST_ASSERT_TRUE(trapVersionFitsTarget("2001:db8::162", false, TRAP_VERSION_V2C));
    TEST_ASSERT_FALSE(trapVersionFitsTarget("2001:db8::162", false, TRAP_VERSION_V1));
    TEST_ASSERT_TRUE(trapVersionFitsTarget("2001:db8::162", true, TRAP_VERSION_V1));
}

static void test_trap_version_defaults_to_v2c_and_round_trips() {
    Settings s;
    settingsDefaults(s);
    TEST_ASSERT_EQUAL_UINT8(TRAP_VERSION_V2C, s.trapVersion);
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_UINT8(TRAP_VERSION_V2C, s.trapVersion);

    Settings in = staticSettings();
    in.trapVersion = TRAP_VERSION_V1;
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    Settings out;
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_EQUAL_UINT8(TRAP_VERSION_V1, out.trapVersion);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    out.trapVersion = TRAP_VERSION_V2C;
    TEST_ASSERT_FALSE(settingsEqual(in, out));
}

static void test_rejects_invalid_trap_version() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    const char* bad[] = {"\"trapVersion\":0", "\"trapVersion\":3", "\"trapVersion\":\"2c\"", "\"trapVersion\":-1"};
    for (const char* field : bad) {
        withField(json, sizeof(json), field);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(SettingsParseError::None, settingsFromJson(json, s), field);
    }
    withField(json, sizeof(json), "\"trapVersion\":1");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

static void test_every_parse_error_has_a_message() {
    for (uint8_t e = 0; e <= (uint8_t)SettingsParseError::TrapIPv6NeedsV2c; e++) {
        TEST_ASSERT_TRUE_MESSAGE(strcmp("unknown error", settingsParseErrorMessage((SettingsParseError)e)) != 0,
                                 "error code has no message");
    }
}

// ---- equality ----

static void test_equal_ignores_bytes_after_terminator() {
    Settings a = staticSettings();
    Settings b = a;
    // Simulate a shorter string copied over a longer one: stale bytes remain after the NUL
    strcpy(b.staticIP, "10.132.0.255");
    strcpy(b.staticIP, "10.132.0.200");
    b.community[10] = 'Z';
    strcpy(b.sysName, "a-much-longer-name");
    strcpy(b.sysName, a.sysName);
    TEST_ASSERT_TRUE(settingsEqual(a, b));
    b.sequence++;
    TEST_ASSERT_FALSE(settingsEqual(a, b));

    // Each SNMP text field takes part in the comparison
    Settings c = a;
    strcpy(c.sysName, "other");
    TEST_ASSERT_FALSE(settingsEqual(a, c));
    c = a;
    strcpy(c.sysContact, "other");
    TEST_ASSERT_FALSE(settingsEqual(a, c));
    c = a;
    strcpy(c.sysLocation, "other");
    TEST_ASSERT_FALSE(settingsEqual(a, c));
}

// ---- sequence and slot selection ----

static void test_sequence_newer_handles_wraparound() {
    TEST_ASSERT_TRUE(sequenceIsNewer(2, 1));
    TEST_ASSERT_FALSE(sequenceIsNewer(1, 2));
    TEST_ASSERT_FALSE(sequenceIsNewer(5, 5));
    TEST_ASSERT_TRUE(sequenceIsNewer(0, 0xFFFFFFFFu));
    TEST_ASSERT_FALSE(sequenceIsNewer(0xFFFFFFFFu, 0));
}

static void test_choose_newest_valid_slot() {
    TEST_ASSERT_EQUAL(SettingsSlot::B, chooseActiveSlot(true, 3, true, 4));
    TEST_ASSERT_EQUAL(SettingsSlot::A, chooseActiveSlot(true, 5, true, 4));
    TEST_ASSERT_EQUAL(SettingsSlot::A, chooseActiveSlot(true, 0, true, 0xFFFFFFFFu));
}

static void test_choose_falls_back_to_the_only_valid_slot() {
    // A newer copy that failed validation must not win over an older valid one
    TEST_ASSERT_EQUAL(SettingsSlot::A, chooseActiveSlot(true, 3, false, 99));
    TEST_ASSERT_EQUAL(SettingsSlot::B, chooseActiveSlot(false, 99, true, 3));
    TEST_ASSERT_EQUAL(SettingsSlot::None, chooseActiveSlot(false, 0, false, 0));
}

static void test_tie_prefers_slot_a() {
    TEST_ASSERT_EQUAL(SettingsSlot::A, chooseActiveSlot(true, 7, true, 7));
}

static void test_next_write_slot_alternates() {
    TEST_ASSERT_EQUAL(SettingsSlot::B, nextWriteSlot(SettingsSlot::A));
    TEST_ASSERT_EQUAL(SettingsSlot::A, nextWriteSlot(SettingsSlot::B));
    TEST_ASSERT_EQUAL(SettingsSlot::A, nextWriteSlot(SettingsSlot::None));
}

// ---- static IPv6 ----

static void test_static_ipv6_defaults_to_none_and_old_files_load() {
    Settings s;
    settingsDefaults(s);
    TEST_ASSERT_EQUAL_STRING("", s.ipv6Address);
    TEST_ASSERT_EQUAL_STRING("", s.ipv6Gateway);
    strcpy(s.ipv6Address, "fd00::1");  // the file has no IPv6 fields: they must come back empty
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(LEGACY_V1_JSON, s));
    TEST_ASSERT_EQUAL_STRING("", s.ipv6Address);
    TEST_ASSERT_EQUAL_STRING("", s.ipv6Gateway);
}

static void test_static_ipv6_round_trips_and_is_compared() {
    Settings in = staticSettings(), out;
    strcpy(in.ipv6Address, "fd16:d833:4f0e:3ece::249");
    strcpy(in.ipv6Gateway, "fe80::1");
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::249", out.ipv6Address);
    TEST_ASSERT_EQUAL_STRING("fe80::1", out.ipv6Gateway);
    TEST_ASSERT_TRUE(settingsEqual(in, out));
    strcpy(out.ipv6Gateway, "fe80::2");
    TEST_ASSERT_FALSE(settingsEqual(in, out));
    strcpy(out.ipv6Gateway, "fe80::1");
    strcpy(out.ipv6Address, "fd16:d833:4f0e:3ece::250");
    TEST_ASSERT_FALSE(settingsEqual(in, out));
}

static void test_static_ipv6_works_with_dhcp_for_ipv4() {
    Settings in, out;
    settingsDefaults(in);  // DHCP
    strcpy(in.ipv6Address, "2001:db8:1:2::10");
    char json[SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, settingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, out));
    TEST_ASSERT_TRUE(out.dhcpEnabled);
    TEST_ASSERT_EQUAL_STRING("2001:db8:1:2::10", out.ipv6Address);
}

static void test_rejects_invalid_static_ipv6() {
    char json[SETTINGS_JSON_MAX];
    Settings s;
    withField(json, sizeof(json), "\"ipv6Address\":\"fe80::10\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadStaticIPv6, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"ipv6Address\":\"fd00::1/64\"");
    TEST_ASSERT_EQUAL(SettingsParseError::BadStaticIPv6, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"ipv6Gateway\":\"fe80::1\"");  // gateway without address
    TEST_ASSERT_EQUAL(SettingsParseError::BadStaticIPv6, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"ipv6Address\":\"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff:1\"");
    TEST_ASSERT_EQUAL(SettingsParseError::FieldTooLong, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"ipv6Address\":7");
    TEST_ASSERT_EQUAL(SettingsParseError::MissingField, settingsFromJson(json, s));
    withField(json, sizeof(json), "\"ipv6Address\":\"\"");
    TEST_ASSERT_EQUAL(SettingsParseError::None, settingsFromJson(json, s));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_dhcp_and_public);
    RUN_TEST(test_defaults_survive_round_trip);
    RUN_TEST(test_round_trip_static_settings);
    RUN_TEST(test_max_length_fields_fit_in_buffer);
    RUN_TEST(test_to_json_fails_when_buffer_too_small);
    RUN_TEST(test_legacy_v1_file_loads_with_sequence_zero);
    RUN_TEST(test_rejects_empty_and_garbage);
    RUN_TEST(test_rejects_wrong_or_missing_version);
    RUN_TEST(test_rejects_missing_or_mistyped_field);
    RUN_TEST(test_rejects_overlong_field_instead_of_truncating);
    RUN_TEST(test_rejects_empty_or_unprintable_community);
    RUN_TEST(test_static_config_validated_only_when_dhcp_disabled);
    RUN_TEST(test_failed_parse_leaves_output_untouched);
    RUN_TEST(test_ignores_unknown_fields);
    RUN_TEST(test_defaults_include_sys_text);
    RUN_TEST(test_round_trip_sys_text_with_characters_json_must_escape);
    RUN_TEST(test_worst_case_escaped_settings_fit_and_round_trip);
    RUN_TEST(test_file_without_sys_text_loads_with_defaults);
    RUN_TEST(test_rejects_overlong_sys_text);
    RUN_TEST(test_web_password_defaults_to_empty_and_round_trips);
    RUN_TEST(test_web_enabled_and_dhcp_trap_target_defaults_and_round_trip);
    RUN_TEST(test_rejects_mistyped_web_enabled_and_trap_from_dhcp);
    RUN_TEST(test_rejects_malformed_web_password_hash);
    RUN_TEST(test_rejects_unprintable_or_mistyped_sys_text);
    RUN_TEST(test_empty_sys_text_is_allowed);
    RUN_TEST(test_settings_text_is_valid);
    RUN_TEST(test_trap_target_defaults_to_off);
    RUN_TEST(test_trap_target_round_trip);
    RUN_TEST(test_rejects_invalid_trap_target);
    RUN_TEST(test_ipv6_trap_target_round_trips);
    RUN_TEST(test_rejects_unusable_ipv6_trap_target);
    RUN_TEST(test_ipv6_trap_target_needs_v2c);
    RUN_TEST(test_trap_version_fits_target);
    RUN_TEST(test_trap_version_defaults_to_v2c_and_round_trips);
    RUN_TEST(test_rejects_invalid_trap_version);
    RUN_TEST(test_every_parse_error_has_a_message);
    RUN_TEST(test_equal_ignores_bytes_after_terminator);
    RUN_TEST(test_sequence_newer_handles_wraparound);
    RUN_TEST(test_choose_newest_valid_slot);
    RUN_TEST(test_choose_falls_back_to_the_only_valid_slot);
    RUN_TEST(test_tie_prefers_slot_a);
    RUN_TEST(test_next_write_slot_alternates);
    RUN_TEST(test_static_ipv6_defaults_to_none_and_old_files_load);
    RUN_TEST(test_static_ipv6_round_trips_and_is_compared);
    RUN_TEST(test_static_ipv6_works_with_dhcp_for_ipv4);
    RUN_TEST(test_rejects_invalid_static_ipv6);
    return UNITY_END();
}
