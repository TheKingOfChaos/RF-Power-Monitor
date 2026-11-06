#pragma once

/**
 * @file boot_info.h
 * @brief What the boot program (boot/) left for the firmware.
 *
 * Just before it starts a slot, the boot program writes the watchdog scratch
 * registers (lib/fw_image fw_layout.h): scratch0 the boot info
 * (bootInfoEncode: slot, reason, reset kind, failed slot), scratch1 the crash
 * counter and scratch2 how long the slot verification took. It also arms the
 * watchdog (~8.3 s), so the SDK's own reset reason reads every restart as a
 * watchdog reset: the restart cause comes from here instead (watchdog.cpp).
 * src/fw_update_service.cpp acts on it and prints it.
 */

#include <stdint.h>
#include <boot_decision.h>

/// Read the scratch registers once. Call first in setup(), before anything writes them.
void bootInfoBegin();

/// true when the boot program left valid boot info (false when started some other way)
bool bootInfoValid();

/// scratch0 as the boot program left it (restartCauseFromBootInfo() reads it)
uint32_t bootInfoRaw();

/// The slot running now (0 = A, 1 = B); FW_NO_SLOT without valid boot info
uint8_t bootInfoSlot();

/// Why that slot was started
BootReason bootInfoReason();

/// The kind of reset before this start (PowerOn without valid boot info)
ResetKind bootInfoResetKind();

/// The slot the boot program gave up on (Fallback), else FW_NO_SLOT
uint8_t bootInfoFailedSlot();

/// The crash counter (scratch1) as the boot program left it
uint32_t bootInfoCrashScratch();

/// How long the boot program took to read the boot record and verify the slot(s), in microseconds
uint32_t bootInfoVerifyMicros();
