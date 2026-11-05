#include "watchdog.h"

#include <Arduino.h>

#include "boot_info.h"

// Survives watchdog and soft resets (the startup code does not clear this section); random after power-on
static RestartRecord restartRecord __attribute__((section(".uninitialized_data.restart")));

static RestartCause cause = RestartCause::Unknown;
static uint32_t restarts = 0;
static bool armed = false;
static CoreHeartbeatCheck core1Check;

/// Incremented by core 1, read by core 0; an aligned 32-bit word, so loads and stores are atomic
static uint32_t core1Beat = 0;
static volatile bool stallCore1 = false;

/**
 * @brief Why this start happened: from the boot program's boot info, else the core's reset reason
 *
 * The boot program arms the watchdog for every start, so the core's reset
 * reason (watchdog armed + watchdog reset) would call every reboot a watchdog
 * restart; it is only used when the firmware was started without the boot program.
 */
static RestartCause readCause() {
    RestartCause fromBoot;
    if (restartCauseFromBootInfo(bootInfoRaw(), fromBoot)) return fromBoot;
    switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:
    case RP2040::BROWNOUT_RESET:
    case RP2040::GLITCH_RESET:  return RestartCause::PowerOn;
    case RP2040::RUN_PIN_RESET: return RestartCause::ResetPin;
    case RP2040::SOFT_RESET:    return RestartCause::Reboot;
    case RP2040::WDT_RESET:     return RestartCause::Watchdog;
    default:                    return RestartCause::Unknown;
    }
}

void watchdogBegin() {
    // Before arming: without boot info, an armed watchdog makes the core's reset reason say "watchdog"
    cause = readCause();
    restarts = restartRecordUpdate(restartRecord, cause);
    // Take over the watchdog the boot program armed (~8.3 s), so watchdogFeed() can feed it
    rp2040.wdt_begin(WATCHDOG_TIMEOUT_MS);
}

void watchdogPrintCause() {
    Serial.printf("Last restart: %s (%lu watchdog restarts since power-on)\r\n", restartCauseName(cause),
                  (unsigned long)restarts);
}

void watchdogFeed() { rp2040.wdt_reset(); }

void watchdogArm() {
    core1Check.begin(__atomic_load_n(&core1Beat, __ATOMIC_RELAXED), millis());
    rp2040.wdt_begin(WATCHDOG_TIMEOUT_MS);  // again: diagBegin() may have shortened it
    armed = true;
}

void watchdogPoll() {
    if (!armed) return;
    // Not fed while core 1 is stuck: the watchdog then restarts the whole chip
    if (core1Check.alive(__atomic_load_n(&core1Beat, __ATOMIC_RELAXED), millis())) rp2040.wdt_reset();
}

void watchdogCore1Beat() {
    while (stallCore1) {
    }
    __atomic_store_n(&core1Beat, core1Beat + 1, __ATOMIC_RELAXED);  // only core 1 writes it
}

RestartCause watchdogRestartCause() { return cause; }

uint32_t watchdogRestarts() { return restarts; }

void watchdogStall(uint8_t core) {
    if (core == 1) {
        stallCore1 = true;
        return;
    }
    noInterrupts();
    while (true) {
    }
}
