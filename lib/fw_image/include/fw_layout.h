// Flash layout of the A/B firmware slots. Offsets are from the start of
// flash; add FW_FLASH_XIP_BASE for the XIP (memory mapped) address.
// Shared by the boot program, the firmware and the PC signing tool.
#pragma once
#include <stdint.h>

static const uint32_t FW_FLASH_XIP_BASE = 0x10000000u;

// One 4 KB sector per boot record (slot A, slot B).
static const uint32_t FW_BOOT_RECORD_OFFSET[2] = {0xE000u, 0xF000u};

// Start of each slot (header first, then the image).
static const uint32_t FW_SLOT_OFFSET[2] = {0x10000u, 0xE7000u};

static const uint32_t FW_SLOT_SIZE = 0xD7000u;
static const uint32_t FW_SLOT_HEADER_SIZE = 0x100u;
// Largest image a slot can hold (slot minus its header).
static const uint32_t FW_IMAGE_MAX = FW_SLOT_SIZE - FW_SLOT_HEADER_SIZE;
static const uint32_t FW_SECTOR_SIZE = 4096u;

// Watchdog scratch registers the boot program sets just before it starts a
// slot (they survive a watchdog reset or reboot, not a power-on or RUN pin
// reset). scratch4-7 belong to the Pico SDK (watchdog_enable/watchdog_reboot);
// nothing in the Arduino-Pico core or libpico uses scratch0-3 (checked for
// core 5.4.3). scratch3 is free.
static const uint32_t FW_SCRATCH_BOOT_INFO = 0;  // bootInfoEncode(...) (boot_decision.h)
static const uint32_t FW_SCRATCH_CRASH = 1;      // crash counter (decideBoot's crashScratch)
static const uint32_t FW_SCRATCH_VERIFY_US = 2;  // microseconds the slot verification took
