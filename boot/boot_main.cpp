// Boot program: starts the firmware image in slot A.
//
// Runs after boot2 (flash offset 0x100, see boot/boot.ld). The image in a
// slot is linked at slot base + 0x100 and starts with its vector table
// (scripts/slot_build.py). For now the boot program always starts slot A,
// with no checks and no clock setup: the firmware sets up the clocks itself,
// as it does when boot2 starts it directly.

#include <stdint.h>

namespace {

/// Vector table of the image in slot A (flash offset 0x10000 + 0x100).
constexpr uint32_t SLOT_A_VECTORS = 0x10010100u;

/// Cortex-M0+ Vector Table Offset Register.
volatile uint32_t* const VTOR = reinterpret_cast<volatile uint32_t*>(0xE000ED08u);

/// Start the image whose vector table is at `vectors`: point VTOR at it,
/// load the main stack pointer from word 0 and branch to the reset handler
/// in word 1. This is what boot2 does for an image at offset 0x100.
[[noreturn]] void start_image(uint32_t vectors) {
    const uint32_t* table = reinterpret_cast<const uint32_t*>(vectors);
    const uint32_t stack = table[0];
    const uint32_t reset = table[1];
    *VTOR = vectors;
    __asm__ volatile(
        "msr msp, %0\n\t"
        "bx %1\n\t"
        :
        : "r"(stack), "r"(reset)
        : "memory");
    __builtin_unreachable();
}

}  // namespace

/// Entry from boot_reset (boot/startup.S): start slot A.
extern "C" [[noreturn]] void boot_main() {
    start_image(SLOT_A_VECTORS);
}
