#pragma once

/**
 * @file snmp_mib.h
 * @brief The device MIB: a sorted table of OIDs and how to read each one.
 *
 * Objects (see docs/MIB.md and mibs/RF-POWER-MONITOR-MIB.txt):
 *
 *   1.3.6.1.2.1.1.1.0      sysDescr              OCTET STRING
 *   1.3.6.1.2.1.1.2.0      sysObjectID           OBJECT IDENTIFIER (1.3.6.1.4.1.63050)
 *   1.3.6.1.2.1.1.3.0      sysUpTime             TimeTicks
 *   1.3.6.1.2.1.1.4.0      sysContact            OCTET STRING
 *   1.3.6.1.2.1.1.5.0      sysName               OCTET STRING
 *   1.3.6.1.2.1.1.6.0      sysLocation           OCTET STRING
 *   1.3.6.1.2.1.1.7.0      sysServices           INTEGER (72)
 *   1.3.6.1.2.1.1.8.0      sysORLastChange       TimeTicks (0)
 *   1.3.6.1.2.1.1.9.1.c.n  sysORTable, columns 2-4 for the 9 MIB modules implemented
 *   1.3.6.1.2.1.2.1.0      ifNumber              INTEGER (1)
 *   1.3.6.1.2.1.2.2.1.x.1  ifTable, columns 1-22 for interface 1 (IF-MIB)
 *   1.3.6.1.2.1.4-7.n.0    MIB-II ip (1-19, 23), icmp (1-26), tcp (1-12, 14, 15), udp (1-4): lwIP's counters
 *   1.3.6.1.2.1.4.20.1.c   ipAddrTable (IPv4) and 1.3.6.1.2.1.4.34.1.c ipAddressTable: one row per address,
 *                          built from the state at each request (dynamic columns)
 *   1.3.6.1.2.1.11.n.0     snmp group (SNMPv2-MIB), n = 1-32 without 7 and 23
 *   1.3.6.1.2.1.25...      HOST-RESOURCES-MIB: hrSystem, hrMemorySize, hrStorageTable (1 RAM, 2 LittleFS),
 *                          hrDeviceTable and hrProcessorTable (768 core 0, 769 core 1)
 *   1.3.6.1.2.1.31.1.1.1.x.1  ifXTable, columns 1-19 for interface 1 (IF-MIB)
 *   1.3.6.1.2.1.47.1.1.1.1.x.1  entPhysicalTable columns 2-16 for the device (ENTITY-MIB), entLastChangeTime
 *   1.3.6.1.4.1.63050.1.0  mainsPowerStatus      INTEGER 0 = lost, 1 = present
 *   1.3.6.1.4.1.63050.2.0  mainsPowerChanges     Counter32
 *   1.3.6.1.4.1.63050.3.0  mainsPowerLastChange  TimeTicks (sysUpTime at the last change)
 *   1.3.6.1.4.1.63050.4.0  cpuTemperature        INTEGER, tenths of a degree Celsius (RP2040 die)
 *   1.3.6.1.4.1.63050.5.0  ambientTemperature    INTEGER, tenths of a degree Celsius (TMP1075),
 *                                                absent while the sensor does not answer
 *   1.3.6.1.6.3.1.1.6.1.0  snmpSetSerialNo       INTEGER (0)
 *
 * The ifHC* columns are Counter64. SNMPv1 cannot carry Counter64, so in v1
 * they do not exist (RFC 3584 4.2.2): Get returns noSuchName and GetNext
 * skips them.
 *
 * Pure C++: values come from an SnmpDeviceState snapshot that the firmware
 * fills in, so the MIB is unit tested on the host.
 */

#include <if_stats.h>
#include <lldp.h>

#include "snmp_message.h"

/// Enterprise number assigned to this project
constexpr uint32_t ENTERPRISE_NUMBER = 63050;

/**
 * @brief State of the one Ethernet interface (ifIndex 1)
 */
struct SnmpInterfaceState {
    bool linkUp;               ///< ifOperStatus up(1) / down(2)
    uint32_t speedBps;         ///< ifSpeed; 0 without link
    uint8_t mac[6];            ///< ifPhysAddress
    uint32_t lastChangeTicks;  ///< ifLastChange: sysUpTime when the link last changed, 0 if never
    IfCounters counters;       ///< Traffic since boot
};

/**
 * @brief LLDP data for LLDP-MIB lldpLocPortTable and lldpRemTable (static in
 *        the firmware, reached by pointer so no copy lands on the stack)
 */
struct SnmpLldpState {
    uint8_t localMac[6];      ///< lldpLocPortId of the one port (MAC subtype)
    bool present;             ///< A neighbour is held: lldpRemTable has its one row (0.1.1)
    LldpNeighbour neighbour;  ///< Valid while present
};

