// Host unit tests for lib/dhcp6_client/dhcp6_message: building client messages and reading
// Advertise/Reply. Server messages are real captures (test/native/dhcp6_frames/frames.h).
// Run: pio test -e native -f native/test_dhcp6_message

#include <unity.h>
#include <initializer_list>
#include <string.h>
#include <dhcp6_message.h>

#include "../dhcp6_frames/frames.h"

void setUp() {}
void tearDown() {}

/// Ethernet (14) + IPv6 (40) + UDP (8): where the DHCPv6 message starts in a captured frame
static const size_t PAYLOAD = 62;
static const uint8_t KEA_DUID[] = {0x00, 0x03, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0xd6, 0x00};
static const uint8_t LEASED[16] = {0xfd, 0x00, 0x00, 0xd6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00};

/// Copy of a captured frame's DHCPv6 message
struct Msg {
    uint8_t b[512];
    size_t len;
};

static Msg payloadOf(const uint8_t* frame, size_t len) {
    Msg m;
    m.len = len - PAYLOAD;
    memcpy(m.b, frame + PAYLOAD, m.len);
    return m;
}

/// Find option `code` at the top level of a message (after type and xid); returns its value or nullptr
static const uint8_t* findOption(const uint8_t* msg, size_t len, uint16_t code, uint16_t* optLen) {
    size_t i = 4;
    while (i + 4 <= len) {
        uint16_t c = (uint16_t)(msg[i] << 8 | msg[i + 1]);
        uint16_t l = (uint16_t)(msg[i + 2] << 8 | msg[i + 3]);
        if (i + 4 + l > len) return nullptr;
        if (c == code) {
            *optLen = l;
            return msg + i + 4;
        }
        i += 4 + l;
    }
    return nullptr;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void putBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/// The IA Address option inside the IA_NA of a message (value pointer), or nullptr
static uint8_t* iaAddress(Msg& m) {
    uint16_t iaLen;
    const uint8_t* ia = findOption(m.b, m.len, 3, &iaLen);
    if (ia == nullptr || iaLen < 12 + 4 + 24) return nullptr;
    const uint8_t* sub = ia + 12;
    if ((sub[0] << 8 | sub[1]) != 5) return nullptr;
    return const_cast<uint8_t*>(sub + 4);
}

// ---- building ----

static void test_build_solicit_matches_rfc_layout() {
    Dhcp6Out o = {DHCP6_SOLICIT, 0x123456, 0, nullptr, 0, nullptr, "rf-ups-monitor"};
    uint8_t buf[256];
    size_t n = dhcp6Build(o, DEVICE_MAC, buf, sizeof(buf));
    const uint8_t want[] = {
        0x01, 0x12, 0x34, 0x56,
        0x00, 0x01, 0x00, 0x0a, 0x00, 0x03, 0x00, 0x01, 0x1e, 0xb2, 0xcf, 0x4e, 0xa2, 0x2c,  // Client ID, DUID-LL
        0x00, 0x03, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,                // IA_NA, IAID 1
        0x00, 0x06, 0x00, 0x04, 0x00, 0x27, 0x00, 0x52,                                        // ORO: FQDN, SOL_MAX_RT
        0x00, 0x08, 0x00, 0x02, 0x00, 0x00,                                                    // Elapsed Time
        0x00, 0x27, 0x00, 0x10, 0x01, 0x0e, 'r', 'f', '-', 'u', 'p', 's', '-', 'm', 'o', 'n', 'i', 't', 'o', 'r',
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, sizeof(want));
}

static void test_build_matches_dhcpcd_for_the_same_fields() {
    // dhcpcd's Solicit has the same Client ID, IA_NA and FQDN encoding for its own MAC and name
    Msg real = payloadOf(LAB_CLIENT_SOLICIT, LAB_CLIENT_SOLICIT_LEN);
    Dhcp6Out o = {DHCP6_SOLICIT, 0x27c74b, 0, nullptr, 0, nullptr, "lab-client"};
    uint8_t buf[256];
    size_t n = dhcp6Build(o, LAB_MAC_C1, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(real.b, buf, 4);  // type and xid
    for (uint16_t code : {1, 3, 39}) {
        uint16_t a, b;
        const uint8_t* mine = findOption(buf, n, code, &a);
        const uint8_t* theirs = findOption(real.b, real.len, code, &b);
        TEST_ASSERT_NOT_NULL(mine);
        TEST_ASSERT_NOT_NULL(theirs);
        TEST_ASSERT_EQUAL_UINT16(b, a);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(theirs, mine, a);
    }
}

static void test_build_request_carries_server_id_and_address() {
    Dhcp6Out o = {DHCP6_REQUEST, 0x5ad9b6, 0, KEA_DUID, sizeof(KEA_DUID), LEASED, "lab-client"};
    uint8_t buf[256];
    size_t n = dhcp6Build(o, LAB_MAC_C1, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REQUEST, buf[0]);
    uint16_t l;
    const uint8_t* sid = findOption(buf, n, 2, &l);
    TEST_ASSERT_NOT_NULL(sid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(KEA_DUID, sid, sizeof(KEA_DUID));
    // IA_NA with an IA Address, lifetimes 0, exactly as dhcpcd's Request
    Msg real = payloadOf(LAB_CLIENT_REQUEST, LAB_CLIENT_REQUEST_LEN);
    uint16_t a, b;
    const uint8_t* mine = findOption(buf, n, 3, &a);
    const uint8_t* theirs = findOption(real.b, real.len, 3, &b);
    TEST_ASSERT_EQUAL_UINT16(b, a);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(theirs, mine, a);
}

static void test_build_rebind_has_no_server_id() {
    Dhcp6Out o = {DHCP6_REBIND, 1, 100, KEA_DUID, sizeof(KEA_DUID), LEASED, "rf-ups-monitor"};
    uint8_t buf[256];
    size_t n = dhcp6Build(o, DEVICE_MAC, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    uint16_t l;
    TEST_ASSERT_NULL(findOption(buf, n, 2, &l));
    const uint8_t* el = findOption(buf, n, 8, &l);
    TEST_ASSERT_NOT_NULL(el);
    TEST_ASSERT_EQUAL_UINT8(0, el[0]);
    TEST_ASSERT_EQUAL_UINT8(100, el[1]);
}

static void test_build_decline_has_address_and_no_fqdn() {
    Dhcp6Out o = {DHCP6_DECLINE, 2, 0, KEA_DUID, sizeof(KEA_DUID), LEASED, "rf-ups-monitor"};
    uint8_t buf[256];
    size_t n = dhcp6Build(o, DEVICE_MAC, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    uint16_t l;
    TEST_ASSERT_NULL(findOption(buf, n, 39, &l));
    TEST_ASSERT_NULL(findOption(buf, n, 6, &l));
    const uint8_t* ia = findOption(buf, n, 3, &l);
    TEST_ASSERT_NOT_NULL(ia);
    TEST_ASSERT_EQUAL_UINT16(12 + 28, l);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(LEASED, ia + 16, 16);
}

static void test_build_returns_zero_when_buffer_too_small() {
    Dhcp6Out o = {DHCP6_SOLICIT, 1, 0, nullptr, 0, nullptr, "rf-ups-monitor"};
    uint8_t buf[256];
    size_t full = dhcp6Build(o, DEVICE_MAC, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, full);
    TEST_ASSERT_EQUAL_UINT32(0, dhcp6Build(o, DEVICE_MAC, buf, full - 1));
    TEST_ASSERT_EQUAL_UINT32(full, dhcp6Build(o, DEVICE_MAC, buf, full));
}

static void test_build_rejects_hostname_that_is_not_one_label() {
    uint8_t buf[256];
    char long64[65];
    memset(long64, 'a', 64);
    long64[64] = '\0';
    for (const char* bad : {(const char*)"rf.ups", (const char*)long64}) {
        Dhcp6Out o = {DHCP6_SOLICIT, 1, 0, nullptr, 0, nullptr, bad};
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, dhcp6Build(o, DEVICE_MAC, buf, sizeof(buf)), bad);
    }
    Dhcp6Out none = {DHCP6_SOLICIT, 1, 0, nullptr, 0, nullptr, nullptr};
    size_t n = dhcp6Build(none, DEVICE_MAC, buf, sizeof(buf));
    uint16_t l;
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NULL(findOption(buf, n, 39, &l));
}

// ---- parsing ----

static void test_parse_kea_advertise() {
    Msg m = payloadOf(KEA_ADVERTISE, KEA_ADVERTISE_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT8(DHCP6_ADVERTISE, in.type);
    TEST_ASSERT_EQUAL_HEX32(0x27c74b, in.xid);
    TEST_ASSERT_EQUAL_UINT8(sizeof(KEA_DUID), in.serverIdLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(KEA_DUID, in.serverId, sizeof(KEA_DUID));
    TEST_ASSERT_EQUAL_UINT8(0, in.preference);
    TEST_ASSERT_EQUAL_UINT16(DHCP6_STATUS_SUCCESS, in.status);
    TEST_ASSERT_EQUAL_UINT16(DHCP6_STATUS_SUCCESS, in.iaStatus);
    TEST_ASSERT_TRUE(in.hasLease);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(LEASED, in.lease.addr, 16);
    TEST_ASSERT_EQUAL_UINT32(20, in.lease.preferred);
    TEST_ASSERT_EQUAL_UINT32(40, in.lease.valid);
    TEST_ASSERT_EQUAL_UINT32(6, in.lease.t1);
    TEST_ASSERT_EQUAL_UINT32(12, in.lease.t2);
}

static void test_parse_kea_reply() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REPLY, in.type);
    TEST_ASSERT_TRUE(in.hasLease);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(LEASED, in.lease.addr, 16);
    TEST_ASSERT_EQUAL_UINT32(40, in.lease.valid);
}

static void test_parse_noaddrsavail_has_no_lease_and_status_2() {
    Msg m = payloadOf(KEA_ADVERTISE_NOADDRSAVAIL, KEA_ADVERTISE_NOADDRSAVAIL_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C2, in));
    TEST_ASSERT_FALSE(in.hasLease);
    TEST_ASSERT_EQUAL_UINT16(DHCP6_STATUS_NOADDRSAVAIL, in.iaStatus);
    TEST_ASSERT_EQUAL_UINT16(DHCP6_STATUS_SUCCESS, in.status);
}

static void test_parse_rejects_reply_for_other_client() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::NotForUs, dhcp6Parse(m.b, m.len, LAB_MAC_C2, in));
    TEST_ASSERT_EQUAL(Dhcp6Parse::NotForUs, dhcp6Parse(m.b, m.len, DEVICE_MAC, in));
}

static void test_parse_rejects_client_messages() {
    Msg m = payloadOf(LAB_CLIENT_SOLICIT, LAB_CLIENT_SOLICIT_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::NotServerMessage, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    Msg info = payloadOf(DEVICE_INFO_REQUEST, DEVICE_INFO_REQUEST_LEN);
    TEST_ASSERT_EQUAL(Dhcp6Parse::NotServerMessage, dhcp6Parse(info.b, info.len, DEVICE_MAC, in));
}

static void test_parse_reply_without_client_id_is_malformed() {
    // Kea's reply to lwIP's Information-request has a Server ID but no Client ID
    Msg m = payloadOf(KEA_INFO_REPLY, KEA_INFO_REPLY_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Malformed, dhcp6Parse(m.b, m.len, DEVICE_MAC, in));
}

static void test_parse_reply_without_server_id_is_malformed() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    uint16_t len;
    uint8_t* v = const_cast<uint8_t*>(findOption(m.b, m.len, 2, &len));
    TEST_ASSERT_NOT_NULL(v);
    uint8_t* opt = v - 4;
    size_t size = 4 + (size_t)len;
    memmove(opt, opt + size, m.len - (size_t)(opt - m.b) - size);
    m.len -= size;
    TEST_ASSERT_NULL(findOption(m.b, m.len, 2, &len));
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Malformed, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
}

static void test_parse_drops_address_with_preferred_above_valid() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    uint8_t* addr = iaAddress(m);
    TEST_ASSERT_NOT_NULL(addr);
    putBe32(addr + 16, 41);  // preferred 41 > valid 40
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_FALSE(in.hasLease);
}

