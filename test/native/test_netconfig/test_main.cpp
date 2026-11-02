// Host unit tests for lib/netconfig: IPv4 parsing and static configuration validation.
// Run: pio test -e native -f native/test_netconfig

#include <unity.h>
#include <string.h>
#include <netconfig.h>

void setUp() {}
void tearDown() {}

// ---- parseIPv4 ----

static void test_parse_valid_addresses() {
    uint8_t out[4];
    TEST_ASSERT_TRUE(parseIPv4("10.10.6.111", out));
    const uint8_t expected[4] = {10, 10, 6, 111};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 4);

    TEST_ASSERT_TRUE(parseIPv4("0.0.0.0", out));
    TEST_ASSERT_TRUE(parseIPv4("255.255.255.255", out));
    const uint8_t all[4] = {255, 255, 255, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(all, out, 4);
}

static void test_parse_accepts_leading_zeros_within_three_digits() {
    uint8_t out[4];
    TEST_ASSERT_TRUE(parseIPv4("010.001.000.009", out));
    const uint8_t expected[4] = {10, 1, 0, 9};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 4);
}

static void test_parse_rejects_out_of_range_octet() {
    uint8_t out[4];
    TEST_ASSERT_FALSE(parseIPv4("256.1.1.1", out));
    TEST_ASSERT_FALSE(parseIPv4("1.1.1.999", out));
}

static void test_parse_rejects_wrong_octet_count() {
    uint8_t out[4];
    TEST_ASSERT_FALSE(parseIPv4("1.2.3", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.4.5", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.", out));
    TEST_ASSERT_FALSE(parseIPv4(".1.2.3", out));
    TEST_ASSERT_FALSE(parseIPv4("1..2.3", out));
}

