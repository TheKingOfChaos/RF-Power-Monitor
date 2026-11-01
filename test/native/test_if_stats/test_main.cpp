// Host unit tests for lib/if_stats: frame classification, counters and W5500 PHY decoding.
// Run: pio test -e native -f native/test_if_stats

#include <unity.h>
#include <string.h>
#include <if_stats.h>

void setUp() {}
void tearDown() {}

static const uint8_t UNICAST[] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C, 0, 0, 0, 0, 0, 0, 0x08, 0x00};
static const uint8_t BROADCAST[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x08, 0x06};
static const uint8_t MULTICAST_V4[] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB, 0, 0, 0, 0, 0, 0, 0x08, 0x00};
static const uint8_t MULTICAST_V6[] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0x86, 0xDD};

static void test_classify_frames() {
    TEST_ASSERT_EQUAL(FrameKind::Unicast, classifyFrame(UNICAST, sizeof(UNICAST)));
    TEST_ASSERT_EQUAL(FrameKind::Broadcast, classifyFrame(BROADCAST, sizeof(BROADCAST)));
    TEST_ASSERT_EQUAL(FrameKind::Multicast, classifyFrame(MULTICAST_V4, sizeof(MULTICAST_V4)));
    TEST_ASSERT_EQUAL(FrameKind::Multicast, classifyFrame(MULTICAST_V6, sizeof(MULTICAST_V6)));
}

static void test_almost_broadcast_is_multicast() {
    const uint8_t f[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    TEST_ASSERT_EQUAL(FrameKind::Multicast, classifyFrame(f, sizeof(f)));
}

static void test_short_or_null_frame_counts_as_unicast() {
    TEST_ASSERT_EQUAL(FrameKind::Unicast, classifyFrame(BROADCAST, 5));
    TEST_ASSERT_EQUAL(FrameKind::Unicast, classifyFrame(nullptr, 0));
}

static void test_counts_inbound_and_outbound_separately() {
    IfCounters c;
    memset(&c, 0, sizeof(c));
    ifCountFrame(c, UNICAST, 60, false);
    ifCountFrame(c, UNICAST, 1514, false);
    ifCountFrame(c, BROADCAST, 60, false);
    ifCountFrame(c, MULTICAST_V4, 100, false);
    ifCountFrame(c, UNICAST, 90, true);
    ifCountFrame(c, BROADCAST, 42, true);
    ifCountFrame(c, MULTICAST_V6, 86, true);
    TEST_ASSERT_TRUE(c.inOctets == 60 + 1514 + 60 + 100);
    TEST_ASSERT_TRUE(c.inUcastPkts == 2 && c.inBroadcastPkts == 1 && c.inMulticastPkts == 1);
    TEST_ASSERT_TRUE(c.outOctets == 90 + 42 + 86);
    TEST_ASSERT_TRUE(c.outUcastPkts == 1 && c.outBroadcastPkts == 1 && c.outMulticastPkts == 1);
    TEST_ASSERT_EQUAL_UINT32(0, c.outErrors);
}

static void test_octets_go_past_32_bits() {
    IfCounters c;
    memset(&c, 0, sizeof(c));
    c.inOctets = 0xFFFFFFF0ull;
    ifCountFrame(c, UNICAST, 1514, false);
    TEST_ASSERT_TRUE(c.inOctets == 0xFFFFFFF0ull + 1514);
    TEST_ASSERT_TRUE(c.inOctets > 0xFFFFFFFFull);
}

// A full-size buffer so every length up to the maximum can be passed
static void frameWith(uint16_t etherType, uint8_t* buf) {
    memset(buf, 0, ETH_MAX_FRAME_LEN + 1);
    memcpy(buf, UNICAST, 12);
    buf[12] = (uint8_t)(etherType >> 8);
    buf[13] = (uint8_t)etherType;
}

static void test_ipv4_and_arp_are_delivered() {
    static uint8_t f[ETH_MAX_FRAME_LEN + 1];
    frameWith(0x0800, f);
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(f, 60, false));
    frameWith(0x0806, f);
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(f, 60, false));
}

static void test_ipv6_is_delivered_only_with_ipv6_enabled() {
    TEST_ASSERT_EQUAL(RxVerdict::UnknownProto, classifyReceived(MULTICAST_V6, sizeof(MULTICAST_V6), false));
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(MULTICAST_V6, sizeof(MULTICAST_V6), true));
}

static void test_lldp_is_delivered() {
    // The firmware reads LLDP (src/lldp_service.cpp), so it is not an unknown protocol
    static const uint8_t LLDP[] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E, 0, 0, 0, 0, 0, 0, 0x88, 0xCC};
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(LLDP, sizeof(LLDP), false));
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(LLDP, sizeof(LLDP), true));
    IfCounters c;
    memset(&c, 0, sizeof(c));
    ifCountReceived(c, LLDP, sizeof(LLDP), true);
    TEST_ASSERT_TRUE(c.inMulticastPkts == 1);
    TEST_ASSERT_EQUAL_UINT32(0, c.inUnknownProtos);
}

