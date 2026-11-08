#pragma once

/**
 * @file dhcp6_service.h
 * @brief DHCPv6 address (stateful, one IA_NA) on the W5500 interface.
 *
 * Starts when a router advertisement carries the M flag; nothing happens on other
 * networks. Frames go around lwIP (whose stateless DHCPv6 owns UDP 546): replies and
 * router advertisements are read in the netif input hook, messages are sent as
 * complete frames (src/net_stats.cpp). The protocol is lib/dhcp6_client (host tested).
 */

#include <stddef.h>
#include <stdint.h>
#include <dhcp6_client.h>
#include <netconfig.h>

struct netif;

/// Start watching for router advertisements on nif (after netStatsBegin)
void dhcp6Begin(netif* nif);

/// Run the client: start it on an M flag, feed it replies, send, add/remove the address. Call every loop().
void dhcp6Poll();

/**
 * @brief Look at a received frame: M flag of a router advertisement, or a DHCPv6 reply for this device
 *
 * Called from the netif input hook (lwIP context, driver lock held); keeps at most one
 * message for dhcp6Poll() and never changes the frame.
 */
void dhcp6InspectFrame(const uint8_t* frame, size_t len);

/**
 * @brief Hand the SNTP servers from the last Reply (option 31) to lwIP again at the next dhcp6Poll()
 *
 * For timeServiceApply(), which clears lwIP's servers when NTP is set to dhcp. lwIP ignores them
 * unless NTP is set to dhcp.
 */
void dhcp6NtpRefresh();

/// True if lwIP address slot i holds the DHCPv6 address (status screen)
bool dhcp6IsSlot(int i);

/**
 * @brief State for the status screen and the web page
 */
struct Dhcp6Status {
    Dhcp6State state;
    bool hasAddress;               ///< A lease whose address is in an lwIP slot
    char address[IPV6_TEXT_MAX];   ///< RFC 5952 text, empty without a lease
    uint32_t validLeftS;           ///< 0xFFFFFFFF for infinity
    const char* problem;           ///< "" or why the leased address is not in use
    uint32_t sent, accepted, ignored;
};

void dhcp6GetStatus(Dhcp6Status& out);
