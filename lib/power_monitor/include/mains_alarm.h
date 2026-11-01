#pragma once

/**
 * @file mains_alarm.h
 * @brief Alarm for a long mains outage: raised once mains has been lost for 60 s.
 *
 * Fed the core 1 mains snapshot from the core 0 loop. A short outage never raises it, so a
 * manager (LibreNMS) can alert on this instead of on every loss. One trap per outage: the
 * pending flag stays set until the caller has sent it, so a trap destination that appears
 * later still gets it. Pure C++, host tested.
 */

#include <stdint.h>

/// How long mains must be lost before the alarm is raised
static const uint32_t MAINS_ALARM_AFTER_MS = 60000;

class MainsAlarm {
public:
    /**
     * @brief Feed the latest mains snapshot
     * @param present Debounced mains state
     * @param changes Accepted changes since boot (a new count means a new outage)
     * @param lastChangeMs millis() of the last change, the boot time when changes == 0
     * @param nowMs Current millis() (wrap-around safe)
     */
    void update(bool present, uint32_t changes, uint32_t lastChangeMs, uint32_t nowMs);

    /// mainsPowerAlarm: mains lost for 60 s or more
    bool raised() const { return _raised; }

    /// Raised and its trap not sent yet
    bool trapPending() const { return _pending; }

    /// The trap went out; nothing more is pending until the next outage
    void trapSent() { _pending = false; }

private:
    bool _timing = false;    ///< An outage is being timed
    uint32_t _changes = 0;   ///< Change count of that outage
    bool _raised = false;    ///< Latched until mains returns, also past the millis() wrap
    bool _pending = false;
};
