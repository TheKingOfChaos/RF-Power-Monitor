#pragma once

/**
 * @file trial_monitor.h
 * @brief Decides the outcome of a firmware on trial: confirmed once a valid
 *        management request arrived (SNMP or an authenticated web request),
 *        unreachable when the network was up for FW_TRIAL_REACH_MS without one
 *        or when FW_TRIAL_LIMIT_MS passed since begin(). Pure, host tested.
 */

#include <stdint.h>

// Test build (tools/fw_release.py --test-variant shorttrial): a trial that ends in minutes
#if defined(RFPOWER_FWTEST_SHORTTRIAL) && !defined(FW_TRIAL_REACH_MS) && !defined(FW_TRIAL_LIMIT_MS)
#define FW_TRIAL_REACH_MS 60000u
#define FW_TRIAL_LIMIT_MS 180000u
#endif

/// Network up this long without a valid request: the firmware cannot be reached
#ifndef FW_TRIAL_REACH_MS
#define FW_TRIAL_REACH_MS 600000u
#endif
/// Hard limit since begin(), network or not
#ifndef FW_TRIAL_LIMIT_MS
#define FW_TRIAL_LIMIT_MS 3600000u
#endif
/// Running this long after the trial: the slot counts as healthy (crash counter cleared)
#ifndef FW_HEALTHY_MS
#define FW_HEALTHY_MS 300000u
#endif

enum class TrialVerdict : uint8_t { Pending, Confirmed, Unreachable };

class TrialMonitor {
public:
    /// Start the trial at nowMs (the time base of poll(); wrap-safe)
    void begin(uint32_t nowMs);

    /// A valid request arrived (callable from the main loop only)
    void noteValidRequest() { _requested = true; }

    /**
     * @brief Advance the monitor
     * @param netReady true while the network is up (link and address); the time since the last poll
     *        counts as network time only if it was up at that poll too
     * @return Confirmed on the first poll after a valid request (and from then on),
     *         Unreachable when the network time or the hard limit ran out, else Pending
     */
    TrialVerdict poll(uint32_t nowMs, bool netReady);

private:
    uint32_t _startMs = 0;
    uint32_t _lastMs = 0;
    uint32_t _netMs = 0;  ///< time with the network up at both ends of an interval between polls
    bool _lastNet = false;  ///< netReady at the previous poll
    bool _requested = false;
    bool _confirmed = false;
};
