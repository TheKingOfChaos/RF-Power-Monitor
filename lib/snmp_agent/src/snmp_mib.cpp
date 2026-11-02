#include "snmp_mib.h"

// ---- system group (RFC 1213) ----

static SnmpValue getSysDescr(const SnmpDeviceState& s) { return SnmpValue::makeString(s.sysDescr); }
static SnmpValue getSysContact(const SnmpDeviceState& s) { return SnmpValue::makeString(s.sysContact); }
static SnmpValue getSysName(const SnmpDeviceState& s) { return SnmpValue::makeString(s.sysName); }
static SnmpValue getSysLocation(const SnmpDeviceState& s) { return SnmpValue::makeString(s.sysLocation); }

static SnmpValue getSysObjectId(const SnmpDeviceState&) {
    Oid o = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER}, 7};
    return SnmpValue::makeOid(o);
}

static SnmpValue getSysUpTime(const SnmpDeviceState& s) {
    return SnmpValue::makeUnsigned(BerTag::TimeTicks, s.uptimeTicks);
}

static SnmpValue getSysServices(const SnmpDeviceState&) {
    // RFC 1213: layers 4 (end-to-end, 8) + 7 (applications, 64)
    return SnmpValue::makeInteger(72);
}

static SnmpValue integer(int32_t v) { return SnmpValue::makeInteger(v); }
static SnmpValue counter32(uint64_t v) { return SnmpValue::makeUnsigned(BerTag::Counter32, (uint32_t)v); }
static SnmpValue counter64(uint64_t v) { return SnmpValue::makeCounter64(v); }
static SnmpValue gauge(uint32_t v) { return SnmpValue::makeUnsigned(BerTag::Gauge32, v); }
static SnmpValue timeTicks(uint32_t v) { return SnmpValue::makeUnsigned(BerTag::TimeTicks, v); }

// ---- sysORTable (SNMPv2-MIB, RFC 3418): the MIB modules the agent implements ----

struct SysOREntry {
    Oid id;              ///< The module's MODULE-IDENTITY
    const char* descr;
};

static const SysOREntry SYS_OR[] = {
    {{{1, 3, 6, 1, 6, 3, 1}, 7}, "SNMPv2-MIB"},
    {{{1, 3, 6, 1, 2, 1, 31}, 7}, "IF-MIB"},
    {{{1, 3, 6, 1, 2, 1, 48}, 7}, "IP-MIB"},
    {{{1, 3, 6, 1, 2, 1, 50}, 7}, "UDP-MIB"},
    {{{1, 3, 6, 1, 2, 1, 49}, 7}, "TCP-MIB"},
    {{{1, 3, 6, 1, 2, 1, 25, 7, 1}, 9}, "HOST-RESOURCES-MIB"},
    {{{1, 3, 6, 1, 2, 1, 47}, 7}, "ENTITY-MIB"},
    {{{1, 0, 8802, 1, 1, 2}, 6}, "LLDP-MIB"},
    {{{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER}, 7}, "RF-POWER-MONITOR-MIB"},
};

template <uint8_t ROW> static SnmpValue getSysORID(const SnmpDeviceState&) {
    return SnmpValue::makeOid(SYS_OR[ROW - 1].id);
}
template <uint8_t ROW> static SnmpValue getSysORDescr(const SnmpDeviceState&) {
    return SnmpValue::makeString(SYS_OR[ROW - 1].descr);
}
static SnmpValue getZeroTicks(const SnmpDeviceState&) { return timeTicks(0); }  // sysORLastChange, sysORUpTime

// ---- snmp group (SNMPv2-MIB, RFC 3418; the RFC 1213 counters it keeps) ----

/// One of the agent's counters, 0 without them
template <uint32_t SnmpStats::*FIELD> static SnmpValue getSnmpStat(const SnmpDeviceState& s) {
    return counter32(s.snmp ? s.snmp->*FIELD : 0);
}
static SnmpValue getSnmpOutPkts(const SnmpDeviceState& s) {
    return counter32((uint64_t)(s.snmp ? s.snmp->outResponses : 0) + s.snmpOutTraps);
}
static SnmpValue getSnmpOutTraps(const SnmpDeviceState& s) { return counter32(s.snmpOutTraps); }
static SnmpValue getSnmpEnableAuthenTraps(const SnmpDeviceState&) { return integer(1); }  // enabled
static SnmpValue getSnmpSetSerialNo(const SnmpDeviceState&) { return integer(0); }  // no Set is ever accepted

