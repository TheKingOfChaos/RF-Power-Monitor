#include "cpu_load.h"

static const uint32_t SECOND_US = 1000000;

void CpuLoad::add(uint32_t nowUs, uint32_t busyUs) {
    if (!_started) {
        // The first call only starts the clock: there is no previous call to measure from
        _started = true;
        _slotStartUs = nowUs - busyUs;
    }
    _busyUs += busyUs;
    uint32_t elapsed = nowUs - _slotStartUs;
    if (elapsed < SECOND_US) return;

    // Close the second (a little longer if calls are sparse); busy time beyond it counts as 100 %
    uint32_t pct = _busyUs >= elapsed ? 100 : (uint32_t)((uint64_t)_busyUs * 100 / elapsed);
    _slots[_next] = (uint8_t)pct;
    _next = (uint8_t)((_next + 1) % CPU_LOAD_SECONDS);
    if (_count < CPU_LOAD_SECONDS) _count++;
    _slotStartUs = nowUs;
    _busyUs = 0;
}

uint8_t CpuLoad::percent() const {
    if (_count == 0) return 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < _count; i++) sum += _slots[i];
    return (uint8_t)((sum + _count / 2) / _count);
}

void LoopLoad::pass(uint32_t nowUs) {
    if (!_started) {
        _started = true;
        _secondStartUs = nowUs;
        _load.add(nowUs, 0);
        return;
    }
    _secondPasses++;
    uint32_t elapsed = nowUs - _secondStartUs;
    if (elapsed < SECOND_US) return;

    // Passes per second: scale a second that ran long (one slow pass at its end) to one second
    uint32_t passes = (uint32_t)((uint64_t)_secondPasses * SECOND_US / elapsed);
    if (passes > _currentMost) _currentMost = passes;

    // The idle reference: the most passes of a second in the last 15 minutes, this one included
    uint32_t most = _currentMost;
    for (uint8_t i = 0; i < _count; i++) {
        if (_minuteMost[i] > most) most = _minuteMost[i];
    }
    if (++_secondsInMinute >= 60) {
        _minuteMost[_next] = _currentMost;
        _next = (uint8_t)((_next + 1) % LOOP_LOAD_REFERENCE_MINUTES);
        if (_count < LOOP_LOAD_REFERENCE_MINUTES) _count++;
        _currentMost = 0;
        _secondsInMinute = 0;
    }
    uint32_t busyUs = most == 0 ? 0 : (uint32_t)((uint64_t)elapsed * (most - passes) / most);
    _load.add(nowUs, busyUs);

    _secondStartUs = nowUs;
    _secondPasses = 0;
}
