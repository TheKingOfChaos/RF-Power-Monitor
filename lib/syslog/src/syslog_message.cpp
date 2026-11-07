#include "syslog_message.h"

#include <clock.h>
#include <stdio.h>
#include <string.h>

size_t syslogFormat(char* out, size_t size, SyslogSeverity sev, bool dated, uint64_t epochMs, const char* hostname,
                    const char* msgId, const char* text) {
    if (out == nullptr || size == 0) return 0;
    char stamp[32] = "-";
    if (dated && formatUtcRfc5424(epochMs, stamp, sizeof(stamp)) == 0) stamp[0] = '-', stamp[1] = '\0';
    char body[SYSLOG_TEXT_MAX + 1];
    size_t n = 0;
    for (; text != nullptr && text[n] != '\0' && n < SYSLOG_TEXT_MAX; n++) {
        unsigned char c = (unsigned char)text[n];
        body[n] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    body[n] = '\0';
    const unsigned pri = SYSLOG_FACILITY_DAEMON * 8u + (unsigned)sev;
    const char* host = (hostname != nullptr && hostname[0] != '\0') ? hostname : "-";
    const char* id = (msgId != nullptr && msgId[0] != '\0') ? msgId : "-";
    int len = snprintf(out, size, "<%u>1 %s %s rfpower - %s - %s", pri, stamp, host, id, body);
    if (len < 0 || (size_t)len >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)len;
}

bool SyslogQueue::push(const SyslogEntry& e) {
    if (_count == CAPACITY) {
        _head = (uint8_t)((_head + 1) % CAPACITY);
        _count--;
        _dropped++;
    }
    _entries[(_head + _count) % CAPACITY] = e;
    _count++;
    return true;
}

bool SyslogQueue::peek(SyslogEntry& out) const {
    if (_count == 0) return false;
    out = _entries[_head];
    return true;
}

void SyslogQueue::pop() {
    if (_count == 0) return;
    _head = (uint8_t)((_head + 1) % CAPACITY);
    _count--;
}

uint32_t SyslogQueue::takeDropped() {
    uint32_t d = _dropped;
    _dropped = 0;
    return d;
}

bool SyslogQueue::peekNext(SyslogEntry& out, uint32_t nowMs) const {
    if (_dropped > 0) {
        out.uptimeMs = nowMs;
        out.severity = SyslogSeverity::Notice;
        snprintf(out.msgId, sizeof(out.msgId), "SYSLOG");
        snprintf(out.text, sizeof(out.text), "%lu events dropped (queue full)", (unsigned long)_dropped);
        return true;
    }
    return peek(out);
}

void SyslogQueue::popNext() {
    if (_dropped > 0) {
        _dropped = 0;  // the notice went out
    } else {
        pop();
    }
}

bool SyslogThrottle::allow(uint32_t nowMs, uint8_t kind) {
    if (!_started || nowMs - _windowStartMs >= _windowMs) {
        _started = true;
        _windowStartMs = nowMs;
        _used = 0;
    }
    if (_used < _perWindow) {
        _used++;
        return true;
    }
    _suppressed[kind < KINDS ? kind : 0]++;
    return false;
}

uint32_t SyslogThrottle::takeSuppressed(uint8_t kind) {
    if (kind >= KINDS) return 0;
    uint32_t s = _suppressed[kind];
    _suppressed[kind] = 0;
    return s;
}

uint64_t syslogEventEpochMs(uint32_t eventUptimeMs, uint32_t nowUptimeMs, uint64_t nowEpochMs) {
    return nowEpochMs - (uint32_t)(nowUptimeMs - eventUptimeMs);
}