// ---- interfaces (IF-MIB, RFC 2863), one Ethernet interface with ifIndex 1 ----

static const char* const IF_NAME = "eth0";
static const int32_t IF_TYPE_ETHERNET_CSMACD = 6;
static const int32_t IF_MTU = 1500;

static SnmpValue getIfNumber(const SnmpDeviceState&) { return integer(1); }
static SnmpValue getIfIndex(const SnmpDeviceState&) { return integer(1); }
static SnmpValue getIfDescr(const SnmpDeviceState&) { return SnmpValue::makeString(SNMP_IF_DESCR); }
static SnmpValue getIfType(const SnmpDeviceState&) { return integer(IF_TYPE_ETHERNET_CSMACD); }
static SnmpValue getIfMtu(const SnmpDeviceState&) { return integer(IF_MTU); }
static SnmpValue getIfSpeed(const SnmpDeviceState& s) { return gauge(s.iface.linkUp ? s.iface.speedBps : 0); }
static SnmpValue getIfPhysAddress(const SnmpDeviceState& s) { return SnmpValue::makeBytes(s.iface.mac, 6); }
static SnmpValue getIfAdminStatus(const SnmpDeviceState&) { return integer(1); }  // up
static SnmpValue getIfOperStatus(const SnmpDeviceState& s) { return integer(s.iface.linkUp ? 1 : 2); }
static SnmpValue getIfLastChange(const SnmpDeviceState& s) {
    return SnmpValue::makeUnsigned(BerTag::TimeTicks, s.iface.lastChangeTicks);
}
static SnmpValue getIfInOctets(const SnmpDeviceState& s) { return counter32(s.iface.counters.inOctets); }
static SnmpValue getIfInUcastPkts(const SnmpDeviceState& s) { return counter32(s.iface.counters.inUcastPkts); }
static SnmpValue getIfInNUcastPkts(const SnmpDeviceState& s) {
    return counter32(s.iface.counters.inMulticastPkts + s.iface.counters.inBroadcastPkts);
}
static SnmpValue getZeroCounter(const SnmpDeviceState&) { return counter32(0); }  // not measurable
static SnmpValue getIfInDiscards(const SnmpDeviceState& s) { return counter32(s.iface.counters.inDiscards); }
static SnmpValue getIfInErrors(const SnmpDeviceState& s) { return counter32(s.iface.counters.inErrors); }
static SnmpValue getIfInUnknownProtos(const SnmpDeviceState& s) {
    return counter32(s.iface.counters.inUnknownProtos);
}
static SnmpValue getIfOutOctets(const SnmpDeviceState& s) { return counter32(s.iface.counters.outOctets); }
static SnmpValue getIfOutUcastPkts(const SnmpDeviceState& s) { return counter32(s.iface.counters.outUcastPkts); }
static SnmpValue getIfOutNUcastPkts(const SnmpDeviceState& s) {
    return counter32(s.iface.counters.outMulticastPkts + s.iface.counters.outBroadcastPkts);
}
static SnmpValue getIfOutErrors(const SnmpDeviceState& s) { return counter32(s.iface.counters.outErrors); }
static SnmpValue getIfOutQLen(const SnmpDeviceState&) { return gauge(0); }
static SnmpValue getIfSpecific(const SnmpDeviceState&) {
    Oid zero = {{0, 0}, 2};
    return SnmpValue::makeOid(zero);
}

