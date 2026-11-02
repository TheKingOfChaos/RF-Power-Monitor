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

// ---- HOST-RESOURCES-MIB (RFC 2790): two processors, RAM and the LittleFS partition ----

static const uint32_t HR_UNIT = 1024;  ///< hrStorageAllocationUnits: sizes and use in KB

/// hrStorage row R (1 RAM, 2 LittleFS) exists now
static bool storageRow(const SnmpDeviceState& s, uint8_t row) {
    return s.host != nullptr && (row == 1 || s.host->fsValid);
}
static uint32_t storageBytes(const SnmpHostState& h, uint8_t row, bool used) {
    if (row == 1) return used ? h.ramUsedBytes : h.ramTotalBytes;
    return used ? h.fsUsedBytes : h.fsTotalBytes;
}

static SnmpValue getHrSystemUptime(const SnmpDeviceState& s) {
    return s.host ? timeTicks(s.uptimeTicks) : noInstance();
}
static SnmpValue getHrSystemNumUsers(const SnmpDeviceState& s) { return s.host ? gauge(0) : noInstance(); }
/// The two core loops are the only "processes"
static SnmpValue getHrSystemProcesses(const SnmpDeviceState& s) { return s.host ? gauge(2) : noInstance(); }
static SnmpValue getHrSystemMaxProcesses(const SnmpDeviceState& s) { return s.host ? integer(2) : noInstance(); }
static SnmpValue getHrMemorySize(const SnmpDeviceState& s) {
    return s.host ? integer((int32_t)(s.host->ramTotalBytes / HR_UNIT)) : noInstance();
}

template <uint8_t ROW> static SnmpValue getHrStorageIndex(const SnmpDeviceState& s) {
    return storageRow(s, ROW) ? integer(ROW) : noInstance();
}
template <uint8_t ROW> static SnmpValue getHrStorageType(const SnmpDeviceState& s) {
    // hrStorageTypes: hrStorageRam(2), hrStorageFlashMemory(9)
    static const Oid RAM = {{1, 3, 6, 1, 2, 1, 25, 2, 1, 2}, 10};
    static const Oid FLASH = {{1, 3, 6, 1, 2, 1, 25, 2, 1, 9}, 10};
    return storageRow(s, ROW) ? SnmpValue::makeOid(ROW == 1 ? RAM : FLASH) : noInstance();
}
template <uint8_t ROW> static SnmpValue getHrStorageDescr(const SnmpDeviceState& s) {
    return storageRow(s, ROW) ? SnmpValue::makeString(ROW == 1 ? "RAM" : "LittleFS (settings)") : noInstance();
}
template <uint8_t ROW> static SnmpValue getHrStorageUnits(const SnmpDeviceState& s) {
    return storageRow(s, ROW) ? integer((int32_t)HR_UNIT) : noInstance();
}
template <uint8_t ROW> static SnmpValue getHrStorageSize(const SnmpDeviceState& s) {
    return storageRow(s, ROW) ? integer((int32_t)(storageBytes(*s.host, ROW, false) / HR_UNIT)) : noInstance();
}
/// Rounded up, so a partly used KB counts
template <uint8_t ROW> static SnmpValue getHrStorageUsed(const SnmpDeviceState& s) {
    if (!storageRow(s, ROW)) return noInstance();
    return integer((int32_t)((storageBytes(*s.host, ROW, true) + HR_UNIT - 1) / HR_UNIT));
}
template <uint8_t ROW> static SnmpValue getHrStorageFailures(const SnmpDeviceState& s) {
    return storageRow(s, ROW) ? counter32(0) : noInstance();
}

/// hrDeviceIndex of core 0 and core 1 (net-snmp also numbers processors from 768)
static const uint32_t HR_CORE0 = 768;

