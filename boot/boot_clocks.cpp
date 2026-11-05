// Clock setup for the boot program: a port of the Arduino-Pico core's
// ota/ota_clocks.c (itself the Pico SDK's clocks_init), reduced to what the
// boot program needs: XOSC, PLL_SYS at 125 MHz for clk_sys (fast flash reads
// for the signature check), clk_ref from XOSC and the 1 MHz tick.
//
// The firmware's own clocks_init then finds PLL_SYS locked with the same
// settings and leaves it running (pll_init), switches clk_sys and clk_ref
// glitch-free, and sets up USB, ADC, RTC and peri clocks itself.

#include "boot_clocks.h"

#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/xosc.h"

namespace {

// XOSC_KHZ: the 12 MHz crystal (hardware/platform_defs.h)
// Crystal start-up time, in units of 256 XOSC cycles: 1 ms times the SDK's
// default PICO_XOSC_STARTUP_DELAY_MULTIPLIER (6) for slow-starting crystals
const uint32_t XOSC_STARTUP_DELAY = ((XOSC_KHZ + 128) / 256) * 6;

// PLL SYS: 12 MHz / 1 * 125 = 1500 MHz VCO, / 6 / 2 = 125 MHz
const uint32_t PLL_SYS_REFDIV = 1;
const uint32_t PLL_SYS_FBDIV = 125;
const uint32_t PLL_SYS_POSTDIV1 = 6;
const uint32_t PLL_SYS_POSTDIV2 = 2;

const uint32_t DIV_1 = 1u << CLOCKS_CLK_REF_DIV_INT_LSB;  // divider 1.0 (24.8 fixed point)

/// Start the crystal oscillator and wait until it is stable (SDK xosc_init).
void xoscInit() {
    xosc_hw->ctrl = XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ;
    xosc_hw->startup = XOSC_STARTUP_DELAY;
    hw_set_bits(&xosc_hw->ctrl, XOSC_CTRL_ENABLE_VALUE_ENABLE << XOSC_CTRL_ENABLE_LSB);
    while (!(xosc_hw->status & XOSC_STATUS_STABLE_BITS)) {
    }
}

/// Reset PLL_SYS, lock it to 1500 MHz and switch on its post dividers (SDK pll_init).
void pllSysInit() {
    hw_set_bits(&resets_hw->reset, RESETS_RESET_PLL_SYS_BITS);
    hw_clear_bits(&resets_hw->reset, RESETS_RESET_PLL_SYS_BITS);
    while (!(resets_hw->reset_done & RESETS_RESET_PLL_SYS_BITS)) {
    }
    pll_sys_hw->cs = PLL_SYS_REFDIV;
    pll_sys_hw->fbdiv_int = PLL_SYS_FBDIV;
    hw_clear_bits(&pll_sys_hw->pwr, PLL_PWR_PD_BITS | PLL_PWR_VCOPD_BITS);
    while (!(pll_sys_hw->cs & PLL_CS_LOCK_BITS)) {
    }
    pll_sys_hw->prim = (PLL_SYS_POSTDIV1 << PLL_PRIM_POSTDIV1_LSB) | (PLL_SYS_POSTDIV2 << PLL_PRIM_POSTDIV2_LSB);
    hw_clear_bits(&pll_sys_hw->pwr, PLL_PWR_POSTDIVPD_BITS);
}

}  // namespace

void bootClocksInit() {
    clock_hw_t* ref = &clocks_hw->clk[clk_ref];
    clock_hw_t* sys = &clocks_hw->clk[clk_sys];

    clocks_hw->resus.ctrl = 0;  // disable resus that earlier software may have enabled
    xoscInit();

    // Before touching PLL_SYS, switch clk_sys and clk_ref to their glitchless
    // sources (clk_ref, ROSC)
    hw_clear_bits(&sys->ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (sys->selected != 0x1) {
    }
    hw_clear_bits(&ref->ctrl, CLOCKS_CLK_REF_CTRL_SRC_BITS);
    while (ref->selected != 0x1) {
    }

    pllSysInit();

    // clk_ref = XOSC / 1 = 12 MHz (glitchless mux, no aux source)
    ref->div = DIV_1;
    hw_write_masked(&ref->ctrl, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC << CLOCKS_CLK_REF_CTRL_SRC_LSB,
                    CLOCKS_CLK_REF_CTRL_SRC_BITS);
    while (!(ref->selected & (1u << CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC))) {
    }

    // clk_sys = PLL_SYS / 1 = 125 MHz: set the aux mux while clk_sys runs from
    // clk_ref, then switch the glitchless mux to aux
    sys->div = DIV_1;
    hw_write_masked(&sys->ctrl, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS << CLOCKS_CLK_SYS_CTRL_AUXSRC_LSB,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_BITS);
    hw_write_masked(&sys->ctrl, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX << CLOCKS_CLK_SYS_CTRL_SRC_LSB,
                    CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (!(sys->selected & (1u << CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX))) {
    }

    // 1 MHz tick for the timer and the watchdog: 12 clk_ref cycles (SDK watchdog_start_tick)
    watchdog_hw->tick = (XOSC_KHZ / 1000) | WATCHDOG_TICK_ENABLE_BITS;
}