static SnmpValue getIfName(const SnmpDeviceState&) { return SnmpValue::makeString(IF_NAME); }
static SnmpValue getIfInMcast(const SnmpDeviceState& s) { return counter32(s.iface.counters.inMulticastPkts); }
static SnmpValue getIfInBcast(const SnmpDeviceState& s) { return counter32(s.iface.counters.inBroadcastPkts); }
static SnmpValue getIfOutMcast(const SnmpDeviceState& s) { return counter32(s.iface.counters.outMulticastPkts); }
static SnmpValue getIfOutBcast(const SnmpDeviceState& s) { return counter32(s.iface.counters.outBroadcastPkts); }
static SnmpValue getIfHCInOctets(const SnmpDeviceState& s) { return counter64(s.iface.counters.inOctets); }
static SnmpValue getIfHCInUcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.inUcastPkts); }
static SnmpValue getIfHCInMcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.inMulticastPkts); }
static SnmpValue getIfHCInBcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.inBroadcastPkts); }
static SnmpValue getIfHCOutOctets(const SnmpDeviceState& s) { return counter64(s.iface.counters.outOctets); }
static SnmpValue getIfHCOutUcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.outUcastPkts); }
static SnmpValue getIfHCOutMcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.outMulticastPkts); }
static SnmpValue getIfHCOutBcast(const SnmpDeviceState& s) { return counter64(s.iface.counters.outBroadcastPkts); }
static SnmpValue getIfLinkUpDownTrapEnable(const SnmpDeviceState&) { return integer(2); }  // disabled
static SnmpValue getIfHighSpeed(const SnmpDeviceState& s) {
    return gauge(s.iface.linkUp ? s.iface.speedBps / 1000000 : 0);
}
static SnmpValue getIfPromiscuousMode(const SnmpDeviceState&) { return integer(2); }  // false
static SnmpValue getIfConnectorPresent(const SnmpDeviceState&) { return integer(1); }  // true
static SnmpValue getIfAlias(const SnmpDeviceState&) { return SnmpValue::makeString(""); }
static SnmpValue getIfCounterDiscontinuityTime(const SnmpDeviceState&) {
    return SnmpValue::makeUnsigned(BerTag::TimeTicks, 0);  // counters start at boot
}

// ---- enterprise 63050 ----

static SnmpValue getMainsStatus(const SnmpDeviceState& s) {
    return SnmpValue::makeInteger(s.mainsPresent ? 1 : 0);
}

static SnmpValue getMainsChanges(const SnmpDeviceState& s) {
    return SnmpValue::makeUnsigned(BerTag::Counter32, s.mainsChanges);
}

static SnmpValue getMainsLastChange(const SnmpDeviceState& s) {
    return SnmpValue::makeUnsigned(BerTag::TimeTicks, s.mainsLastChangeTicks);
}

static SnmpValue getCpuTemperature(const SnmpDeviceState& s) {
    return SnmpValue::makeInteger(s.cpuTempDeciC);
}

/// No instance while the TMP1075 does not answer (no carrier board): the agent skips it
static SnmpValue getAmbientTemperature(const SnmpDeviceState& s) {
    if (!s.ambientTempValid) return SnmpValue::makeException(BerTag::NoSuchInstance);
    return SnmpValue::makeInteger(s.ambientTempDeciC);
}

/// 1 once mains has been lost for 60 s (lib/power_monitor MainsAlarm)
static SnmpValue getMainsAlarm(const SnmpDeviceState& s) {
    return SnmpValue::makeInteger(s.mainsAlarm ? 1 : 0);
}

// ---- LLDP-MIB (IEEE 802.1AB-2005 Annex F): our one port and the one neighbour ----

static const int32_t LLDP_PORT_ID_MAC = 3;

static SnmpValue noInstance() { return SnmpValue::makeException(BerTag::NoSuchInstance); }

/// The neighbour, or nullptr when lldpRemTable has no row
static const LldpNeighbour* remote(const SnmpDeviceState& s) {
    return (s.lldp != nullptr && s.lldp->present) ? &s.lldp->neighbour : nullptr;
}

static SnmpValue getLocPortIdSubtype(const SnmpDeviceState& s) {
    return s.lldp ? integer(LLDP_PORT_ID_MAC) : noInstance();
}
static SnmpValue getLocPortId(const SnmpDeviceState& s) {
    return s.lldp ? SnmpValue::makeBytes(s.lldp->localMac, 6) : noInstance();
}
static SnmpValue getLocPortDesc(const SnmpDeviceState& s) {
    return s.lldp ? SnmpValue::makeString(SNMP_IF_DESCR) : noInstance();
}

static SnmpValue getRemChassisIdSubtype(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    return n ? integer(n->chassisIdSubtype) : noInstance();
}
static SnmpValue getRemChassisId(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    return n ? SnmpValue::makeBytes(n->chassisIdRaw, n->chassisIdLen) : noInstance();
}
static SnmpValue getRemPortIdSubtype(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    if (!n) return noInstance();
    uint8_t subtype;
    const uint8_t* data;
    size_t len;
    lldpSnmpPortId(*n, subtype, data, len);
    return integer(subtype);
}
static SnmpValue getRemPortId(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    if (!n) return noInstance();
    uint8_t subtype;
    const uint8_t* data;
    size_t len;
    lldpSnmpPortId(*n, subtype, data, len);
    return SnmpValue::makeBytes(data, len);
}
static SnmpValue getRemPortDesc(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    return n ? SnmpValue::makeString(n->portDescr) : noInstance();
}
static SnmpValue getRemSysName(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    return n ? SnmpValue::makeString(n->sysName) : noInstance();
}
static SnmpValue getRemSysDesc(const SnmpDeviceState& s) {
    const LldpNeighbour* n = remote(s);
    return n ? SnmpValue::makeString(n->sysDescr) : noInstance();
}
/// The byte with its bit order reversed
static uint8_t reverseBits(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r |= (uint8_t)(((b >> i) & 1) << (7 - i));
    return r;
}

