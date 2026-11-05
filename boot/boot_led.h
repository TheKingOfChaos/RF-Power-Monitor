// Onboard LED for the boot program (boot/boot_led.cpp).
#pragma once

/// Nothing to boot: blink LED_PIN (include/pins.h) 3 short, 1 long, forever.
/// Works with or without bootClocksInit (from a fault); the watchdog is left as it is.
[[noreturn]] void bootLedNothingBootable();
