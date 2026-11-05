// Onboard LED for the boot program: the "nothing bootable" pattern.

#include "boot_led.h"

#include "hardware/structs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/watchdog.h"
#include "pins.h"

namespace {

const uint32_t SHORT_MS = 150;
const uint32_t LONG_MS = 600;
const uint32_t GAP_MS = 150;
const uint32_t PAUSE_MS = 1000;  // after each 3 short, 1 long

/// Busy-wait `ms` milliseconds on the 1 MHz timer.
void waitMs(uint32_t ms) {
    uint32_t start = timer_hw->timerawl;
    while (timer_hw->timerawl - start < ms * 1000u) {
    }
}

/// LED on for `ms`, then off for `offMs`.
void flash(uint32_t ms, uint32_t offMs) {
    sio_hw->gpio_set = 1u << LED_PIN;
    waitMs(ms);
    sio_hw->gpio_clr = 1u << LED_PIN;
    waitMs(offMs);
}

}  // namespace

void bootLedNothingBootable() {
    // Also reached from a fault, possibly before bootClocksInit: make sure the
    // tick runs (12 clk_ref cycles: 1 us after bootClocksInit, ~2 us on the
    // ring oscillator before it) and the timer is out of reset
    if (!(watchdog_hw->tick & WATCHDOG_TICK_ENABLE_BITS)) watchdog_hw->tick = 12 | WATCHDOG_TICK_ENABLE_BITS;
    const uint32_t banks = RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS | RESETS_RESET_TIMER_BITS;
    hw_clear_bits(&resets_hw->reset, banks);
    while ((resets_hw->reset_done & banks) != banks) {
    }
    // Pad: output enabled, input not needed; pin driven by SIO
    hw_write_masked(&pads_bank0_hw->io[LED_PIN], 0, PADS_BANK0_GPIO0_OD_BITS);
    io_bank0_hw->io[LED_PIN].ctrl = IO_BANK0_GPIO0_CTRL_FUNCSEL_VALUE_SIO_0 << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    sio_hw->gpio_clr = 1u << LED_PIN;
    sio_hw->gpio_oe_set = 1u << LED_PIN;
    while (true) {
        flash(SHORT_MS, GAP_MS);
        flash(SHORT_MS, GAP_MS);
        flash(SHORT_MS, GAP_MS);
        flash(LONG_MS, PAUSE_MS);
    }
}
