#pragma once

/**
 * @file diag.h
 * @brief Temporary hang diagnosis (build with -DRFPOWER_DIAG): hardware watchdog plus
 *        breadcrumbs in RAM the startup code does not clear, which survive the watchdog reset.
 *
 * Not in the watchdog scratch registers: the boot program (boot/) writes
 * scratch0-2 on every start (lib/fw_image fw_layout.h).
 *
 * Without RFPOWER_DIAG every macro is empty.
 */

#include <stdint.h>

#ifdef RFPOWER_DIAG
/// Breadcrumbs of the running firmware, read after the watchdog restart
struct DiagCrumbs {
    uint32_t mark;   ///< last step of loop() reached
    uint32_t hook;   ///< inside a frame hook: 100/101 input enter/leave, 200/201 output enter/leave
    uint32_t stack;  ///< core 0 stack never used
    uint32_t fault;  ///< 0xDEAD0000 | mark after a hard fault or panic
    uint32_t pc;     ///< hard fault: the faulting instruction
    uint32_t lr;     ///< hard fault: who called it
};
extern volatile DiagCrumbs diagCrumbs;

/// Last step of loop() reached
#define DIAG_MARK(n) (diagCrumbs.mark = (n))
/// Inside a frame hook
#define DIAG_HOOK(n) (diagCrumbs.hook = (n))

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