/// ifDescr of the one interface (ifIndex 1); LLDP sends it as Port Description
static const char* const SNMP_IF_DESCR = "W5500 10/100 Ethernet";

/// Where an address came from (IP-MIB IpAddressOriginTC)
enum class SnmpAddrOrigin : uint8_t { Manual = 2, Dhcp = 4, LinkLayer = 5, Random = 6 };

/// One address of the interface, for ipAddrTable (IPv4) and ipAddressTable (both)
struct SnmpIpAddress {
    bool ipv6;
    uint8_t addr[16];        ///< IPv4 in the first 4 bytes
    uint8_t prefixLen;       ///< IPv4 from the netmask; IPv6 64
    SnmpAddrOrigin origin;
    bool preferred;          ///< false: deprecated (still valid)
};

/// Most addresses at once: one IPv4 and lwIP's LWIP_IPV6_NUM_ADDRESSES (4) IPv6 slots
constexpr uint8_t SNMP_MAX_IP_ADDRESSES = 5;

/**
 * @brief The interface's valid addresses, in any order (the MIB sorts the rows by their index)
 */
struct SnmpIpState {
    uint8_t count;
    SnmpIpAddress addr[SNMP_MAX_IP_ADDRESSES];
};

/**
 * @brief HOST-RESOURCES-MIB and ENTITY-MIB data (static in the firmware, reached by pointer)
 */
struct SnmpHostState {
    uint32_t ramTotalBytes;        ///< Main SRAM (hrStorageRam size, hrMemorySize)
    uint32_t ramUsedBytes;         ///< Static data, stacks and heap in use: total minus free heap
    bool fsValid;                  ///< LittleFS mounted; false makes its hrStorage row absent
    uint32_t fsTotalBytes;         ///< LittleFS size
    uint32_t fsUsedBytes;          ///< LittleFS blocks in use
    uint8_t cpuLoad[2];            ///< hrProcessorLoad of core 0 and core 1, percent over the last minute
    const char* serialNumber;      ///< entPhysicalSerialNum: the flash chip's unique ID in hex
    const char* firmwareRevision;  ///< entPhysicalFirmwareRev
    const char* hardwareRevision;  ///< entPhysicalHardwareRev
};

/**
 * @brief MIB-II protocol statistics (ip, icmp, tcp, udp groups), lwIP's own counters (MIB2_STATS),
 *        copied once per request. The counters come first, in MIB order; the firmware fills the
 *        constants from lwIP's options.
 */
struct SnmpProtoCounters {
    // ip .3-.12, .14-.19, .23 (IPv4)
    uint32_t ipInReceives, ipInHdrErrors, ipInAddrErrors, ipForwDatagrams, ipInUnknownProtos, ipInDiscards,
        ipInDelivers, ipOutRequests, ipOutDiscards, ipOutNoRoutes, ipReasmReqds, ipReasmOKs, ipReasmFails,
        ipFragOKs, ipFragFails, ipFragCreates, ipRoutingDiscards;
    uint32_t icmpIn[13];   ///< icmpInMsgs .. icmpInAddrMaskReps (.1-.13, IPv4)
    uint32_t icmpOut[13];  ///< icmpOutMsgs .. icmpOutAddrMaskReps (.14-.26, IPv4)
    // tcp .5-.12, .14, .15 (IPv4 and IPv6); tcpCurrEstab is a gauge
    uint32_t tcpActiveOpens, tcpPassiveOpens, tcpAttemptFails, tcpEstabResets, tcpCurrEstab, tcpInSegs,
        tcpOutSegs, tcpRetransSegs, tcpInErrs, tcpOutRsts;
    // udp .1-.4 (IPv4 and IPv6)
    uint32_t udpInDatagrams, udpNoPorts, udpInErrors, udpOutDatagrams;
    // Constants of the stack
    int32_t ipDefaultTtl;     ///< lwIP IP_DEFAULT_TTL
    int32_t ipReasmTimeoutS;  ///< lwIP IP_REASS_MAXAGE, in seconds
    int32_t tcpRtoMinMs;      ///< Nominal, as lwIP's own SNMP agent reports it
    int32_t tcpRtoMaxMs;      ///< Nominal, as lwIP's own SNMP agent reports it
    int32_t tcpMaxConn;       ///< lwIP MEMP_NUM_TCP_PCB
};

/// Number of uint32_t counters at the start of SnmpProtoCounters (ipInReceives .. udpOutDatagrams)
constexpr uint32_t SNMP_PROTO_COUNTERS = 57;

/**
 * @brief The agent's counters: the status screen and the snmp group (SNMPv2-MIB, RFC 3418)
 */
