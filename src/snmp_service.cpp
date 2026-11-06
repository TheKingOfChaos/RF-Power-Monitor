#include "snmp_service.h"

#include <Arduino.h>
#include <WiFiUdp.h>
#include <mains_alarm.h>
#include <mains_input.h>
#include <netconfig.h>
#include <settings.h>
#include <snmp_trap.h>
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "fw_update_service.h"
#include "host_stats.h"
#include "hostname.h"
#include "lldp_service.h"
#include "net_stats.h"
#include "source_key.h"
#include "version.h"

// Requests handled per snmpPoll() call, so a flood cannot starve the menu
const uint8_t MAX_PACKETS_PER_POLL = 4;

static const char* const SYS_DESCR = "RF-Power-Monitor " FIRMWARE_VERSION " mains power monitor (RP2040, W5500)";

const char* snmpSysDescr() {
    return SYS_DESCR;
}

static WiFiUDP udp;
static bool started = false;
static bool coldStartSent = false;
static uint8_t agentAddr[4] = {0, 0, 0, 0};
static FwTrapTiming fwTrapTiming = {};  ///< Core 0 only: snmpNetworkUp/Down() and snmpPollFirmwareTrap()
static MainsAlarm mainsAlarm;  ///< Core 0 only: snmpPollMainsAlarm() and captureState()

static SnmpAgent agent(currentSettings.community);
static RateLimiter limiter(SNMP_RATE_PER_SECOND, SNMP_RATE_BURST, SNMP_GLOBAL_RATE_PER_SECOND,
                           SNMP_GLOBAL_RATE_BURST);
static AccessLog accessLog;
static TrapThrottle authTrapThrottle(AUTH_TRAP_INTERVAL_MS);
static SnmpServiceStats serviceStats = {};

static uint8_t inBuf[SNMP_MAX_MESSAGE];
static uint8_t outBuf[SNMP_MAX_MESSAGE];
/// Largest reply over IPv6: 1500 - 40 (IPv6) - 8 (UDP); IPv4 allows SNMP_MAX_MESSAGE (1472)
static const size_t SNMP_MAX_MESSAGE_IPV6 = 1452;

/**
 * @brief sysUpTime in hundredths of a second, from the 64-bit microsecond timer
 *
 * Wraps after 497 days like any TimeTicks counter, not after millis()' 49 days.
 */
static uint32_t uptimeTicks() {
    return (uint32_t)(time_us_64() / 10000);
}

/**
 * @brief Collect the values the MIB reports
 */
static SnmpDeviceState captureState() {
    SnmpDeviceState s = {};
    s.uptimeTicks = uptimeTicks();
    // Read from the settings on every request, so menu changes apply immediately
    s.sysDescr = SYS_DESCR;
    s.sysContact = currentSettings.sysContact;
    s.sysName = hostnameCurrent();  // the name in use: the setting, or RF-Power-<MAC tail> while it is not set
    s.sysLocation = currentSettings.sysLocation;
    netStatsCapture(s.iface);
    s.cpuTempDeciC = cpuTempDeciC();
    s.ambientTempValid = ambientTempValid();
    s.ambientTempDeciC = ambientTempDeciC();

    MainsSnapshot mains;
    if (mainsRead(mains)) {
        s.mainsPresent = mains.present;
        s.mainsChanges = mains.changes;
        if (mains.changes > 0) {
            // sysUpTime at the change, from the elapsed time (wrap-safe for 49 days)
            uint32_t agoTicks = (millis() - mains.lastChangeMs) / 10;
            s.mainsLastChangeTicks = s.uptimeTicks - agoTicks;
        }
    }
    s.mainsAlarm = mainsAlarm.raised();
    FwSnmpState fw;
    fwUpdateSnmpState(fw);
    s.fw.runningSlot = fw.runningSlot;
    s.fw.trial = fw.trial;
    memcpy(s.fw.slotVersion, fw.slotVersion, sizeof(s.fw.slotVersion));
    s.fw.lastResult = (uint8_t)fw.lastResult;
    // LLDP-MIB: a static copy (core 0 only), so the response never reads the table lwIP updates
    static SnmpLldpState lldp;
    if (lldpServiceSnmpState(lldp)) s.lldp = &lldp;
    // snmp group: the agent's live counters, so snmpInPkts includes the request being answered
    s.snmp = &agent.stats();
    s.snmpOutTraps = serviceStats.trapsSent;
    s.host = &hostStatsSnapshot();
    static SnmpProtoCounters proto;  // ~250 bytes: static (core 0 only), not on the stack
    netStatsCaptureProto(proto);
    s.proto = &proto;
    static SnmpIpState ip;  // ~110 bytes: static (core 0 only)
    netStatsCaptureAddresses(ip);
    s.ip = &ip;
    return s;
}

