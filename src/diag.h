#pragma once

/**
 * @file diag.h
 * @brief Temporary hang diagnosis (build with -DRFUPS_DIAG): hardware watchdog plus
 *        breadcrumbs in the watchdog scratch registers, which survive the watchdog reset.
 *
 * Without RFUPS_DIAG every macro is empty.
 */

#include <stdint.h>

#ifdef RFUPS_DIAG
#include <hardware/structs/watchdog.h>

/// Last step of loop() reached (scratch 0)
#define DIAG_MARK(n) (watchdog_hw->scratch[0] = (n))
/// Inside a frame hook: 100/101 input enter/leave, 200/201 output enter/leave (scratch 2)
#define DIAG_HOOK(n) (watchdog_hw->scratch[2] = (n))

/// Start the watchdog and report what the previous run left behind. Call first in setup().
void diagBegin();
/// Record the stack high-water mark; call every loop() (watchdogPoll() feeds the watchdog)
void diagPoll();
/// What diagBegin() found (shown on the status screen)
const char* diagReport();
#else
#define DIAG_MARK(n) ((void)0)
#define DIAG_HOOK(n) ((void)0)
inline void diagBegin() {}
inline void diagPoll() {}
inline const char* diagReport() { return nullptr; }
#endif