static void test_parse_rejects_non_digits_and_whitespace() {
    uint8_t out[4];
    TEST_ASSERT_FALSE(parseIPv4("abc", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.4 ", out));
    TEST_ASSERT_FALSE(parseIPv4(" 1.2.3.4", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.-4", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.+4", out));
    TEST_ASSERT_FALSE(parseIPv4("1.2.3.4a", out));
    TEST_ASSERT_FALSE(parseIPv4("0x1.2.3.4", out));
}

static void test_parse_rejects_four_digit_octet() {
    uint8_t out[4];
    TEST_ASSERT_FALSE(parseIPv4("0001.2.3.4", out));
}

static void test_parse_rejects_empty_and_null() {
    uint8_t out[4];
    TEST_ASSERT_FALSE(parseIPv4("", out));
    TEST_ASSERT_FALSE(parseIPv4(nullptr, out));
}

static void test_parse_leaves_output_untouched_on_failure() {
    uint8_t out[4] = {1, 2, 3, 4};
    TEST_ASSERT_FALSE(parseIPv4("9.9.9.x", out));
    const uint8_t expected[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 4);
}

// ---- isValidSubnetMask ----

static void test_mask_accepts_every_prefix_length_1_to_32() {
    for (int prefix = 1; prefix <= 32; prefix++) {
        uint32_t m = 0xFFFFFFFFu << (32 - prefix);
        uint8_t mask[4] = {(uint8_t)(m >> 24), (uint8_t)(m >> 16), (uint8_t)(m >> 8), (uint8_t)m};
        TEST_ASSERT_TRUE_MESSAGE(isValidSubnetMask(mask), "contiguous mask rejected");
    }
}

static void test_mask_rejects_zero_and_non_contiguous() {
    const uint8_t zero[4] = {0, 0, 0, 0};
    const uint8_t gap[4] = {255, 0, 255, 0};
    const uint8_t inverted[4] = {0, 0, 0, 255};
    const uint8_t hole[4] = {255, 255, 253, 0};
    TEST_ASSERT_FALSE(isValidSubnetMask(zero));
    TEST_ASSERT_FALSE(isValidSubnetMask(gap));
    TEST_ASSERT_FALSE(isValidSubnetMask(inverted));
    TEST_ASSERT_FALSE(isValidSubnetMask(hole));
}

// ---- isSameSubnet ----

static void test_same_subnet() {
    const uint8_t a[4] = {10, 132, 0, 200};
    const uint8_t b[4] = {10, 132, 3, 1};
    const uint8_t c[4] = {10, 132, 4, 1};
    const uint8_t mask22[4] = {255, 255, 252, 0};
    TEST_ASSERT_TRUE(isSameSubnet(a, b, mask22));
    TEST_ASSERT_FALSE(isSameSubnet(a, c, mask22));
}

// ---- validateStaticConfig ----

static void test_validate_accepts_typical_configs() {
    TEST_ASSERT_EQUAL(StaticConfigError::None, validateStaticConfig("192.168.1.100", "255.255.255.0", "192.168.1.1"));
    TEST_ASSERT_EQUAL(StaticConfigError::None, validateStaticConfig("10.132.0.200", "255.255.252.0", "10.132.0.1"));
    TEST_ASSERT_EQUAL(StaticConfigError::None, validateStaticConfig("10.10.6.111", "255.255.0.0", "10.10.0.1"));
}

static void test_validate_reports_first_failing_field() {
    TEST_ASSERT_EQUAL(StaticConfigError::InvalidIP, validateStaticConfig("x", "y", "z"));
    TEST_ASSERT_EQUAL(StaticConfigError::InvalidMask, validateStaticConfig("10.0.0.5", "y", "z"));
    TEST_ASSERT_EQUAL(StaticConfigError::NonContiguousMask, validateStaticConfig("10.0.0.5", "255.0.255.0", "z"));
    TEST_ASSERT_EQUAL(StaticConfigError::InvalidGateway, validateStaticConfig("10.0.0.5", "255.0.0.0", "z"));
}

static void test_validate_rejects_network_and_broadcast_host() {
    TEST_ASSERT_EQUAL(StaticConfigError::HostIsNetworkOrBroadcast,
                      validateStaticConfig("192.168.1.0", "255.255.255.0", "192.168.1.1"));
    TEST_ASSERT_EQUAL(StaticConfigError::HostIsNetworkOrBroadcast,
                      validateStaticConfig("192.168.1.255", "255.255.255.0", "192.168.1.1"));
}

static void test_validate_allows_point_to_point_masks() {
    // /31 (RFC 3021) and /32 have no network/broadcast address
    TEST_ASSERT_EQUAL(StaticConfigError::None, validateStaticConfig("10.0.0.0", "255.255.255.254", "10.0.0.1"));
    TEST_ASSERT_EQUAL(StaticConfigError::GatewayNotInSubnet,
                      validateStaticConfig("10.0.0.5", "255.255.255.255", "10.0.0.1"));
}

static void test_validate_rejects_gateway_outside_subnet() {
    TEST_ASSERT_EQUAL(StaticConfigError::GatewayNotInSubnet,
                      validateStaticConfig("10.10.6.200", "255.255.0.0", "192.168.1.1"));
}

static void test_validate_rejects_gateway_equal_to_host() {
    TEST_ASSERT_EQUAL(StaticConfigError::GatewayIsHost,
                      validateStaticConfig("192.168.1.1", "255.255.255.0", "192.168.1.1"));
}

static void test_every_error_has_a_message() {
    for (uint8_t e = 0; e <= (uint8_t)StaticConfigError::GatewayIsHost; e++) {
        const char* msg = staticConfigErrorMessage((StaticConfigError)e);
        TEST_ASSERT_NOT_NULL(msg);
        TEST_ASSERT_TRUE_MESSAGE(strcmp("Unknown error", msg) != 0, "error code has no message");
    }
}

// ---- hostnames ----

static void test_hostname_rules() {
    TEST_ASSERT_TRUE(hostnameIsValid("rf-power-monitor"));
    TEST_ASSERT_TRUE(hostnameIsValid("UPS4"));
    TEST_ASSERT_TRUE(hostnameIsValid("a"));
    TEST_ASSERT_TRUE(hostnameIsValid("4ups"));                               // RFC 1123 allows a digit first
    TEST_ASSERT_TRUE(hostnameIsValid("a--b"));
    TEST_ASSERT_TRUE(hostnameIsValid("abcdefghijklmnopqrstuvwxyz012345"));  // 32
    TEST_ASSERT_FALSE(hostnameIsValid("abcdefghijklmnopqrstuvwxyz0123456")); // 33
    TEST_ASSERT_FALSE(hostnameIsValid(""));
    TEST_ASSERT_FALSE(hostnameIsValid(nullptr));
    TEST_ASSERT_FALSE(hostnameIsValid("-ups"));
    TEST_ASSERT_FALSE(hostnameIsValid("ups-"));
    TEST_ASSERT_FALSE(hostnameIsValid("-"));
    TEST_ASSERT_FALSE(hostnameIsValid("ups rack"));
    TEST_ASSERT_FALSE(hostnameIsValid("ups.local"));   // one label only
    TEST_ASSERT_FALSE(hostnameIsValid("ups_4"));
    TEST_ASSERT_FALSE(hostnameIsValid("r\xc3\xb8""d"));  // non-ASCII
}

static void test_hostname_fallback_is_the_project_name() {
    TEST_ASSERT_EQUAL_STRING("rf-power-monitor", HOSTNAME_FALLBACK);
}

static void test_hostname_from_free_text() {
    char out[HOSTNAME_MAX_LENGTH + 1];
    struct { const char* in; const char* out; } cases[] = {
        {"rf-power-monitor", "rf-power-monitor"},
        {"a--b", "a--b"},                           // valid names stay exactly as they are
        {"Rack 4 / UPS", "Rack-4-UPS"},
        {"  Server room, rack 4  ", "Server-room-rack-4"},
        {"-ups-", "ups"},
        {"ups_4.local", "ups-4-local"},
        {"caf\xc3\xa9 ups", "caf-ups"},
        {"", HOSTNAME_FALLBACK},
        {" !!! ", HOSTNAME_FALLBACK},
        {nullptr, HOSTNAME_FALLBACK},
        // 40 characters are cut to 32, and a '-' at the cut is dropped
        {"abcdefghijklmnopqrstuvwxyz01234 6789abc", "abcdefghijklmnopqrstuvwxyz01234"},
        {"abcdefghijklmnopqrstuvwxyz012345678901", "abcdefghijklmnopqrstuvwxyz012345"},
    };
    for (auto& c : cases) {
        hostnameFromText(c.in, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.out, out, c.in ? c.in : "(null)");
        TEST_ASSERT_TRUE(hostnameIsValid(out));
    }
}

static const uint8_t DEVICE_MAC[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};

static void test_default_hostname_is_rf_power_and_the_mac_tail() {
    char out[HOSTNAME_MAX_LENGTH + 1];
    hostnameDefault(DEVICE_MAC, out);
    TEST_ASSERT_EQUAL_STRING("RF-Power-4EA22C", out);
    TEST_ASSERT_TRUE(hostnameIsValid(out));
    const uint8_t zeros[6] = {0, 0, 0, 0, 0, 0};
    hostnameDefault(zeros, out);
    TEST_ASSERT_EQUAL_STRING("RF-Power-000000", out);  // leading zeros kept: always 6 digits
}

static void test_unset_hostname_uses_the_mac_default() {
    char out[HOSTNAME_MAX_LENGTH + 1];
    // Empty (new device, factory reset) and the old default from firmware before 3.2.0 count as not set
    const char* unset[] = {"", nullptr, "rf-power-monitor"};
    for (const char* name : unset) {
        hostnameForDevice(name, DEVICE_MAC, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("RF-Power-4EA22C", out, name ? name : "(null)");
    }
}

static void test_chosen_hostname_is_kept() {
    char out[HOSTNAME_MAX_LENGTH + 1];
    hostnameForDevice("ups-rack4", DEVICE_MAC, out);
    TEST_ASSERT_EQUAL_STRING("ups-rack4", out);
    hostnameForDevice("RF-Power-Monitor", DEVICE_MAC, out);  // only the exact old default counts as unset
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor", out);
    hostnameForDevice("Rack 4 / UPS", DEVICE_MAC, out);       // free text from older firmware, cleaned up
    TEST_ASSERT_EQUAL_STRING("Rack-4-UPS", out);
}

static void test_unusable_hostname_text_falls_back_to_the_mac_default() {
    char out[HOSTNAME_MAX_LENGTH + 1];
    hostnameForDevice(" !!! ", DEVICE_MAC, out);
    TEST_ASSERT_EQUAL_STRING("RF-Power-4EA22C", out);
}

static void test_every_printable_text_gives_a_valid_hostname() {
    char text[70], out[HOSTNAME_MAX_LENGTH + 1];
    for (int start = 0x20; start < 0x7F; start++) {
        for (int len = 1; len < 69; len += 7) {
            for (int i = 0; i < len; i++) text[i] = (char)(0x20 + (start + i * 13) % 95);
            text[len] = '\0';
            hostnameFromText(text, out);
            TEST_ASSERT_TRUE_MESSAGE(hostnameIsValid(out), text);
        }
    }
}

// ---- IPv6 ----

static const uint8_t DEVICE_ULA[16] = {0xfd, 0x16, 0xd8, 0x33, 0x4f, 0x0e, 0x3e, 0xce,  // fd16:d833:4f0e:3ece::249
                                       0, 0, 0, 0, 0, 0, 0x02, 0x49};

static void test_parse_ipv6_compressed_and_full_forms() {
    uint8_t a[16];
    TEST_ASSERT_TRUE(parseIPv6("fd16:d833:4f0e:3ece::249", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(DEVICE_ULA, a, 16);
    TEST_ASSERT_TRUE(parseIPv6("FD16:D833:4F0E:3ECE:0000:0000:0000:0249", a));  // upper case, leading zeros
    TEST_ASSERT_EQUAL_UINT8_ARRAY(DEVICE_ULA, a, 16);
    TEST_ASSERT_TRUE(parseIPv6("fd16:d833:4f0e:3ece:0:0:0:249", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(DEVICE_ULA, a, 16);
}

static void test_parse_ipv6_double_colon_at_start_middle_end() {
    uint8_t a[16];
    const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    TEST_ASSERT_TRUE(parseIPv6("::1", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(loopback, a, 16);
    const uint8_t zero[16] = {0};
    TEST_ASSERT_TRUE(parseIPv6("::", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, a, 16);
    const uint8_t prefix[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_TRUE(parseIPv6("2001:db8::", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(prefix, a, 16);
    // "::" for exactly one group
    const uint8_t one[16] = {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 0, 0, 8};
    TEST_ASSERT_TRUE(parseIPv6("1:2:3:4:5:6::8", a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(one, a, 16);
}

static void test_parse_ipv6_rejects_malformed_text() {
    uint8_t a[16];
    const char* bad[] = {
        "", ":", ":::", "1::2::3", "1:::2", ":1:2:3:4:5:6:7", "1:2:3:4:5:6:7:", "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8", "12345::1", "g::1", "fd00::1/64",
        "fe80::1%eth0", "::ffff:10.10.6.65", " ::1", "::1 ", "fd00: :1", "fd00::-1", "10.10.6.65",
        "0000:0000:0000:0000:0000:0000:0000:00001",  // 40 characters
    };
    for (const char* text : bad) TEST_ASSERT_FALSE_MESSAGE(parseIPv6(text, a), text);
    TEST_ASSERT_FALSE(parseIPv6(nullptr, a));
}

static void test_parse_ipv6_longest_text_is_accepted() {
    uint8_t a[16];
    TEST_ASSERT_TRUE(parseIPv6("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", a));  // 39 characters
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, a[i]);
}

static void test_parse_ipv6_leaves_output_untouched_on_failure() {
    uint8_t a[16];
    memset(a, 0xAB, sizeof(a));
    TEST_ASSERT_FALSE(parseIPv6("1:2:3:4:5:6:7:8:9", a));  // fails after parsing eight groups
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX8(0xAB, a[i]);
}

static void test_parse_ipv6_survives_every_truncation() {
    // Every prefix of a valid address either parses or is refused, never read past its end
    const char* full = "fd16:d833:4f0e:3ece:1cb2:cfff:fe4e:a22c";
    char buf[IPV6_TEXT_MAX];
    uint8_t a[16];
    size_t accepted = 0;
    for (size_t n = 0; n <= strlen(full); n++) {
        memcpy(buf, full, n);
        buf[n] = '\0';
        if (parseIPv6(buf, a)) accepted++;
    }
    // The complete text, and the three cut inside the last group ("...:a" is a valid address)
    TEST_ASSERT_EQUAL_size_t(4, accepted);
}

static void test_format_ipv6_rfc5952() {
    char t[IPV6_TEXT_MAX];
    formatIPv6(DEVICE_ULA, t);
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::249", t);
    uint8_t a[16];
    const struct { const char* in; const char* out; } cases[] = {
        {"::", "::"},
        {"::1", "::1"},
        {"2001:db8::", "2001:db8::"},
        {"2001:0db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"},      // tie: the first run
        {"2001:0:0:1:0:0:0:1", "2001:0:0:1::1"},             // the longer run
        {"2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"},    // one zero group stays
        {"FE80::1CB2:CFFF:FE4E:A22C", "fe80::1cb2:cfff:fe4e:a22c"},
        {"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"},
    };
    for (const auto& c : cases) {
        TEST_ASSERT_TRUE_MESSAGE(parseIPv6(c.in, a), c.in);
        formatIPv6(a, t);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.out, t, c.in);
        TEST_ASSERT_TRUE(strlen(t) < IPV6_TEXT_MAX);
    }
}

static void test_validate_ipv6_empty_means_none() {
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6("", ""));
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6(nullptr, nullptr));
    TEST_ASSERT_EQUAL(StaticIPv6Error::GatewayWithoutAddress, validateStaticIPv6("", "fe80::1"));
}

static void test_validate_ipv6_accepts_ula_and_global_with_or_without_gateway() {
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6("fd16:d833:4f0e:3ece::249", ""));
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6("2001:db8:1:2::10", "fe80::1"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6("2001:db8:1:2::10", "2001:db8:1:2::1"));
}

static void test_validate_ipv6_rejects_unusable_addresses() {
    TEST_ASSERT_EQUAL(StaticIPv6Error::InvalidAddress, validateStaticIPv6("fd00::1/64", ""));
    const char* unusable[] = {"::1", "::ffff:a0a:641", "::a0a:641", "fe80::10", "febf::10", "ff02::1",
                              "fd16:d833:4f0e:3ece::"};
    for (const char* a : unusable) {
        TEST_ASSERT_EQUAL_MESSAGE(StaticIPv6Error::AddressNotUsable, validateStaticIPv6(a, ""), a);
    }
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6("fec0::10", ""));  // just outside fe80::/10
}

static void test_validate_ipv6_gateway_rules() {
    const char* addr = "fd16:d833:4f0e:3ece::249";
    TEST_ASSERT_EQUAL(StaticIPv6Error::InvalidGateway, validateStaticIPv6(addr, "router"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::GatewayNotUsable, validateStaticIPv6(addr, "ff02::2"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::GatewayNotUsable, validateStaticIPv6(addr, "::"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::GatewayNotOnLink, validateStaticIPv6(addr, "fd16:d833:4f0e:3ecf::1"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::GatewayIsHost, validateStaticIPv6(addr, "fd16:d833:4f0e:3ece:0::249"));
    TEST_ASSERT_EQUAL(StaticIPv6Error::None, validateStaticIPv6(addr, "fd16:d833:4f0e:3ece::1"));
}

static void test_every_ipv6_error_has_a_message() {
    for (uint8_t e = 0; e <= (uint8_t)StaticIPv6Error::GatewayWithoutAddress; e++) {
        TEST_ASSERT_TRUE(strcmp("Unknown error", staticIPv6ErrorMessage((StaticIPv6Error)e)) != 0);
    }
}

// ---- Trap destination ----

static void test_trap_target_accepts_ipv4() {
    TrapAddress t;
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("10.132.0.45", t));
    TEST_ASSERT_FALSE(t.ipv6);
    const uint8_t want[4] = {10, 132, 0, 45};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, t.addr, 4);
}

static void test_trap_target_accepts_global_and_ula_ipv6() {
    TrapAddress t;
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("2001:db8::162", t));
    TEST_ASSERT_TRUE(t.ipv6);
    const uint8_t want[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x62};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, t.addr, 16);
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("fd16:d833:4f0e:3ece::45", t));
    TEST_ASSERT_TRUE(t.ipv6);
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("fec0::1", t));  // just outside fe80::/10
}

static void test_trap_target_rejects_unusable_ipv6() {
    TrapAddress t;
    const char* unusable[] = {"::", "::1", "::ffff:a0a:641", "fe80::1", "febf::1", "ff02::1", "ff0e::162"};
    for (const char* a : unusable) TEST_ASSERT_EQUAL_MESSAGE(TrapTargetError::NotUsable, parseTrapTarget(a, t), a);
}

static void test_trap_target_rejects_malformed_text() {
    TrapAddress t;
    const char* bad[] = {"", "dhcp", "-", "10.132.0", "10.132.0.256", "fd00::1/64", "fe80::1%eth0",
                         "::ffff:10.132.0.45", " 10.132.0.45", "2001:db8::1 "};
    for (const char* a : bad) TEST_ASSERT_EQUAL_MESSAGE(TrapTargetError::Invalid, parseTrapTarget(a, t), a);
    TEST_ASSERT_EQUAL(TrapTargetError::Invalid, parseTrapTarget(nullptr, t));
}

static void test_trap_target_leaves_output_untouched_on_failure() {
    TrapAddress t;
    memset(&t, 0xAA, sizeof(t));
    TrapAddress before = t;
    TEST_ASSERT_NOT_EQUAL(TrapTargetError::None, parseTrapTarget("ff02::1", t));
    TEST_ASSERT_EQUAL_MEMORY(&before, &t, sizeof(t));
}

static void test_trap_target_survives_every_truncation() {
    const char* full = "fd16:d833:4f0e:3ece:1234:5678:9abc:def0";
    char buf[IPV6_TEXT_MAX];
    TrapAddress t;
    for (size_t n = 0; n < strlen(full); n++) {
        memcpy(buf, full, n);
        buf[n] = '\0';
        parseTrapTarget(buf, t);  // must not crash or read past the end (sanitizers)
    }
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget(full, t));
}

static void test_format_trap_address_ipv4_and_ipv6() {
    TrapAddress t;
    char text[IPV6_TEXT_MAX];
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("255.255.255.255", t));
    formatTrapAddress(t, text);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", text);
    TEST_ASSERT_EQUAL(TrapTargetError::None, parseTrapTarget("2001:DB8:0:0:0:0:0:0162", t));
    formatTrapAddress(t, text);
    TEST_ASSERT_EQUAL_STRING("2001:db8::162", text);
}

static void test_every_trap_target_error_has_a_message() {
    for (uint8_t e = 0; e <= (uint8_t)TrapTargetError::NotUsable; e++) {
        TEST_ASSERT_TRUE(strcmp("Unknown error", trapTargetErrorMessage((TrapTargetError)e)) != 0);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid_addresses);
    RUN_TEST(test_parse_accepts_leading_zeros_within_three_digits);
    RUN_TEST(test_parse_rejects_out_of_range_octet);
    RUN_TEST(test_parse_rejects_wrong_octet_count);
    RUN_TEST(test_parse_rejects_non_digits_and_whitespace);
    RUN_TEST(test_parse_rejects_four_digit_octet);
    RUN_TEST(test_parse_rejects_empty_and_null);
    RUN_TEST(test_parse_leaves_output_untouched_on_failure);
    RUN_TEST(test_mask_accepts_every_prefix_length_1_to_32);
    RUN_TEST(test_mask_rejects_zero_and_non_contiguous);
    RUN_TEST(test_same_subnet);
    RUN_TEST(test_validate_accepts_typical_configs);
    RUN_TEST(test_validate_reports_first_failing_field);
    RUN_TEST(test_validate_rejects_network_and_broadcast_host);
    RUN_TEST(test_validate_allows_point_to_point_masks);
    RUN_TEST(test_validate_rejects_gateway_outside_subnet);
    RUN_TEST(test_validate_rejects_gateway_equal_to_host);
    RUN_TEST(test_every_error_has_a_message);
    RUN_TEST(test_hostname_rules);
    RUN_TEST(test_hostname_fallback_is_the_project_name);
    RUN_TEST(test_hostname_from_free_text);
    RUN_TEST(test_default_hostname_is_rf_power_and_the_mac_tail);
    RUN_TEST(test_unset_hostname_uses_the_mac_default);
    RUN_TEST(test_chosen_hostname_is_kept);
    RUN_TEST(test_unusable_hostname_text_falls_back_to_the_mac_default);
    RUN_TEST(test_every_printable_text_gives_a_valid_hostname);
    RUN_TEST(test_parse_ipv6_compressed_and_full_forms);
    RUN_TEST(test_parse_ipv6_double_colon_at_start_middle_end);
    RUN_TEST(test_parse_ipv6_rejects_malformed_text);
    RUN_TEST(test_parse_ipv6_longest_text_is_accepted);
    RUN_TEST(test_parse_ipv6_leaves_output_untouched_on_failure);
    RUN_TEST(test_parse_ipv6_survives_every_truncation);
    RUN_TEST(test_format_ipv6_rfc5952);
    RUN_TEST(test_validate_ipv6_empty_means_none);
    RUN_TEST(test_validate_ipv6_accepts_ula_and_global_with_or_without_gateway);
    RUN_TEST(test_validate_ipv6_rejects_unusable_addresses);
    RUN_TEST(test_validate_ipv6_gateway_rules);
    RUN_TEST(test_every_ipv6_error_has_a_message);
    RUN_TEST(test_trap_target_accepts_ipv4);
    RUN_TEST(test_trap_target_accepts_global_and_ula_ipv6);
    RUN_TEST(test_trap_target_rejects_unusable_ipv6);
    RUN_TEST(test_trap_target_rejects_malformed_text);
    RUN_TEST(test_trap_target_leaves_output_untouched_on_failure);
    RUN_TEST(test_trap_target_survives_every_truncation);
    RUN_TEST(test_format_trap_address_ipv4_and_ipv6);
    RUN_TEST(test_every_trap_target_error_has_a_message);
    return UNITY_END();
}
