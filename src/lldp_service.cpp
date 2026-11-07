#include "lldp_service.h"
#include "syslog_service.h"

#include <Arduino.h>
#include <string.h>
#include <lwip/netif.h>
#include <lldp.h>
#include <snmp_mib.h>
#include "hostname.h"
#include "net_stats.h"
#include "snmp_service.h"

static netif* lldpNetif = nullptr;

// Written in lwIP context (input hook), read on core 0 under the driver lock; static, core 0's stack is small
static LldpNeighbour scratch;
static LldpNeighbourTable table;
static uint8_t lastFrame[512];  // the last LLDPDU received, for lldpdump
static size_t lastFrameLen = 0;
static bool linkWasUp = false;

// Owned by lldpPoll() on core 0
static LldpSchedule schedule;
static uint8_t frame[LLDP_FRAME_MAX];
static uint8_t sentFrame[LLDP_FRAME_MAX];
static size_t sentLen = 0;

void lldpBegin(netif* nif) {
    lldpNetif = nif;
}

void lldpInspectFrame(const uint8_t* f, size_t len) {
    if (lldpParseFrame(f, len, scratch) != LldpParse::Ok) return;
    table.received(scratch, millis());
    lastFrameLen = len < sizeof(lastFrame) ? len : sizeof(lastFrame);
    memcpy(lastFrame, f, lastFrameLen);
}

/// What we announce now: MAC, hostname, sysDescr, ifDescr and the addresses in use
static void localInfo(LldpLocal& l) {
    memset(&l, 0, sizeof(l));
    memcpy(l.mac, lldpNetif->hwaddr, 6);
    l.sysName = hostnameCurrent();
    l.sysDescr = snmpSysDescr();
    l.portDescr = SNMP_IF_DESCR;
    l.ttl = LLDP_TTL_S;
    DriverLock lock;  // addresses change in lwIP context
    uint32_t v4 = ip4_addr_get_u32(netif_ip4_addr(lldpNetif));
    if (v4 != 0) {
        l.hasIPv4 = true;
        memcpy(l.ipv4, &v4, 4);  // network order
    }
#if LWIP_IPV6
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES && !l.hasIPv6; i++) {
        const ip6_addr_t* a = netif_ip6_addr(lldpNetif, i);
        if (ip6_addr_isvalid(netif_ip6_addr_state(lldpNetif, i)) && !ip6_addr_islinklocal(a)) {
            l.hasIPv6 = true;
            memcpy(l.ipv6, a->addr, 16);  // network order
        }
    }
#endif
}

/// Syslog when the switch port changes: a new neighbour, another port, or none any more
static void logNeighbourChange() {
    static SnmpLldpState now;    // static: ~600 bytes (core 0 only)
    static LldpNeighbour last;
    static bool lastPresent = false;
    if (!lldpServiceSnmpState(now)) return;
    if (now.present && (!lastPresent || !lldpSameNeighbour(last, now.neighbour))) {
        syslogEvent(SyslogSeverity::Notice, "LLDP", "switch %s port %s",
                    now.neighbour.sysName[0] ? now.neighbour.sysName : now.neighbour.chassisId,
                    now.neighbour.portId);
        last = now.neighbour;
    } else if (!now.present && lastPresent) {
        syslogEvent(SyslogSeverity::Warning, "LLDP", "switch no longer seen");
    }
    lastPresent = now.present;
}

void lldpPoll() {
    static uint32_t lastPoll = 0;
    if (lldpNetif == nullptr || millis() - lastPoll < 100) return;
    lastPoll = millis();
    bool linkUp = netStatsPhy().linkUp;
    {
        DriverLock lock;
        table.expire(millis());
        if (!linkUp && linkWasUp) table.clear();  // plugged into another port or switch, perhaps
    }
    linkWasUp = linkUp;
    logNeighbourChange();

    if (!linkUp) {
        schedule.due(millis(), false, false);
        return;
    }
    LldpLocal l;
    localInfo(l);
    size_t len = lldpBuildFrame(l, frame, sizeof(frame));
    if (len == 0) return;
    bool changed = len != sentLen || memcmp(frame, sentFrame, len) != 0;
    if (!schedule.due(millis(), true, changed)) return;
    if (netStatsSendFrame(frame, len)) {
        memcpy(sentFrame, frame, len);
        sentLen = len;
    }
    schedule.sent(millis());  // also after a failed send: try again at the next interval, not every loop
}

void lldpServiceSwitchText(char* out, size_t cap) {
    DriverLock lock;
    lldpSwitchText(table, out, cap);
}

void lldpServiceSwitchAddress(char* out, size_t cap) {
    if (cap == 0) return;
    DriverLock lock;
    strlcpy(out, table.present() ? table.neighbour().mgmtAddr : "", cap);
}

void lldpDump() {
    static uint8_t copy[sizeof(lastFrame)];
    size_t len;
    {
        DriverLock lock;
        len = table.present() ? lastFrameLen : 0;
        memcpy(copy, lastFrame, len);
    }
    if (len == 0) {
        Serial.println("LLDP none");
        return;
    }
    Serial.print("LLDP ");
    for (size_t i = 0; i < len; i++) Serial.printf("%02x", copy[i]);
    Serial.println();
}

bool lldpServiceSnmpState(SnmpLldpState& out) {
    if (lldpNetif == nullptr) return false;
    DriverLock lock;
    memcpy(out.localMac, lldpNetif->hwaddr, 6);
    out.present = table.present();
    if (out.present) out.neighbour = table.neighbour();
    return true;
}
