#include "fw_update_service.h"

#include <Arduino.h>
#include <new>
#include <hardware/flash.h>
#include <hardware/structs/watchdog.h>
#include <boot_decision.h>
#include <fw_image.h>
#include <fw_key.h>
#include <trial_monitor.h>
#include <update_receiver.h>
#include "boot_info.h"
#include "version.h"

#ifndef FW_SLOT
#error "FW_SLOT is set by scripts/slot_build.py"
#endif

// The slot this image was linked for (it can run nowhere else)
static const uint8_t RUNNING_SLOT = FW_SLOT;
static const uint8_t OTHER_SLOT = 1 - FW_SLOT;

// The crash counter's encoding (boot_decision.h): 0xC7A50000 | slot << 8 | count
static const uint32_t CRASH_SCRATCH_MAGIC = 0xC7A50000u;

// ---- flash ----

/// Flash address range a source buffer must not be in: programming reads it while XIP is off
static bool inXip(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x10000000u && a < 0x14000000u;
}

/// Feed the watchdog, but only once the firmware runs it: before watchdog_enable() the SDK's
/// reload value is 0, and feeding would reset the chip at once
static void feedWatchdog() {
    if (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) rp2040.wdt_reset();
}

/// Erase one 4 KB sector at a flash offset; both cores are paused meanwhile (as LittleFS does)
static void flashErase(uint32_t offset) {
    noInterrupts();
    rp2040.idleOtherCore();
    flash_range_erase(offset, FW_SECTOR_SIZE);
    rp2040.resumeOtherCore();
    interrupts();
    feedWatchdog();
}

/// Program len bytes (a multiple of 256) from RAM at a flash offset
static void flashProgram(uint32_t offset, const uint8_t* data, size_t len) {
    noInterrupts();
    rp2040.idleOtherCore();
    flash_range_program(offset, data, len);
    rp2040.resumeOtherCore();
    interrupts();
    feedWatchdog();
}

static const uint8_t* xip(uint32_t offset) { return (const uint8_t*)(FW_FLASH_XIP_BASE + offset); }

/**
 * @brief SlotWriter on the real flash, for the slot that is not running
 *
 * Refuses on its own (whatever the receiver asks) the running slot, offsets
 * outside the slot or not on a sector boundary, and source data in flash.
 * Each written sector is read back.
 */
class FlashSlotWriter : public SlotWriter {
public:
    explicit FlashSlotWriter(uint8_t slot) : _slot(slot) {}

    bool eraseSector(uint32_t offsetInSlot) override {
        if (!allowed(offsetInSlot)) return false;
        _touched = true;
        uint32_t at = FW_SLOT_OFFSET[_slot] + offsetInSlot;
        flashErase(at);
        const uint8_t* p = xip(at);
        for (uint32_t i = 0; i < FW_SECTOR_SIZE; i++) {
            if (p[i] != 0xFF) return false;
        }
        return true;
    }

    bool writeSector(uint32_t offsetInSlot, const uint8_t* data) override {
        if (!allowed(offsetInSlot) || data == nullptr || inXip(data)) return false;
        _touched = true;
        uint32_t at = FW_SLOT_OFFSET[_slot] + offsetInSlot;
        flashProgram(at, data, FW_SECTOR_SIZE);
        return memcmp(xip(at), data, FW_SECTOR_SIZE) == 0;
    }

    /// true once anything was erased or written (the slot no longer holds what it did)
    bool touched() const { return _touched; }

private:
    bool allowed(uint32_t offsetInSlot) const {
        if (_slot > 1 || _slot == RUNNING_SLOT) return false;
        if (bootInfoValid() && _slot == bootInfoSlot()) return false;  // the boot program says it runs
        return offsetInSlot < FW_SLOT_SIZE && offsetInSlot % FW_SECTOR_SIZE == 0;
    }

    uint8_t _slot;
    bool _touched = false;
};

// ---- boot record ----

static bool haveRecord = false;
static int recordCopy = -1;  // the newest valid copy (0/1), -1 for none
static BootRecord record;
static uint8_t recordPage[256];  // one flash page; static: kept off core 0's stack

