#include "boot_decision.h"

namespace {

const uint32_t CRASH_MAGIC = 0xC7A50000u;
const uint32_t WATCHDOG_NON_REBOOT_MAGIC = 0x6ab73121u;
const uint32_t CHIP_RESET_HAD_RUN = 1u << 16;
const uint32_t BOOT_INFO_MAGIC = 0xB1u;

uint32_t crashEncode(uint8_t slot, uint8_t count) { return CRASH_MAGIC | ((uint32_t)slot << 8) | count; }

// Count previously recorded for `slot` (0 if the register holds anything else)
uint8_t crashCountFor(uint32_t scratch, uint8_t slot) {
    if ((scratch & 0xFFFF0000u) != CRASH_MAGIC) return 0;
    if (((scratch >> 8) & 0xFFu) != slot) return 0;
    return (uint8_t)(scratch & 0xFFu);
}

}  // namespace

ResetKind classifyReset(uint32_t watchdogReason, uint32_t scratch4, uint32_t chipReset) {
    if (watchdogReason != 0) return scratch4 == WATCHDOG_NON_REBOOT_MAGIC ? ResetKind::Crash : ResetKind::Planned;
    if (chipReset & CHIP_RESET_HAD_RUN) return ResetKind::ResetPin;
    return ResetKind::PowerOn;
}

BootDecision decideBoot(bool haveRecord, const BootRecord& rec, const bool slotOk[2], ResetKind reset,
                        uint32_t crashScratch) {
    BootDecision d = {FW_NO_SLOT, BootReason::Normal, 0};
    uint8_t target = 0;
    BootReason reason = BootReason::NoRecord;
    if (haveRecord) {
        target = rec.trial != FW_NO_SLOT ? rec.trial : rec.active;
        reason = rec.trial != FW_NO_SLOT ? BootReason::Trial : BootReason::Normal;
    }
    if (target > 1) return d;  // not a valid record
    uint8_t other = (uint8_t)(1 - target);

    uint8_t slot = target;
    if (!slotOk[target]) {
        if (!slotOk[other]) return d;
        slot = other;
        reason = BootReason::Fallback;
    }
    uint8_t alt = (uint8_t)(1 - slot);

    uint8_t count = 0;
    if (reset == ResetKind::Crash) {
        uint8_t prev = crashCountFor(crashScratch, slot);
        count = (uint8_t)(prev < 255 ? prev + 1 : prev);
    }
    if (count >= FW_CRASH_LIMIT && slotOk[alt]) {
        slot = alt;
        reason = BootReason::Fallback;
        count = 0;
    }
    d.slot = slot;
    d.reason = reason;
    d.crashScratch = crashEncode(slot, count);
    return d;
}

uint32_t bootInfoEncode(uint8_t slot, BootReason reason, ResetKind reset, uint8_t failedSlot) {
    return (BOOT_INFO_MAGIC << 24) | ((uint32_t)failedSlot << 16) | ((uint32_t)reset << 12) | ((uint32_t)reason << 8) |
           slot;
}

bool bootInfoDecode(uint32_t v, uint8_t& slot, BootReason& reason, ResetKind& reset, uint8_t& failedSlot) {
    if ((v >> 24) != BOOT_INFO_MAGIC) return false;
    uint8_t s = (uint8_t)(v & 0xFF), f = (uint8_t)((v >> 16) & 0xFF);
    uint8_t rs = (uint8_t)((v >> 8) & 0xF), rk = (uint8_t)((v >> 12) & 0xF);
    if (s > 1 && s != FW_NO_SLOT) return false;
    if (f > 1 && f != FW_NO_SLOT) return false;
    if (rs > (uint8_t)BootReason::NoRecord || rk > (uint8_t)ResetKind::Crash) return false;
    slot = s;
    failedSlot = f;
    reason = (BootReason)rs;
    reset = (ResetKind)rk;
    return true;
}
