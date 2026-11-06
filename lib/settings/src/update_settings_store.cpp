#include "update_settings_store.h"

#include <LittleFS.h>

#include <new>

#include <monocypher.h>

#include "ab_files.h"

const char* const UPDATE_SETTINGS_FILE_A = "data/update_a.json";
const char* const UPDATE_SETTINGS_FILE_B = "data/update_b.json";

UpdateSettings currentUpdateSettings;

static UpdateSettingsStore defaultStore(UPDATE_SETTINGS_FILE_A, UPDATE_SETTINGS_FILE_B);

/// Work buffers for one load or save, on the heap only while it runs: static RAM
/// is near its 50 % limit, and these are needed rarely (boot, a settings change)
struct UpdateWork {
    char json[UPDATE_SETTINGS_JSON_MAX];
    UpdateSettings a, b;
};

bool UpdateSettingsStore::readSlot(const char* path, char* json, UpdateSettings& out) {
    bool tooBig;
    if (!abReadFile(path, json, UPDATE_SETTINGS_JSON_MAX, tooBig)) return false;
    if (tooBig || !updateSettingsFromJson(json, out)) {
        Serial.printf("Update settings %s: invalid, ignored\r\n", path);
        return false;
    }
    return true;
}

SettingsSlot UpdateSettingsStore::load(UpdateSettings& out) {
    UpdateWork* w = new (std::nothrow) UpdateWork;
    if (w == nullptr) {
        updateSettingsDefaults(out);
        _active = SettingsSlot::None;
        return _active;
    }
    bool aValid = readSlot(_pathA, w->json, w->a);
    bool bValid = readSlot(_pathB, w->json, w->b);
    _active = chooseActiveSlot(aValid, w->a.sequence, bValid, w->b.sequence);
    switch (_active) {
    case SettingsSlot::A: out = w->a; break;
    case SettingsSlot::B: out = w->b; break;
    default:              updateSettingsDefaults(out); break;
    }
    crypto_wipe(w, sizeof(*w));  // it held the key (not optimized away, unlike memset before delete)
    delete w;
    _activeSequence = out.sequence;
    return _active;
}

bool UpdateSettingsStore::save(UpdateSettings& s) {
    UpdateWork* w = new (std::nothrow) UpdateWork;
    if (w == nullptr) return false;
    SettingsSlot target = nextWriteSlot(_active);
    const char* path = pathFor(target);
    UpdateSettings& pending = w->a;
    UpdateSettings& verify = w->b;
    pending = s;
    pending.version = UPDATE_SETTINGS_VERSION;
    pending.sequence = (_active == SettingsSlot::None) ? 1 : _activeSequence + 1;

    size_t len = updateSettingsToJson(pending, w->json, sizeof(w->json));
    bool ok = len != 0 && abWriteFile("Update settings", path, w->json, len);
    if (ok && (!readSlot(path, w->json, verify) || !updateSettingsEqual(verify, pending) ||
               verify.sequence != pending.sequence)) {
        Serial.printf("Update settings %s: verification failed, keeping previous copy\r\n", path);
        ok = false;
    }
    if (ok) {
        _active = target;
        _activeSequence = pending.sequence;
        s = pending;
    }
    crypto_wipe(w, sizeof(*w));  // it held the key (not optimized away, unlike memset before delete)
    delete w;
    return ok;
}

bool UpdateSettingsStore::clear() {
    bool ok = true;
    const char* paths[] = {_pathA, _pathB};
    for (const char* path : paths) {
        if (LittleFS.exists(path) && !LittleFS.remove(path)) ok = false;
    }
    _active = SettingsSlot::None;
    _activeSequence = 0;
    return ok;
}

void loadUpdateSettings() {
    SettingsSlot slot = defaultStore.load(currentUpdateSettings);
    if (slot != SettingsSlot::None) {
        Serial.printf("Update settings: loaded (sequence %lu)\r\n", (unsigned long)currentUpdateSettings.sequence);
    }
}

bool saveUpdateSettings() {
    return defaultStore.save(currentUpdateSettings);
}

bool clearUpdateSettings() {
    updateSettingsDefaults(currentUpdateSettings);
    return defaultStore.clear();
}