static void test_parse_drops_address_with_valid_zero() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    uint8_t* addr = iaAddress(m);
    TEST_ASSERT_NOT_NULL(addr);
    putBe32(addr + 16, 0);
    putBe32(addr + 20, 0);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_FALSE(in.hasLease);
}

static void test_parse_prefers_address_with_nonzero_preferred_lifetime() {
    // Renumbering: the old address comes first, deprecated (preferred 0), then the new one
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    uint16_t iaLen;
    uint8_t* ia = const_cast<uint8_t*>(findOption(m.b, m.len, 3, &iaLen));
    uint8_t* addr = iaAddress(m);
    TEST_ASSERT_NOT_NULL(addr);
    uint8_t* opt = addr - 4;
    size_t optSize = 4 + (size_t)(opt[2] << 8 | opt[3]);
    memmove(opt + optSize, opt, m.len - (size_t)(opt - m.b));  // a copy of the IA Address in front
    m.len += optSize;
    ia[-2] = (uint8_t)((iaLen + optSize) >> 8);
    ia[-1] = (uint8_t)(iaLen + optSize);
    addr[15] = 0x99;          // the old address
    putBe32(addr + 16, 0);    // deprecated
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_TRUE(in.hasLease);
    TEST_ASSERT_EQUAL_MEMORY(LEASED, in.lease.addr, 16);
    TEST_ASSERT_TRUE(in.lease.preferred > 0);
}