struct SnmpStats {
    uint32_t inPackets;          ///< Every packet passed to handle() (snmpInPkts)
    uint32_t inBadVersions;      ///< Valid message, not SNMP v1 or v2c
    uint32_t inBadCommunity;     ///< Wrong community string
    uint32_t inParseErrors;      ///< Malformed packets (snmpInASNParseErrs)
    uint32_t inUnsupported;      ///< Response/trap/inform/report PDUs, too many varbinds
    uint32_t inGetResponses;     ///< Response PDUs received (also in inUnsupported)
    uint32_t inTraps;            ///< v1 Trap and SNMPv2-Trap PDUs received (also in inUnsupported)
    uint32_t inGetRequests;
    uint32_t inGetNextRequests;
    uint32_t inGetBulkRequests;
    uint32_t inSetRequests;
    uint32_t inV2cRequests;      ///< Requests in v2c (included in the counters above)
    uint32_t inTotalReqVars;     ///< Varbinds answered with a value by Get, GetNext and GetBulk
    uint32_t outResponses;       ///< Responses returned for sending
    uint32_t outNoSuchName;
    uint32_t outTooBig;
    uint32_t silentDrops;        ///< Requests dropped because not even an empty response fit
};

/**
 * @brief Everything the MIB reports, captured once per request
 */
struct SnmpDeviceState {
    uint32_t uptimeTicks;          ///< Hundredths of a second since boot
    bool mainsPresent;             ///< Debounced mains state
    uint32_t mainsChanges;         ///< Accepted mains changes since boot
    uint32_t mainsLastChangeTicks; ///< uptimeTicks at the last change, 0 if none
    bool mainsAlarm;               ///< mainsPowerAlarm: mains lost for 60 s or more
    const char* sysDescr;
    const char* sysContact;
    const char* sysName;
    const char* sysLocation;
    SnmpInterfaceState iface;      ///< IF-MIB data
    int32_t cpuTempDeciC;          ///< RP2040 die temperature, tenths of a degree Celsius
    bool ambientTempValid;         ///< TMP1075 answers; false makes ambientTemperature absent
    int32_t ambientTempDeciC;      ///< TMP1075 temperature, tenths of a degree Celsius
    const SnmpLldpState* lldp;     ///< LLDP-MIB data; nullptr: no LLDP-MIB object has an instance
    const SnmpStats* snmp;         ///< The agent's live counters (snmp group); nullptr: all 0
    uint32_t snmpOutTraps;         ///< Traps sent (snmpOutTraps, part of snmpOutPkts)
    const SnmpHostState* host;     ///< HOST-RESOURCES-MIB and ENTITY-MIB; nullptr: none of their objects exist
    const SnmpProtoCounters* proto;  ///< ip, icmp, tcp and udp groups; nullptr: none of their objects exist
    const SnmpIpState* ip;         ///< ipAddrTable and ipAddressTable rows; nullptr: both tables empty
};

/**
 * @brief One MIB object
 */
struct MibEntry {
    Oid oid;
    SnmpValue (*get)(const SnmpDeviceState& state);
    uint8_t indexArcs = 1;  ///< Arcs of the instance (.0 or ifIndex: 1; lldpRemTable: 3)
};

/**
 * @brief An instance with its current value, as found by mibGet / mibGetNext (~300 bytes: keep it off the stack)
 */
struct MibObject {
    Oid oid;
    SnmpValue value;
};

/// Which instances a GetNext may return
enum class MibView : uint8_t {
    V2c,  ///< Every instance that has a value now
    V1    ///< Also skips Counter64, which v1 cannot encode (RFC 3584 4.2.2.1)
};

/**
 * @brief Number of entries in the static table
 */
uint16_t mibSize();

/**
 * @brief Static entry by index, in OID order (0 .. mibSize()-1)
 */
const MibEntry& mibEntry(uint16_t index);

/**
 * @brief The exact instance oid with its value now, for GetRequest
 * @return false if oid is no instance, or the instance is absent now (getter says noSuchInstance)
 */
bool mibGet(const Oid& oid, const SnmpDeviceState& state, MibObject& out);

/**
 * @brief The first instance after oid that has a value now and is visible in view, for GetNext and GetBulk
 * @return false past the end of the MIB
 */
bool mibGetNext(const Oid& oid, const SnmpDeviceState& state, MibView view, MibObject& out);

/**
 * @brief true if oid lies under an object of the MIB (a scalar's OID without
 *        its ".0" instance), e.g. sysDescr.5 or sysDescr itself
 *
 * Distinguishes v2c noSuchInstance (object exists, wrong instance) from
 * noSuchObject (no such object at all).
 */
bool mibHasObject(const Oid& oid);
