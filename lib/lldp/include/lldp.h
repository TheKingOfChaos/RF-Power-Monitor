#pragma once

/**
 * @file lldp.h
 * @brief LLDP (IEEE 802.1AB): the frames the device sends, the switch's frames
 *        it reads, the one neighbour it keeps, and when to send.
 *
 * Pure C++, unit tested on the host. src/lldp_service.cpp runs it: received
 * frames come from the input hook in src/net_stats.cpp, built frames go out
 * with netStatsSendFrame().
 */

#include <stddef.h>
#include <stdint.h>

static const uint16_t LLDP_ETHERTYPE = 0x88CC;
static const uint16_t LLDP_TTL_S = 120;    ///< Time to live we announce: 4 x the send interval
static const size_t LLDP_FRAME_MAX = 320;  ///< Send buffer: our frame with 32-byte names and both addresses fits

/**
 * @brief What the device announces about itself
 */
struct LldpLocal {
    uint8_t mac[6];         ///< Chassis ID and Port ID (MAC subtypes) and the frame's source
    const char* sysName;    ///< Hostname (= sysName)
    const char* sysDescr;   ///< Same text as SNMP sysDescr
    const char* portDescr;  ///< Same text as IF-MIB ifDescr
    bool hasIPv4;
    uint8_t ipv4[4];
    bool hasIPv6;
    uint8_t ipv6[16];       ///< A global address (not link-local)
    uint16_t ttl;           ///< Seconds, normally LLDP_TTL_S
};

/**
 * @brief Build the whole Ethernet frame (header + LLDPDU) to the nearest-bridge
 *        address 01:80:c2:00:00:0e
 *
 * TLVs: Chassis ID (MAC), Port ID (MAC), TTL, Port Description, System Name,
 * System Description, System Capabilities (station only), one Management
 * Address per address family present (ifIndex 1, no OID), End.
 *
 * @return Frame length, or 0 if out is null, cap is too small or a string is
 *         longer than a TLV holds (255 bytes)
 */
size_t lldpBuildFrame(const LldpLocal& local, uint8_t* out, size_t cap);
