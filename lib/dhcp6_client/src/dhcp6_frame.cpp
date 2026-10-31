#include "dhcp6_frame.h"

#include <string.h>

static const uint16_t ETHERTYPE_IPV6 = 0x86DD;
static const uint8_t NEXT_UDP = 17;
static const uint8_t NEXT_ICMPV6 = 58;
static const uint16_t PORT_CLIENT = 546;
static const uint16_t PORT_SERVER = 547;
static const uint8_t ICMPV6_ROUTER_ADVERT = 134;
static const size_t ETH = 14;
static const size_t IP6 = 40;
static const size_t RA_MIN = 16;  ///< ICMPv6 header (4) + hop limit, flags, lifetime, reachable, retrans timer

/// ff02::1:2, All_DHCP_Relay_Agents_and_Servers
static const uint8_t ALL_SERVERS[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x02};
static const uint8_t ALL_SERVERS_MAC[6] = {0x33, 0x33, 0x00, 0x01, 0x00, 0x02};

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/// One's-complement sum of 16-bit words (an odd last byte is padded with zero)
static uint32_t sum16(const uint8_t* p, size_t n, uint32_t acc) {
    for (size_t i = 0; i + 1 < n; i += 2) acc += be16(p + i);
    if (n & 1) acc += (uint32_t)p[n - 1] << 8;
    return acc;
}

static uint16_t fold(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

/// Upper-layer checksum over the IPv6 pseudo header (RFC 8200 section 8.1) and the data as is
static uint16_t ip6Checksum(const uint8_t* ip, uint8_t next, const uint8_t* data, uint32_t len) {
    uint32_t acc = sum16(ip + 8, 32, 0);  // source and destination address
    acc += len >> 16;
    acc += len & 0xFFFF;
    acc += next;
    return fold(sum16(data, len, acc));
}

size_t dhcp6Frame(const uint8_t mac[6], const uint8_t linkLocal[16], const uint8_t* payload, size_t len,
                  uint8_t* out, size_t size) {
    size_t udpLen = 8 + len;
    if (size < DHCP6_FRAME_HEADER || len > size - DHCP6_FRAME_HEADER || udpLen > 0xFFFF) return 0;
    memcpy(out, ALL_SERVERS_MAC, 6);
    memcpy(out + 6, mac, 6);
    out[12] = (uint8_t)(ETHERTYPE_IPV6 >> 8);
    out[13] = (uint8_t)ETHERTYPE_IPV6;

    uint8_t* ip = out + ETH;
    ip[0] = 0x60;  // version 6, traffic class 0, flow label 0
    ip[1] = ip[2] = ip[3] = 0;
    ip[4] = (uint8_t)(udpLen >> 8);
    ip[5] = (uint8_t)udpLen;
    ip[6] = NEXT_UDP;
    ip[7] = 1;  // hop limit: the link only, as dhcpcd does
    memcpy(ip + 8, linkLocal, 16);
    memcpy(ip + 24, ALL_SERVERS, 16);

    uint8_t* udp = ip + IP6;
    udp[0] = (uint8_t)(PORT_CLIENT >> 8);
    udp[1] = (uint8_t)PORT_CLIENT;
    udp[2] = (uint8_t)(PORT_SERVER >> 8);
    udp[3] = (uint8_t)PORT_SERVER;
    udp[4] = (uint8_t)(udpLen >> 8);
    udp[5] = (uint8_t)udpLen;
    udp[6] = udp[7] = 0;
    memcpy(udp + 8, payload, len);
    uint16_t sum = ip6Checksum(ip, NEXT_UDP, udp, (uint32_t)udpLen);
    if (sum == 0) sum = 0xFFFF;  // UDP over IPv6 must not send 0
    udp[6] = (uint8_t)(sum >> 8);
    udp[7] = (uint8_t)sum;
    return DHCP6_FRAME_HEADER + len;
}

/**
 * @brief The IPv6 header of an Ethernet frame and its payload length, checked against the frame
 * @return nullptr unless EtherType IPv6, version 6 and the payload fits in the frame
 */
static const uint8_t* ip6Of(const uint8_t* frame, size_t len, uint16_t* payloadLen) {
    if (frame == nullptr || len < ETH + IP6) return nullptr;
    if (be16(frame + 12) != ETHERTYPE_IPV6) return nullptr;
    const uint8_t* ip = frame + ETH;
    if ((ip[0] >> 4) != 6) return nullptr;
    *payloadLen = be16(ip + 4);
    if (*payloadLen > len - ETH - IP6) return nullptr;  // Ethernet padding after it is fine
    return ip;
}

bool dhcp6FromFrame(const uint8_t* frame, size_t len, const uint8_t mac[6], const uint8_t** payload,
                    size_t* payloadLen) {
    uint16_t ipLen;
    const uint8_t* ip = ip6Of(frame, len, &ipLen);
    if (ip == nullptr || memcmp(frame, mac, 6) != 0 || ip[6] != NEXT_UDP || ipLen < 8) return false;
    const uint8_t* udp = ip + IP6;
    uint16_t udpLen = be16(udp + 4);
    if (be16(udp) != PORT_SERVER || be16(udp + 2) != PORT_CLIENT || udpLen < 8 || udpLen != ipLen) return false;
    if (be16(udp + 6) == 0 || ip6Checksum(ip, NEXT_UDP, udp, udpLen) != 0) return false;
    *payload = udp + 8;
    *payloadLen = udpLen - 8u;
    return true;
}

bool raRead(const uint8_t* frame, size_t len, RouterAdvert& out) {
    uint16_t ipLen;
    const uint8_t* ip = ip6Of(frame, len, &ipLen);
    if (ip == nullptr || ip[6] != NEXT_ICMPV6 || ip[7] != 255 || ipLen < RA_MIN) return false;
    if (ip[8] != 0xFE || (ip[9] & 0xC0) != 0x80) return false;  // source must be link-local
    const uint8_t* icmp = ip + IP6;
    if (icmp[0] != ICMPV6_ROUTER_ADVERT || icmp[1] != 0) return false;
    if (ip6Checksum(ip, NEXT_ICMPV6, icmp, ipLen) != 0) return false;
    out.managed = (icmp[5] & 0x80) != 0;
    out.other = (icmp[5] & 0x40) != 0;
    return true;
}