/// Read both copies and pick the newest valid one
static void loadRecord() {
    BootRecord r[2];
    bool ok[2];
    for (int i = 0; i < 2; i++) ok[i] = bootRecordDecode(xip(FW_BOOT_RECORD_OFFSET[i]), BOOT_RECORD_SIZE, r[i]);
    recordCopy = bootRecordNewest(ok[0], r[0], ok[1], r[1]);
    haveRecord = recordCopy >= 0;
    if (haveRecord) record = r[recordCopy];
}

/**
 * @brief Write r (its sequence is set here) to the copy that is not the newest valid one
 * @return true when the copy reads back as written; the in-RAM record follows only then
 */
static bool writeRecord(BootRecord r) {
    int target = recordCopy == 0 ? 1 : 0;
    r.sequence = haveRecord ? record.sequence + 1 : 1;
    memset(recordPage, 0xFF, sizeof(recordPage));
    bootRecordEncode(r, recordPage);
    flashErase(FW_BOOT_RECORD_OFFSET[target]);
    flashProgram(FW_BOOT_RECORD_OFFSET[target], recordPage, sizeof(recordPage));
    BootRecord check;
    if (!bootRecordDecode(xip(FW_BOOT_RECORD_OFFSET[target]), BOOT_RECORD_SIZE, check) ||
        check.sequence != r.sequence) {
        return false;
    }
    record = check;
    recordCopy = target;
    haveRecord = true;
    return true;
}

/// A record with no trial and the given outcome
static BootRecord settledRecord(uint8_t active, FwResult result, const char* version, bool trap) {
    BootRecord r = {};
    r.active = active;
    r.trial = FW_NO_SLOT;
    r.lastResult = result;
    r.trapPending = trap;
    strlcpy(r.lastVersion, version, sizeof(r.lastVersion));
    return r;
}

// ---- state ----

enum class OtherSlot : uint8_t { Unchecked, Checked };

static TrialMonitor trialMonitor;
static bool onTrial = false;
static bool trapPending = false;
static bool clearTrapInRecord = false;  // fwUpdateTakeTrap() asked; written by fwUpdatePoll()
static bool crashCleared = false;
static OtherSlot otherState = OtherSlot::Unchecked;
static SlotCheck otherCheck = SlotCheck::Empty;
static char otherVersion[17] = "";
static uint8_t keyId[8];
static char keyFingerprint[17];
static char bootNote[96] = "";        // what fwUpdateBegin() did, printed by fwUpdatePrintBoot()
static char uploadResultText[64] = "";  // this boot's upload outcome (RAM only)

// Two 4 KB sector buffers: allocated only while an upload runs, so they do not
// count against the static RAM budget (and never go on core 0's stack)
static UpdateReceiver* receiver = nullptr;
static FlashSlotWriter otherWriter(OTHER_SLOT);
static bool uploading = false;

/// The upload ended: give the receiver's buffers back
static void releaseReceiver() {
    delete receiver;
    receiver = nullptr;
    uploading = false;
}

/// Version in a slot's header (signature not checked), or "" when the header is not valid
static void headerVersion(uint8_t slot, char* out, size_t size) {
    SlotHeader h;
    out[0] = '\0';
    if (slotHeaderParse(xip(FW_SLOT_OFFSET[slot]), FW_SLOT_HEADER_SIZE, slot, keyId, h) == SlotCheck::Ok) {
        strlcpy(out, h.version, size);
    }
}

/// Check the other slot's signature from flash (about a second for a full image)
static void checkOtherSlot() {
    SlotHeader h;
    otherCheck = slotVerify(xip(FW_SLOT_OFFSET[OTHER_SLOT]), OTHER_SLOT, FW_PUBLIC_KEY, h);
    if (otherCheck == SlotCheck::Ok) {
        strlcpy(otherVersion, h.version, sizeof(otherVersion));
    } else {
        otherVersion[0] = '\0';
    }
    otherState = OtherSlot::Checked;
    feedWatchdog();
}

static void clearCrashCounter() { watchdog_hw->scratch[FW_SCRATCH_CRASH] = 0; }

