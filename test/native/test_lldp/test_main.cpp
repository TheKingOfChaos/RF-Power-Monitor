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
    l.sysName = "rfpow";
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
    0x0a, 0x05, 'r', 'f', 'p', 'o', 'w',             // System Name
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

static void test_build_frame_fits_with_real_texts() {
    // The real sysDescr and the longest hostname, with both management addresses
    LldpLocal l = local();
    l.sysDescr = "RF-Power-Monitor 3.0.0 mains power monitor (RP2040, W5500)";
    l.sysName = "abcdefghijklmnopqrstuvwxyz-12345";  // 32 characters, HOSTNAME_MAX_LENGTH
    l.portDescr = "W5500 10/100 Ethernet";
    l.hasIPv6 = true;
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(l, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_LESS_OR_EQUAL(LLDP_FRAME_MAX, n);
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

// ---- parsing ----

// Built by hand the way Junos sends it: Port ID is the ifIndex (subtype 7, locally assigned) and the
// interface name is in Port Description; organisational TLVs (802.1, LLDP-MED) the parser must skip
static const uint8_t JUNOS_STYLE[] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e, 0x4c, 0x96, 0x14, 0xaa, 0xbb, 0xcc, 0x88, 0xcc,
    0x02, 0x07, 0x04, 0x4c, 0x96, 0x14, 0xaa, 0xbb, 0xc0,  // Chassis ID, MAC
    0x04, 0x04, 0x07, '5', '1', '3',                     // Port ID, locally assigned "513"
    0x06, 0x02, 0x00, 0x78,                              // TTL 120
    0x08, 0x0a, 'g', 'e', '-', '0', '/', '0', '/', '5', '.', '0',  // Port Description
    0x0a, 0x06, 'e', 'x', '2', '2', '0', '0',            // System Name
    0xfe, 0x06, 0x00, 0x80, 0xc2, 0x01, 0x00, 0x01,      // 802.1 Port VLAN ID 1
    0xfe, 0x07, 0x00, 0x12, 0xbb, 0x01, 0x00, 0x0f, 0x04,  // LLDP-MED capabilities
    0x10, 0x0c, 0x05, 0x01, 0x0a, 0x84, 0x00, 0x02,      // Management Address 10.132.0.2
    0x02, 0x00, 0x00, 0x00, 0x22, 0x00,
    0x00, 0x00,                                          // End
};
static const size_t JUNOS_TTL_AT = 14 + 9 + 6;  // offset of the TTL TLV

static LldpNeighbour nb;  // static: the struct is over 256 bytes, as in the firmware

static void test_parse_reads_junos_style_frame() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(JUNOS_STYLE, sizeof(JUNOS_STYLE), nb));
    TEST_ASSERT_EQUAL_STRING("4c:96:14:aa:bb:c0", nb.chassisId);
    TEST_ASSERT_EQUAL(7, nb.portIdSubtype);
    TEST_ASSERT_EQUAL_STRING("513", nb.portId);
    TEST_ASSERT_EQUAL_STRING("ge-0/0/5.0", nb.portDescr);
    TEST_ASSERT_EQUAL_STRING("ex2200", nb.sysName);
    TEST_ASSERT_EQUAL_STRING("10.132.0.2", nb.mgmtAddr);
    TEST_ASSERT_EQUAL(120, nb.ttl);
}

static void test_parse_reads_own_built_frame() {
    LldpLocal l = local();
    l.hasIPv6 = true;
    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05};
    memcpy(l.ipv6, v6, 16);
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(l, out, sizeof(out));
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(out, n, nb));
    TEST_ASSERT_EQUAL_STRING("02:00:00:00:00:01", nb.chassisId);
    TEST_ASSERT_EQUAL(3, nb.portIdSubtype);
    TEST_ASSERT_EQUAL_STRING("02:00:00:00:00:01", nb.portId);
    TEST_ASSERT_EQUAL_STRING("eth", nb.portDescr);
    TEST_ASSERT_EQUAL_STRING("rfpow", nb.sysName);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", nb.mgmtAddr);  // the first management address
    l.hasIPv4 = false;
    n = lldpBuildFrame(l, out, sizeof(out));
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(out, n, nb));
    TEST_ASSERT_EQUAL_STRING("2001:db8::5", nb.mgmtAddr);
}

