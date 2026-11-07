// Syslog messages (RFC 5424 over UDP), the queue events wait in until they can
// be sent, and the throttle that keeps an SNMP request flood from becoming a
// syslog flood. Pure C++, host tested.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum class SyslogSeverity : uint8_t { Critical = 2, Error = 3, Warning = 4, Notice = 5, Info = 6 };

static const uint8_t SYSLOG_FACILITY_DAEMON = 3;
static const size_t SYSLOG_TEXT_MAX = 100;
static const size_t SYSLOG_PACKET_MAX = 256;

/**
 * @brief One RFC 5424 message: "<PRI>1 TIMESTAMP HOST rfpower - MSGID - TEXT"
 *
 * PRI = facility daemon * 8 + severity. TIMESTAMP is UTC with milliseconds, or
 * "-" when not dated; an empty hostname or MSGID is "-". The text is cut at
 * SYSLOG_TEXT_MAX characters, control characters become '?'.
 * @return The length, or 0 when it does not fit `size`
 */
size_t syslogFormat(char* out, size_t size, SyslogSeverity sev, bool dated, uint64_t epochMs, const char* hostname,
                    const char* msgId, const char* text);

/// An event waiting to be sent: when it happened (millis()) and what
struct SyslogEntry {
    uint32_t uptimeMs;
    SyslogSeverity severity;
    char msgId[9];
    char text[SYSLOG_TEXT_MAX + 1];
};

/// The last CAPACITY events, oldest first; a full queue drops its oldest
class SyslogQueue {
public:
    static const uint8_t CAPACITY = 16;
    /// Add an event (dropping the oldest when full); always true
    bool push(const SyslogEntry& e);
    /// The oldest event; false when empty
    bool peek(SyslogEntry& out) const;
    /// Remove the oldest event
    void pop();
    uint8_t size() const { return _count; }
    /// Events dropped since the last call (and reset the count)
    uint32_t takeDropped();
    /**
     * @brief What to send next: "N events dropped (queue full)" (MSGID SYSLOG, dated nowMs) when
     *        events were dropped, else the oldest event; false when there is nothing
     *
     * Never changes the queue, so it can be asked on every pass while nothing can be sent.
     */
    bool peekNext(SyslogEntry& out, uint32_t nowMs) const;
    /// What peekNext() returned went out: clear the drop count, or remove the oldest event
    void popNext();

private:
    SyslogEntry _entries[CAPACITY];
    uint8_t _head = 0;
    uint8_t _count = 0;
    uint32_t _dropped = 0;
};

/// At most `perWindow` events per `windowMs`; counts the ones it refused
class SyslogThrottle {
public:
    SyslogThrottle(uint8_t perWindow, uint32_t windowMs) : _perWindow(perWindow), _windowMs(windowMs) {}
    /// true if an event may go out now (a new window starts when the old one has passed)
    bool allow(uint32_t nowMs);
    /// Events refused since the last call (and reset the count)
    uint32_t takeSuppressed();

private:
    uint8_t _perWindow;
    uint32_t _windowMs;
    bool _started = false;
    uint32_t _windowStartMs = 0;
    uint8_t _used = 0;
    uint32_t _suppressed = 0;
};

/// Real time of an event that happened at `eventUptimeMs`, given the clock now (millis() wrap-safe)
uint64_t syslogEventEpochMs(uint32_t eventUptimeMs, uint32_t nowUptimeMs, uint64_t nowEpochMs);