void fwUpdateResultText(FwResult result, const char* version, char* out, size_t size) {
    const char* what = "none";
    switch (result) {
    case FwResult::None: break;
    case FwResult::Installed: what = "installed"; break;
    case FwResult::RolledBackCrash: what = "rolled back (crash)"; break;
    case FwResult::RolledBackUnreachable: what = "rolled back (unreachable)"; break;
    case FwResult::FallbackCrash: what = "fallback (crash)"; break;
    }
    if (result == FwResult::None || version == nullptr || version[0] == '\0') {
        strlcpy(out, what, size);
    } else {
        snprintf(out, size, "%s %s", what, version);
    }
}

/// After a fallback: the running slot becomes the active one, and the rollback is reported
static void recordFallback() {
    uint8_t failed = bootInfoFailedSlot();
    bool wasTrial = haveRecord && failed <= 1 && record.trial == failed;
    // boot_decision.h: the crash limit falls back only after a crash and restarts the count
    uint32_t crash = bootInfoCrashScratch();
    uint8_t count = (crash & 0xFFFF0000u) == CRASH_SCRATCH_MAGIC ? (uint8_t)(crash & 0xFF) : 0;
    bool crashLimit = bootInfoResetKind() == ResetKind::Crash && count == 0;
    char version[17] = "";
    if (failed <= 1) headerVersion(failed, version, sizeof(version));
    BootRecord r = settledRecord(RUNNING_SLOT, wasTrial ? FwResult::RolledBackCrash : FwResult::FallbackCrash,
                                 version, true);
    bool ok = writeRecord(r);
    trapPending = ok;
    snprintf(bootNote, sizeof(bootNote), "Firmware: slot %c %s, back on slot %c%s", failed <= 1 ? 'A' + failed : '?',
             crashLimit ? "crashed 3 times" : "failed its signature check", 'A' + RUNNING_SLOT,
             ok ? "" : " (boot record NOT written)");
}

void fwUpdateBegin() {
    fwKeyId(FW_PUBLIC_KEY, keyId);
    fwKeyFingerprint(FW_PUBLIC_KEY, keyFingerprint);
    loadRecord();
    trapPending = haveRecord && record.trapPending;
    BootReason reason = bootInfoValid() ? bootInfoReason() : BootReason::NoRecord;
    if (bootInfoValid() && bootInfoSlot() != RUNNING_SLOT) {
        snprintf(bootNote, sizeof(bootNote), "Firmware: built for slot %c but started as slot %c",
                 'A' + RUNNING_SLOT, 'A' + bootInfoSlot());
    } else if (reason == BootReason::Fallback) {
        recordFallback();  // before the network: a crash now would boot the failed slot again
    } else if (reason == BootReason::Trial) {
        onTrial = true;
        trialMonitor.begin(millis());
    }
}

void fwUpdatePrintBoot() {
    if (!bootInfoValid() || bootInfoSlot() > 1) {
        Serial.println("Boot: no information from the boot program");
    } else {
        Serial.printf("Boot: slot %c verified in %lu ms (%s)\r\n", 'A' + bootInfoSlot(),
                      (unsigned long)((bootInfoVerifyMicros() + 500) / 1000), bootReasonName(bootInfoReason()));
    }
    if (bootNote[0]) Serial.println(bootNote);
    if (onTrial) {
        Serial.printf("Firmware: on trial, kept after a valid SNMP or web request within %lu min\r\n",
                      (unsigned long)(FW_TRIAL_REACH_MS / 60000));
    }
}

/// Confirmed: keep the running slot. Unreachable: go back to the other one (does not return)
static void endTrial(TrialVerdict verdict) {
    if (verdict == TrialVerdict::Confirmed) {
        onTrial = false;
        clearCrashCounter();
        bool ok = writeRecord(settledRecord(RUNNING_SLOT, FwResult::Installed, FIRMWARE_VERSION, true));
        trapPending = ok;
        Serial.printf("\r\nFirmware: trial passed, %s kept in slot %c%s\r\n", FIRMWARE_VERSION, 'A' + RUNNING_SLOT,
                      ok ? "" : " (boot record NOT written, trial again at the next start)");
        return;
    }
    Serial.printf("\r\nFirmware: no valid request on trial, going back to slot %c\r\n", 'A' + OTHER_SLOT);
    if (!writeRecord(settledRecord(OTHER_SLOT, FwResult::RolledBackUnreachable, FIRMWARE_VERSION, true))) {
        // A planned restart would start this trial again; a hang counts as a crash, and three fall back
        Serial.println("Firmware: boot record NOT written, waiting for the watchdog");
        Serial.flush();
        while (true) {
        }
    }
    Serial.flush();
    rp2040.reboot();
}