static void test_parse_ignores_ia_na_with_other_iaid() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    uint16_t l;
    uint8_t* ia = const_cast<uint8_t*>(findOption(m.b, m.len, 3, &l));
    TEST_ASSERT_NOT_NULL(ia);
    TEST_ASSERT_EQUAL_UINT32(1, be32(ia));
    putBe32(ia, 2);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_FALSE(in.hasLease);
}

static void test_parse_reads_preference() {
    // Append a Preference option (7) of 255 to Kea's Advertise
    Msg m = payloadOf(KEA_ADVERTISE, KEA_ADVERTISE_LEN);
    const uint8_t pref[] = {0x00, 0x07, 0x00, 0x01, 0xff};
    memcpy(m.b + m.len, pref, sizeof(pref));
    m.len += sizeof(pref);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT8(255, in.preference);
}

/// Kea's Advertise with a SOL_MAX_RT option (82) of `seconds` appended
static Msg advertiseWithSolMaxRt(uint32_t seconds) {
    Msg m = payloadOf(KEA_ADVERTISE, KEA_ADVERTISE_LEN);
    const uint8_t opt[] = {0x00, 0x52, 0x00, 0x04};
    memcpy(m.b + m.len, opt, sizeof(opt));
    putBe32(m.b + m.len + 4, seconds);
    m.len += 8;
    return m;
}

