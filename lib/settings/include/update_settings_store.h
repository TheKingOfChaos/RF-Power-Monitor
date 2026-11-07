#pragma once

/**
 * @file update_settings_store.h
 * @brief The update server settings (URL, URL from DHCP, shared key) on LittleFS.
 *
 * Two copies (A/B) with a sequence number, written and read back like the main
 * settings (settings.h), but in files of their own: older firmware neither reads
 * nor needs them, and the main settings file keeps its size. The pure codec is
 * lib/update_client update_settings.h.
 */

#include <Arduino.h>
#include <settings_codec.h>
#include <update_settings.h>

/// Default paths of the two copies
extern const char* const UPDATE_SETTINGS_FILE_A;
extern const char* const UPDATE_SETTINGS_FILE_B;

/**
 * @brief Reads and writes the two update settings copies at a pair of paths
 *
 * Production code uses the default paths; tests use their own.
 */
class UpdateSettingsStore {
public:
    UpdateSettingsStore(const char* pathA, const char* pathB) : _pathA(pathA), _pathB(pathB) {}

    /**
     * @brief Load the newest valid copy (LittleFS must be mounted)
     * @param out Receives it, or the defaults (check-ins off) if no copy is valid
     * @return The slot loaded from, or SettingsSlot::None
     */
    SettingsSlot load(UpdateSettings& out);

    /**
     * @brief Save to the inactive copy, read it back, then make it active
     * @param s Saved; its sequence and version are updated on success
     */
    bool save(UpdateSettings& s);

    /// Remove both copies (factory reset: the key must not stay in flash)
    bool clear();

private:
    bool readSlot(const char* path, char* json, UpdateSettings& out);
    const char* pathFor(SettingsSlot slot) const { return slot == SettingsSlot::B ? _pathB : _pathA; }

    const char* _pathA;
    const char* _pathB;
    SettingsSlot _active = SettingsSlot::None;
    uint32_t _activeSequence = 0;
};

/// Update server settings in use by the firmware
extern UpdateSettings currentUpdateSettings;

/// Load currentUpdateSettings from the default store (defaults if none is valid; nothing is written)
void loadUpdateSettings();

/// Save currentUpdateSettings to the default store
bool saveUpdateSettings();

/// Remove the stored update settings and reset currentUpdateSettings to the defaults
bool clearUpdateSettings();

/// Paths of the bench defaults (hidden menu command updatedefault): kept through a factory reset
extern const char* const UPDATE_BENCH_DEFAULTS_FILE_A;
extern const char* const UPDATE_BENCH_DEFAULTS_FILE_B;

/**
 * @brief Update settings after a factory reset: the bench defaults when stored, else none
 *
 * The current copies are always removed (a key set in the field does not stay in
 * flash); with bench defaults those are saved as the new current settings.
 * @param current  Store of the settings in use
 * @param defaults Store of the bench defaults (left unchanged)
 * @param out      Receives the settings now in use
 * @return false if removing or saving failed
 */
bool factoryResetUpdateSettings(UpdateSettingsStore& current, UpdateSettingsStore& defaults, UpdateSettings& out);

/// factoryResetUpdateSettings on the default stores, into currentUpdateSettings (factory reset)
bool resetUpdateSettingsToBenchDefaults();

/**
 * @brief Keep currentUpdateSettings as the bench defaults (hidden command updatedefault)
 *
 * With no URL, DHCP or key set, the bench defaults are removed instead.
 */
bool saveUpdateBenchDefaults();

/// The bench defaults into out; false (out = defaults) if none are stored
bool loadUpdateBenchDefaults(UpdateSettings& out);
