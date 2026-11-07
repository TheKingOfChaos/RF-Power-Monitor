#include "checkin_stats.h"

void CheckinStats::active(bool due, uint32_t nowMs) {
    if (due && !_active) _sinceMs = nowMs;
    _active = due;
}

CheckinChange CheckinStats::done(bool ok, uint32_t nowMs) {
    if (ok) {
        CheckinChange change = _failedInRow > 0 ? CheckinChange::WorksAgain : CheckinChange::None;
        _good = true;
        _lastGoodMs = nowMs;
        _failedInRow = 0;
        return change;
    }
    return _failedInRow++ == 0 ? CheckinChange::StartedFailing : CheckinChange::None;
}

void CheckinStats::restart(uint32_t nowMs) {
    _good = false;
    _failedInRow = 0;
    _sinceMs = nowMs;
}

CheckinSnapshot CheckinStats::snapshot(bool configured, uint32_t nowMs) const {
    if (!configured) return {0, 0, 0};
    CheckinSnapshot n;
    n.failedInRow = _failedInRow;
    n.result = _failedInRow > 0 ? 3 : _good ? 2 : 1;
    uint32_t sinceMs = _good ? _lastGoodMs : _sinceMs;
    n.minutesSinceGood = (_good || _active) ? (nowMs - sinceMs) / 60000u : 0;
    return n;
}