static void test_parse_reads_sol_max_rt() {
    Msg m = advertiseWithSolMaxRt(120);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT32(120, in.solMaxRt);
}

static void test_parse_sol_max_rt_absent_is_zero() {
    Msg m = payloadOf(KEA_ADVERTISE, KEA_ADVERTISE_LEN);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT32(0, in.solMaxRt);
}

static void test_parse_ignores_sol_max_rt_out_of_range() {
    // RFC 8415 section 21.24: 60 to 86400 s, other values are ignored
    const uint32_t bad[] = {0, 59, 86401, 0xFFFFFFFFu};
    for (uint32_t s : bad) {
        Msg m = advertiseWithSolMaxRt(s);
        Dhcp6In in;
        TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
        TEST_ASSERT_EQUAL_UINT32(0, in.solMaxRt);
    }
    Msg m = advertiseWithSolMaxRt(60);
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT32(60, in.solMaxRt);
    m = advertiseWithSolMaxRt(86400);
    TEST_ASSERT_EQUAL(Dhcp6Parse::Ok, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_UINT32(86400, in.solMaxRt);
}

static void test_parse_rejects_sol_max_rt_with_wrong_length() {
    Msg m = advertiseWithSolMaxRt(120);
    m.b[m.len - 5] = 3;  // length 3
    m.len -= 1;
    Dhcp6In in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::Malformed, dhcp6Parse(m.b, m.len, LAB_MAC_C1, in));
}

struct Fixture {
    const uint8_t* frame;
    size_t len;
    const uint8_t* mac;
};

