#pragma once

/**
 * @file cpu_load.h
 * @brief Processor load as HOST-RESOURCES-MIB hrProcessorLoad defines it (RFC 2790): "the average, over
 *        the last minute, of the percentage of time that this processor was not idle", 0-100.
 *
 * Two ways to measure "not idle", one per core:
 *   - CpuLoad: the caller knows its busy time. Core 1 sleeps in delay(1) each pass, so its busy time is
 *     the time outside that delay.
 *   - LoopLoad: core 0 never sleeps; it polls the network, SNMP, the web page and the menu in a loop.
 *     A pass with nothing to do still costs time, so the busy time of a pass is how much longer it took
 *     than the shortest pass of the last minute (an idle pass). An idle device reads 0 %.
 *     Limit: under load that lasts over a minute without a single idle pass, the baseline rises and the
 *     load reads too low.
 *
 * Times are time_us_32() values: differences are taken modulo 2^32, so the 71-minute wrap is harmless.
 * Pure C++, no allocation; unit tested on the host.
 */

#include <stdint.h>

/// Seconds averaged (hrProcessorLoad: one minute)
constexpr uint8_t CPU_LOAD_SECONDS = 60;

class CpuLoad {
public:
    /**
     * @brief Account busy time up to nowUs. Every full second since the last closed one closes a slot
     * @param nowUs  time_us_32() now
     * @param busyUs Busy time since the previous call
     */
    void add(uint32_t nowUs, uint32_t busyUs);

    /// Average of the completed seconds of the last minute, 0-100; 0 before the first second
    uint8_t percent() const;

private:
    uint8_t _slots[CPU_LOAD_SECONDS] = {};  ///< Busy percent per completed second, ring
    uint8_t _next = 0;                      ///< Slot written next
    uint8_t _count = 0;                     ///< Completed seconds, up to CPU_LOAD_SECONDS
    bool _started = false;
    uint32_t _slotStartUs = 0;
    uint32_t _busyUs = 0;                   ///< Busy time in the open second
};

class LoopLoad {
public:
    /// Call once per loop() pass with time_us_32() at its start
    void pass(uint32_t nowUs);

    /// Average of the completed seconds of the last minute, 0-100
    uint8_t percent() const { return _load.percent(); }

private:
    CpuLoad _load;
    uint16_t _minima[CPU_LOAD_SECONDS] = {};  ///< Shortest pass of each completed second (us), ring
    uint8_t _next = 0;
    uint8_t _count = 0;
    bool _started = false;
    uint32_t _lastUs = 0;                     ///< Start of the previous pass
    uint32_t _secondStartUs = 0;
    uint16_t _secondMin = 0xFFFF;             ///< Shortest pass in the open second
};
