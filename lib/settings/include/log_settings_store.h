#pragma once

/**
 * @file log_settings_store.h
 * @brief The NTP and syslog server settings on LittleFS.
 *
 * Two copies (A/B) with a sequence number, like the update server settings
 * (update_settings_store.h), in files of their own: the main settings file is
 * full, and 3.5.1 neither reads nor needs these. The pure codec is lib/syslog
 * log_settings.h.
 */

#include <Arduino.h>
#include <log_settings.h>
#include <settings_codec.h>

/// Default paths of the two copies
extern const char* const LOG_SETTINGS_FILE_A;
extern const char* const LOG_SETTINGS_FILE_B;

/**
 * @brief Reads and writes the two copies at a pair of paths
 *
 * Production code uses the default paths; tests use their own.
 */
class LogSettingsStore {
public:
    LogSettingsStore(const char* pathA, const char* pathB) : _pathA(pathA), _pathB(pathB) {}

    /**
     * @brief Load the newest valid copy (LittleFS must be mounted)
     * @param out Receives it, or the defaults (both from DHCP) if no copy is valid
     * @return The slot loaded from, or SettingsSlot::None
     */
    SettingsSlot load(LogSettings& out);

    /**
     * @brief Save to the inactive copy, read it back, then make it active
     * @param s Saved; its sequence and version are updated on success
     */
    bool save(LogSettings& s);

    /// Remove both copies
    bool clear();

private:
    bool readSlot(const char* path, char* json, LogSettings& out);
    const char* pathFor(SettingsSlot slot) const { return slot == SettingsSlot::B ? _pathB : _pathA; }

    const char* _pathA;
    const char* _pathB;
    SettingsSlot _active = SettingsSlot::None;
    uint32_t _activeSequence = 0;
};

/// NTP and syslog server settings in use by the firmware
extern LogSettings currentLogSettings;

/// Load currentLogSettings from the default store (defaults if none is valid; nothing is written)
void loadLogSettings();

/// Save currentLogSettings to the default store
bool saveLogSettings();

/// Factory reset of one store: remove both copies, `out` = defaults (both from DHCP)
bool resetLogSettings(LogSettingsStore& store, LogSettings& out);

/// resetLogSettings on the default store, into currentLogSettings
bool factoryResetLogSettings();
