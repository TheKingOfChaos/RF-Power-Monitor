#pragma once

/**
 * @file syslog_service.h
 * @brief Events to a syslog server (RFC 5424 over UDP)
 *
 * Events wait in a queue (lib/syslog SyslogQueue, the last 16) until there is
 * a network, a server (typed, or DHCP option 7) and the time from NTP; then
 * they go out one per poll, dated from when they happened. Without the time
 * after 2 minutes they go out undated, with the uptime in the text. Core 0 only.
 */

#include <access_control.h>
#include <stddef.h>
#include <syslog_message.h>

/// Apply currentLogSettings (typed server, DHCP option 7 request, or off)
void syslogServiceApply();

/// Send what is due; call every loop pass
void syslogServicePoll(bool netReady);

/**
 * @brief Queue an event (printf-style text, cut at SYSLOG_TEXT_MAX)
 * @param msgId MAINS, BOOT, FIRMWARE, WEB, SETTINGS, SNMP, NETWORK, LLDP or UPDATE
 *
 * Safe before the network is up. Never pass passwords, keys or the community.
 */
void syslogEvent(SyslogSeverity sev, const char* msgId, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/// A rejected SNMP or web request (access log), at most 5 a minute plus a count of the rest
void syslogAccessEvent(const SourceKey& source, AccessEvent event);

/// Before a planned restart: send what is queued now (at most a few ms per event)
void syslogServiceFlush();

/// Status line text, e.g. "10.132.0.45:514 (from DHCP), 12 sent" or "off"
void syslogServiceStatusText(char* out, size_t size);
