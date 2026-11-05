// Boot program: picks a firmware slot, verifies it and starts it.
//
// Runs after boot2 (flash offset 0x100, see boot/boot.ld). The image in a
// slot is linked at slot base + 0x100 and starts with its vector table
// (scripts/slot_build.py). Sequence:
//   1. read why the chip reset (before anything changes it);
//   2. clocks to 125 MHz (boot_clocks.cpp), so the signature check is fast;
//   3. read both boot record copies from flash and pick the newest;
//   4. verify the record's target slot, and the other slot only when the
//      decision depends on it (bootSelect, lib/fw_update);
//   5. write the result to the watchdog scratch registers (fw_layout.h) and
//      arm the watchdog at its maximum (~8.3 s), so a firmware that hangs
//      before it feeds the watchdog restarts as a crash;
//   6. start the slot (VTOR, MSP, reset vector).
// With nothing bootable the LED blinks 3 short, 1 long forever and the
// watchdog stays off (boot_led.cpp). The boot record is only read here; the
// firmware writes it.

#include <stdint.h>

#include "boot_clocks.h"
#include "boot_decision.h"
#include "boot_led.h"
#include "boot_record.h"
#include "fw_image.h"
#include "fw_key.h"
#include "fw_layout.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"

namespace {

/// scratch4 value of a watchdog armed by watchdog_enable (Pico SDK): a reset
/// from it is a crash; a reboot (watchdog_reboot) leaves 0 or 0xb007c0d3
const uint32_t WATCHDOG_NON_REBOOT_MAGIC = 0x6ab73121u;
/// Largest watchdog load: 0xFFFFFF ticks of 1 us, counted down twice per tick
/// (RP2040-E1), so ~8.4 s
const uint32_t WATCHDOG_MAX_LOAD = WATCHDOG_LOAD_BITS;

/// The slot's first byte in the XIP (memory mapped) flash.
const uint8_t* slotBytes(uint8_t slot) {
    return reinterpret_cast<const uint8_t*>(FW_FLASH_XIP_BASE + FW_SLOT_OFFSET[slot]);
}

/// bootSelect's slot check: Ed25519 signature with the key built into this
/// boot program (include/fw_key.h), read straight from flash.
bool slotGood(uint8_t slot, void*) {
    SlotHeader header;
    return slotVerify(slotBytes(slot), slot, FW_PUBLIC_KEY, header) == SlotCheck::Ok;
}

/// The newest valid boot record copy; false when neither copy is valid.
bool readBootRecord(BootRecord& out) {
    BootRecord copy[2];
    bool ok[2];
    for (int i = 0; i < 2; i++) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(FW_FLASH_XIP_BASE + FW_BOOT_RECORD_OFFSET[i]);
        ok[i] = bootRecordDecode(p, BOOT_RECORD_SIZE, copy[i]);
    }
    int newest = bootRecordNewest(ok[0], copy[0], ok[1], copy[1]);
    if (newest < 0) return false;
    out = copy[newest];
    return true;
}

/// Take the 1 MHz timer out of reset (it counts from 0). The firmware resets it again.
void timerStart() {
    hw_clear_bits(&resets_hw->reset, RESETS_RESET_TIMER_BITS);
    while (!(resets_hw->reset_done & RESETS_RESET_TIMER_BITS)) {
    }
}

void watchdogDisable() { hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS); }

/// Arm the watchdog as the SDK's watchdog_enable(max, pause_on_debug) does:
/// reset everything but the oscillators, pause while a debugger halts the CPU.
void watchdogArmMax() {
    watchdogDisable();
    watchdog_hw->scratch[4] = WATCHDOG_NON_REBOOT_MAGIC;
    hw_set_bits(&psm_hw->wdsel, PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    hw_set_bits(&watchdog_hw->ctrl,
                WATCHDOG_CTRL_PAUSE_DBG0_BITS | WATCHDOG_CTRL_PAUSE_DBG1_BITS | WATCHDOG_CTRL_PAUSE_JTAG_BITS);
    watchdog_hw->load = WATCHDOG_MAX_LOAD;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

/// Start the image in `slot`: point VTOR at its vector table, load the main
/// stack pointer from word 0 and branch to the reset handler in word 1. This
/// is what boot2 does for an image at offset 0x100.
[[noreturn]] void startSlot(uint8_t slot) {
    const uint32_t vectors = FW_FLASH_XIP_BASE + FW_SLOT_OFFSET[slot] + FW_SLOT_HEADER_SIZE;
    const uint32_t* table = reinterpret_cast<const uint32_t*>(vectors);
    const uint32_t stack = table[0];
    const uint32_t reset = table[1];
    scb_hw->vtor = vectors;
    __asm__ volatile(
        "msr msp, %0\n\t"
        "bx %1\n\t"
        :
        : "r"(stack), "r"(reset)
        : "memory");
    __builtin_unreachable();
}

}  // namespace

/// Entry from boot_reset (boot/startup.S).
extern "C" [[noreturn]] void boot_main() {
    // 1. Why we are here, before anything (our own watchdog arming) changes it
    const ResetKind reset = classifyReset(watchdog_hw->reason, watchdog_hw->scratch[4],
                                          vreg_and_chip_reset_hw->chip_reset);
    const uint32_t crashScratch = watchdog_hw->scratch[FW_SCRATCH_CRASH];

    // 2. Clocks, then the timer that measures the verification
    bootClocksInit();
    timerStart();

    // 3.-5. Boot record, slot checks, decision
    const uint32_t start = timer_hw->timerawl;
    BootRecord record;
    const bool haveRecord = readBootRecord(record);
    const BootSelection sel = bootSelect(haveRecord, record, reset, crashScratch, slotGood, nullptr);
    const uint32_t micros = timer_hw->timerawl - start;

    if (sel.decision.slot == FW_NO_SLOT) {
        watchdogDisable();
        bootLedNothingBootable();
    }

    // 6. Hand over to the firmware (boot_decision.h, fw_layout.h). failedSlot
    // with reset Crash and a crash count of 0 means the crash limit; see bootSelect
    watchdog_hw->scratch[FW_SCRATCH_BOOT_INFO] =
        bootInfoEncode(sel.decision.slot, sel.decision.reason, reset, sel.failedSlot);
    watchdog_hw->scratch[FW_SCRATCH_CRASH] = sel.decision.crashScratch;
    watchdog_hw->scratch[FW_SCRATCH_VERIFY_US] = micros;
    watchdogArmMax();
    startSlot(sel.decision.slot);
}

/// NMI and HardFault in the boot program (boot/startup.S): report it like
/// "nothing bootable" (LED pattern, watchdog off) rather than hang silently.
extern "C" [[noreturn]] void boot_fault() {
    watchdogDisable();
    bootLedNothingBootable();
}