static void test_parse_ignores_other_ethertype() {
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[13] = 0xcd;
    TEST_ASSERT_EQUAL(LldpParse::NotLldp, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_rejects_short_frame() {
    TEST_ASSERT_EQUAL(LldpParse::NotLldp, lldpParseFrame(JUNOS_STYLE, 0, nb));
    TEST_ASSERT_EQUAL(LldpParse::NotLldp, lldpParseFrame(JUNOS_STYLE, 13, nb));
    TEST_ASSERT_EQUAL(LldpParse::NotLldp, lldpParseFrame(nullptr, sizeof(JUNOS_STYLE), nb));
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(JUNOS_STYLE, 14, nb));  // LLDP, no TLVs
}

static void test_parse_rejects_tlv_past_end() {
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[14 + 9 + 6 + 4 + 1] = 0x7f;  // Port Description claims 127 bytes
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_rejects_cut_tlv_header() {
    // Every cut inside the frame: a TLV header or value cut short, or no End (heap copy, so ASan sees overreads)
    for (size_t len = 14; len < sizeof(JUNOS_STYLE); len++) {
        uint8_t* f = new uint8_t[len];
        memcpy(f, JUNOS_STYLE, len);
        TEST_ASSERT_EQUAL_MESSAGE(LldpParse::Malformed, lldpParseFrame(f, len, nb), "cut frame accepted");
        delete[] f;
    }
}

static void test_parse_rejects_missing_mandatory_tlv() {
    // The TTL TLV turned into a Port Description: no TTL
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[JUNOS_TTL_AT] = 0x08;
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_rejects_mandatory_tlvs_out_of_order() {
    // Port ID first, Chassis ID second
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, 14);
    size_t at = 14;
    memcpy(f + at, JUNOS_STYLE + 23, 6);
    at += 6;
    memcpy(f + at, JUNOS_STYLE + 14, 9);
    at += 9;
    memcpy(f + at, JUNOS_STYLE + at, sizeof(JUNOS_STYLE) - at);
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_rejects_frame_without_end() {
    // End replaced by a Port Description of 0 bytes: the TLVs run out without an End
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[sizeof(f) - 2] = 0x08;
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_rejects_bad_ttl_length() {
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[JUNOS_TTL_AT + 1] = 0x01;  // TTL of 1 byte; the next TLV header is then misread too
    TEST_ASSERT_EQUAL(LldpParse::Malformed, lldpParseFrame(f, sizeof(f), nb));
}

static void test_parse_sanitises_and_cuts_text() {
    static uint8_t f[600];
    size_t at = 14 + 9 + 6 + 4;
    memcpy(f, JUNOS_STYLE, at);
    f[at++] = 0x0a;  // System Name, 255 bytes
    f[at++] = 0xff;
    for (int i = 0; i < 255; i++) f[at + i] = 'a';
    f[at] = 0x01;
    f[at + 1] = 0xff;
    f[at + 2] = '<';
    at += 255;
    f[at++] = 0x00;
    f[at++] = 0x00;
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(f, at, nb));
    TEST_ASSERT_EQUAL(LLDP_TEXT_MAX, strlen(nb.sysName));
    TEST_ASSERT_EQUAL_STRING_LEN("?" "?<aaa", nb.sysName, 6);  // split: "??<" is a trigraph
}

// Captured 2026-11-01 from a Juniper EX2200-C (Junos 12.3R12-S21) with the hidden lldpdump command.
// Only change: the switch's serial number (Juniper chassis TLV, OUI 00:90:69) replaced by X's.
static const uint8_t EX2200_C_LLDPDU[] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e, 0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xcc, 0x88, 0xcc, 0x02, 0x07,
    0x04, 0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xc0, 0x04, 0x04, 0x07, 0x35, 0x32, 0x33, 0x06, 0x02, 0x00,
    0x78, 0x0a, 0x04, 0x72, 0x31, 0x36, 0x39, 0x0c, 0x61, 0x4a, 0x75, 0x6e, 0x69, 0x70, 0x65, 0x72,
    0x20, 0x4e, 0x65, 0x74, 0x77, 0x6f, 0x72, 0x6b, 0x73, 0x2c, 0x20, 0x49, 0x6e, 0x63, 0x2e, 0x20,
    0x65, 0x78, 0x32, 0x32, 0x30, 0x30, 0x2d, 0x63, 0x2d, 0x31, 0x32, 0x70, 0x2d, 0x32, 0x67, 0x20,
    0x2c, 0x20, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x31, 0x32, 0x2e, 0x33, 0x52, 0x31,
    0x32, 0x2d, 0x53, 0x32, 0x31, 0x20, 0x42, 0x75, 0x69, 0x6c, 0x64, 0x20, 0x64, 0x61, 0x74, 0x65,
    0x3a, 0x20, 0x32, 0x30, 0x32, 0x32, 0x2d, 0x30, 0x33, 0x2d, 0x30, 0x32, 0x20, 0x31, 0x36, 0x3a,
    0x32, 0x38, 0x3a, 0x35, 0x30, 0x20, 0x55, 0x54, 0x43, 0x20, 0x0e, 0x04, 0x00, 0x14, 0x00, 0x14,
    0x08, 0x0a, 0x67, 0x65, 0x2d, 0x30, 0x2f, 0x30, 0x2f, 0x39, 0x2e, 0x30, 0xfe, 0x09, 0x00, 0x12,
    0x0f, 0x01, 0x03, 0x6c, 0x11, 0x00, 0x00, 0xfe, 0x07, 0x00, 0x12, 0x0f, 0x02, 0x07, 0x01, 0x01,
    0xfe, 0x09, 0x00, 0x12, 0x0f, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x06, 0x00, 0x12, 0x0f,
    0x04, 0x05, 0xea, 0xfe, 0x06, 0x00, 0x80, 0xc2, 0x01, 0x00, 0x0b, 0xfe, 0x10, 0x00, 0x90, 0x69,
    0x01, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0xfe, 0x0d, 0x00,
    0x80, 0xc2, 0x03, 0x00, 0x0b, 0x06, 0x61, 0x63, 0x63, 0x65, 0x73, 0x73, 0xfe, 0x07, 0x00, 0x12,
    0xbb, 0x01, 0x00, 0x0f, 0x04, 0xfe, 0x07, 0x00, 0x12, 0xbb, 0x04, 0x13, 0x00, 0x0e, 0x00, 0x00,
};

static void test_parse_reads_real_ex2200_c_frame() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(EX2200_C_LLDPDU, sizeof(EX2200_C_LLDPDU), nb));
    TEST_ASSERT_EQUAL_STRING("f8:c0:01:c7:90:c0", nb.chassisId);
    TEST_ASSERT_EQUAL(7, nb.portIdSubtype);
    TEST_ASSERT_EQUAL_STRING("523", nb.portId);
    TEST_ASSERT_EQUAL_STRING("ge-0/0/9.0", nb.portDescr);
    TEST_ASSERT_EQUAL_STRING("r169", nb.sysName);
    TEST_ASSERT_EQUAL_STRING("", nb.mgmtAddr);  // this switch sends no Management Address TLV
    TEST_ASSERT_EQUAL(120, nb.ttl);
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(nb, 0);
    char text[160];
    lldpSwitchText(t, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("r169, port ge-0/0/9.0", text);
}

// ---- the neighbour ----

static LldpNeighbour parsed(const uint8_t* frame, size_t len) {
    static LldpNeighbour n;
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(frame, len, n));
    return n;
}