template <uint8_t CORE> static SnmpValue getHrDeviceIndex(const SnmpDeviceState& s) {
    return s.host ? integer((int32_t)(HR_CORE0 + CORE)) : noInstance();
}
static SnmpValue getHrDeviceType(const SnmpDeviceState& s) {
    static const Oid PROCESSOR = {{1, 3, 6, 1, 2, 1, 25, 3, 1, 3}, 10};  // hrDeviceProcessor
    return s.host ? SnmpValue::makeOid(PROCESSOR) : noInstance();
}
template <uint8_t CORE> static SnmpValue getHrDeviceDescr(const SnmpDeviceState& s) {
    if (!s.host) return noInstance();
    return SnmpValue::makeString(CORE == 0 ? "RP2040 core 0 (network, SNMP)" : "RP2040 core 1 (mains input)");
}
static SnmpValue getZeroDotZero(const SnmpDeviceState& s) {
    static const Oid ZERO = {{0, 0}, 2};
    return s.host ? SnmpValue::makeOid(ZERO) : noInstance();
}
static SnmpValue getHrDeviceStatus(const SnmpDeviceState& s) { return s.host ? integer(2) : noInstance(); }  // running
static SnmpValue getHrDeviceErrors(const SnmpDeviceState& s) { return s.host ? counter32(0) : noInstance(); }
template <uint8_t CORE> static SnmpValue getHrProcessorLoad(const SnmpDeviceState& s) {
    return s.host ? integer(s.host->cpuLoad[CORE]) : noInstance();
}

// ---- ENTITY-MIB (RFC 6933): the device itself, entPhysicalIndex 1 ----

static SnmpValue hostString(const SnmpDeviceState& s, const char* text) {
    return s.host ? SnmpValue::makeString(text) : noInstance();
}
static SnmpValue getEntDescr(const SnmpDeviceState& s) { return hostString(s, "RF-Power-Monitor mains power monitor"); }
static SnmpValue getEntVendorType(const SnmpDeviceState& s) {
    return s.host ? getSysObjectId(s) : noInstance();
}
static SnmpValue getEntContainedIn(const SnmpDeviceState& s) { return s.host ? integer(0) : noInstance(); }
static SnmpValue getEntClass(const SnmpDeviceState& s) { return s.host ? integer(3) : noInstance(); }  // chassis
static SnmpValue getEntParentRelPos(const SnmpDeviceState& s) { return s.host ? integer(-1) : noInstance(); }
static SnmpValue getEntName(const SnmpDeviceState& s) { return hostString(s, s.sysName); }
static SnmpValue getEntHardwareRev(const SnmpDeviceState& s) {
    return s.host ? SnmpValue::makeString(s.host->hardwareRevision) : noInstance();
}
static SnmpValue getEntFirmwareRev(const SnmpDeviceState& s) {
    return s.host ? SnmpValue::makeString(s.host->firmwareRevision) : noInstance();
}
static SnmpValue getEntEmpty(const SnmpDeviceState& s) { return hostString(s, ""); }  // software rev, alias, asset ID
static SnmpValue getEntSerialNum(const SnmpDeviceState& s) {
    return s.host ? SnmpValue::makeString(s.host->serialNumber) : noInstance();
}
/// UTF-8, as SnmpAdminString allows
static SnmpValue getEntMfgName(const SnmpDeviceState& s) { return hostString(s, "Kenneth Foli J\xC3\xB8rgensen"); }
static SnmpValue getEntModelName(const SnmpDeviceState& s) { return hostString(s, "RF-Power-Monitor"); }
static SnmpValue getEntIsFRU(const SnmpDeviceState& s) { return s.host ? integer(2) : noInstance(); }  // false
static SnmpValue getEntLastChangeTime(const SnmpDeviceState& s) { return s.host ? timeTicks(0) : noInstance(); }

// ---- table, must be sorted by OID (checked by tests) ----