void fwUpdatePoll(bool netReady) {
    if (otherState == OtherSlot::Unchecked && !uploading) checkOtherSlot();
    if (clearTrapInRecord) {
        clearTrapInRecord = false;
        BootRecord r = record;
        r.trapPending = false;
        if (haveRecord && record.trapPending && !writeRecord(r)) {
            Serial.println("\r\nFirmware: boot record NOT written (trap flag)");
        }
    }
    if (!crashCleared && millis() >= FW_HEALTHY_MS) {
        crashCleared = true;
        clearCrashCounter();
    }
    if (onTrial) {
        TrialVerdict v = trialMonitor.poll(millis(), netReady);
        if (v != TrialVerdict::Pending) endTrial(v);
    }
}

void fwUpdateNoteValidRequest() {
#if !defined(RFPOWER_FWTEST_NOCONFIRM) && !defined(RFPOWER_FWTEST_SHORTTRIAL)
    trialMonitor.noteValidRequest();
#endif
}

/// Upload outcome for the page: "rejected: <reason>"
static void rejectUpload(const char* reason) {
    releaseReceiver();
    snprintf(uploadResultText, sizeof(uploadResultText), "rejected: %s", reason);
    if (otherWriter.touched()) {
        otherState = OtherSlot::Checked;  // its first sector is erased: Empty
        otherCheck = SlotCheck::Empty;
        otherVersion[0] = '\0';
    }
    Serial.printf("\r\nFirmware: upload %s\r\n", uploadResultText);
}

bool fwUpdateUploadBegin(uint32_t contentLength, const char*& error) {
    error = nullptr;
    if (uploading) error = "upload in progress";
    else if (onTrial) error = "on trial";
    else if (haveRecord && (record.trial != FW_NO_SLOT || record.active != RUNNING_SLOT)) error = "restart pending";
    else if (bootInfoValid() && bootInfoSlot() != RUNNING_SLOT) error = "slot mismatch";
    else if (contentLength == 0) error = "empty file";
    else if (contentLength > FW_UPLOAD_MAX) error = "file too large";
    if (error) return false;
    receiver = new (std::nothrow) UpdateReceiver();
    if (receiver == nullptr) {
        error = "out of memory";
        return false;
    }
    otherWriter = FlashSlotWriter(OTHER_SLOT);
    receiver->begin(OTHER_SLOT, contentLength, keyId, otherWriter);
    uploading = true;
    uploadResultText[0] = '\0';
    Serial.printf("\r\nFirmware: receiving %lu bytes for slot %c\r\n", (unsigned long)contentLength, 'A' + OTHER_SLOT);
    return true;
}

bool fwUpdateUploadFeed(const uint8_t* data, size_t len) {
    if (!uploading) return false;
    return receiver->feed(data, len) != UpdateStatus::Failed;
}

