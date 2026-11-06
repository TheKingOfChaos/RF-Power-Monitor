#pragma once

/**
 * @file fw_update_service.h
 * @brief Firmware updates on the device: the boot record, the trial of a new
 *        firmware, uploads to the slot that is not running and the switch to
 *        the other slot (lib/fw_update and lib/fw_image do the pure work).
 *
 * The boot program (boot/) starts a slot and leaves its decision in the
 * watchdog scratch registers (src/boot_info.cpp reads them). This service
 * acts on it:
 *  - after a fallback (the target slot crashed 3 times or failed its
 *    verification) it rewrites the boot record at once, so the running slot
 *    stays the active one, and reports the rollback;
 *  - on a trial boot it waits for a valid management request (SNMP with the
 *    right community or an authenticated web request): then the new slot is
 *    kept; without one (lib/fw_update TrialMonitor) it goes back to the old slot;
 *  - it clears the crash counter when a trial is confirmed, or after
 *    FW_HEALTHY_MS of uptime when not on trial (fwShouldClearCrashCount).
 *
 * Uploads write only the slot that is not running. Flash writes idle core 1
 * (mains detection pauses, as for settings saves) and feed the watchdog after
 * each sector erase and page write. All functions run on core 0 (loop()).
 */

#include <stddef.h>
#include <stdint.h>
#include <boot_record.h>
#include <watchdog_logic.h>
#include <web_app.h>
#include <fw_layout.h>

/// Largest update file accepted: the file header and both slot blobs
static const uint32_t FW_UPLOAD_MAX = 64u + 2u * FW_SLOT_SIZE;

/**
 * @brief Act on what the boot program left: read the boot record, rewrite it
 *        after a fallback, start the trial monitor on a trial boot
 *
 * Call in setup() after bootInfoBegin() and watchdogBegin() (flash writes feed
 * the watchdog, which must be the firmware's by then), before the network starts.
 */
void fwUpdateBegin();

/// Print the boot program's line ("Boot: slot A verified in <n> ms (normal)") and what fwUpdateBegin() did
void fwUpdatePrintBoot();

/**
 * @brief Trial monitor, crash counter, deferred record writes; call every loop() on core 0
 *
 * The first call also checks the other slot's signature (about a second, once).
 * @param netReady fwTrialNetReady(): link up and an IPv4 or a non-link-local IPv6 address
 */
void fwUpdatePoll(bool netReady);

/// A valid management request arrived (SNMP with the right community, an authenticated web request)
void fwUpdateNoteValidRequest();

/// Why an upload was not accepted (the web layer picks the HTTP status from it)
enum class FwUploadFault : uint8_t {
    None,
    BadFile,      ///< the file was refused (bad header, signature, length...): 400
    WrongKey,     ///< signed with another key than the one this firmware trusts: 403
    Busy,         ///< not now: on trial, uploading, restart pending: 409
    TooLarge,     ///< larger than FW_UPLOAD_MAX: 413
    DeviceError,  ///< the device failed to write flash or the boot record: 500
};

/**
 * @brief Start receiving an update file of contentLength bytes into the slot that is not running
 * @param error Set to a short reason on failure ("on trial", "upload in progress", ...)
 * @param fault Set to the kind of failure (None on success)
 * @return false when an upload cannot start now (nothing is written then)
 */
bool fwUpdateUploadBegin(uint32_t contentLength, const char*& error, FwUploadFault& fault);

/// The next bytes of the file; false once the file was refused (fwUpdateUploadFinish() then gives the reason)
bool fwUpdateUploadFeed(const uint8_t* data, size_t len);

/**
 * @brief All bytes are in: write the slot header (page 0) and verify the signature from flash
 *
 * On success the boot record is written with the new slot on trial; the
 * caller then restarts the device (a planned restart). On failure the other
 * slot reads as empty when anything was written to it.
 * @param versionOut 17 bytes: the new firmware version on success
 * @param error A short reason on failure, e.g. "bad signature"
 * @param fault The kind of failure (None on success)
 */
bool fwUpdateUploadFinish(char* versionOut, const char*& error, FwUploadFault& fault);

/// The connection was lost or timed out: end the upload (the other slot reads as empty if it was written)
void fwUpdateUploadAbort();

/**
 * @brief Make the other slot the one to boot, on trial; the caller restarts the device
 * @return false while on trial or uploading, or when the other slot does not verify
 */
bool fwUpdateSwitchSlot();

/// Everything the Firmware web page shows (strings always NUL terminated)
void fwUpdateStatus(WebFirmware& out);

/// What the SNMP agent reports (fwUpdate group)
struct FwSnmpState {
    uint8_t runningSlot;        ///< 0 = slot A, 1 = slot B
    bool trial;                 ///< the running firmware is on trial
    char slotVersion[2][17];    ///< version of each slot, empty when it does not verify
    FwResult lastResult;        ///< outcome of the last update (boot record)
};

/// Fill out for the SNMP agent
void fwUpdateSnmpState(FwSnmpState& out);

/**
 * @brief The pending trap (an update kept or rolled back), if any; stays pending until fwUpdateTrapSent()
 * @param result The outcome the trap is about (Installed = kept, else rolled back)
 * @return false when none is pending
 */
bool fwUpdatePendingTrap(FwResult& result);

/// The pending trap was sent: it is not pending any more (the boot record is marked at the next fwUpdatePoll())
void fwUpdateTrapSent();

/// Why the device started, from the boot program (Planned -> Reboot, Crash -> Watchdog); Unknown without boot info
RestartCause fwUpdateRestartCause();

/**
 * @brief The boot record's last update result and its version (update server check-ins)
 * @param version 17 bytes, empty when none
 */
void fwUpdateLastRecord(FwResult& result, char* version);

/// Short text for a result, e.g. "installed 3.3.0", "rolled back (crash) 3.4.0" or "none"
void fwUpdateResultText(FwResult result, const char* version, char* out, size_t size);