#define SYSTEM(n) {{1, 3, 6, 1, 2, 1, 1, n, 0}, 9}
#define SYS_OR_COL(c, r) {{1, 3, 6, 1, 2, 1, 1, 9, 1, c, r}, 11}
#define SYS_OR_ROW(r) {SYS_OR_COL(2, r), getSysORID<r>}
#define SNMP_GROUP(n) {{1, 3, 6, 1, 2, 1, 11, n, 0}, 9}
#define SNMP_SET_SERIAL_NO {{1, 3, 6, 1, 6, 3, 1, 1, 6, 1, 0}, 11}
#define HR(a, b) {{1, 3, 6, 1, 2, 1, 25, a, b, 0}, 10}
#define HR_STORAGE(c, r) {{1, 3, 6, 1, 2, 1, 25, 2, 3, 1, c, r}, 12}
#define HR_DEVICE(c, core) {{1, 3, 6, 1, 2, 1, 25, 3, 2, 1, c, 768 + core}, 12}
#define HR_PROCESSOR(c, core) {{1, 3, 6, 1, 2, 1, 25, 3, 3, 1, c, 768 + core}, 12}
#define ENT_PHYSICAL(c) {{1, 3, 6, 1, 2, 1, 47, 1, 1, 1, 1, c, 1}, 13}
#define ENT_LAST_CHANGE {{1, 3, 6, 1, 2, 1, 47, 1, 4, 1, 0}, 11}
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

    // HOST-RESOURCES-MIB; hrSystemDate is left out: the device has no clock
    {HR(1, 1), getHrSystemUptime},
    {HR(1, 5), getHrSystemNumUsers},
    {HR(1, 6), getHrSystemProcesses},
    {HR(1, 7), getHrSystemMaxProcesses},
    {HR(2, 2), getHrMemorySize},
    {HR_STORAGE(1, 1), getHrStorageIndex<1>},    {HR_STORAGE(1, 2), getHrStorageIndex<2>},
    {HR_STORAGE(2, 1), getHrStorageType<1>},     {HR_STORAGE(2, 2), getHrStorageType<2>},
    {HR_STORAGE(3, 1), getHrStorageDescr<1>},    {HR_STORAGE(3, 2), getHrStorageDescr<2>},
    {HR_STORAGE(4, 1), getHrStorageUnits<1>},    {HR_STORAGE(4, 2), getHrStorageUnits<2>},
    {HR_STORAGE(5, 1), getHrStorageSize<1>},     {HR_STORAGE(5, 2), getHrStorageSize<2>},
    {HR_STORAGE(6, 1), getHrStorageUsed<1>},     {HR_STORAGE(6, 2), getHrStorageUsed<2>},
    {HR_STORAGE(7, 1), getHrStorageFailures<1>}, {HR_STORAGE(7, 2), getHrStorageFailures<2>},
    {HR_DEVICE(1, 0), getHrDeviceIndex<0>},      {HR_DEVICE(1, 1), getHrDeviceIndex<1>},
    {HR_DEVICE(2, 0), getHrDeviceType},          {HR_DEVICE(2, 1), getHrDeviceType},
    {HR_DEVICE(3, 0), getHrDeviceDescr<0>},      {HR_DEVICE(3, 1), getHrDeviceDescr<1>},
    {HR_DEVICE(4, 0), getZeroDotZero},           {HR_DEVICE(4, 1), getZeroDotZero},  // hrDeviceID: unknown
    {HR_DEVICE(5, 0), getHrDeviceStatus},        {HR_DEVICE(5, 1), getHrDeviceStatus},
    {HR_DEVICE(6, 0), getHrDeviceErrors},        {HR_DEVICE(6, 1), getHrDeviceErrors},
    {HR_PROCESSOR(1, 0), getZeroDotZero},        {HR_PROCESSOR(1, 1), getZeroDotZero},  // hrProcessorFrwID
    {HR_PROCESSOR(2, 0), getHrProcessorLoad<0>}, {HR_PROCESSOR(2, 1), getHrProcessorLoad<1>},

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

    // ENTITY-MIB: entPhysicalTable, one row (the device), then entLastChangeTime
    {ENT_PHYSICAL(2), getEntDescr},
    {ENT_PHYSICAL(3), getEntVendorType},
    {ENT_PHYSICAL(4), getEntContainedIn},
    {ENT_PHYSICAL(5), getEntClass},
    {ENT_PHYSICAL(6), getEntParentRelPos},
    {ENT_PHYSICAL(7), getEntName},
    {ENT_PHYSICAL(8), getEntHardwareRev},
    {ENT_PHYSICAL(9), getEntFirmwareRev},
    {ENT_PHYSICAL(10), getEntEmpty},  // entPhysicalSoftwareRev
    {ENT_PHYSICAL(11), getEntSerialNum},
    {ENT_PHYSICAL(12), getEntMfgName},
    {ENT_PHYSICAL(13), getEntModelName},
    {ENT_PHYSICAL(14), getEntEmpty},  // entPhysicalAlias
    {ENT_PHYSICAL(15), getEntEmpty},  // entPhysicalAssetID
    {ENT_PHYSICAL(16), getEntIsFRU},
    {ENT_LAST_CHANGE, getEntLastChangeTime},

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