static void test_neighbour_expires_after_ttl() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), 1000);
    t.expire(120999);
    TEST_ASSERT_TRUE(t.present());
    TEST_ASSERT_EQUAL_STRING("ex2200", t.neighbour().sysName);
    t.expire(121000);
    TEST_ASSERT_FALSE(t.present());
}

static void test_neighbour_expires_across_millis_wrap() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    const uint32_t at = 0xFFFFF000u;
    t.received(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), at);
    t.expire(at + 119999u);
    TEST_ASSERT_TRUE(t.present());
    t.expire(at + 120000u);
    TEST_ASSERT_FALSE(t.present());
}

static void test_ttl_zero_forgets_neighbour() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), 1000);
    static uint8_t f[sizeof(JUNOS_STYLE)];
    memcpy(f, JUNOS_STYLE, sizeof(f));
    f[JUNOS_TTL_AT + 3] = 0;  // shutdown LLDPDU
    t.received(parsed(f, sizeof(f)), 2000);
    TEST_ASSERT_FALSE(t.present());
}

static void test_clear_forgets_neighbour() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), 1000);
    t.clear();
    TEST_ASSERT_FALSE(t.present());
}

static void test_latest_neighbour_wins() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), 1000);
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(local(), out, sizeof(out));
    t.received(parsed(out, n), 2000);
    TEST_ASSERT_EQUAL_STRING("rfpow", t.neighbour().sysName);
    t.expire(121000);  // the TTL counts from the latest frame
    TEST_ASSERT_TRUE(t.present());
}

