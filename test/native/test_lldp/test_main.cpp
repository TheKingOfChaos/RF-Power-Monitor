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
    TEST_ASSERT_EQUAL_STRING("rfups", nb.sysName);
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
    TEST_ASSERT_EQUAL_STRING("rfups", t.neighbour().sysName);
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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_build_frame_matches_expected_bytes);
    RUN_TEST(test_build_frame_adds_ipv6_management_address);
    RUN_TEST(test_build_frame_without_addresses_has_no_management_tlv);
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
    return UNITY_END();
}
