// Host unit tests for lib/dhcp6_client/dhcp6_frame: framing DHCPv6 client messages and
// reading server messages and router advertisement flags from captured frames.
// Run: pio test -e native -f native/test_dhcp6_frame

#include <unity.h>
#include <initializer_list>
#include <string.h>
#include <dhcp6_frame.h>

#include "../dhcp6_frames/frames.h"

void setUp() {}
void tearDown() {}

struct Frame {
    uint8_t b[512];
    size_t len;
};

static Frame copyOf(const uint8_t* f, size_t len) {
    Frame c;
    memcpy(c.b, f, len);
    c.len = len;
    return c;
}

// ---- building ----

static void test_frame_matches_device_capture() {
    // The device's own Information-request (built by lwIP): same headers and checksum from our framing
    const uint8_t* payload = DEVICE_INFO_REQUEST + DHCP6_FRAME_HEADER;
    size_t plen = DEVICE_INFO_REQUEST_LEN - DHCP6_FRAME_HEADER;
    uint8_t out[512];
    size_t n = dhcp6Frame(DEVICE_MAC, DEVICE_LL, payload, plen, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(DEVICE_INFO_REQUEST_LEN, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DEVICE_INFO_REQUEST, out, 14);              // Ethernet
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DEVICE_INFO_REQUEST + 18, out + 18, 3);     // payload length, next header
    TEST_ASSERT_EQUAL_UINT8(1, out[21]);                                     // hop limit 1 (lwIP used 255)
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DEVICE_INFO_REQUEST + 22, out + 22, 32);    // addresses
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DEVICE_INFO_REQUEST + 54, out + 54, 8);     // UDP incl. checksum
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DEVICE_INFO_REQUEST + 62, out + 62, plen);
}

