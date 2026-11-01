#include "mains_alarm.h"

void MainsAlarm::update(bool present, uint32_t changes, uint32_t lastChangeMs, uint32_t nowMs) {
    if (present) {
        _timing = _raised = _pending = false;
        return;
    }
    if (!_timing || changes != _changes) {
        // A new outage (restored and lost again between two updates counts as new)
        _timing = true;
        _changes = changes;
        _raised = _pending = false;
    }
    if (!_raised && nowMs - lastChangeMs >= MAINS_ALARM_AFTER_MS) {
        _raised = _pending = true;
    }
}
