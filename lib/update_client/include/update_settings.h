#pragma once

/**
 * @file update_settings.h
 * @brief The update server settings (URL, URL from DHCP, shared key) as JSON.
 *
 * Kept in their own A/B file pair (lib/settings UpdateSettingsStore), not in
 * the main settings file: that file is near its 1024-byte limit, which older
 * firmware also enforces, so it must not grow. Older firmware ignores these files.
 * Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

#include "update_protocol.h"
#include "update_url.h"

/// Layout version of the update settings file
constexpr uint32_t UPDATE_SETTINGS_VERSION = 1;

/// Largest update settings document, including the terminating NUL
constexpr size_t UPDATE_SETTINGS_JSON_MAX = 512;

/// Update server settings
struct UpdateSettings {
    char url[UPDATE_URL_MAX + 1];  ///< Stored URL, empty = no check-ins (unless from DHCP)
    bool fromDhcp;                 ///< true: the URL from DHCP option 225 replaces url
    bool hasKey;                   ///< A shared key is set
    uint8_t key[UPDATE_KEY_LEN];   ///< The shared key (only when hasKey)
    uint32_t version;              ///< Layout version, must equal UPDATE_SETTINGS_VERSION
    uint32_t sequence;             ///< Save counter, newest valid copy wins on load
};

/// Defaults: no URL, not from DHCP, no key (check-ins off)
void updateSettingsDefaults(UpdateSettings& s);

/**
 * @brief Serialize to JSON: version, sequence, url, fromDhcp, key (64 hex or "")
 * @return Length written (out is NUL terminated), 0 if it does not fit
 */
size_t updateSettingsToJson(const UpdateSettings& s, char* out, size_t size);

/**
 * @brief Parse and validate a JSON document
 *
 * Every field is required and typed; the URL must be empty or pass
 * parseUpdateUrl(), the key empty or 64 hex characters.
 * @return false (out unspecified) for anything else
 */
bool updateSettingsFromJson(const char* json, UpdateSettings& out);

/**
 * @brief Apply a change entered in the serial menu or on the web page
 * @param url Empty (no stored URL) or a URL parseUpdateUrl() accepts
 * @param keyHex 64 hex characters, or empty to keep the key
 * @param clearKey Remove the key (wins over keyHex); its bytes are wiped
 * @param error On false: a sentence for the user
 * @return false (s unchanged) when the URL or the key is refused
 */
bool updateSettingsApply(UpdateSettings& s, const char* url, bool fromDhcp, const char* keyHex, bool clearKey,
                         const char*& error);

/// Same URL, DHCP flag and key (version and sequence are not compared)
bool updateSettingsEqual(const UpdateSettings& a, const UpdateSettings& b);
