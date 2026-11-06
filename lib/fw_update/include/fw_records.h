// Which boot record the firmware writes, and when: after a fallback, when a
// trial is confirmed or given up, for an upload or a slot switch, and after
// the pending trap was sent. Pure (no flash I/O); the caller writes the
// result (src/fw_update_service.cpp), which also sets the sequence number.
#pragma once
#include <stdint.h>

#include "boot_decision.h"
#include "boot_record.h"

// After the boot program fell back from failedSlot to runningSlot: the running
// slot becomes the active one, no trial, the trap is pending. RolledBackCrash
// when failedSlot was the record's trial (a new firmware given up on), else
// FallbackCrash (the kept firmware given up on). lastVersion = failedVersion
// (the failed slot's header version, "" when unknown).
BootRecord fwRecordAfterFallback(bool haveRecord, const BootRecord& rec, uint8_t runningSlot, uint8_t failedSlot,
                                 const char* failedVersion);

// Trial confirmed: the running slot is kept (Installed, trap pending).
BootRecord fwRecordConfirmed(uint8_t runningSlot, const char* runningVersion);

// Trial not reached in time: back to the other slot (RolledBackUnreachable,
// lastVersion = the trial firmware's version, trap pending).
BootRecord fwRecordUnreachable(uint8_t runningSlot, const char* runningVersion);

// Upload verified or switch asked for: the other slot boots next, on trial;
// the running slot stays the active one. Result None, no trap pending.
BootRecord fwRecordTrialOfOther(uint8_t runningSlot, const char* otherVersion);

// The pending trap was taken: the same record without the trap flag.
BootRecord fwRecordTrapSent(const BootRecord& rec);

// May an upload or a switch write the other slot / the record now? Only when
// the record (if any) has the running slot active and nothing on trial.
bool fwRecordSettled(bool haveRecord, const BootRecord& rec, uint8_t runningSlot);

// Why the boot program gave up on a slot (boot_decision.h): the crash limit
// falls back only after a crash and restarts the count, so reset Crash with a
// crash count of 0 in crashScratch means the crash limit; anything else means
// the slot failed its signature check.
bool fwFallbackWasCrashLimit(ResetKind reset, uint32_t crashScratch);
