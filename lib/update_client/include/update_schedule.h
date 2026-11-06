#pragma once

/**
 * @file update_schedule.h
 * @brief When the device checks in with the update server.
 *
 * First check-in CHECKIN_FIRST_MS after the network becomes ready (so a trial
 * firmware is confirmed quickly), then every CHECKIN_INTERVAL_MS; a failed
 * check-in is retried after CHECKIN_RETRY_MS; something worth reporting (a
 * failed download, a confirmed trial) brings the next one to CHECKIN_SOON_MS.
 * Times are millis(), wrap-around safe. Pure C++, host tested.
 */

#include <stdint.h>

static const uint32_t CHECKIN_FIRST_MS = 60000;
static const uint32_t CHECKIN_INTERVAL_MS = 1800000;
static const uint32_t CHECKIN_RETRY_MS = 300000;
static const uint32_t CHECKIN_SOON_MS = 60000;

/// Decides when the next check-in is due
class CheckinScheduler {
public:
    /// Call every poll; a change to ready schedules the first check-in, not ready stops them
    void netReady(bool ready, uint32_t nowMs);

    /// A check-in should start now
    bool due(uint32_t nowMs) const;

    /// A check-in ended: the next one after the interval (ok) or the retry delay
    void checkinDone(bool ok, uint32_t nowMs);

    /// Check in within CHECKIN_SOON_MS (never later than already planned)
    void reportSoon(uint32_t nowMs);

    /// Check in as soon as the network is ready (hidden menu command for tests); stays
    /// pending through network changes and settings saves until a check-in ends
    void forceNow(uint32_t nowMs);

private:
    /// Plan the next check-in at nowMs + delayMs unless one is planned earlier
    void notLaterThan(uint32_t nowMs, uint32_t delayMs);

    bool _ready = false;
    bool _forced = false;
    uint32_t _next = 0;
};
