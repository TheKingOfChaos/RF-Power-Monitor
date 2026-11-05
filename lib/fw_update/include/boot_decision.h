// Boot decision: which slot the boot program starts, from the boot record,
// slot verification results and the kind of reset. The crash counter lives in
// a watchdog scratch register (survives watchdog resets, cleared at power-on);
// this code only computes the next value. Freestanding safe.
#pragma once
#include <stdint.h>

#include "boot_record.h"

enum class ResetKind : uint8_t { PowerOn, ResetPin, Planned, Crash };

// watchdogReason: WATCHDOG_REASON register, scratch4: watchdog scratch 4,
// chipReset: CHIP_RESET register (HAD_RUN is bit 16).
ResetKind classifyReset(uint32_t watchdogReason, uint32_t scratch4, uint32_t chipReset);

enum class BootReason : uint8_t { Normal, Trial, Fallback, NoRecord };

static const uint8_t FW_CRASH_LIMIT = 3;

// Lowercase name for the boot log, e.g. "no record"; "?" for an unknown value.
const char* bootReasonName(BootReason r);

struct BootDecision {
    uint8_t slot;  // FW_NO_SLOT: nothing bootable
    BootReason reason;
    uint32_t crashScratch;  // value to store in the scratch register
};

// crashScratch encoding: 0xC7A50000 | slot << 8 | count; anything else = count 0.
// Crash on the same slot adds one, a crash on another slot restarts at one,
// any other reset clears. At FW_CRASH_LIMIT the other slot is booted when it
// verifies (reason Fallback, count 0). The slot returned is always slotOk.
// A record whose active/trial is out of range is treated as no record.
// The record is not changed by the boot program: after a Fallback the firmware
// must rewrite it promptly. Until then a crash or planned reboot of the
// fallback slot boots the original target again (count 1 after a crash, 0
// after a planned reboot).
BootDecision decideBoot(bool haveRecord, const BootRecord& rec, const bool slotOk[2], ResetKind reset,
                        uint32_t crashScratch);

// Slot the record asks for: its trial slot when one is set, else its active
// slot; slot 0 (A) without a record or with an out-of-range one (as decideBoot).
uint8_t bootTarget(bool haveRecord, const BootRecord& rec);

// Checks one slot for bootSelect (the boot program: slotVerify on flash).
typedef bool (*BootSlotCheck)(uint8_t slot, void* ctx);

struct BootSelection {
    BootDecision decision;
    // The slot given up on: the target, when the decision boots the other slot
    // (the target failed verification or reached the crash limit); else
    // FW_NO_SLOT, also when nothing is bootable.
    uint8_t failedSlot;
};

// decideBoot with the slots checked lazily, as the boot program runs it: the
// target first, the other slot only when the decision depends on it (the
// target failed, or a crash reached FW_CRASH_LIMIT). Each slot is checked at
// most once; the result equals decideBoot with both slots checked.
//
// How the firmware tells why failedSlot was given up on: the crash limit
// falls back only after a crash and restarts the count, so reset Crash with a
// crash count of 0 (scratch1) means the crash limit; any other reset, or a
// count of 1 or more, means the target failed verification.
BootSelection bootSelect(bool haveRecord, const BootRecord& rec, ResetKind reset, uint32_t crashScratch,
                         BootSlotCheck check, void* ctx);

// Packs the boot program's result for the firmware (scratch register):
// magic 0xB1 in bits 24-31, failedSlot bits 16-23, reset 12-15, reason 8-11, slot 0-7.
// failedSlot is the slot that was given up on (FW_NO_SLOT for none).
uint32_t bootInfoEncode(uint8_t slot, BootReason reason, ResetKind reset, uint8_t failedSlot);
bool bootInfoDecode(uint32_t v, uint8_t& slot, BootReason& reason, ResetKind& reset, uint8_t& failedSlot);
