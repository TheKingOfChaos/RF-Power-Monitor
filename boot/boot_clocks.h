// Clock setup for the boot program (boot/boot_clocks.cpp).
#pragma once

/// Run clk_sys at 125 MHz from PLL_SYS on the 12 MHz crystal, clk_ref from
/// the crystal, and start the 1 MHz tick (timer, watchdog). Leaves the other
/// clocks alone: the firmware sets up all of its clocks again.
void bootClocksInit();
