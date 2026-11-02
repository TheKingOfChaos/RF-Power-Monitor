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
 *   1.3.6.1.2.1.11.n.0     snmp group (SNMPv2-MIB), n = 1-32 without 7 and 23
 *   1.3.6.1.2.1.31.1.1.1.x.1  ifXTable, columns 1-19 for interface 1 (IF-MIB)
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