static void test_frame_matches_dhcpcd_headers() {
    // dhcpcd's Solicit: hop limit 1, UDP 546 -> 547, same checksum for the same bytes
    const uint8_t* payload = LAB_CLIENT_SOLICIT + DHCP6_FRAME_HEADER;
    size_t plen = LAB_CLIENT_SOLICIT_LEN - DHCP6_FRAME_HEADER;
    const uint8_t* srcLL = LAB_CLIENT_SOLICIT + 22;
    uint8_t out[512];
    size_t n = dhcp6Frame(LAB_MAC_C1, srcLL, payload, plen, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(LAB_CLIENT_SOLICIT_LEN, n);
    TEST_ASSERT_EQUAL_UINT8(1, out[21]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(LAB_CLIENT_SOLICIT + 18, out + 18, 36 + 8);
}

static void test_frame_round_trips_through_from_frame_only_for_the_other_direction() {
    // A client frame (546 -> 547, multicast) is never taken for a server message
    uint8_t payload[20] = {11, 1, 2, 3};
    uint8_t out[128];
    size_t n = dhcp6Frame(DEVICE_MAC, DEVICE_LL, payload, sizeof(payload), out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);
    const uint8_t* p;
    size_t pl;
    TEST_ASSERT_FALSE(dhcp6FromFrame(out, n, DEVICE_MAC, &p, &pl));
}

static void test_frame_returns_zero_when_buffer_too_small() {
    uint8_t payload[20] = {1};
    uint8_t out[128];
    TEST_ASSERT_EQUAL_UINT32(0, dhcp6Frame(DEVICE_MAC, DEVICE_LL, payload, sizeof(payload), out, 81));
    TEST_ASSERT_EQUAL_UINT32(82, dhcp6Frame(DEVICE_MAC, DEVICE_LL, payload, sizeof(payload), out, 82));
}

// ---- reading server messages ----

static void test_from_frame_reads_kea_reply() {
    const uint8_t* p;
    size_t pl;
    TEST_ASSERT_TRUE(dhcp6FromFrame(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN, LAB_MAC_C1, &p, &pl));
    TEST_ASSERT_EQUAL_PTR(KEA_REPLY_REQUEST + DHCP6_FRAME_HEADER, p);
    TEST_ASSERT_EQUAL_UINT32(KEA_REPLY_REQUEST_LEN - DHCP6_FRAME_HEADER, pl);
    TEST_ASSERT_TRUE(dhcp6FromFrame(KEA_INFO_REPLY, KEA_INFO_REPLY_LEN, DEVICE_MAC, &p, &pl));
}

static void test_from_frame_rejects_other_mac() {
    const uint8_t* p;
    size_t pl;
    TEST_ASSERT_FALSE(dhcp6FromFrame(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN, LAB_MAC_C2, &p, &pl));
}

static void test_from_frame_rejects_bad_checksum() {
    Frame f = copyOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    f.b[f.len - 1] ^= 0x01;
    const uint8_t* p;
    size_t pl;
    TEST_ASSERT_FALSE(dhcp6FromFrame(f.b, f.len, LAB_MAC_C1, &p, &pl));
}

static void test_from_frame_rejects_extension_headers() {
    Frame f = copyOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    f.b[20] = 0;  // next header: hop-by-hop options instead of UDP
    const uint8_t* p;
    size_t pl;
    TEST_ASSERT_FALSE(dhcp6FromFrame(f.b, f.len, LAB_MAC_C1, &p, &pl));
}

static void test_from_frame_rejects_wrong_ports() {
    for (size_t at : {54, 56}) {  // source port 547 -> 548, destination 546 -> 547
        Frame f = copyOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
        f.b[at + 1]++;
        const uint8_t* p;
        size_t pl;
        TEST_ASSERT_FALSE(dhcp6FromFrame(f.b, f.len, LAB_MAC_C1, &p, &pl));
    }
}

static void test_from_frame_rejects_udp_length_past_the_frame() {
    Frame f = copyOf(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN);
    const uint8_t* p;
    size_t pl;
    // Ethernet padding after the datagram is fine, a datagram longer than the frame is not
    TEST_ASSERT_FALSE(dhcp6FromFrame(f.b, f.len - 1, LAB_MAC_C1, &p, &pl));
    f.b[f.len] = 0;
    TEST_ASSERT_TRUE(dhcp6FromFrame(f.b, f.len + 1, LAB_MAC_C1, &p, &pl));
    TEST_ASSERT_EQUAL_UINT32(KEA_REPLY_REQUEST_LEN - DHCP6_FRAME_HEADER, pl);
}

// ---- router advertisements ----

static void test_ra_reads_managed_flag() {
    RouterAdvert ra;
    TEST_ASSERT_TRUE(raRead(LAB_RA_MANAGED, LAB_RA_MANAGED_LEN, ra));
    TEST_ASSERT_TRUE(ra.managed);
    TEST_ASSERT_FALSE(ra.other);
}

static void test_ra_reads_plain_router_without_m() {
    RouterAdvert ra;
    TEST_ASSERT_TRUE(raRead(LAB_RA_PLAIN, LAB_RA_PLAIN_LEN, ra));
    TEST_ASSERT_FALSE(ra.managed);
    TEST_ASSERT_FALSE(ra.other);
}

static void test_ra_rejects_hop_limit_below_255() {
    Frame f = copyOf(LAB_RA_MANAGED, LAB_RA_MANAGED_LEN);
    f.b[21] = 254;  // not in the checksum: only the hop limit check can catch it
    RouterAdvert ra;
    TEST_ASSERT_FALSE(raRead(f.b, f.len, ra));
}

static void test_ra_rejects_global_source() {
    Frame f = copyOf(LAB_RA_MANAGED, LAB_RA_MANAGED_LEN);
    f.b[22] = 0xfd;  // source fd80::... (the checksum no longer matches either)
    RouterAdvert ra;
    TEST_ASSERT_FALSE(raRead(f.b, f.len, ra));
}

static void test_ra_rejects_bad_checksum_and_other_icmp() {
    Frame f = copyOf(LAB_RA_MANAGED, LAB_RA_MANAGED_LEN);
    f.b[f.len - 1] ^= 0x01;
    RouterAdvert ra;
    TEST_ASSERT_FALSE(raRead(f.b, f.len, ra));
    TEST_ASSERT_FALSE(raRead(KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN, ra));  // UDP, not ICMPv6
}

// ---- hostile input ----

static const struct {
    const uint8_t* f;
    size_t len;
} ALL[] = {
    {LAB_CLIENT_SOLICIT, LAB_CLIENT_SOLICIT_LEN}, {KEA_ADVERTISE, KEA_ADVERTISE_LEN},
    {KEA_REPLY_REQUEST, KEA_REPLY_REQUEST_LEN},   {KEA_ADVERTISE_NOADDRSAVAIL, KEA_ADVERTISE_NOADDRSAVAIL_LEN},
    {LAB_RA_MANAGED, LAB_RA_MANAGED_LEN},         {LAB_RA_PLAIN, LAB_RA_PLAIN_LEN},
    {DEVICE_INFO_REQUEST, DEVICE_INFO_REQUEST_LEN}, {KEA_INFO_REPLY, KEA_INFO_REPLY_LEN},
};

static void test_readers_survive_every_truncation() {
    for (const auto& a : ALL) {
        for (size_t n = 0; n < a.len; n++) {
            uint8_t* cut = new uint8_t[n + 1];  // exact size for the sanitizers
            memcpy(cut, a.f, n);
            const uint8_t* p;
            size_t pl;
            RouterAdvert ra;
            TEST_ASSERT_FALSE(dhcp6FromFrame(cut, n, LAB_MAC_C1, &p, &pl) && pl > n);
            raRead(cut, n, ra);
            delete[] cut;
        }
    }
}

static void test_readers_survive_every_single_byte_corruption() {
    for (const auto& a : ALL) {
        for (size_t i = 0; i < a.len; i++) {
            uint8_t* bad = new uint8_t[a.len];
            memcpy(bad, a.f, a.len);
            bad[i] ^= 0xff;
            const uint8_t* p;
            size_t pl;
            RouterAdvert ra;
            if (dhcp6FromFrame(bad, a.len, LAB_MAC_C1, &p, &pl)) TEST_ASSERT_TRUE(p + pl <= bad + a.len);
            raRead(bad, a.len, ra);
            delete[] bad;
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_frame_matches_device_capture);
    RUN_TEST(test_frame_matches_dhcpcd_headers);
    RUN_TEST(test_frame_round_trips_through_from_frame_only_for_the_other_direction);
    RUN_TEST(test_frame_returns_zero_when_buffer_too_small);
    RUN_TEST(test_from_frame_reads_kea_reply);
    RUN_TEST(test_from_frame_rejects_other_mac);
    RUN_TEST(test_from_frame_rejects_bad_checksum);
    RUN_TEST(test_from_frame_rejects_extension_headers);
    RUN_TEST(test_from_frame_rejects_wrong_ports);
    RUN_TEST(test_from_frame_rejects_udp_length_past_the_frame);
    RUN_TEST(test_ra_reads_managed_flag);
    RUN_TEST(test_ra_reads_plain_router_without_m);
    RUN_TEST(test_ra_rejects_hop_limit_below_255);
    RUN_TEST(test_ra_rejects_global_source);
    RUN_TEST(test_ra_rejects_bad_checksum_and_other_icmp);
    RUN_TEST(test_readers_survive_every_truncation);
    RUN_TEST(test_readers_survive_every_single_byte_corruption);
    return UNITY_END();
}
