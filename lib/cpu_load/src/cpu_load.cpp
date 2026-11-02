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
        _lastUs = nowUs;
        _secondStartUs = nowUs;
        _load.add(nowUs, 0);
        return;
    }
    uint32_t passUs = nowUs - _lastUs;
    _lastUs = nowUs;
    uint16_t shortPass = passUs > 0xFFFF ? 0xFFFF : (uint16_t)passUs;
    if (shortPass < _secondMin) _secondMin = shortPass;

    // The idle pass: the shortest of the last minute, this pass included
    uint16_t baseline = _secondMin;
    for (uint8_t i = 0; i < _count; i++) {
        if (_minima[i] < baseline) baseline = _minima[i];
    }
    _load.add(nowUs, passUs - baseline);

    if (nowUs - _secondStartUs >= SECOND_US) {
        _minima[_next] = _secondMin;
        _next = (uint8_t)((_next + 1) % CPU_LOAD_SECONDS);
        if (_count < CPU_LOAD_SECONDS) _count++;
        _secondStartUs = nowUs;
        _secondMin = 0xFFFF;
    }
}