bool fwUpdateUploadFinish(char* versionOut, const char*& error) {
    versionOut[0] = '\0';
    error = "no upload";
    if (!uploading) return false;
    if (receiver->finish() != UpdateStatus::Done) {
        error = updateErrorName(receiver->error());
        rejectUpload(error);
        return false;
    }
    SlotHeader h;
    SlotCheck c = slotVerify(xip(FW_SLOT_OFFSET[OTHER_SLOT]), OTHER_SLOT, FW_PUBLIC_KEY, h);
    feedWatchdog();
    if (c != SlotCheck::Ok) {
        // Erase its header, so the slot reads as empty rather than as a bad image
        flashErase(FW_SLOT_OFFSET[OTHER_SLOT]);
        error = slotCheckName(c);
        rejectUpload(error);
        return false;
    }
    BootRecord r = settledRecord(RUNNING_SLOT, FwResult::None, h.version, false);
    r.trial = OTHER_SLOT;
    if (!writeRecord(r)) {
        error = "boot record not written";
        rejectUpload(error);
        return false;
    }
    releaseReceiver();
    trapPending = false;
    otherState = OtherSlot::Checked;
    otherCheck = SlotCheck::Ok;
    strlcpy(otherVersion, h.version, sizeof(otherVersion));
    strlcpy(versionOut, h.version, 17);
    snprintf(uploadResultText, sizeof(uploadResultText), "verified %s, restarting on trial", h.version);
    Serial.printf("\r\nFirmware: %s verified in slot %c, restarting into it on trial\r\n", h.version, 'A' + OTHER_SLOT);
    error = nullptr;
    return true;
}

void fwUpdateUploadAbort() {
    if (!uploading) return;
    rejectUpload(receiver->error() != UpdateError::None ? updateErrorName(receiver->error()) : "connection lost");
}

bool fwUpdateSwitchSlot() {
    if (uploading || onTrial || otherState != OtherSlot::Checked || otherCheck != SlotCheck::Ok) return false;
    if (haveRecord && (record.trial != FW_NO_SLOT || record.active != RUNNING_SLOT)) return false;
    BootRecord r = settledRecord(RUNNING_SLOT, FwResult::None, otherVersion, false);
    r.trial = OTHER_SLOT;
    if (!writeRecord(r)) return false;
    trapPending = false;
    Serial.printf("\r\nFirmware: switching to slot %c (%s) on trial\r\n", 'A' + OTHER_SLOT, otherVersion);
    return true;
}

void fwUpdateStatus(WebFirmware& out) {
    out.runningSlot = RUNNING_SLOT;
    strlcpy(out.runningVersion, FIRMWARE_VERSION, sizeof(out.runningVersion));
    out.trial = onTrial;
    out.uploading = uploading;
    out.otherVersion[0] = '\0';
    out.otherProblem[0] = '\0';
    if (uploading) {
        strlcpy(out.otherProblem, "upload in progress", sizeof(out.otherProblem));
    } else if (otherState == OtherSlot::Unchecked) {
        strlcpy(out.otherProblem, "not checked yet", sizeof(out.otherProblem));
    } else if (otherCheck == SlotCheck::Ok) {
        strlcpy(out.otherVersion, otherVersion, sizeof(out.otherVersion));
    } else {
        strlcpy(out.otherProblem, slotCheckName(otherCheck), sizeof(out.otherProblem));
    }
    if (uploadResultText[0]) {
        strlcpy(out.lastResult, uploadResultText, sizeof(out.lastResult));
    } else if (haveRecord) {
        fwUpdateResultText(record.lastResult, record.lastVersion, out.lastResult, sizeof(out.lastResult));
    } else {
        strlcpy(out.lastResult, "none", sizeof(out.lastResult));
    }
    strlcpy(out.keyFingerprint, keyFingerprint, sizeof(out.keyFingerprint));
}

void fwUpdateSnmpState(FwSnmpState& out) {
    out.runningSlot = RUNNING_SLOT;
    out.trial = onTrial;
    strlcpy(out.slotVersion[RUNNING_SLOT], FIRMWARE_VERSION, sizeof(out.slotVersion[0]));
    bool otherOk = !uploading && otherState == OtherSlot::Checked && otherCheck == SlotCheck::Ok;
    strlcpy(out.slotVersion[OTHER_SLOT], otherOk ? otherVersion : "", sizeof(out.slotVersion[0]));
    out.lastResult = haveRecord ? record.lastResult : FwResult::None;
}

bool fwUpdateTakeTrap(FwResult& result) {
    if (!trapPending || !haveRecord) return false;
    trapPending = false;
    clearTrapInRecord = true;
    result = record.lastResult;
    return true;
}

RestartCause fwUpdateRestartCause() {
    RestartCause cause = RestartCause::Unknown;
    restartCauseFromBootInfo(bootInfoRaw(), cause);
    return cause;
}
