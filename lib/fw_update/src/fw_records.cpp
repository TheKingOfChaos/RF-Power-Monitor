#include "fw_records.h"

namespace {

const uint32_t CRASH_MAGIC = 0xC7A50000u;  // crash scratch encoding, boot_decision.h

/// Copy a NUL terminated version into the record's 17-byte field (cut at 16)
void setVersion(BootRecord& r, const char* v) {
    size_t i = 0;
    for (; v != nullptr && v[i] != '\0' && i < sizeof(r.lastVersion) - 1; i++) r.lastVersion[i] = v[i];
    for (; i < sizeof(r.lastVersion); i++) r.lastVersion[i] = '\0';
}

/// A record without trial
BootRecord settled(uint8_t active, FwResult result, const char* version, bool trap) {
    BootRecord r = {};
    r.active = active;
    r.trial = FW_NO_SLOT;
    r.lastResult = result;
    r.trapPending = trap;
    setVersion(r, version);
    return r;
}

}  // namespace

BootRecord fwRecordAfterFallback(bool haveRecord, const BootRecord& rec, uint8_t runningSlot, uint8_t failedSlot,
                                 const char* failedVersion) {
    bool wasTrial = haveRecord && failedSlot <= 1 && rec.trial == failedSlot;
    return settled(runningSlot, wasTrial ? FwResult::RolledBackCrash : FwResult::FallbackCrash, failedVersion, true);
}

BootRecord fwRecordConfirmed(uint8_t runningSlot, const char* runningVersion) {
    return settled(runningSlot, FwResult::Installed, runningVersion, true);
}

BootRecord fwRecordUnreachable(uint8_t runningSlot, const char* runningVersion) {
    return settled((uint8_t)(1 - runningSlot), FwResult::RolledBackUnreachable, runningVersion, true);
}

BootRecord fwRecordTrialOfOther(uint8_t runningSlot, const char* otherVersion) {
    BootRecord r = settled(runningSlot, FwResult::None, otherVersion, false);
    r.trial = (uint8_t)(1 - runningSlot);
    return r;
}

BootRecord fwRecordTrapSent(const BootRecord& rec) {
    BootRecord r = rec;
    r.trapPending = false;
    return r;
}

bool fwRecordSettled(bool haveRecord, const BootRecord& rec, uint8_t runningSlot) {
    return !haveRecord || (rec.active == runningSlot && rec.trial == FW_NO_SLOT);
}

bool fwRecordForgetOther(bool haveRecord, const BootRecord& rec, uint8_t runningSlot, bool otherSectorErased) {
    if (!haveRecord || otherSectorErased || !fwRecordSettled(haveRecord, rec, runningSlot)) return false;
    return rec.lastResult == FwResult::RolledBackCrash || rec.lastResult == FwResult::RolledBackUnreachable;
}

bool fwFallbackWasCrashLimit(ResetKind reset, uint32_t crashScratch) {
    uint8_t count = (crashScratch & 0xFFFF0000u) == CRASH_MAGIC ? (uint8_t)(crashScratch & 0xFFu) : 0;
    return reset == ResetKind::Crash && count == 0;
}
