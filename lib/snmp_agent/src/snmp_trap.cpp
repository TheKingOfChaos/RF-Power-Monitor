#include "snmp_trap.h"

#include <string.h>

/// sysObjectID / enterprise of this device: 1.3.6.1.4.1.63050
static Oid enterpriseOid() {
    Oid o = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER}, 7};
    return o;
}

size_t snmpBuildTrap(const SnmpTrap& trap, uint8_t* out, size_t capacity) {
    if (trap.community == nullptr || trap.varbindCount > SNMP_TRAP_MAX_VARBINDS) return 0;

    BerWriter w(out, capacity);
    w.beginSequence();
    w.writeInteger(SNMP_VERSION_1);
    w.writeOctetString(trap.community);
    w.beginSequence(BerTag::Trap);
    w.writeOid(trap.enterprise);
    w.writeIpAddress(trap.agentAddr);
    w.writeInteger((int32_t)trap.generic);
    w.writeInteger(trap.specific);
    w.writeUnsigned(BerTag::TimeTicks, trap.timestamp);
    w.beginSequence();
    for (uint8_t i = 0; i < trap.varbindCount; i++) {
        w.beginSequence();
        w.writeOid(trap.names[i]);
        if (!snmpWriteValue(w, trap.values[i])) return 0;
        w.endSequence();
    }
    w.endSequence();
    w.endSequence();
    w.endSequence();
    return w.failed() || w.hasOpenSequence() ? 0 : w.length();
}

/// A trap with no variable bindings
static size_t buildGenericTrap(SnmpGenericTrap generic, const char* community, const uint8_t agentAddr[4],
                               uint32_t uptimeTicks, uint8_t* out, size_t capacity) {
    SnmpTrap t;
    memset(&t, 0, sizeof(t));
    t.community = community;
    t.enterprise = enterpriseOid();
    memcpy(t.agentAddr, agentAddr, 4);
    t.generic = generic;
    t.specific = 0;
    t.timestamp = uptimeTicks;
    return snmpBuildTrap(t, out, capacity);
}

size_t snmpBuildColdStartTrap(const char* community, const uint8_t agentAddr[4], uint32_t uptimeTicks,
                              uint8_t* out, size_t capacity) {
    return buildGenericTrap(SnmpGenericTrap::ColdStart, community, agentAddr, uptimeTicks, out, capacity);
}

size_t snmpBuildAuthFailureTrap(const char* community, const uint8_t agentAddr[4], uint32_t uptimeTicks,
                                uint8_t* out, size_t capacity) {
    return buildGenericTrap(SnmpGenericTrap::AuthenticationFailure, community, agentAddr, uptimeTicks, out,
                            capacity);
}

/// mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange and, for the alarm trap, mainsPowerAlarm
static const uint32_t MAINS_OBJECTS[] = {1, 2, 3, 6};

/**
 * @brief Read the first count mains objects through the MIB
 * @return false if one is missing from the MIB
 */
static bool mainsVarbinds(const SnmpDeviceState& state, uint8_t count, Oid* names, SnmpValue* values) {
    for (uint8_t i = 0; i < count; i++) {
        names[i] = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, MAINS_OBJECTS[i], 0}, 9};
        static MibObject found;  // ~300 bytes: off the stack (traps are built on core 0 only)
        if (!mibGet(names[i], state, found)) return false;
        values[i] = found.value;
    }
    return true;
}

/// An enterprise-specific mains trap with the first count mains objects, stamped with the last change
static size_t buildMainsTrap(MainsTrap specific, uint8_t count, const char* community, const uint8_t agentAddr[4],
                             const SnmpDeviceState& state, uint8_t* out, size_t capacity) {
    SnmpTrap t;
    memset(&t, 0, sizeof(t));
    t.community = community;
    t.enterprise = enterpriseOid();
    memcpy(t.agentAddr, agentAddr, 4);
    t.generic = SnmpGenericTrap::EnterpriseSpecific;
    t.specific = (int32_t)specific;
    t.timestamp = state.mainsLastChangeTicks;
    if (!mainsVarbinds(state, count, t.names, t.values)) return 0;
    t.varbindCount = count;
    return snmpBuildTrap(t, out, capacity);
}

