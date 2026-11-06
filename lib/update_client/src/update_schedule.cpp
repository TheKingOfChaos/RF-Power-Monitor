#include "update_schedule.h"

void CheckinScheduler::netReady(bool ready, uint32_t nowMs) {
    if (ready && !_ready) _next = nowMs + CHECKIN_FIRST_MS;
    _ready = ready;
}

bool CheckinScheduler::due(uint32_t nowMs) const {
    return _ready && (int32_t)(nowMs - _next) >= 0;
}

void CheckinScheduler::checkinDone(bool ok, uint32_t nowMs) {
    _next = nowMs + (ok ? CHECKIN_INTERVAL_MS : CHECKIN_RETRY_MS);
}

void CheckinScheduler::reportSoon(uint32_t nowMs) {
    notLaterThan(nowMs, CHECKIN_SOON_MS);
}

void CheckinScheduler::forceNow(uint32_t nowMs) {
    _next = nowMs;
}

void CheckinScheduler::notLaterThan(uint32_t nowMs, uint32_t delayMs) {
    // Compare remaining times from now, so a plan across a millis() wrap stays right;
    // one already due stays due
    if ((int32_t)(nowMs - _next) < 0 && _next - nowMs > delayMs) _next = nowMs + delayMs;
}
