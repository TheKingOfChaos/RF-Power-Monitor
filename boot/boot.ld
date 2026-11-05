/* Boot program linker script (scripts/build_boot.py).

   Flash layout (offsets from 0x10000000):
     0x00000  boot2 (256 bytes, from the core; the boot ROM runs it, it sets
              up XIP and starts the vector table at 0x100)
     0x00100  boot program: vector table, code, read-only data, .data image
     0x0E000  boot record (2 x 4 KB, not touched here), so the code must end
              before it
     0x10000  slot A, 0xE7000 slot B (firmware image at slot + 0x100)

   The boot program runs from flash and uses RAM freely: the firmware it
   starts sets up its own RAM. Its stack is at the top of SCRATCH_Y, where the
   firmware's core 0 stack also lives. */

MEMORY
{
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 0xE000
    RAM(rwx)  : ORIGIN = 0x20000000, LENGTH = 0x42000
}

ENTRY(boot_reset)

__StackTop = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .boot2 : {
        KEEP(*(.boot2))
    } > FLASH
    ASSERT(SIZEOF(.boot2) == 256, "boot2 must be 256 bytes")

    /* boot2 starts what is at 0x10000100: stack pointer, reset handler */
    .text 0x10000100 : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .data : {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM AT> FLASH
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    __boot_end__ = LOADADDR(.data) + SIZEOF(.data);
    ASSERT(__boot_end__ <= 0x1000E000, "boot program passes 0xE000 (boot record)")

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.init_array*)
        *(.fini_array*)
    }
}