static const Fixture SERVER_MESSAGES[] = {
    {KEA_ADVERTISE, KEA_ADVERTISE_LEN, LAB_MAC_C1},
    {KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN, LAB_MAC_C1},
    {KEA_REPLY_RENEW, KEA_REPLY_RENEW_LEN, LAB_MAC_C1},
    {KEA_ADVERTISE_NOADDRSAVAIL, KEA_ADVERTISE_NOADDRSAVAIL_LEN, LAB_MAC_C2},
    {KEA_INFO_REPLY, KEA_INFO_REPLY_LEN, DEVICE_MAC},
};

static void test_parse_survives_every_truncation() {
    for (const Fixture& f : SERVER_MESSAGES) {
        Msg m = payloadOf(f.frame, f.len);
        for (size_t n = 0; n < m.len; n++) {
            uint8_t* cut = new uint8_t[n + 1];  // exact size, so the sanitizers see any over-read
            memcpy(cut, m.b, n);
            Dhcp6In in;
            Dhcp6Parse r = dhcp6Parse(cut, n, f.mac, in);
            delete[] cut;
            // A cut inside the IA_NA or the Client ID can never give a lease for us
            if (r == Dhcp6Parse::Ok) TEST_ASSERT_TRUE(n >= 4);
        }
    }
}

static void test_parse_survives_every_single_byte_corruption() {
    for (const Fixture& f : SERVER_MESSAGES) {
        Msg m = payloadOf(f.frame, f.len);
        for (size_t i = 0; i < m.len; i++) {
            for (uint8_t v : {(uint8_t)0x00, (uint8_t)0xff, (uint8_t)(m.b[i] ^ 0x80)}) {
                uint8_t* bad = new uint8_t[m.len];
                memcpy(bad, m.b, m.len);
                bad[i] = v;
                Dhcp6In in;
                dhcp6Parse(bad, m.len, f.mac, in);
                delete[] bad;
            }
        }
    }
}

static void test_parse_leaves_output_untouched_on_failure() {
    Msg m = payloadOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    Dhcp6In in;
    memset(&in, 0xA5, sizeof(in));
    Dhcp6In before = in;
    TEST_ASSERT_EQUAL(Dhcp6Parse::NotForUs, dhcp6Parse(m.b, m.len, DEVICE_MAC, in));
    TEST_ASSERT_EQUAL_MEMORY(&before, &in, sizeof(in));
    TEST_ASSERT_EQUAL(Dhcp6Parse::Malformed, dhcp6Parse(m.b, 3, LAB_MAC_C1, in));
    TEST_ASSERT_EQUAL_MEMORY(&before, &in, sizeof(in));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_build_solicit_matches_rfc_layout);
    RUN_TEST(test_build_matches_dhcpcd_for_the_same_fields);
    RUN_TEST(test_build_request_carries_server_id_and_address);
    RUN_TEST(test_build_rebind_has_no_server_id);
    RUN_TEST(test_build_decline_has_address_and_no_fqdn);
    RUN_TEST(test_build_returns_zero_when_buffer_too_small);
    RUN_TEST(test_build_rejects_hostname_that_is_not_one_label);
    RUN_TEST(test_parse_kea_advertise);
    RUN_TEST(test_parse_kea_reply);
    RUN_TEST(test_parse_noaddrsavail_has_no_lease_and_status_2);
    RUN_TEST(test_parse_rejects_reply_for_other_client);
    RUN_TEST(test_parse_rejects_client_messages);
    RUN_TEST(test_parse_reply_without_client_id_is_malformed);
    RUN_TEST(test_parse_reply_without_server_id_is_malformed);
    RUN_TEST(test_parse_drops_address_with_preferred_above_valid);
    RUN_TEST(test_parse_drops_address_with_valid_zero);
    RUN_TEST(test_parse_prefers_address_with_nonzero_preferred_lifetime);
    RUN_TEST(test_parse_ignores_ia_na_with_other_iaid);
    RUN_TEST(test_parse_reads_preference);
    RUN_TEST(test_parse_reads_sol_max_rt);
    RUN_TEST(test_parse_sol_max_rt_absent_is_zero);
    RUN_TEST(test_parse_ignores_sol_max_rt_out_of_range);
    RUN_TEST(test_parse_rejects_sol_max_rt_with_wrong_length);
    RUN_TEST(test_parse_survives_every_truncation);
    RUN_TEST(test_parse_survives_every_single_byte_corruption);
    RUN_TEST(test_parse_leaves_output_untouched_on_failure);
    return UNITY_END();
}
