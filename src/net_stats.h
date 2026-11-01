#pragma once

/**
 * @file net_stats.h
 * @brief Ethernet traffic counters and link state for IF-MIB.
 *
 * The W5500 has no traffic counters in the MACRAW mode used by lwIP, so every
 * frame is counted where it passes between lwIP and the driver: the netif
 * input and linkoutput hooks are wrapped. Link, speed and duplex come from
 * the W5500 PHYCFGR register, read under the same lock the driver uses.
 * Frames the driver drops before lwIP sees them (no free receive buffer)
 * are counted by CountingW5500. Counting logic is in lib/if_stats (host tested).
 */

#include <Arduino.h>
#include <LwipEthernet.h>
#include <W5500lwIP.h>
#include <dhcp_option.h>
#include <snmp_mib.h>

struct netif;

/**
 * @brief Hold the lwIP/driver lock, as the W5500 driver does; needed for every lwIP call from loop()
 */
struct DriverLock {
    DriverLock() {
        if (ethernet_arch_lwip_gpio_mask) ethernet_arch_lwip_gpio_mask();
        if (ethernet_arch_lwip_begin) ethernet_arch_lwip_begin();
    }
    ~DriverLock() {
        if (ethernet_arch_lwip_end) ethernet_arch_lwip_end();
        if (ethernet_arch_lwip_gpio_unmask) ethernet_arch_lwip_gpio_unmask();
    }
};

/**
 * @brief Count a received frame the driver dropped. Called with the driver lock held.
 */
void netStatsCountDiscard(uint16_t length);

/**
 * @brief The core's W5500 driver, counting the frames it has to drop
 *
 * LwipIntfDev::handlePackets() calls RawDev::discardFrame() exactly when it
 * cannot get a pbuf for a received frame, so hiding discardFrame here sees
 * every such drop (ifInDiscards) without changing the core.
 */
class CountingW5500 : public Wiznet5500 {
public:
    using Wiznet5500::Wiznet5500;

    /// Count the dropped frame, then let the W5500 skip it
    void discardFrame(uint16_t framesize) {
        netStatsCountDiscard(framesize);
        Wiznet5500::discardFrame(framesize);
    }
};

/// Wiznet5500lwIP with ifInDiscards counting
using CountingW5500lwIP = LwipIntfDev<CountingW5500>;

/**
 * @brief Start counting frames on this interface. Call once after Ethernet has started.
 */
void netStatsBegin(netif* nif);

/**
 * @brief Poll the PHY for link changes (every 500 ms). Call from loop() on core 0.
 * @param uptimeTicks Current sysUpTime, recorded as ifLastChange on a change
 */
void netStatsPoll(uint32_t uptimeTicks);

/**
 * @brief Consistent copy of the counters and link state for the MIB
 */
void netStatsCapture(SnmpInterfaceState& out);

/**
 * @brief Latest decoded PHY status (link, 10/100, duplex)
 */
PhyStatus netStatsPhy();

/**
 * @brief Whether the interface has a valid (preferred or deprecated) IPv6 address beyond link-local,
 *        i.e. one a trap to a global or ULA destination can be sent from
 */
bool netStatsHasRoutableIPv6();

/**
 * @brief What the DHCP server said about option 224 (SNMP trap destination)
 */
struct DhcpTrapInfo {
    uint32_t requests;   ///< Outgoing DHCP messages that had option 224 added
    uint32_t acks;       ///< DHCPACKs lwIP accepted (only these count)
    uint32_t ignored;    ///< ACK-like frames for this MAC lwIP did not accept (possibly forged)
    bool seen;           ///< At least one ACK read
    bool hasOption;      ///< The latest ACK carried option 224
    uint8_t target[4];   ///< Its value (when hasOption)
    uint8_t yiaddr[4];   ///< Address the latest ACK assigned
    uint32_t ackMs;      ///< millis() of the latest ACK
};

/**
 * @brief Send a complete Ethernet frame through the driver, bypassing lwIP (DHCPv6 client:
 *        lwIP's stateless DHCPv6 owns UDP 546). Counted like any other frame. Takes the driver lock.
 * @return false before netStatsBegin(), without memory, or if the driver refused it
 */
bool netStatsSendFrame(const uint8_t* frame, size_t len);

/**
 * @brief Ask for option 224 in every DHCP DISCOVER/REQUEST (or stop asking)
 *
 * Switching it on renews the lease at once, so the answer arrives in seconds.
 */
void netStatsRequestDhcpTrapTarget(bool on);

/// Copy of the latest DHCP option 224 information
DhcpTrapInfo netStatsDhcpTrap();
