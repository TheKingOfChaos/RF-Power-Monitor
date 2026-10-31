#pragma once

/**
 * @file watchdog_logic.h
 * @brief Decisions behind the hardware watchdog (pure C++, unit tested on the host).
 *
 * Core 0 feeds the RP2040 watchdog, but only while core 1 shows it is alive
 * by moving a heartbeat counter, so a hang on either core resets the device.
 * A small record in RAM that survives resets (not power loss) counts the
 * watchdog restarts since power-on.
 */

#include <stdint.h>

/**
 * @brief Core 1 liveness, judged by core 0 from a counter core 1 increments
 */
class CoreHeartbeatCheck {
public:
    /// How long the counter may stand still before core 1 counts as hung
    static constexpr uint32_t DEFAULT_STALE_MS = 1000;

    explicit CoreHeartbeatCheck(uint32_t staleMs = DEFAULT_STALE_MS) : _staleMs(staleMs) {}

    /// Start watching from the current counter value
    void begin(uint32_t beat, uint32_t nowMs);

    /**
     * @brief Check the counter; call regularly from core 0
     * @return true while the counter changed within the last staleMs (wrap-safe for millis())
     */
    bool alive(uint32_t beat, uint32_t nowMs);

private:
    uint32_t _staleMs;
    uint32_t _lastBeat = 0;
    uint32_t _lastChangeMs = 0;
};

/**
 * @brief Why the device last started
 */
enum class RestartCause : uint8_t {
    Unknown = 0,
    PowerOn,   ///< Power applied (or brown-out): the RAM record starts over
    ResetPin,  ///< RUN pin pulled low
    Reboot,    ///< Software reboot: menu, web page, factory reset, flashing
    Watchdog   ///< The watchdog expired: the firmware hung
};

/**
 * @brief Kept in RAM the startup code does not clear; valid only with the right magic and check
 */
struct RestartRecord {
    uint32_t magic;
    uint32_t watchdogRestarts;
    uint32_t check;
};

/**
 * @brief Update the record for this start
 *
 * A record that fails its check (random RAM after power-on) or a PowerOn
 * start begins at zero; a Watchdog start adds one (saturating).
 *
 * @return Watchdog restarts since power-on, including this one
 */
uint32_t restartRecordUpdate(RestartRecord& record, RestartCause cause);

/**
 * @brief Short name for the status screen and boot log, e.g. "watchdog"
 */
const char* restartCauseName(RestartCause cause);