/**
 * @brief Record a rejected request and print it when it starts a new log entry
 */
static void logAccess(const SourceKey& source, AccessEvent event) {
    if (accessLog.record(source, event, millis())) {
        char ip[SOURCE_TEXT_MAX];
        formatSource(source, ip);
        bool web = event == AccessEvent::WebLoginFailed || event == AccessEvent::WebLoginLocked ||
                   event == AccessEvent::WebUploadRefused;
        Serial.printf("\r\n%s access: %s from %s\r\n", web ? "Web" : "SNMP", accessEventName(event), ip);
    }
}

void accessLogRecord(const SourceKey& source, AccessEvent event) {
    logAccess(source, event);
}

/// request-id of the next v2c trap
static int32_t nextTrapRequestId = 1;

enum class TrapKind { ColdStart, AuthFailure, Mains, MainsAlarm, Firmware };

/**
 * @brief Encode a trap in the configured version (v1 Trap-PDU or v2c SNMPv2-Trap)
 */
static size_t buildTrap(TrapKind kind, const SnmpDeviceState& state) {
    const char* c = currentSettings.community;
    if (currentSettings.trapVersion == TRAP_VERSION_V1) {
        switch (kind) {
        case TrapKind::ColdStart:   return snmpBuildColdStartTrap(c, agentAddr, state.uptimeTicks, outBuf, sizeof(outBuf));
        case TrapKind::AuthFailure: return snmpBuildAuthFailureTrap(c, agentAddr, state.uptimeTicks, outBuf, sizeof(outBuf));
        case TrapKind::Mains:       return snmpBuildMainsTrap(c, agentAddr, state, outBuf, sizeof(outBuf));
        case TrapKind::MainsAlarm:  return snmpBuildMainsAlarmTrap(c, agentAddr, state, outBuf, sizeof(outBuf));
        case TrapKind::Firmware:    return snmpBuildFirmwareTrap(c, agentAddr, state, outBuf, sizeof(outBuf));
        }
        return 0;
    }
    int32_t id = nextTrapRequestId++;
    switch (kind) {
    case TrapKind::ColdStart:   return snmpBuildColdStartTrapV2c(c, agentAddr, state.uptimeTicks, id, outBuf, sizeof(outBuf));
    case TrapKind::AuthFailure: return snmpBuildAuthFailureTrapV2c(c, agentAddr, state.uptimeTicks, id, outBuf, sizeof(outBuf));
    case TrapKind::Mains:       return snmpBuildMainsTrapV2c(c, agentAddr, state, id, outBuf, sizeof(outBuf));
    case TrapKind::MainsAlarm:  return snmpBuildMainsAlarmTrapV2c(c, agentAddr, state, id, outBuf, sizeof(outBuf));
    case TrapKind::Firmware:    return snmpBuildFirmwareTrapV2c(c, agentAddr, state, id, outBuf, sizeof(outBuf));
    }
    return 0;
}

bool snmpTrapTarget(TrapAddress& out) {
    if (!currentSettings.trapFromDhcp) {
        // The settings were validated when loaded, so an IPv6 address here comes with v2c
        return currentSettings.trapTarget[0] != '\0' &&
               parseTrapTarget(currentSettings.trapTarget, out) == TrapTargetError::None;
    }
    // Only the ACK for the lease in use counts; a static IP never has one
    DhcpTrapInfo d = netStatsDhcpTrap();
    if (!currentSettings.dhcpEnabled || !d.seen || !d.hasOption || memcmp(d.yiaddr, agentAddr, 4) != 0) return false;
    out.ipv6 = false;
    memcpy(out.addr, d.target, 4);
    return true;
}

/**
 * @brief lwIP/Arduino address for a trap destination
 */
static IPAddress trapDestination(const TrapAddress& t) {
    if (!t.ipv6) return IPAddress(t.addr[0], t.addr[1], t.addr[2], t.addr[3]);
    uint32_t w[4];
    memcpy(w, t.addr, sizeof(w));  // ip6_addr_t words are in network order
    ip_addr_t a;
    IP_ADDR6(&a, w[0], w[1], w[2], w[3]);  // global or ULA only (parseTrapTarget), so no zone
    return IPAddress(a);
}

