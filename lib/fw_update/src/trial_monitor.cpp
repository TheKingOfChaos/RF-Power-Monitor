#include "trial_monitor.h"

void TrialMonitor::begin(uint32_t nowMs) {
    _startMs = nowMs;
    _lastMs = nowMs;
    _netMs = 0;
    _lastNet = false;  // no network time until a poll has seen it up
    _requested = false;
    _confirmed = false;
}

TrialVerdict TrialMonitor::poll(uint32_t nowMs, bool netReady) {
    uint32_t step = nowMs - _lastMs;  // unsigned: correct across the millis() wrap
    _lastMs = nowMs;
    if (_requested) _confirmed = true;
    if (_confirmed) return TrialVerdict::Confirmed;
    // The interval counts only when the network was up at both ends of it
    if (netReady && _lastNet) {
        _netMs += step;
        if (_netMs < step) _netMs = 0xFFFFFFFFu;  // saturate, never wrap back to small
    }
    _lastNet = netReady;
    if (_netMs >= FW_TRIAL_REACH_MS || (nowMs - _startMs) >= FW_TRIAL_LIMIT_MS) return TrialVerdict::Unreachable;
    return TrialVerdict::Pending;
}

bool fwTrialNetReady(bool linkUp, bool hasIPv4, bool hasRoutableIPv6) {
    return linkUp && (hasIPv4 || hasRoutableIPv6);
}

bool fwShouldClearCrashCount(bool onTrial, bool confirmed, uint32_t uptimeMs) {
    if (confirmed) return true;
    return !onTrial && uptimeMs >= FW_HEALTHY_MS;
}
