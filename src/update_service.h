#pragma once

/**
 * @file update_service.h
 * @brief Check-ins with the update server and the firmware downloads it asks for.
 *
 * With an update server URL (stored, or from DHCP option 225) and a shared key
 * (lib/settings update_settings_store.h), the device checks in 60 s after its
 * network is ready and then every 30 minutes (lib/update_client
 * CheckinScheduler). A check-in reports serial, hostname, addresses, version,
 * slot, trial and update state, authenticated with the key; only a reply whose
 * MAC is right is acted on. Such a reply also confirms a firmware on trial.
 * When the reply names a release, the device downloads it from the same server
 * and feeds it to the existing update path (fwUpdateUploadBegin/Feed/Finish):
 * signature check, write to the other slot, restart on trial. What went wrong
 * is reported at the next check-in. Core 0 only; never blocks (src/http_fetch.h).
 */

#include <stddef.h>
#include <stdint.h>
#include <update_url.h>

/// What the status screen and web page show
struct UpdateServiceStatus {
    char urlInUse[UPDATE_URL_MAX + 1];  ///< The URL check-ins go to, empty when none
    bool fromDhcp;                      ///< The URL source setting is DHCP option 225
    bool hasKey;                        ///< A shared key is set
    bool downloading;                   ///< A firmware download is in progress
    bool checkedIn;                     ///< At least one check-in ended since boot
    uint32_t lastCheckinAgeS;           ///< Seconds since the last check-in ended (when checkedIn)
    char lastResult[48];                ///< "off", "no key", "ok", "update 3.4.1", "badauth", "connect timeout", ...
};

/// Call in setup() after loadUpdateSettings()
void updateServiceBegin();

/**
 * @brief Run check-ins and downloads; call every loop() pass on core 0
 * @param netReady Link up and an address (as for the trial monitor)
 */
void updateServicePoll(bool netReady);

/// Check in now (hidden menu command "checkin", for tests); ignored without URL, key or network
void updateServiceForceCheckin();

/// Fill out for the status screen and web page
void updateServiceStatus(UpdateServiceStatus& out);

/**
 * @brief The check-ins in words: "off", "no key", "bad url", "waiting for the first check-in",
 *        "downloading", or "last check-in 3 min ago: ok"
 */
void updateServiceStateText(char* out, size_t size);

/**
 * @brief Check, apply and save update server settings (serial menu, web page); active at once
 *
 * Takes effect at once: the first check-in with the new settings follows 60 s later.
 * @param keyHex 64 hex characters, or empty to keep the key
 * @param clearKey Remove the key
 * @param error On false: a sentence for the user; "Saving failed, nothing changed." when the flash write failed
 * @return false when a value is refused or saving failed (the settings in use are then unchanged)
 */
bool updateServiceSave(const char* url, bool fromDhcp, const char* keyHex, bool clearKey, const char*& error);
