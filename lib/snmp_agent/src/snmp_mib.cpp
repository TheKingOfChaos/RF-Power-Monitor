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

// ---- interfaces (IF-MIB, RFC 2863), one Ethernet interface with ifIndex 1 ----

static const char* const IF_DESCR = "W5500 10/100 Ethernet";
static const char* const IF_NAME = "eth0";
static const int32_t IF_TYPE_ETHERNET_CSMACD = 6;
static const int32_t IF_MTU = 1500;

static SnmpValue integer(int32_t v) { return SnmpValue::makeInteger(v); }
static SnmpValue counter32(uint64_t v) { return SnmpValue::makeUnsigned(BerTag::Counter32, (uint32_t)v); }
static SnmpValue counter64(uint64_t v) { return SnmpValue::makeCounter64(v); }
static SnmpValue gauge(uint32_t v) { return SnmpValue::makeUnsigned(BerTag::Gauge32, v); }

static SnmpValue getIfNumber(const SnmpDeviceState&) { return integer(1); }
static SnmpValue getIfIndex(const SnmpDeviceState&) { return integer(1); }
static SnmpValue getIfDescr(const SnmpDeviceState&) { return SnmpValue::makeString(IF_DESCR); }
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

// ---- table, must be sorted by OID (checked by tests) ----

#define SYSTEM(n) {{1, 3, 6, 1, 2, 1, 1, n, 0}, 9}
#define IF_NUMBER {{1, 3, 6, 1, 2, 1, 2, 1, 0}, 9}
#define IF(n) {{1, 3, 6, 1, 2, 1, 2, 2, 1, n, 1}, 11}
#define IFX(n) {{1, 3, 6, 1, 2, 1, 31, 1, 1, 1, n, 1}, 12}
#define PRIVATE(n) {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, n, 0}, 9}

static const MibEntry MIB[] = {
    {SYSTEM(1), getSysDescr},
    {SYSTEM(2), getSysObjectId},
    {SYSTEM(3), getSysUpTime},
    {SYSTEM(4), getSysContact},
    {SYSTEM(5), getSysName},
    {SYSTEM(6), getSysLocation},
    {SYSTEM(7), getSysServices},

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
};

static const uint8_t MIB_SIZE = sizeof(MIB) / sizeof(MIB[0]);

uint8_t mibSize() {
    return MIB_SIZE;
}

const MibEntry& mibEntry(uint8_t index) {
    return MIB[index < MIB_SIZE ? index : MIB_SIZE - 1];
}

const MibEntry* mibFind(const Oid& oid) {
    for (uint8_t i = 0; i < MIB_SIZE; i++) {
        if (oidCompare(MIB[i].oid, oid) == 0) return &MIB[i];
    }
    return nullptr;
}

bool mibHasObject(const Oid& oid) {
    for (uint8_t i = 0; i < MIB_SIZE; i++) {
        // Scalars and the single table row: the object is the entry without its last arc (.0 or ifIndex)
        Oid object = MIB[i].oid;
        object.length--;
        if (oidStartsWith(oid, object)) return true;
    }
    return false;
}

const MibEntry* mibNext(const Oid& oid) {
    for (uint8_t i = 0; i < MIB_SIZE; i++) {
        if (oidCompare(MIB[i].oid, oid) > 0) return &MIB[i];
    }
    return nullptr;
}
