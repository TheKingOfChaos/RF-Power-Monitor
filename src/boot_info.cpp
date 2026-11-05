#include "boot_info.h"

#include <Arduino.h>
#include <hardware/structs/watchdog.h>
#include <fw_layout.h>

static bool valid = false;
static uint32_t raw = 0;
static uint8_t slot = FW_NO_SLOT;
static BootReason reason = BootReason::NoRecord;
static uint32_t verifyMicros = 0;

void bootInfoBegin() {
    raw = watchdog_hw->scratch[FW_SCRATCH_BOOT_INFO];
    verifyMicros = watchdog_hw->scratch[FW_SCRATCH_VERIFY_US];
    ResetKind resetKind;  // not kept: watchdog.cpp reads the restart cause from raw
    uint8_t failedSlot;   // not kept: nothing uses it yet
    valid = bootInfoDecode(raw, slot, reason, resetKind, failedSlot);
    if (!valid) slot = FW_NO_SLOT;
}

bool bootInfoValid() { return valid; }

uint32_t bootInfoRaw() { return raw; }

uint8_t bootInfoSlot() { return slot; }

BootReason bootInfoReason() { return reason; }

uint32_t bootInfoVerifyMicros() { return verifyMicros; }

void bootInfoPrint() {
    if (!valid || slot > 1) {
        Serial.println("Boot: no information from the boot program");
        return;
    }
    Serial.printf("Boot: slot %c verified in %lu ms (%s)\r\n", 'A' + slot,
                  (unsigned long)((verifyMicros + 500) / 1000), bootReasonName(reason));
}