static void test_other_ethertypes_are_unknown_protocols() {
    static uint8_t f[ETH_MAX_FRAME_LEN + 1];
    // 802.3 length field (spanning tree), the boundary, VLAN tag, almost LLDP, almost IPv4
    const uint16_t types[] = {0x0026, 0x05FF, 0x0600, 0x8100, 0x88CD, 0x0801, 0x0000, 0xFFFF};
    for (uint16_t t : types) {
        frameWith(t, f);
        TEST_ASSERT_EQUAL_MESSAGE(RxVerdict::UnknownProto, classifyReceived(f, 60, true), "ethertype");
    }
}

static void test_frame_length_boundaries() {
    static uint8_t f[ETH_MAX_FRAME_LEN + 1];
    frameWith(0x0800, f);
    TEST_ASSERT_EQUAL(RxVerdict::Error, classifyReceived(f, 0, false));
    TEST_ASSERT_EQUAL(RxVerdict::Error, classifyReceived(f, ETH_HEADER_LEN - 1, false));
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(f, ETH_HEADER_LEN, false));
    TEST_ASSERT_EQUAL(RxVerdict::Deliver, classifyReceived(f, ETH_MAX_FRAME_LEN, false));
    TEST_ASSERT_EQUAL(RxVerdict::Error, classifyReceived(f, ETH_MAX_FRAME_LEN + 1, false));
    TEST_ASSERT_EQUAL(RxVerdict::Error, classifyReceived(nullptr, 60, false));
}

static void test_received_frames_land_in_the_right_counter() {
    static uint8_t ip[ETH_MAX_FRAME_LEN + 1], vlan[ETH_MAX_FRAME_LEN + 1];
    frameWith(0x0800, ip);
    frameWith(0x8100, vlan);
    IfCounters c;
    memset(&c, 0, sizeof(c));
    ifCountReceived(c, ip, 100, false);
    ifCountReceived(c, vlan, 60, false);
    ifCountReceived(c, MULTICAST_V6, 86, false);
    ifCountReceived(c, ip, 10, false);
    ifCountDiscard(c, 1514);
    ifCountDiscard(c, 64);
    // Only the deliverable frame counts as a packet
    TEST_ASSERT_TRUE(c.inUcastPkts == 1 && c.inMulticastPkts == 0 && c.inBroadcastPkts == 0);
    TEST_ASSERT_EQUAL_UINT32(2, c.inUnknownProtos);
    TEST_ASSERT_EQUAL_UINT32(1, c.inErrors);
    TEST_ASSERT_EQUAL_UINT32(2, c.inDiscards);
    // Every received octet counts, delivered or not
    TEST_ASSERT_TRUE(c.inOctets == 100 + 60 + 86 + 10 + 1514 + 64);
    TEST_ASSERT_TRUE(c.outOctets == 0 && c.outUcastPkts == 0);
}

static void test_decode_phycfgr() {
    // RST and OPMD bits (7..3) are ignored
    PhyStatus p = decodePhyCfgr(0xBF);   // 1011 1111: link, 100M, full
    TEST_ASSERT_TRUE(p.linkUp);
    TEST_ASSERT_TRUE(p.speed100);
    TEST_ASSERT_TRUE(p.fullDuplex);
    TEST_ASSERT_EQUAL_UINT32(100000000u, phySpeedBps(p));

    p = decodePhyCfgr(0xB9);             // 1011 1001: link, 10M, half duplex
    TEST_ASSERT_TRUE(p.linkUp);
    TEST_ASSERT_FALSE(p.speed100);
    TEST_ASSERT_FALSE(p.fullDuplex);
    TEST_ASSERT_EQUAL_UINT32(10000000u, phySpeedBps(p));
}

static void test_no_link_means_speed_zero() {
    PhyStatus p = decodePhyCfgr(0xBE);   // speed/duplex bits set but no link
    TEST_ASSERT_FALSE(p.linkUp);
    TEST_ASSERT_EQUAL_UINT32(0, phySpeedBps(p));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_classify_frames);
    RUN_TEST(test_almost_broadcast_is_multicast);
    RUN_TEST(test_short_or_null_frame_counts_as_unicast);
    RUN_TEST(test_counts_inbound_and_outbound_separately);
    RUN_TEST(test_octets_go_past_32_bits);
    RUN_TEST(test_ipv4_and_arp_are_delivered);
    RUN_TEST(test_ipv6_is_delivered_only_with_ipv6_enabled);
    RUN_TEST(test_lldp_is_delivered);
    RUN_TEST(test_other_ethertypes_are_unknown_protocols);
    RUN_TEST(test_frame_length_boundaries);
    RUN_TEST(test_received_frames_land_in_the_right_counter);
    RUN_TEST(test_decode_phycfgr);
    RUN_TEST(test_no_link_means_speed_zero);
    return UNITY_END();
}
