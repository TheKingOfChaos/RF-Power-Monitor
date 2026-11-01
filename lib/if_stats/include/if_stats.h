#pragma once

/**
 * @file if_stats.h
 * @brief Ethernet interface counters and W5500 PHY status, for IF-MIB.
 *
 * Pure C++, unit tested on the host. The W5500 (in the MACRAW mode used by
 * lwIP) has no traffic counters, so the firmware counts every frame passing
 * between lwIP and the driver with ifCountFrame(). The PHY register gives
 * link, speed and duplex.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Traffic counters of one interface (64-bit, for ifHC* objects)
 */
struct IfCounters {
    uint64_t inOctets, inUcastPkts, inMulticastPkts, inBroadcastPkts;
    uint64_t outOctets, outUcastPkts, outMulticastPkts, outBroadcastPkts;
    uint32_t outErrors;        ///< Frames the driver failed to send
    uint32_t inDiscards;       ///< Frames dropped for lack of a receive buffer
    uint32_t inErrors;         ///< Frames with an impossible length
    uint32_t inUnknownProtos;  ///< Frames with an EtherType the stack does not handle
};

/// Shortest frame lwIP accepts: the 14-byte Ethernet header
static const size_t ETH_HEADER_LEN = 14;
/// Longest untagged frame without FCS: 1500-byte payload + header
static const size_t ETH_MAX_FRAME_LEN = 1514;

/// How a frame was addressed
enum class FrameKind : uint8_t { Unicast, Multicast, Broadcast };

/**
 * @brief Classify a frame by its destination MAC address (the first 6 bytes)
 *
 * ff:ff:ff:ff:ff:ff is broadcast, any other address with the group bit set
 * (lowest bit of the first byte) is multicast. Frames shorter than 6 bytes
 * count as unicast.
 */
FrameKind classifyFrame(const uint8_t* frame, size_t length);

/**
 * @brief Count one frame
 * @param frame   Ethernet frame starting with the destination MAC
 * @param length  Frame length in bytes (counted in in/outOctets)
 * @param outbound true for a frame sent, false for one received
 */
void ifCountFrame(IfCounters& c, const uint8_t* frame, size_t length, bool outbound);

/// What happens to a received frame in the stack
enum class RxVerdict : uint8_t { Deliver, Error, UnknownProto };

/**
 * @brief Decide whether lwIP can use a received frame
 *
 * Shorter than the Ethernet header or longer than 1514 bytes: Error. IPv4
 * (0x0800), ARP (0x0806), LLDP (0x88CC, read by the firmware) and, with ipv6,
 * IPv6 (0x86DD) are delivered. Any other EtherType is UnknownProto: this
 * includes 802.3 length fields (< 0x0600, e.g. spanning tree) and VLAN tags.
 */
RxVerdict classifyReceived(const uint8_t* frame, size_t length, bool ipv6);

/**
 * @brief Count one received frame that reached the stack
 *
 * The octets always count (ifInOctets includes every frame received). A
 * frame lwIP can use counts as unicast/multicast/broadcast, otherwise it
 * counts in inErrors or inUnknownProtos (IF-MIB: not delivered upwards).
 */
void ifCountReceived(IfCounters& c, const uint8_t* frame, size_t length, bool ipv6);

/// Count a received frame the driver dropped because no buffer was free
void ifCountDiscard(IfCounters& c, size_t length);

/**
 * @brief Link state decoded from the W5500 PHYCFGR register (0x002E)
 */
struct PhyStatus {
    bool linkUp;
    bool speed100;     ///< 100 Mbit/s, else 10 Mbit/s
    bool fullDuplex;
};

/// Decode PHYCFGR: bit 0 link, bit 1 speed (1 = 100M), bit 2 duplex (1 = full)
PhyStatus decodePhyCfgr(uint8_t reg);

/// ifSpeed in bit/s: 10 or 100 Mbit/s with link, 0 without
uint32_t phySpeedBps(const PhyStatus& phy);