/**
 * @brief LldpSystemCapabilitiesMap (BITS): the TLV numbers capabilities from its lowest bit
 *        (bridge = 0x0004), BITS from the top bit of the first byte (RFC 2578 7.1.4), so bridge(2)
 *        becomes 0x20 in byte 0. buf must outlive the response
 */
static SnmpValue caps(uint16_t bits, uint8_t* buf) {
    buf[0] = reverseBits((uint8_t)bits);         // capabilities 0-7
    buf[1] = reverseBits((uint8_t)(bits >> 8));  // capabilities 8-15
    return SnmpValue::makeBytes(buf, 2);
}
static SnmpValue getRemSysCapSupported(const SnmpDeviceState& s) {
    static uint8_t buf[2];
    const LldpNeighbour* n = remote(s);
    return n ? caps(n->capsSupported, buf) : noInstance();
}
static SnmpValue getRemSysCapEnabled(const SnmpDeviceState& s) {
    static uint8_t buf[2];
    const LldpNeighbour* n = remote(s);
    return n ? caps(n->capsEnabled, buf) : noInstance();
}

// ---- table, must be sorted by OID (checked by tests) ----

#define SYSTEM(n) {{1, 3, 6, 1, 2, 1, 1, n, 0}, 9}
#define SYS_OR_COL(c, r) {{1, 3, 6, 1, 2, 1, 1, 9, 1, c, r}, 11}
#define SYS_OR_ROW(r) {SYS_OR_COL(2, r), getSysORID<r>}
#define SNMP_GROUP(n) {{1, 3, 6, 1, 2, 1, 11, n, 0}, 9}
#define SNMP_SET_SERIAL_NO {{1, 3, 6, 1, 6, 3, 1, 1, 6, 1, 0}, 11}
#define IF_NUMBER {{1, 3, 6, 1, 2, 1, 2, 1, 0}, 9}
#define IF(n) {{1, 3, 6, 1, 2, 1, 2, 2, 1, n, 1}, 11}
#define IFX(n) {{1, 3, 6, 1, 2, 1, 31, 1, 1, 1, n, 1}, 12}
#define PRIVATE(n) {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, n, 0}, 9}
// lldpLocPortTable row lldpLocPortNum 1; lldpRemTable row lldpRemTimeMark 0, lldpRemLocalPortNum 1, lldpRemIndex 1
#define LLDP_LOC(n) {{1, 0, 8802, 1, 1, 2, 1, 3, 7, 1, n, 1}, 12}
#define LLDP_REM(n) {{1, 0, 8802, 1, 1, 2, 1, 4, 1, 1, n, 0, 1, 1}, 14}

