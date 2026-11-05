#include "watchdog_logic.h"

/// "WDRS": marks a record written by this firmware
static const uint32_t RECORD_MAGIC = 0x57445253u;

void CoreHeartbeatCheck::begin(uint32_t beat, uint32_t nowMs) {
    _lastBeat = beat;
    _lastChangeMs = nowMs;
}

bool CoreHeartbeatCheck::alive(uint32_t beat, uint32_t nowMs) {
    if (beat != _lastBeat) {
        _lastBeat = beat;
        _lastChangeMs = nowMs;
    }
    return nowMs - _lastChangeMs <= _staleMs;  // unsigned difference: wrap-safe
}

/// The check ties the count to the magic, so random RAM after power-on is not taken for a record
static uint32_t recordCheck(uint32_t count) {
    return ~(RECORD_MAGIC ^ count);
}

uint32_t restartRecordUpdate(RestartRecord& record, RestartCause cause) {
    bool valid = record.magic == RECORD_MAGIC && record.check == recordCheck(record.watchdogRestarts);
    uint32_t count = valid && cause != RestartCause::PowerOn ? record.watchdogRestarts : 0;
    if (cause == RestartCause::Watchdog && count != 0xFFFFFFFFu) count++;
    record.magic = RECORD_MAGIC;
    record.watchdogRestarts = count;
    record.check = recordCheck(count);
    return count;
}

const char* restartCauseName(RestartCause cause) {
    switch (cause) {
    case RestartCause::Unknown:  return "unknown";
    case RestartCause::PowerOn:  return "power-on";
    case RestartCause::ResetPin: return "reset pin";
    case RestartCause::Reboot:   return "reboot";
    case RestartCause::Watchdog: return "watchdog";
    }
    return "?";
}

bool restartCauseFromBootInfo(uint32_t scratch0, RestartCause& out) {
    uint8_t slot, failedSlot;
    BootReason reason;
    ResetKind reset;
    if (!bootInfoDecode(scratch0, slot, reason, reset, failedSlot)) return false;
    switch (reset) {
    case ResetKind::PowerOn:  out = RestartCause::PowerOn; break;
    case ResetKind::ResetPin: out = RestartCause::ResetPin; break;
    case ResetKind::Planned:  out = RestartCause::Reboot; break;
    case ResetKind::Crash:    out = RestartCause::Watchdog; break;
    }
    return true;
}