// ---- display text ----

static void textOf(const LldpNeighbour& n, char* out, size_t cap) {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    t.received(n, 0);
    lldpSwitchText(t, out, cap);
}

static void test_switch_text_uses_port_description_for_local_port_id() {
    char text[160];
    textOf(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("ex2200, port ge-0/0/5.0", text);
}

static void test_switch_text_uses_port_id_otherwise() {
    static LldpNeighbour n;
    n = parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE));
    n.portIdSubtype = 5;  // interface name
    strcpy(n.portId, "ge-0/0/1");
    char text[160];
    textOf(n, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("ex2200, port ge-0/0/1", text);
    n.portIdSubtype = 7;  // locally assigned, but no description to show instead
    n.portDescr[0] = '\0';
    strcpy(n.portId, "513");
    textOf(n, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("ex2200, port 513", text);
}

static void test_switch_text_falls_back_to_chassis_id() {
    static LldpNeighbour n;
    n = parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE));
    n.sysName[0] = '\0';
    char text[160];
    textOf(n, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("4c:96:14:aa:bb:c0, port ge-0/0/5.0", text);
}

static void test_switch_text_when_none_seen() {
    static LldpNeighbourTable t;
    t = LldpNeighbourTable();
    char text[160];
    lldpSwitchText(t, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("none seen", text);
}

static void test_switch_text_cut_to_cap() {
    char text[160];
    memset(text, 'x', sizeof(text));
    textOf(parsed(JUNOS_STYLE, sizeof(JUNOS_STYLE)), text, 8);
    TEST_ASSERT_EQUAL_STRING("ex2200,", text);
    TEST_ASSERT_EQUAL_HEX8('x', text[8]);
}

// ---- raw values for the LLDP-MIB ----

static void test_parse_keeps_raw_ids() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(EX2200_C_LLDPDU, sizeof(EX2200_C_LLDPDU), nb));
    TEST_ASSERT_EQUAL(4, nb.chassisIdSubtype);
    const uint8_t chassis[] = {0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xc0};
    TEST_ASSERT_EQUAL(6, nb.chassisIdLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(chassis, nb.chassisIdRaw, 6);
    TEST_ASSERT_EQUAL(3, nb.portIdLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("523", nb.portIdRaw, 3);
}

static void test_parse_reads_sys_descr_and_caps() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(EX2200_C_LLDPDU, sizeof(EX2200_C_LLDPDU), nb));
    TEST_ASSERT_EQUAL_STRING_LEN("Juniper Networks, Inc. ex2200-c-12p-2g", nb.sysDescr, 38);
    TEST_ASSERT_EQUAL_HEX16(0x0014, nb.capsSupported);  // bridge, router
    TEST_ASSERT_EQUAL_HEX16(0x0014, nb.capsEnabled);
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(local(), out, sizeof(out));
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(out, n, nb));
    TEST_ASSERT_EQUAL_STRING("RF", nb.sysDescr);
    TEST_ASSERT_EQUAL_HEX16(0x0080, nb.capsSupported);
    TEST_ASSERT_EQUAL_HEX16(0x0080, nb.capsEnabled);
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(JUNOS_STYLE, sizeof(JUNOS_STYLE), nb));
    TEST_ASSERT_EQUAL_STRING("", nb.sysDescr);  // not sent
    TEST_ASSERT_EQUAL_HEX16(0, nb.capsSupported);
}

/// A frame with a 255-byte chassis ID (subtype 7, local) and a 255-byte System Description
static size_t longFieldsFrame(uint8_t* f) {
    size_t at = 0;
    memcpy(f, JUNOS_STYLE, 14);
    at = 14;
    f[at++] = 0x03;  // Chassis ID, 256 bytes: subtype + 255
    f[at++] = 0x00;
    f[at++] = 7;
    for (int i = 0; i < 255; i++) f[at++] = (uint8_t)('A' + i % 26);
    memcpy(f + at, JUNOS_STYLE + 23, 6 + 4);  // Port ID and TTL from the Junos-style frame
    at += 10;
    f[at++] = 0x0c;  // System Description, 255 bytes
    f[at++] = 0xff;
    for (int i = 0; i < 255; i++) f[at++] = 'd';
    f[at++] = 0x00;
    f[at++] = 0x00;
    return at;
}

