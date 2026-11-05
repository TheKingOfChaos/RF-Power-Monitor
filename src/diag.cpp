#include "diag.h"

#ifdef RFPOWER_DIAG
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "stack_watch.h"
#include "watchdog.h"

static const uint32_t DIAG_WATCHDOG_MS = 4000;
static char diagText[300] = "";

const char* diagReport() { return diagText; }

// Breadcrumbs, kept in RAM that the startup code does not clear (nor the boot program, which
// only uses SCRATCH_X/Y), read at the next boot
volatile DiagCrumbs diagCrumbs __attribute__((section(".uninitialized_data.diagcrumbs")));

// A hard fault (e.g. a stack overflow corrupting memory) normally spins forever with USB dead.
// Record it and spin: the watchdog then resets the device, and the next boot reports it.
// panic() message, kept in RAM that the startup code does not clear, read at the next boot
static const uint32_t PANIC_MAGIC = 0x50414e43;  // "PANC"
static const uint32_t FAULT_MAGIC = 0xDEAD0000u;
struct PanicRecord {
    uint32_t magic;
    uint32_t caller;
    char text[200];
};
static PanicRecord panicRecord __attribute__((section(".uninitialized_data.panic")));

// Linked with -Wl,--wrap=panic in the diag build: record the message, then let the watchdog reset
extern "C" void __attribute__((noreturn)) __wrap_panic(const char* fmt, ...) {
    panicRecord.caller = (uint32_t)__builtin_return_address(0);
    va_list args;
    va_start(args, fmt);
    vsnprintf(panicRecord.text, sizeof(panicRecord.text), fmt, args);
    va_end(args);
    panicRecord.magic = PANIC_MAGIC;
    diagCrumbs.fault = FAULT_MAGIC | (diagCrumbs.mark & 0xFFFF);
    while (true) {
    }
}

/// Called with the exception frame: r0 r1 r2 r3 r12 lr pc xpsr
extern "C" void diagHardFault(uint32_t* frame) {
    diagCrumbs.pc = frame[6];  // the faulting instruction
    diagCrumbs.lr = frame[5];  // who called it
    diagCrumbs.fault = FAULT_MAGIC | (diagCrumbs.mark & 0xFFFF);
    while (true) {
    }
}

// Cortex-M0+: find the stack the exception frame is on (MSP or PSP) and pass it on
extern "C" void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "movs r1, #4\n"
        "mov r2, lr\n"
        "tst r2, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1: mrs r0, msp\n"
        "2: ldr r1, =diagHardFault\n"
        "bx r1\n");
}

void diagBegin() {
    // The restart cause comes from the boot program (watchdog.cpp): every start runs with the
    // watchdog armed, so the SDK's watchdog_enable_caused_reboot() no longer tells a hang
    bool hung = watchdogRestartCause() == RestartCause::Watchdog;
    DiagCrumbs last;
    last.mark = diagCrumbs.mark;
    last.hook = diagCrumbs.hook;
    last.stack = diagCrumbs.stack;
    last.fault = diagCrumbs.fault;
    last.pc = diagCrumbs.pc;
    last.lr = diagCrumbs.lr;
    diagCrumbs.mark = diagCrumbs.hook = diagCrumbs.stack = diagCrumbs.fault = diagCrumbs.pc = diagCrumbs.lr = 0;
    rp2040.wdt_begin(DIAG_WATCHDOG_MS);
    uint32_t start = millis();
    while (!Serial && millis() - start < 3000) {
        watchdogFeed();
        delay(10);
    }
    if (hung) {
        if (panicRecord.magic == PANIC_MAGIC) {
            panicRecord.text[sizeof(panicRecord.text) - 1] = '\0';
            snprintf(diagText, sizeof(diagText), "DIAG: previous run PANICKED (called from 0x%08lx): %s",
                          (unsigned long)panicRecord.caller, panicRecord.text);
            panicRecord.magic = 0;
        } else if ((last.fault & 0xFFFF0000u) == FAULT_MAGIC) {
            snprintf(diagText, sizeof(diagText), "DIAG: previous run had a HARD FAULT: pc 0x%08lx lr 0x%08lx, last loop mark %lu",
                          (unsigned long)last.pc, (unsigned long)last.lr, (unsigned long)last.mark);
        } else {
            snprintf(diagText, sizeof(diagText), "DIAG: previous run hung (watchdog): last loop mark %lu, frame hook %lu, "
                          "core 0 stack never used %lu", (unsigned long)last.mark, (unsigned long)last.hook,
                          (unsigned long)last.stack);
        }
    } else {
        snprintf(diagText, sizeof(diagText), "DIAG: normal start (watchdog armed)");
    }
    Serial.printf("\r\n%s\r\n", diagText);
}

void diagPoll() {
    // Feeding is left to watchdogPoll(), which also checks core 1
    diagCrumbs.stack = stackWatchNeverUsed();
}
#endif
