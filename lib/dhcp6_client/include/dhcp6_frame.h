#pragma once

/**
 * @file dhcp6_frame.h
 * @brief Ethernet/IPv6/UDP framing for DHCPv6 client messages, and the M/O flags of
 *        router advertisements, at the driver level.
 *
 * lwIP's stateless DHCPv6 client owns UDP port 546, so the firmware sends its
 * DHCPv6 messages as complete frames through the link output and reads replies in
 * the netif input hook (src/net_stats.cpp), the way DHCP option 224 is handled.
 * Clients send everything to All_DHCP_Relay_Agents_and_Servers (ff02::1:2), so no
 * neighbour discovery is needed. Pure C++, host tested with captured frames.
 */

#include <stddef.h>
#include <stdint.h>

/// Ethernet + IPv6 + UDP header bytes in front of a DHCPv6 message
constexpr size_t DHCP6_FRAME_HEADER = 14 + 40 + 8;

/**
 * @brief Wrap a client message: Ethernet to 33:33:00:01:00:02, IPv6 from the link-local
 *        address to ff02::1:2 with hop limit 1, UDP 546 -> 547 with checksum
 * @return Frame length, 0 if out is too small
 */
size_t dhcp6Frame(const uint8_t mac[6], const uint8_t linkLocal[16], const uint8_t* payload, size_t len,
                  uint8_t* out, size_t size);

/**
 * @brief Find a DHCPv6 server message for this device in a received frame
 *
 * True only for an Ethernet frame to `mac`, EtherType IPv6, next header UDP (no
 * extension headers), UDP 547 -> 546, consistent lengths and a valid UDP checksum.
 *
 * @param payload Set to the DHCPv6 message inside frame
 */
bool dhcp6FromFrame(const uint8_t* frame, size_t len, const uint8_t mac[6], const uint8_t** payload,
                    size_t* payloadLen);

/**
 * @brief The address-configuration flags of a router advertisement
 */
struct RouterAdvert {
    bool managed;  ///< M: addresses from DHCPv6
    bool other;    ///< O: other configuration from DHCPv6
};

/**
 * @brief Read a router advertisement's flags
 *
 * True only for ICMPv6 type 134 code 0 with hop limit 255 from a link-local source
 * and a valid ICMPv6 checksum (RFC 4861 section 6.1.2).
 */
bool raRead(const uint8_t* frame, size_t len, RouterAdvert& out);
