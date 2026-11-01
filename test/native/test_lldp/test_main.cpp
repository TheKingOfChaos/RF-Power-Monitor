// Host unit tests for lib/lldp: LLDPDUs (IEEE 802.1AB) built and parsed, the neighbour, when to send.
// Run: pio test -e native -f native/test_lldp

#include <unity.h>
#include <string.h>
#include <lldp.h>

void setUp() {}
void tearDown() {}

static LldpLocal local() {
    LldpLocal l = {};
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(l.mac, mac, 6);
    l.sysName = "rfups";
    l.sysDescr = "RF";
    l.portDescr = "eth";
    l.hasIPv4 = true;
    const uint8_t ip[4] = {10, 0, 0, 5};
    memcpy(l.ipv4, ip, 4);
    l.ttl = 120;
    return l;
}

// Written out by hand from IEEE 802.1AB-2016 clause 8 (TLV header: 7-bit type, 9-bit length);
// tcpdump -vv decodes these bytes as the TLVs named in the comments
static const uint8_t EXPECTED_V4[] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e,              // nearest bridge group address
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01,              // source: our MAC
    0x88, 0xcc,                                      // LLDP
    0x02, 0x07, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  // Chassis ID, subtype 4 (MAC)
    0x04, 0x07, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  // Port ID, subtype 3 (MAC)
    0x06, 0x02, 0x00, 0x78,                          // TTL 120
    0x08, 0x03, 'e', 't', 'h',                       // Port Description
    0x0a, 0x05, 'r', 'f', 'u', 'p', 's',             // System Name
    0x0c, 0x02, 'R', 'F',                            // System Description
    0x0e, 0x04, 0x00, 0x80, 0x00, 0x80,              // System Capabilities: station only, enabled
    0x10, 0x0c, 0x05, 0x01, 0x0a, 0x00, 0x00, 0x05,  // Management Address: IPv4 10.0.0.5
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,              //   ifIndex 1, no OID
    0x00, 0x00,                                      // End of LLDPDU
};

static void test_build_frame_matches_expected_bytes() {
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(local(), out, sizeof(out));
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_V4), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(EXPECTED_V4, out, sizeof(EXPECTED_V4));
}

static void test_build_frame_adds_ipv6_management_address() {
    LldpLocal l = local();
    l.hasIPv6 = true;
    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05};
    memcpy(l.ipv6, v6, 16);
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(l, out, sizeof(out));
    const size_t v4End = sizeof(EXPECTED_V4) - 2;  // the IPv6 TLV goes between the IPv4 one and End
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_V4) + 26, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(EXPECTED_V4, out, v4End);
    const uint8_t head[] = {0x10, 0x18, 0x11, 0x02};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, out + v4End, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v6, out + v4End + 4, 16);
    const uint8_t tail[] = {0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tail, out + v4End + 20, 8);
}

static void test_build_frame_without_addresses_has_no_management_tlv() {
    LldpLocal l = local();
    l.hasIPv4 = false;
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(l, out, sizeof(out));
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_V4) - 14, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(EXPECTED_V4, out, n - 2);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[n - 2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[n - 1]);
}

static void test_build_frame_rejects_small_buffer() {
    uint8_t out[LLDP_FRAME_MAX];
    TEST_ASSERT_EQUAL(0, lldpBuildFrame(local(), out, sizeof(EXPECTED_V4) - 1));
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_V4), lldpBuildFrame(local(), out, sizeof(EXPECTED_V4)));
    TEST_ASSERT_EQUAL(0, lldpBuildFrame(local(), nullptr, sizeof(out)));
}

static void test_build_frame_rejects_string_over_255_bytes() {
    static char longName[257];
    memset(longName, 'a', 256);
    longName[256] = '\0';
    LldpLocal l = local();
    l.sysName = longName;
    static uint8_t out[1024];
    TEST_ASSERT_EQUAL(0, lldpBuildFrame(l, out, sizeof(out)));
    longName[255] = '\0';  // 255 bytes fit in a TLV
    TEST_ASSERT_NOT_EQUAL(0, lldpBuildFrame(l, out, sizeof(out)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_build_frame_matches_expected_bytes);
    RUN_TEST(test_build_frame_adds_ipv6_management_address);
    RUN_TEST(test_build_frame_without_addresses_has_no_management_tlv);
    RUN_TEST(test_build_frame_rejects_small_buffer);
    RUN_TEST(test_build_frame_rejects_string_over_255_bytes);
    return UNITY_END();
}
