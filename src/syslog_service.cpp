#include "syslog_service.h"

#include <Arduino.h>
#include <WiFiUdp.h>
#include <lwip/ip_addr.h>
#include <log_settings_store.h>
#include <netconfig.h>
#include <stdarg.h>

#include "hostname.h"
#include "net_stats.h"
#include "time_service.h"

namespace {

const uint32_t UNDATED_AFTER_MS = 120000;     // send without the time 2 minutes after the server is known
const uint32_t RETRY_MS = 1000;               // after a failed send
const uint32_t SUPPRESSED_REPORT_MS = 60000;  // how often a count of throttled access events goes out

SyslogQueue queue;
SyslogThrottle accessThrottle(5, 60000);
WiFiUDP sock;

bool typedValid = false;         // the typed server parsed (currentLogSettings.syslogMode == Typed)
SyslogTarget typedTarget;
bool targetKnown = false;        // a server was known at the last poll
uint32_t targetKnownMs = 0;      // since when
uint32_t lastFailMs = 0;
bool failedOnce = false;
uint32_t lastSuppressedReportMs = 0;
uint32_t sent = 0;

/// The server to send to now; false when there is none
bool currentTarget(SyslogTarget& out) {
    switch (currentLogSettings.syslogMode) {
    case ServerMode::Typed:
        if (!typedValid) return false;
        out = typedTarget;
        return true;
    case ServerMode::Dhcp: {
        uint8_t a[4];
        if (!netStatsDhcpLogServer(a)) return false;
        memset(&out, 0, sizeof(out));
        memcpy(out.addr, a, 4);
        out.port = SYSLOG_DEFAULT_PORT;
        return true;
    }
    case ServerMode::None:
        break;
    }
    return false;
}

IPAddress toIPAddress(const SyslogTarget& t) {
    if (!t.ipv6) return IPAddress(t.addr[0], t.addr[1], t.addr[2], t.addr[3]);
    uint32_t w[4];
    memcpy(w, t.addr, sizeof(w));  // ip6_addr_t words are in network order
    ip_addr_t a;
    IP_ADDR6(&a, w[0], w[1], w[2], w[3]);
    return IPAddress(a);
}

void targetText(const SyslogTarget& t, char* out, size_t size) {
    char host[IPV6_TEXT_MAX];
    if (t.ipv6) {
        formatIPv6(t.addr, host);
        snprintf(out, size, "[%s]:%u", host, t.port);
    } else {
        snprintf(out, size, "%u.%u.%u.%u:%u", t.addr[0], t.addr[1], t.addr[2], t.addr[3], t.port);
    }
}

void pushText(SyslogSeverity sev, const char* msgId, const char* text) {
    static SyslogEntry e;  // ~115 bytes: static (core 0 only)
    e.uptimeMs = millis();
    e.severity = sev;
    snprintf(e.msgId, sizeof(e.msgId), "%s", msgId);
    snprintf(e.text, sizeof(e.text), "%s", text);
    queue.push(e);
}

/// Send the oldest queued event; false if it could not go out
bool sendOldest(const SyslogTarget& target) {
    static SyslogEntry e;
    static char packet[SYSLOG_PACKET_MAX];
    if (!queue.peek(e)) return true;
    bool dated = timeServiceSynced();
    uint64_t epochMs = dated ? syslogEventEpochMs(e.uptimeMs, millis(), timeServiceEpochMs()) : 0;
    char text[SYSLOG_TEXT_MAX + 24];
    if (dated) {
        snprintf(text, sizeof(text), "%s", e.text);
    } else {
        snprintf(text, sizeof(text), "[uptime %lu s] %s", (unsigned long)(e.uptimeMs / 1000), e.text);
    }
    size_t len = syslogFormat(packet, sizeof(packet), e.severity, dated, epochMs, hostnameCurrent(), e.msgId, text);
    if (len == 0) {
        queue.pop();  // cannot be formatted: never will be
        return true;
    }
    bool ok = sock.beginPacket(toIPAddress(target), target.port) == 1 &&
              sock.write((const uint8_t*)packet, len) == len && sock.endPacket() == 1;
    if (ok) {
        queue.pop();
        sent++;
    }
    return ok;
}

}  // namespace

void syslogServiceApply() {
    typedValid = currentLogSettings.syslogMode == ServerMode::Typed &&
                 parseSyslogTarget(currentLogSettings.syslogServer, typedTarget);
    netStatsRequestDhcpLogServer(currentLogSettings.syslogMode == ServerMode::Dhcp);
    targetKnown = false;  // a new server: the undated wait starts again
}

void syslogServicePoll(bool netReady) {
    uint32_t now = millis();
    if (now - lastSuppressedReportMs >= SUPPRESSED_REPORT_MS) {
        lastSuppressedReportMs = now;
        uint32_t n = accessThrottle.takeSuppressed();
        if (n > 0) {
            char t[48];
            snprintf(t, sizeof(t), "%lu more access events not sent", (unsigned long)n);
            pushText(SyslogSeverity::Notice, "SNMP", t);
        }
    }
    uint32_t dropped = queue.takeDropped();
    if (dropped > 0) {
        char t[48];
        snprintf(t, sizeof(t), "%lu events dropped (queue full)", (unsigned long)dropped);
        pushText(SyslogSeverity::Notice, "SYSLOG", t);
    }

    SyslogTarget target;
    if (!netReady || !currentTarget(target)) {
        targetKnown = false;
        return;
    }
    if (!targetKnown) {
        targetKnown = true;
        targetKnownMs = now;
    }
    if (queue.size() == 0) return;
    if (!timeServiceSynced() && now - targetKnownMs < UNDATED_AFTER_MS) return;  // wait for the time
    if (failedOnce && now - lastFailMs < RETRY_MS) return;
    failedOnce = !sendOldest(target);
    if (failedOnce) lastFailMs = now;
}

void syslogServiceFlush() {
    SyslogTarget target;
    if (!currentTarget(target)) return;
    for (uint8_t i = SyslogQueue::CAPACITY; i > 0 && queue.size() > 0; i--) {
        if (!sendOldest(target)) return;
    }
}

void syslogEvent(SyslogSeverity sev, const char* msgId, const char* fmt, ...) {
    char text[SYSLOG_TEXT_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pushText(sev, msgId, text);
}

void syslogAccessEvent(const SourceKey& source, AccessEvent event) {
    if (!accessThrottle.allow(millis())) return;
    char ip[SOURCE_TEXT_MAX];
    formatSource(source, ip);
    bool web = event == AccessEvent::WebLoginFailed || event == AccessEvent::WebLoginLocked ||
               event == AccessEvent::WebUploadRefused;
    syslogEvent(SyslogSeverity::Warning, web ? "WEB" : "SNMP", "%s from %s", accessEventName(event), ip);
}

void syslogServiceStatusText(char* out, size_t size) {
    switch (currentLogSettings.syslogMode) {
    case ServerMode::None:
        snprintf(out, size, "off");
        return;
    case ServerMode::Typed:
        if (!typedValid) {
            snprintf(out, size, "invalid setting");
            return;
        }
        break;
    case ServerMode::Dhcp:
        break;
    }
    SyslogTarget t;
    if (!currentTarget(t)) {
        snprintf(out, size, "DHCP, none offered (%u waiting)", queue.size());
        return;
    }
    char where[64];
    targetText(t, where, sizeof(where));
    snprintf(out, size, "%s (%s), %lu sent, %u waiting", where,
             currentLogSettings.syslogMode == ServerMode::Dhcp ? "from DHCP" : "typed", (unsigned long)sent,
             queue.size());
}