static const MibEntry MIB[] = {
    {LLDP_LOC(2), getLocPortIdSubtype},
    {LLDP_LOC(3), getLocPortId},
    {LLDP_LOC(4), getLocPortDesc},
    {LLDP_REM(4), getRemChassisIdSubtype, 3},
    {LLDP_REM(5), getRemChassisId, 3},
    {LLDP_REM(6), getRemPortIdSubtype, 3},
    {LLDP_REM(7), getRemPortId, 3},
    {LLDP_REM(8), getRemPortDesc, 3},
    {LLDP_REM(9), getRemSysName, 3},
    {LLDP_REM(10), getRemSysDesc, 3},
    {LLDP_REM(11), getRemSysCapSupported, 3},
    {LLDP_REM(12), getRemSysCapEnabled, 3},
    {SYSTEM(1), getSysDescr},
    {SYSTEM(2), getSysObjectId},
    {SYSTEM(3), getSysUpTime},
    {SYSTEM(4), getSysContact},
    {SYSTEM(5), getSysName},
    {SYSTEM(6), getSysLocation},
    {SYSTEM(7), getSysServices},
    {SYSTEM(8), getZeroTicks},  // sysORLastChange: the table never changes
    SYS_OR_ROW(1), SYS_OR_ROW(2), SYS_OR_ROW(3), SYS_OR_ROW(4), SYS_OR_ROW(5),
    SYS_OR_ROW(6), SYS_OR_ROW(7), SYS_OR_ROW(8), SYS_OR_ROW(9),
    {SYS_OR_COL(3, 1), getSysORDescr<1>}, {SYS_OR_COL(3, 2), getSysORDescr<2>},
    {SYS_OR_COL(3, 3), getSysORDescr<3>}, {SYS_OR_COL(3, 4), getSysORDescr<4>},
    {SYS_OR_COL(3, 5), getSysORDescr<5>}, {SYS_OR_COL(3, 6), getSysORDescr<6>},
    {SYS_OR_COL(3, 7), getSysORDescr<7>}, {SYS_OR_COL(3, 8), getSysORDescr<8>},
    {SYS_OR_COL(3, 9), getSysORDescr<9>},
    {SYS_OR_COL(4, 1), getZeroTicks}, {SYS_OR_COL(4, 2), getZeroTicks}, {SYS_OR_COL(4, 3), getZeroTicks},
    {SYS_OR_COL(4, 4), getZeroTicks}, {SYS_OR_COL(4, 5), getZeroTicks}, {SYS_OR_COL(4, 6), getZeroTicks},
    {SYS_OR_COL(4, 7), getZeroTicks}, {SYS_OR_COL(4, 8), getZeroTicks}, {SYS_OR_COL(4, 9), getZeroTicks},

    {IF_NUMBER, getIfNumber},
    {IF(1), getIfIndex},
    {IF(2), getIfDescr},
    {IF(3), getIfType},
    {IF(4), getIfMtu},
    {IF(5), getIfSpeed},
    {IF(6), getIfPhysAddress},
    {IF(7), getIfAdminStatus},
    {IF(8), getIfOperStatus},
    {IF(9), getIfLastChange},
    {IF(10), getIfInOctets},
    {IF(11), getIfInUcastPkts},
    {IF(12), getIfInNUcastPkts},
    {IF(13), getIfInDiscards},     // no receive buffer free in lwIP
    {IF(14), getIfInErrors},       // impossible length; the W5500 drops CRC errors itself
    {IF(15), getIfInUnknownProtos},
    {IF(16), getIfOutOctets},
    {IF(17), getIfOutUcastPkts},
    {IF(18), getIfOutNUcastPkts},
    {IF(19), getZeroCounter},      // ifOutDiscards: lwIP drops before the driver, not visible
    {IF(20), getIfOutErrors},
    {IF(21), getIfOutQLen},
    {IF(22), getIfSpecific},

    // snmp group: counters of events the agent never sees or causes are 0
    {SNMP_GROUP(1), getSnmpStat<&SnmpStats::inPackets>},
    {SNMP_GROUP(2), getSnmpOutPkts},
    {SNMP_GROUP(3), getSnmpStat<&SnmpStats::inBadVersions>},
    {SNMP_GROUP(4), getSnmpStat<&SnmpStats::inBadCommunity>},  // snmpInBadCommunityNames
    {SNMP_GROUP(5), getZeroCounter},   // snmpInBadCommunityUses: one community, allowed everything
    {SNMP_GROUP(6), getSnmpStat<&SnmpStats::inParseErrors>},   // snmpInASNParseErrs
    {SNMP_GROUP(8), getZeroCounter},   // snmpInTooBigs .. snmpInGenErrs: error PDUs in, never accepted
    {SNMP_GROUP(9), getZeroCounter},
    {SNMP_GROUP(10), getZeroCounter},
    {SNMP_GROUP(11), getZeroCounter},
    {SNMP_GROUP(12), getZeroCounter},
    {SNMP_GROUP(13), getSnmpStat<&SnmpStats::inTotalReqVars>},
    {SNMP_GROUP(14), getZeroCounter},  // snmpInTotalSetVars: nothing is writable
    {SNMP_GROUP(15), getSnmpStat<&SnmpStats::inGetRequests>},
    {SNMP_GROUP(16), getSnmpStat<&SnmpStats::inGetNextRequests>},
    {SNMP_GROUP(17), getSnmpStat<&SnmpStats::inSetRequests>},
    {SNMP_GROUP(18), getSnmpStat<&SnmpStats::inGetResponses>},
    {SNMP_GROUP(19), getSnmpStat<&SnmpStats::inTraps>},
    {SNMP_GROUP(20), getSnmpStat<&SnmpStats::outTooBig>},
    {SNMP_GROUP(21), getSnmpStat<&SnmpStats::outNoSuchName>},
    {SNMP_GROUP(22), getZeroCounter},  // snmpOutBadValues
    {SNMP_GROUP(24), getZeroCounter},  // snmpOutGenErrs
    {SNMP_GROUP(25), getZeroCounter},  // snmpOutGetRequests .. snmpOutSetRequests: the device is no manager
    {SNMP_GROUP(26), getZeroCounter},
    {SNMP_GROUP(27), getZeroCounter},
    {SNMP_GROUP(28), getSnmpStat<&SnmpStats::outResponses>},   // snmpOutGetResponses
    {SNMP_GROUP(29), getSnmpOutTraps},
    {SNMP_GROUP(30), getSnmpEnableAuthenTraps},
    {SNMP_GROUP(31), getSnmpStat<&SnmpStats::silentDrops>},
    {SNMP_GROUP(32), getZeroCounter},  // snmpProxyDrops: no proxy

    {IFX(1), getIfName},
    {IFX(2), getIfInMcast},
    {IFX(3), getIfInBcast},
    {IFX(4), getIfOutMcast},
    {IFX(5), getIfOutBcast},
    {IFX(6), getIfHCInOctets},
    {IFX(7), getIfHCInUcast},
    {IFX(8), getIfHCInMcast},
    {IFX(9), getIfHCInBcast},
    {IFX(10), getIfHCOutOctets},
    {IFX(11), getIfHCOutUcast},
    {IFX(12), getIfHCOutMcast},
    {IFX(13), getIfHCOutBcast},
    {IFX(14), getIfLinkUpDownTrapEnable},
    {IFX(15), getIfHighSpeed},
    {IFX(16), getIfPromiscuousMode},
    {IFX(17), getIfConnectorPresent},
    {IFX(18), getIfAlias},
    {IFX(19), getIfCounterDiscontinuityTime},

    {PRIVATE(1), getMainsStatus},
    {PRIVATE(2), getMainsChanges},
    {PRIVATE(3), getMainsLastChange},
    {PRIVATE(4), getCpuTemperature},
    {PRIVATE(5), getAmbientTemperature},
    {PRIVATE(6), getMainsAlarm},

    {SNMP_SET_SERIAL_NO, getSnmpSetSerialNo},
};