/**
 * @brief Send an encoded trap to the destination in use, if any
 * @return true if the packet was handed to the stack
 */
static bool sendTrap(size_t len, const char* what) {
    TrapAddress target;
    if (!snmpTrapTarget(target)) return false;
    char targetText[IPV6_TEXT_MAX];
    formatTrapAddress(target, targetText);
    if (len == 0) {
        serviceStats.trapsFailed++;
        return false;
    }
    IPAddress dest = trapDestination(target);
    bool ok = udp.beginPacket(dest, SNMP_TRAP_PORT) == 1 && udp.write(outBuf, len) == len && udp.endPacket() == 1;
    if (ok) {
        serviceStats.trapsSent++;
        Serial.printf("SNMP %s trap %s sent to %s\r\n", currentSettings.trapVersion == TRAP_VERSION_V1 ? "v1" : "v2c",
                      what, targetText);
    } else {
        serviceStats.trapsFailed++;
        Serial.printf("SNMP trap %s to %s failed\r\n", what, targetText);
    }
    return ok;
}

bool snmpBegin() {
    limiter.configureTrusted(SNMP_TRUSTED_RATE_PER_SECOND, SNMP_TRUSTED_RATE_BURST, SNMP_MAX_TRUSTED_SOURCES,
                             SNMP_TRUST_TIMEOUT_MS, SNMP_TRUSTED_SOURCE_BURST, SNMP_TRUSTED_SOURCE_RATE);
    started = udp.begin(SNMP_PORT) == 1;
    Serial.printf(started ? "SNMP agent listening on UDP %u\r\n" : "SNMP: cannot bind UDP %u\r\n", SNMP_PORT);
    return started;
}

void snmpNetworkUp(const IPAddress& localIP) {
    for (int i = 0; i < 4; i++) agentAddr[i] = localIP[i];
    fwTrapTiming = {true, millis(), false, 0};  // a held firmware trap follows coldStart by FW_TRAP_SETTLE_MS
    if (!started || coldStartSent) return;
    coldStartSent = true;
    sendTrap(buildTrap(TrapKind::ColdStart, captureState()), "coldStart");
}

void snmpNetworkDown() {
    fwTrapTiming = {};
    memset(agentAddr, 0, sizeof(agentAddr));
}

void snmpCommunityChanged() {
    // Trust was earned with the old community and must not carry over
    limiter.clearTrust();
    Serial.println("SNMP: community changed, rate-limit trust cleared");
}

void snmpNotifyMainsChange() {
    if (!started) return;
    SnmpDeviceState state = captureState();
    sendTrap(buildTrap(TrapKind::Mains, state), state.mainsPresent ? "mainsPowerRestored" : "mainsPowerLost");
}

void snmpPollMainsAlarm() {
    MainsSnapshot mains;
    if (!mainsRead(mains)) return;
    bool wasRaised = mainsAlarm.raised();
    mainsAlarm.update(mains.present, mains.changes, mains.lastChangeMs, millis());
    if (mainsAlarm.raised() && !wasRaised) Serial.println("\r\nMains power lost for 60 s: alarm raised");
    if (!mainsAlarm.trapPending()) return;

    // Held until a trap can actually leave (trapCanLeave). One attempt, as for every trap.
    static const uint8_t NO_ADDRESS[4] = {0, 0, 0, 0};
    TrapAddress target;
    TrapPath path;
    path.agentStarted = started;
    path.linkUp = netStatsPhy().linkUp;
    path.hasIPv4 = memcmp(agentAddr, NO_ADDRESS, 4) != 0;  // cleared by snmpNetworkDown()
    path.hasIPv6 = netStatsHasRoutableIPv6();
    path.targetKnown = snmpTrapTarget(target);
    path.targetIPv6 = path.targetKnown && target.ipv6;
    if (!trapCanLeave(path)) return;
    sendTrap(buildTrap(TrapKind::MainsAlarm, captureState()), "mainsPowerLostTooLong");
    mainsAlarm.trapSent();
}