static void test_parse_keeps_raw_ids_cut_to_64() {
    static uint8_t f[700];
    size_t n = longFieldsFrame(f);
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(f, n, nb));
    TEST_ASSERT_EQUAL(LLDP_ID_MAX, nb.chassisIdLen);
    TEST_ASSERT_EQUAL_HEX8('A', nb.chassisIdRaw[0]);
    TEST_ASSERT_EQUAL_HEX8('A' + 63 % 26, nb.chassisIdRaw[63]);
}

static void test_parse_cuts_sys_descr_to_128() {
    static uint8_t f[700];
    size_t n = longFieldsFrame(f);
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(f, n, nb));
    TEST_ASSERT_EQUAL(LLDP_SYS_DESCR_MAX, strlen(nb.sysDescr));
}

static void test_snmp_port_id_uses_description_for_local_port_id() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(EX2200_C_LLDPDU, sizeof(EX2200_C_LLDPDU), nb));
    uint8_t subtype;
    const uint8_t* data;
    size_t len;
    lldpSnmpPortId(nb, subtype, data, len);
    TEST_ASSERT_EQUAL(5, subtype);  // interfaceName
    TEST_ASSERT_EQUAL(10, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("ge-0/0/9.0", data, 10);
}

static void test_snmp_port_id_without_description_is_raw() {
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(EX2200_C_LLDPDU, sizeof(EX2200_C_LLDPDU), nb));
    nb.portDescr[0] = '\0';
    uint8_t subtype;
    const uint8_t* data;
    size_t len;
    lldpSnmpPortId(nb, subtype, data, len);
    TEST_ASSERT_EQUAL(7, subtype);
    TEST_ASSERT_EQUAL(3, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("523", data, 3);
}

static void test_snmp_port_id_mac_is_raw() {
    uint8_t out[LLDP_FRAME_MAX];
    size_t n = lldpBuildFrame(local(), out, sizeof(out));
    TEST_ASSERT_EQUAL(LldpParse::Ok, lldpParseFrame(out, n, nb));
    uint8_t subtype;
    const uint8_t* data;
    size_t len;
    lldpSnmpPortId(nb, subtype, data, len);
    TEST_ASSERT_EQUAL(3, subtype);
    TEST_ASSERT_EQUAL(6, len);
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, data, 6);
}

// ---- when to send ----

static void test_nothing_sent_while_link_down() {
    LldpSchedule s;
    TEST_ASSERT_FALSE(s.due(0, false, true));
    TEST_ASSERT_FALSE(s.due(100000, false, true));
}

static void test_link_up_sends_after_one_second() {
    LldpSchedule s;
    TEST_ASSERT_FALSE(s.due(5000, true, false));
    TEST_ASSERT_FALSE(s.due(5999, true, false));
    TEST_ASSERT_TRUE(s.due(6000, true, false));
}

static void test_sends_every_30_s() {
    LldpSchedule s;
    s.due(5000, true, false);
    TEST_ASSERT_TRUE(s.due(6000, true, false));
    s.sent(6000);
    TEST_ASSERT_FALSE(s.due(6001, true, false));
    TEST_ASSERT_FALSE(s.due(35999, true, false));
    TEST_ASSERT_TRUE(s.due(36000, true, false));
}

static void test_changed_content_sends_after_one_second() {
    LldpSchedule s;
    s.due(5000, true, false);
    s.sent(6000);
    TEST_ASSERT_FALSE(s.due(6999, true, true));
    TEST_ASSERT_TRUE(s.due(7000, true, true));
    TEST_ASSERT_FALSE(s.due(7000, true, false));
}

static void test_link_flap_restarts_the_one_second_wait() {
    LldpSchedule s;
    s.due(5000, true, false);
    s.sent(6000);
    TEST_ASSERT_FALSE(s.due(10000, false, true));
    TEST_ASSERT_FALSE(s.due(40000, true, false));  // link back at 40000: 30 s since the last send, but wait 1 s
    TEST_ASSERT_FALSE(s.due(40999, true, true));
    TEST_ASSERT_TRUE(s.due(41000, true, false));
}

