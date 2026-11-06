#pragma once

/**
 * @file snmp_service.h
 * @brief Runs the SNMP v1 agent (lib/snmp_agent) on UDP port 161.
 *
 * All protocol logic is in lib/snmp_agent and lib/access_control; this file
 * moves packets between lwIP and the agent, collects the device state for
 * the MIB, applies the per-source rate limit, logs rejected access and sends
 * traps to the configured destination.
 */

#include <IPAddress.h>
#include <access_control.h>
#include <netconfig.h>
#include <snmp_agent.h>

/// Standard SNMP agent port
constexpr uint16_t SNMP_PORT = 161;

/// Requests per second each source may send on average, and in a burst
constexpr uint32_t SNMP_RATE_PER_SECOND = 50;
constexpr uint32_t SNMP_RATE_BURST = 100;

/// Requests per second from all untrusted sources together, so rotating or
/// spoofed source addresses cannot bypass the per-source limit. Sources that
/// sent the right community are trusted and not counted here.
constexpr uint32_t SNMP_GLOBAL_RATE_PER_SECOND = 100;
constexpr uint32_t SNMP_GLOBAL_RATE_BURST = 200;

/// Trusted sources share their own, larger bucket. Trust is bounded because
/// the community may be the well-known default or sniffed (SNMPv1 is clear text).
constexpr uint32_t SNMP_TRUSTED_RATE_PER_SECOND = 200;
constexpr uint32_t SNMP_TRUSTED_RATE_BURST = 400;
constexpr uint8_t SNMP_MAX_TRUSTED_SOURCES = 4;
constexpr uint32_t SNMP_TRUST_TIMEOUT_MS = 10UL * 60 * 1000;
/// Burst and rate of each trusted source: whole snmpwalks from the root (~300 objects at ~250 requests/s)
/// run at full speed, also back to back. With 50/s and a burst of 100 for everyone a walk of the 3.1.0
/// MIB lost a request near object 116 and stalled for the manager's retry timeout. All trusted sources
/// together stay within SNMP_TRUSTED_RATE_*; unknown sources keep SNMP_RATE_*.
constexpr uint32_t SNMP_TRUSTED_SOURCE_BURST = 400;
constexpr uint32_t SNMP_TRUSTED_SOURCE_RATE = 200;

/// At most one authenticationFailure trap per interval
constexpr uint32_t AUTH_TRAP_INTERVAL_MS = 10000;

/**
 * @brief Counters of the service itself (the agent keeps its own SnmpStats)
 */
struct SnmpServiceStats {
    uint32_t rateLimited;   ///< Requests dropped by the rate limiter
    uint32_t trapsSent;     ///< Traps sent to the trap destination
    uint32_t trapsFailed;   ///< Traps that could not be encoded or sent
};

/**
 * @brief Open the UDP socket. Call once after Ethernet has started.
 * @return true if the port was bound
 */
bool snmpBegin();

/// sysDescr text (also sent in LLDP)
const char* snmpSysDescr();

/**
 * @brief Tell the service the device has an address; sends coldStart the first time
 * @param localIP The device's address, used as agent-addr in traps
 */
void snmpNetworkUp(const IPAddress& localIP);

/**
 * @brief Tell the service the device lost its IPv4 address (e.g. the lease ran out): a held trap waits
 *        for the next snmpNetworkUp() instead of leaving with the old address
 */
void snmpNetworkDown();

/**
 * @brief Handle pending requests. Call from loop() on core 0.
 */
void snmpPoll();

/**
 * @brief Revoke all rate-limit trust; call when the community has changed
 */
void snmpCommunityChanged();

/**
 * @brief The trap destination in use: the fixed setting, or DHCP option 224 when trapFromDhcp is set
 *        (only from the ACK for the current lease)
 * @return false if traps are off or no destination is known
 */
bool snmpTrapTarget(TrapAddress& out);

/**
 * @brief Send mainsPowerLost or mainsPowerRestored for the current mains state
 */
void snmpNotifyMainsChange();

/**
 * @brief Update the mains alarm (lost for 60 s) and send its mainsPowerLostTooLong trap once a trap can leave.
 *        Call from the core 0 loop.
 */
void snmpPollMainsAlarm();

/**
 * @brief Send the pending firmwareUpdateKept / firmwareRolledBack trap (lib/fw_update's boot record keeps it
 *        across the restart) once a trap can leave. Call from the core 0 loop.
 */
void snmpPollFirmwareTrap();

/**
 * @brief mainsPowerAlarm: mains has been lost for 60 s or more
 */
bool snmpMainsAlarm();

/**
 * @brief sysUpTime: hundredths of a second since boot
 */
uint32_t snmpUptimeTicks();

/**
 * @brief Request and error counters for the status screen
 */
const SnmpStats& snmpStats();

/**
 * @brief Rate limiting and trap counters for the status screen
 */
const SnmpServiceStats& snmpServiceStats();

/**
 * @brief Rejected access attempts, newest first
 */
const AccessLog& snmpAccessLog();

/**
 * @brief Record a rejected access attempt from another service (the web page) in the same log
 */
void accessLogRecord(const SourceKey& source, AccessEvent event);