void snmpPollFirmwareTrap() {
    FwResult result;
    if (!fwUpdatePendingTrap(result)) return;
    if (result == FwResult::None) {  // nothing to report (the builders make no trap for it): do not keep asking
        fwUpdateTrapSent();
        return;
    }
    // Held until coldStart is out (lwIP queues one packet per ARP entry) and a trap can leave now (trapCanLeave);
    // it stays pending until a send succeeds and is retried every FW_TRAP_RETRY_MS
    uint32_t now = millis();
    if (!fwTrapDue(fwTrapTiming, now)) return;
    static const uint8_t NO_ADDRESS[4] = {0, 0, 0, 0};
    TrapAddress target;
    TrapPath path;
    path.agentStarted = started;
    path.linkUp = netStatsPhy().linkUp;
    path.hasIPv4 = memcmp(agentAddr, NO_ADDRESS, 4) != 0;
    path.hasIPv6 = netStatsHasRoutableIPv6();
    path.targetKnown = snmpTrapTarget(target);
    path.targetIPv6 = path.targetKnown && target.ipv6;
    if (!trapCanLeave(path)) return;
    fwTrapTiming.attempted = true;
    fwTrapTiming.lastAttemptMs = now;
    if (sendTrap(buildTrap(TrapKind::Firmware, captureState()),
                 result == FwResult::Installed ? "firmwareUpdateKept" : "firmwareRolledBack")) {
        fwUpdateTrapSent();
    }
}

bool snmpMainsAlarm() {
    return mainsAlarm.raised();
}

void snmpPoll() {
    if (!started) return;

    for (uint8_t i = 0; i < MAX_PACKETS_PER_POLL; i++) {
        int size = udp.parsePacket();
        if (size <= 0) return;

        SourceKey source;
        if (!sourceKeyOf(udp.remoteIP(), source)) {
            udp.flush();  // IPv6 source in ::/32: cannot be a real LAN host
            continue;
        }
        if ((size_t)size > sizeof(inBuf)) {
            udp.flush();  // Larger than any valid SNMP v1 request we accept
            logAccess(source, AccessEvent::Malformed);
            continue;
        }
        if (!limiter.allow(source, millis())) {
            udp.flush();
            serviceStats.rateLimited++;
            logAccess(source, AccessEvent::RateLimited);
            continue;
        }

        int n = udp.read(inBuf, sizeof(inBuf));
        if (n <= 0) continue;

        IPAddress remote = udp.remoteIP();
        uint16_t remotePort = udp.remotePort();
        // One Ethernet frame without fragmentation: IPv6 headers are 20 bytes longer than IPv4's
        // (and IPv6 fragmentation is off, see platformio.ini)
        size_t outMax = remote.isV6() ? SNMP_MAX_MESSAGE_IPV6 : sizeof(outBuf);
        size_t len = agent.handle(inBuf, (size_t)n, outBuf, outMax, captureState());

        switch (agent.lastOutcome()) {
        case SnmpOutcome::Responded:
        case SnmpOutcome::NoRoom:
            // Right community: exempt from the shared bucket, so spoofed floods cannot starve it
            limiter.markTrusted(source);
            fwUpdateNoteValidRequest();  // reachable: a firmware on trial is kept
            break;
        case SnmpOutcome::RespondedBulk:
            // A GetBulk with a spoofed source must not give that address the larger trusted budget
            fwUpdateNoteValidRequest();
            break;
        case SnmpOutcome::BadCommunity:
            logAccess(source, AccessEvent::BadCommunity);
            if (authTrapThrottle.allow(millis())) {
                sendTrap(buildTrap(TrapKind::AuthFailure, captureState()), "authenticationFailure");
            }
            break;
        case SnmpOutcome::BadVersion:  logAccess(source, AccessEvent::BadVersion); break;
        case SnmpOutcome::ParseError:  logAccess(source, AccessEvent::Malformed); break;
        case SnmpOutcome::SetRejected:
            limiter.markTrusted(source);
            logAccess(source, AccessEvent::SetRejected);
            break;
        default: break;
        }
        if (len == 0) continue;

        udp.beginPacket(remote, remotePort);
        udp.write(outBuf, len);
        udp.endPacket();
    }
}

uint32_t snmpUptimeTicks() {
    return uptimeTicks();
}

const SnmpStats& snmpStats() {
    return agent.stats();
}

const SnmpServiceStats& snmpServiceStats() {
    return serviceStats;
}

const AccessLog& snmpAccessLog() {
    return accessLog;
}
