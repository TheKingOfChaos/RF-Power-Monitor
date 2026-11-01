#pragma once

/**
 * @file lldp_service.h
 * @brief LLDP on the W5500 interface: announce the device to the switch, and
 *        show which switch and port the device is plugged into.
 *
 * The protocol is lib/lldp (host tested). Frames are read in the netif input
 * hook and sent with netStatsSendFrame() (src/net_stats.cpp). Always on.
 */

#include <stddef.h>
#include <stdint.h>
#include <snmp_mib.h>

struct netif;

/// Start on nif (after netStatsBegin); the first frame goes out 1 s after the link is up
void lldpBegin(netif* nif);

/**
 * @brief Look at a received frame and keep the neighbour if it is an LLDPDU
 *
 * Called from the netif input hook (lwIP context, driver lock held); never
 * changes the frame.
 */
void lldpInspectFrame(const uint8_t* frame, size_t len);

/// Expire the neighbour, forget it on link down, send when due. Call every loop()
void lldpPoll();

/// "<switch>, port <port>" or "none seen" (lib/lldp lldpSwitchText), cut to cap
void lldpServiceSwitchText(char* out, size_t cap);

/// The neighbour's management address as text, "" if none or no neighbour
void lldpServiceSwitchAddress(char* out, size_t cap);

/**
 * @brief Copy what LLDP-MIB serves: our MAC and the neighbour, under the driver lock
 * @return false before lldpBegin()
 */
bool lldpServiceSnmpState(SnmpLldpState& out);

/// Hidden menu command lldpdump: print "LLDP <hex>" of the last LLDPDU received, or "LLDP none"
void lldpDump();