size_t snmpBuildMainsTrap(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                          uint8_t* out, size_t capacity) {
    MainsTrap specific = state.mainsPresent ? MainsTrap::PowerRestored : MainsTrap::PowerLost;
    return buildMainsTrap(specific, 3, community, agentAddr, state, out, capacity);
}

size_t snmpBuildMainsAlarmTrap(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                               uint8_t* out, size_t capacity) {
    return buildMainsTrap(MainsTrap::PowerLostTooLong, 4, community, agentAddr, state, out, capacity);
}

/// fwRunningSlot, fwSlotAVersion, fwSlotBVersion, fwLastResult: 63050.7.<n>.0
static const uint32_t FW_OBJECTS[] = {1, 2, 3, 5};

/**
 * @brief Read the four firmware objects through the MIB
 * @return false if one is missing from the MIB
 */
static bool fwVarbinds(const SnmpDeviceState& state, Oid* names, SnmpValue* values) {
    for (uint8_t i = 0; i < 4; i++) {
        names[i] = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, 7, FW_OBJECTS[i], 0}, 10};
        static MibObject found;  // ~300 bytes: off the stack (traps are built on core 0 only)
        if (!mibGet(names[i], state, found)) return false;
        values[i] = found.value;
    }
    return true;
}

/// lastResult 1 is installed: the update was kept; every other result is a way back to the old firmware
static FwTrap fwTrapKind(const SnmpDeviceState& state) {
    return state.fw.lastResult == 1 ? FwTrap::Kept : FwTrap::RolledBack;
}

size_t snmpBuildFirmwareTrap(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                             uint8_t* out, size_t capacity) {
    SnmpTrap t;
    memset(&t, 0, sizeof(t));
    t.community = community;
    t.enterprise = enterpriseOid();
    memcpy(t.agentAddr, agentAddr, 4);
    t.generic = SnmpGenericTrap::EnterpriseSpecific;
    t.specific = (int32_t)fwTrapKind(state);
    t.timestamp = state.uptimeTicks;
    if (!fwVarbinds(state, t.names, t.values)) return 0;
    t.varbindCount = 4;
    return snmpBuildTrap(t, out, capacity);
}

// ---- v2c ----

/**
 * @brief Encode an SNMPv2-Trap: sysUpTime.0, snmpTrapOID.0, the given varbinds, then
 *        snmpTrapAddress.0 = agentAddr unless agentAddr is 0.0.0.0
 */
static size_t buildV2Trap(const char* community, const uint8_t agentAddr[4], int32_t requestId,
                          uint32_t uptimeTicks, const Oid& trapOid, const Oid* names, const SnmpValue* values,
                          uint8_t count, uint8_t* out, size_t capacity) {
    if (community == nullptr) return 0;
    static const Oid SYS_UPTIME = {{1, 3, 6, 1, 2, 1, 1, 3, 0}, 9};
    static const Oid SNMP_TRAP_OID = {{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0}, 11};
    static const Oid SNMP_TRAP_ADDRESS = {{1, 3, 6, 1, 6, 3, 18, 1, 3, 0}, 10};
    static const uint8_t NO_ADDRESS[4] = {0, 0, 0, 0};

    BerWriter w(out, capacity);
    w.beginSequence();
    w.writeInteger(SNMP_VERSION_2C);
    w.writeOctetString(community);
    w.beginSequence(BerTag::SNMPv2Trap);
    w.writeInteger(requestId);
    w.writeInteger(0);
    w.writeInteger(0);
    w.beginSequence();
    w.beginSequence();
    w.writeOid(SYS_UPTIME);
    w.writeUnsigned(BerTag::TimeTicks, uptimeTicks);
    w.endSequence();
    w.beginSequence();
    w.writeOid(SNMP_TRAP_OID);
    w.writeOid(trapOid);
    w.endSequence();
    for (uint8_t i = 0; i < count; i++) {
        w.beginSequence();
        w.writeOid(names[i]);
        if (!snmpWriteValue(w, values[i])) return 0;
        w.endSequence();
    }
    if (memcmp(agentAddr, NO_ADDRESS, 4) != 0) {
        w.beginSequence();
        w.writeOid(SNMP_TRAP_ADDRESS);
        w.writeIpAddress(agentAddr);
        w.endSequence();
    }
    w.endSequence();
    w.endSequence();
    w.endSequence();
    return w.failed() || w.hasOpenSequence() ? 0 : w.length();
}

