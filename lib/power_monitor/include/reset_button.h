#pragma once

/**
 * @file reset_button.h
 * @brief Factory reset button: debounced 10-second hold detection and LED pattern.
 *
 * Pure C++, unit tested on the host. Core 1 feeds it one sample per
 * millisecond (GPIO22, pressed = LOW) and drives the LED from ledOn(), so
 * nothing blocks while the button is held. When the hold time is reached,
 * update() returns Triggered once and core 0 performs the reset.
 */

#include <stdint.h>

#include "power_debouncer.h"

/// What happened with this sample
enum class ResetButtonEvent : uint8_t {
    None,
    Pressed,    ///< Debounced press started
    Released,   ///< Released before the hold time
    Triggered   ///< Held for the full hold time (reported once per press)
};

class ResetButton {
public:
    /// Default hold time required by .clinerules
    static constexpr uint32_t DEFAULT_HOLD_MS = 10000;
    /// Contact bounce filter
    static constexpr uint32_t DEBOUNCE_MS = 20;
    /// LED blink half-period while the button is held
    static constexpr uint32_t HOLD_BLINK_MS = 250;
    /// LED blink half-period after the reset was triggered
    static constexpr uint32_t TRIGGERED_BLINK_MS = 50;

    explicit ResetButton(uint32_t holdMs = DEFAULT_HOLD_MS) : _holdMs(holdMs), _debouncer(DEBOUNCE_MS) {}

    /// Initial state; a button already held at boot must be released first
    void begin(bool pressed, uint32_t nowMs);

    /// Feed one sample (pressed = true)
    ResetButtonEvent update(bool pressed, uint32_t nowMs);

    /// Debounced state
    bool pressed() const { return _debouncer.state(); }

    /// How long the current press has lasted, 0 if not pressed
    uint32_t heldMs(uint32_t nowMs) const;

    /// How long the last press lasted, set when it is released (heldMs() is 0 by then); 0 before any release
    uint32_t lastPressMs() const { return _lastPressMs; }

    /// true once the reset has been triggered (until the next begin())
    bool triggered() const { return _triggered; }

    /// LED state for the feedback pattern: off when idle, slow blink while held, fast after trigger
    bool ledOn(uint32_t nowMs) const;

private:
    uint32_t _holdMs;
    PowerDebouncer _debouncer;
    bool _armed = false;        // false while a press that started at boot is still held
    bool _firedThisPress = false;
    bool _triggered = false;
    uint32_t _pressStartMs = 0;
    uint32_t _triggerMs = 0;
    uint32_t _lastPressMs = 0;
};
