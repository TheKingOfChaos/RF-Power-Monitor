#include "reset_button.h"

void ResetButton::begin(bool pressed, uint32_t nowMs) {
    _debouncer.begin(pressed, nowMs);
    _armed = !pressed;
    _firedThisPress = false;
    _triggered = false;
    _pressStartMs = nowMs;
    _triggerMs = 0;
    _lastPressMs = 0;
}

ResetButtonEvent ResetButton::update(bool pressed, uint32_t nowMs) {
    bool changed = _debouncer.update(pressed, nowMs);

    if (changed) {
        if (_debouncer.state()) {
            _pressStartMs = nowMs;
            _firedThisPress = false;
            if (_armed) return ResetButtonEvent::Pressed;
        } else {
            bool wasArmed = _armed;
            _lastPressMs = nowMs - _pressStartMs;
            _armed = true;  // Any release arms the button, including one held since boot
            if (wasArmed && !_firedThisPress) return ResetButtonEvent::Released;
        }
        return ResetButtonEvent::None;
    }

    // The raw level must still be pressed: a release that the debouncer has not
    // accepted yet must not let the hold time run out
    if (pressed && _armed && _debouncer.state() && !_firedThisPress && nowMs - _pressStartMs >= _holdMs) {
        _firedThisPress = true;
        _triggered = true;
        _triggerMs = nowMs;
        return ResetButtonEvent::Triggered;
    }
    return ResetButtonEvent::None;
}

uint32_t ResetButton::heldMs(uint32_t nowMs) const {
    return _debouncer.state() ? nowMs - _pressStartMs : 0;
}

bool ResetButton::ledOn(uint32_t nowMs) const {
    if (_triggered) return ((nowMs - _triggerMs) / TRIGGERED_BLINK_MS) % 2 == 0;
    if (_armed && _debouncer.state()) return ((nowMs - _pressStartMs) / HOLD_BLINK_MS) % 2 == 0;
    return false;
}