static const uint16_t MIB_SIZE = sizeof(MIB) / sizeof(MIB[0]);

uint16_t mibSize() {
    return MIB_SIZE;
}

const MibEntry& mibEntry(uint16_t index) {
    return MIB[index < MIB_SIZE ? index : MIB_SIZE - 1];
}

/// Static entry with exactly this OID, or nullptr
static const MibEntry* find(const Oid& oid) {
    for (uint16_t i = 0; i < MIB_SIZE; i++) {
        if (oidCompare(MIB[i].oid, oid) == 0) return &MIB[i];
    }
    return nullptr;
}

/// First static entry whose OID sorts after oid, or nullptr
static const MibEntry* next(const Oid& oid) {
    for (uint16_t i = 0; i < MIB_SIZE; i++) {
        if (oidCompare(MIB[i].oid, oid) > 0) return &MIB[i];
    }
    return nullptr;
}

/// true if a GetNext in view may return this value
static bool visible(const SnmpValue& v, MibView view) {
    if (v.type == BerTag::NoSuchInstance) return false;
    return view == MibView::V2c || v.type != BerTag::Counter64;
}

bool mibGet(const Oid& oid, const SnmpDeviceState& state, MibObject& out) {
    const MibEntry* e = find(oid);
    if (e == nullptr) return false;
    out.value = e->get(state);
    if (out.value.type == BerTag::NoSuchInstance) return false;
    out.oid = e->oid;
    return true;
}

bool mibGetNext(const Oid& oid, const SnmpDeviceState& state, MibView view, MibObject& out) {
    for (const MibEntry* e = next(oid); e != nullptr; e = next(e->oid)) {
        out.value = e->get(state);
        if (visible(out.value, view)) {
            out.oid = e->oid;
            return true;
        }
    }
    return false;
}

bool mibHasObject(const Oid& oid) {
    for (uint16_t i = 0; i < MIB_SIZE; i++) {
        // The object is the entry without its instance (.0, ifIndex, or lldpRemTable's three index arcs)
        Oid object = MIB[i].oid;
        object.length -= MIB[i].indexArcs;
        if (oidStartsWith(oid, object)) return true;
    }
    return false;
}
