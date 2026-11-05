/* Boot program start (boot/boot.ld): vector table and reset handler.

   boot2 sets VTOR to 0x10000100, loads the stack pointer from word 0 and
   branches to word 1 (boot_reset). boot_reset copies .data, clears .bss and
   calls boot_main(), which never returns. NMI and HardFault go to
   boot_fault() (boot_main.cpp): LED pattern, then a watchdog restart. */

.syntax unified
.cpu cortex-m0plus
.thumb

.section .vectors, "a"
.align 2
.global __boot_vectors
__boot_vectors:
    .word __StackTop
    .word boot_reset
    .word boot_fault        /* NMI, boot_main.cpp */
    .word boot_fault        /* HardFault, boot_main.cpp */

.section .text.boot_reset, "ax"
.align 1

/* Reset handler: set up .data and .bss, then boot_main(). */
.thumb_func
.global boot_reset
boot_reset:
    ldr r0, =__data_load__
    ldr r1, =__data_start__
    ldr r2, =__data_end__
1:  cmp r1, r2
    bhs 2f
    ldm r0!, {r3}
    stm r1!, {r3}
    b 1b
2:  ldr r1, =__bss_start__
    ldr r2, =__bss_end__
    movs r3, #0
3:  cmp r1, r2
    bhs 4f
    stm r1!, {r3}
    b 3b
4:  bl boot_main
    /* boot_main does not return */
