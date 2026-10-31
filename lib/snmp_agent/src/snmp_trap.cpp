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

size_t snmpBuildMainsTrap(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                          uint8_t* out, size_t capacity) {
    SnmpTrap t;
    memset(&t, 0, sizeof(t));
    t.community = community;
    t.enterprise = enterpriseOid();
    memcpy(t.agentAddr, agentAddr, 4);
    t.generic = SnmpGenericTrap::EnterpriseSpecific;
    t.specific = (int32_t)(state.mainsPresent ? MainsTrap::PowerRestored : MainsTrap::PowerLost);
    t.timestamp = state.mainsLastChangeTicks;

    // mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange, read through the MIB
    for (uint32_t n = 1; n <= 3; n++) {
        Oid name = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, n, 0}, 9};
        const MibEntry* e = mibFind(name);
        if (e == nullptr) return 0;
        t.names[t.varbindCount] = name;
        t.values[t.varbindCount] = e->get(state);
        t.varbindCount++;
    }
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

size_t snmpBuildMainsTrapV2c(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                             int32_t requestId, uint8_t* out, size_t capacity) {
    uint32_t specific = (uint32_t)(state.mainsPresent ? MainsTrap::PowerRestored : MainsTrap::PowerLost);
    Oid trapOid = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, 0, specific}, 9};

    Oid names[3];
    SnmpValue values[3];
    for (uint32_t n = 1; n <= 3; n++) {
        names[n - 1] = {{1, 3, 6, 1, 4, 1, ENTERPRISE_NUMBER, n, 0}, 9};
        const MibEntry* e = mibFind(names[n - 1]);
        if (e == nullptr) return 0;
        values[n - 1] = e->get(state);
    }
    return buildV2Trap(community, agentAddr, requestId, state.mainsLastChangeTicks, trapOid, names, values, 3,
                       out, capacity);
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
