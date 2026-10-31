#pragma once

/**
 * @file watchdog.h
 * @brief Hardware watchdog in every build: a hang on either core restarts the device.
 *
 * Core 0 feeds the RP2040 watchdog from loop(), but only while core 1 keeps
 * moving its heartbeat (lib/watchdog_logic). The restart cause and the number
 * of watchdog restarts since power-on are shown at boot and on the status screen.
 */

#include <stdint.h>
#include <watchdog_logic.h>

/// Watchdog timeout; the RP2040 allows at most ~8.3 s
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 8000;

/// Record why this start happened and print it. Call early in setup(), before diagBegin().
void watchdogBegin();
/// Arm the watchdog. Call at the end of setup().
void watchdogArm();
/// Feed the watchdog while core 1 is alive. Call every loop() on core 0.
void watchdogPoll();
/// Core 1's heartbeat. Call every loop1().
void watchdogCore1Beat();

/// Why the device last started
RestartCause watchdogRestartCause();
/// Watchdog restarts since power-on, including the last start
uint32_t watchdogRestarts();

/// Test hook (hidden serial command): stop core 0 or core 1 for good, so the watchdog restarts the device
void watchdogStall(uint8_t core);
