#include "settings.h"

#include <LittleFS.h>

const char* const SETTINGS_FILE_A = "data/settings_a.json";
const char* const SETTINGS_FILE_B = "data/settings_b.json";

// Files from older firmware, removed on load
static const char* const LEGACY_FILES[] = {
    "data/active_file",     // v3.0 pointer to the active settings copy
    "config.bak",           // pre-v3 firmware
    "config.schema.json",   // pre-v3 firmware
};

Settings currentSettings;
bool settingsLoaded = false;

static SettingsStore defaultStore(SETTINGS_FILE_A, SETTINGS_FILE_B);

// Work buffers kept off the stack: core 0 has only 8 KB of stack, shared with
// USB, lwIP and W5500 interrupts, and saves are triggered deep in the menu.
// Settings are only loaded and saved from core 0, so one set is enough.
static char jsonBuf[SETTINGS_JSON_MAX];
static Settings slotA, slotB, pending, verify;

/**
 * @brief Name of a slot for log output
 */
static const char* slotName(SettingsSlot slot) {
    switch (slot) {
    case SettingsSlot::A: return "A";
    case SettingsSlot::B: return "B";
    default:              return "none";
    }
}

bool SettingsStore::readSlot(const char* path, Settings& out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    size_t len = f.readBytes(jsonBuf, sizeof(jsonBuf) - 1);
    bool tooBig = f.available() > 0;
    f.close();
    jsonBuf[len] = '\0';

    if (tooBig) {
        Serial.printf("Settings %s: file too large, ignored\r\n", path);
        return false;
    }

    SettingsParseError err = settingsFromJson(jsonBuf, out);
    if (err != SettingsParseError::None) {
        Serial.printf("Settings %s: %s, ignored\r\n", path, settingsParseErrorMessage(err));
        return false;
    }
    return true;
}

SettingsSlot SettingsStore::load(Settings& out) {
    bool aValid = readSlot(_pathA, slotA);
    bool bValid = readSlot(_pathB, slotB);

    _active = chooseActiveSlot(aValid, slotA.sequence, bValid, slotB.sequence);
    switch (_active) {
    case SettingsSlot::A: out = slotA; break;
    case SettingsSlot::B: out = slotB; break;
    default:              settingsDefaults(out); break;
    }
    _activeSequence = out.sequence;
    return _active;
}

bool SettingsStore::save(Settings& s) {
    SettingsSlot target = nextWriteSlot(_active);
    const char* path = pathFor(target);

    pending = s;
    pending.version = SETTINGS_VERSION;
    pending.sequence = (_active == SettingsSlot::None) ? 1 : _activeSequence + 1;

    size_t len = settingsToJson(pending, jsonBuf, sizeof(jsonBuf));
    if (len == 0) {
        Serial.println("Settings: serialization failed");
        return false;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("Settings %s: cannot open for writing\r\n", path);
        return false;
    }
    size_t written = f.write((const uint8_t*)jsonBuf, len);
    f.close();
    if (written != len) {
        Serial.printf("Settings %s: short write (%u of %u bytes)\r\n", path, (unsigned)written, (unsigned)len);
        return false;
    }

    // Only switch to the new copy once it reads back identical
    if (!readSlot(path, verify) || !settingsEqual(verify, pending)) {
        Serial.printf("Settings %s: verification failed, keeping previous copy\r\n", path);
        return false;
    }

    _active = target;
    _activeSequence = pending.sequence;
    s = pending;
    return true;
}

uint8_t removeLegacyFiles() {
    uint8_t removed = 0;
    for (const char* path : LEGACY_FILES) {
        if (LittleFS.exists(path) && LittleFS.remove(path)) {
            Serial.printf("Settings: removed old file %s\r\n", path);
            removed++;
        }
    }
    return removed;
}

bool loadSettings() {
    SettingsSlot slot = defaultStore.load(currentSettings);
    removeLegacyFiles();

    if (slot == SettingsSlot::None) {
        Serial.println("Settings: no valid copy, saving factory defaults");
        settingsLoaded = saveSettings();
        return settingsLoaded;
    }

    Serial.printf("Settings: loaded slot %s (sequence %lu), %s\r\n", slotName(slot),
                  (unsigned long)currentSettings.sequence,
                  currentSettings.dhcpEnabled ? "DHCP" : "static IP");
    settingsLoaded = true;
    return true;
}

bool saveSettings() {
    bool ok = defaultStore.save(currentSettings);
    if (ok) {
        Serial.printf("Settings: saved slot %s (sequence %lu)\r\n", slotName(defaultStore.activeSlot()),
                      (unsigned long)currentSettings.sequence);
    }
    return ok;
}
