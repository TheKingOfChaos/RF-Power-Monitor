#include "factory_reset.h"

#include <reset_button.h>
#include <settings.h>
#include "pins.h"

// How long the fast blink is shown before rebooting (reset must finish within 2 s)
const uint32_t RESET_FEEDBACK_MS = 500;

// Core 1 private state
static ResetButton button;
static bool ledState = false;

// Published by core 1, read by core 0. Single 32-bit words, so each read is
// atomic on the RP2040; barriers order them against the event counter.
static volatile uint32_t eventCount = 0;           // Incremented for every button event
static volatile uint8_t lastEvent = (uint8_t)ResetButtonEvent::None;
static volatile uint32_t lastEventHeldMs = 0;      // Hold duration at that event (whole press for Released)
static volatile bool resetRequested = false;

/**
 * @brief Write the LED only when its state changes, so core 0 can still use it at boot
 */
static void setLed(bool on) {
    if (on == ledState) return;
    ledState = on;
    digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void factoryResetBegin() {
    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
    delayMicroseconds(50);
    button.begin(digitalRead(FACTORY_RESET_PIN) == LOW, millis());
}

void factoryResetSample() {
    uint32_t now = millis();
    ResetButtonEvent e = button.update(digitalRead(FACTORY_RESET_PIN) == LOW, now);
    setLed(button.ledOn(now));

    if (e == ResetButtonEvent::None) return;
    lastEvent = (uint8_t)e;
    lastEventHeldMs = e == ResetButtonEvent::Released ? button.lastPressMs() : button.heldMs(now);
    if (e == ResetButtonEvent::Triggered) resetRequested = true;
    __dmb();
    eventCount = eventCount + 1;
}

void factoryResetPoll() {
    static uint32_t seenEvents = 0;
    uint32_t count = eventCount;
    if (count == seenEvents) return;
    seenEvents = count;
    __dmb();

    switch ((ResetButtonEvent)lastEvent) {
    case ResetButtonEvent::Pressed:
        Serial.println("\r\nFactory reset button pressed, hold 10 s to reset");
        return;
    case ResetButtonEvent::Released:
        Serial.printf("\r\nFactory reset button released after %lu ms, no reset\r\n", (unsigned long)lastEventHeldMs);
        return;
    case ResetButtonEvent::Triggered:
        break;
    default:
        return;
    }

    if (!resetRequested) return;
    uint32_t start = millis();
    Serial.printf("\r\nFactory reset after %lu ms hold\r\n", (unsigned long)lastEventHeldMs);
    settingsDefaults(currentSettings);
    bool saved = saveSettings();
    Serial.printf("Factory defaults %s in %lu ms, rebooting\r\n", saved ? "saved" : "NOT saved",
                  (unsigned long)(millis() - start));
    Serial.flush();
    // Core 1 keeps the LED blinking fast meanwhile
    delay(RESET_FEEDBACK_MS);
    rp2040.reboot();
}
