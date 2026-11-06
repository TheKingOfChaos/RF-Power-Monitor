#include "boot_info.h"

#include <hardware/structs/watchdog.h>
#include <fw_layout.h>

static bool valid = false;
static uint32_t raw = 0;
static uint8_t slot = FW_NO_SLOT;
static BootReason reason = BootReason::NoRecord;
static uint32_t verifyMicros = 0;
static ResetKind resetKind = ResetKind::PowerOn;
static uint8_t failedSlot = FW_NO_SLOT;
static uint32_t crashScratch = 0;

void bootInfoBegin() {
    raw = watchdog_hw->scratch[FW_SCRATCH_BOOT_INFO];
    verifyMicros = watchdog_hw->scratch[FW_SCRATCH_VERIFY_US];
    crashScratch = watchdog_hw->scratch[FW_SCRATCH_CRASH];
    valid = bootInfoDecode(raw, slot, reason, resetKind, failedSlot);
    if (!valid) {
        slot = FW_NO_SLOT;
        reason = BootReason::NoRecord;
        resetKind = ResetKind::PowerOn;
        failedSlot = FW_NO_SLOT;
    }
}

bool bootInfoValid() { return valid; }

uint32_t bootInfoRaw() { return raw; }

uint8_t bootInfoSlot() { return slot; }

BootReason bootInfoReason() { return reason; }

uint32_t bootInfoVerifyMicros() { return verifyMicros; }

ResetKind bootInfoResetKind() { return resetKind; }

uint8_t bootInfoFailedSlot() { return failedSlot; }

uint32_t bootInfoCrashScratch() { return crashScratch; }