static void test_schedule_handles_millis_wrap() {
    LldpSchedule s;
    const uint32_t up = 0xFFFFFC00u;
    TEST_ASSERT_FALSE(s.due(up, true, false));
    TEST_ASSERT_FALSE(s.due(up + 999u, true, false));
    TEST_ASSERT_TRUE(s.due(up + 1000u, true, false));  // wrapped to 0
    s.sent(up + 1000u);
    TEST_ASSERT_FALSE(s.due(up + 30999u, true, false));
    TEST_ASSERT_TRUE(s.due(up + 31000u, true, false));
}

static void test_same_neighbour_compares_chassis_and_port() {
    static LldpNeighbour a, b;
    memset(&a, 0, sizeof(a));
    strcpy(a.chassisId, "f8:c0:01:c7:90:c0");
    strcpy(a.portId, "ge-0/0/9.0");
    strcpy(a.sysName, "r169");
    a.ttl = 120;
    b = a;
    b.ttl = 90;                  // a refresh: same switch and port
    strcpy(b.sysName, "r169b");  // a renamed switch is still the same neighbour
    TEST_ASSERT_TRUE(lldpSameNeighbour(a, b));
    b = a;
    strcpy(b.portId, "ge-0/0/10.0");  // moved to another port
    TEST_ASSERT_FALSE(lldpSameNeighbour(a, b));
    b = a;
    strcpy(b.chassisId, "f8:c0:01:c7:90:c1");  // another switch
    TEST_ASSERT_FALSE(lldpSameNeighbour(a, b));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_build_frame_matches_expected_bytes);
    RUN_TEST(test_build_frame_adds_ipv6_management_address);
    RUN_TEST(test_build_frame_without_addresses_has_no_management_tlv);
    RUN_TEST(test_build_frame_fits_with_real_texts);
    RUN_TEST(test_build_frame_rejects_small_buffer);
    RUN_TEST(test_build_frame_rejects_string_over_255_bytes);
    RUN_TEST(test_parse_reads_junos_style_frame);
    RUN_TEST(test_parse_reads_own_built_frame);
    RUN_TEST(test_parse_ignores_other_ethertype);
    RUN_TEST(test_parse_rejects_short_frame);
    RUN_TEST(test_parse_rejects_tlv_past_end);
    RUN_TEST(test_parse_rejects_cut_tlv_header);
    RUN_TEST(test_parse_rejects_missing_mandatory_tlv);
    RUN_TEST(test_parse_rejects_mandatory_tlvs_out_of_order);
    RUN_TEST(test_parse_rejects_frame_without_end);
    RUN_TEST(test_parse_rejects_bad_ttl_length);
    RUN_TEST(test_parse_sanitises_and_cuts_text);
    RUN_TEST(test_parse_reads_real_ex2200_c_frame);
    RUN_TEST(test_parse_keeps_raw_ids);
    RUN_TEST(test_parse_reads_sys_descr_and_caps);
    RUN_TEST(test_parse_keeps_raw_ids_cut_to_64);
    RUN_TEST(test_parse_cuts_sys_descr_to_128);
    RUN_TEST(test_snmp_port_id_uses_description_for_local_port_id);
    RUN_TEST(test_snmp_port_id_without_description_is_raw);
    RUN_TEST(test_snmp_port_id_mac_is_raw);
    RUN_TEST(test_neighbour_expires_after_ttl);
    RUN_TEST(test_neighbour_expires_across_millis_wrap);
    RUN_TEST(test_ttl_zero_forgets_neighbour);
    RUN_TEST(test_clear_forgets_neighbour);
    RUN_TEST(test_latest_neighbour_wins);
    RUN_TEST(test_switch_text_uses_port_description_for_local_port_id);
    RUN_TEST(test_switch_text_uses_port_id_otherwise);
    RUN_TEST(test_switch_text_falls_back_to_chassis_id);
    RUN_TEST(test_switch_text_when_none_seen);
    RUN_TEST(test_switch_text_cut_to_cap);
    RUN_TEST(test_nothing_sent_while_link_down);
    RUN_TEST(test_link_up_sends_after_one_second);
    RUN_TEST(test_sends_every_30_s);
    RUN_TEST(test_changed_content_sends_after_one_second);
    RUN_TEST(test_link_flap_restarts_the_one_second_wait);
    RUN_TEST(test_schedule_handles_millis_wrap);
    RUN_TEST(test_same_neighbour_compares_chassis_and_port);
    return UNITY_END();
}