size_t snmpBuildColdStartTrapV2c(const char* community, const uint8_t agentAddr[4], uint32_t uptimeTicks,
                                 int32_t requestId, uint8_t* out, size_t capacity) {
    static const Oid COLD_START = {{1, 3, 6, 1, 6, 3, 1, 1, 5, 1}, 10};
    return buildV2Trap(community, agentAddr, requestId, uptimeTicks, COLD_START, nullptr, nullptr, 0, out,
                       capacity);
}

size_t snmpBuildAuthFailureTrapV2c(const char* community, const uint8_t agentAddr[4], uint32_t uptimeTicks,
                                   int32_t requestId, uint8_t* out, size_t capacity) {
    static const Oid AUTH_FAILURE = {{1, 3, 6, 1, 6, 3, 1, 1, 5, 5}, 10};
    return buildV2Trap(community, agentAddr, requestId, uptimeTicks, AUTH_FAILURE, nullptr, nullptr, 0, out,
                       capacity);
}

/// A v2c mains trap (63050.0.specific) with the first count mains objects
static size_t buildMainsTrapV2c(MainsTrap specific, uint8_t count, const char* community,
                                const uint8_t agentAddr[4], const SnmpDeviceState& state, int32_t requestId,
                                uint8_t* out, size_t capacity) {
    Oid trapOid = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, 0, (uint32_t)specific}, 9};
    Oid names[4];
    SnmpValue values[4];
    if (!mainsVarbinds(state, count, names, values)) return 0;
    return buildV2Trap(community, agentAddr, requestId, state.mainsLastChangeTicks, trapOid, names, values, count,
                       out, capacity);
}

size_t snmpBuildMainsTrapV2c(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                             int32_t requestId, uint8_t* out, size_t capacity) {
    MainsTrap specific = state.mainsPresent ? MainsTrap::PowerRestored : MainsTrap::PowerLost;
    return buildMainsTrapV2c(specific, 3, community, agentAddr, state, requestId, out, capacity);
}

size_t snmpBuildMainsAlarmTrapV2c(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                                  int32_t requestId, uint8_t* out, size_t capacity) {
    return buildMainsTrapV2c(MainsTrap::PowerLostTooLong, 4, community, agentAddr, state, requestId, out,
                             capacity);
}

size_t snmpBuildFirmwareTrapV2c(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                                int32_t requestId, uint8_t* out, size_t capacity) {
    Oid trapOid = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, 0, (uint32_t)fwTrapKind(state)}, 9};
    Oid names[4];
    SnmpValue values[4];
    if (!fwVarbinds(state, names, values)) return 0;
    return buildV2Trap(community, agentAddr, requestId, state.uptimeTicks, trapOid, names, values, 4, out, capacity);
}

bool trapCanLeave(const TrapPath& p) {
    return p.agentStarted && p.linkUp && p.targetKnown && (p.targetIPv6 ? p.hasIPv6 : p.hasIPv4);
}

bool TrapThrottle::allow(uint32_t nowMs) {
    if (_sentOnce && nowMs - _lastMs < _intervalMs) {
        _suppressed++;
        return false;
    }
    _sentOnce = true;
    _lastMs = nowMs;
    _suppressed = 0;
    return true;
}
