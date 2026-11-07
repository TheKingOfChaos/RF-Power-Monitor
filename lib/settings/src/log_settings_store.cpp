#include "log_settings_store.h"

#include <LittleFS.h>

#include <new>

#include "ab_files.h"

const char* const LOG_SETTINGS_FILE_A = "data/timelog_a.json";
const char* const LOG_SETTINGS_FILE_B = "data/timelog_b.json";

LogSettings currentLogSettings;

static LogSettingsStore defaultStore(LOG_SETTINGS_FILE_A, LOG_SETTINGS_FILE_B);

/// Work buffers for one load or save, on the heap only while it runs (boot, a settings change)
struct LogWork {
    char json[LOG_SETTINGS_JSON_MAX];
    LogSettings a, b;
};

bool LogSettingsStore::readSlot(const char* path, char* json, LogSettings& out) {
    bool tooBig;
    if (!abReadFile(path, json, LOG_SETTINGS_JSON_MAX, tooBig)) return false;
    if (tooBig || !logSettingsFromJson(json, out)) {
        Serial.printf("Time and syslog settings %s: invalid, ignored\r\n", path);
        return false;
    }
    return true;
}

SettingsSlot LogSettingsStore::load(LogSettings& out) {
    LogWork* w = new (std::nothrow) LogWork;
    if (w == nullptr) {
        logSettingsDefaults(out);
        _active = SettingsSlot::None;
        return _active;
    }
    bool aValid = readSlot(_pathA, w->json, w->a);
    bool bValid = readSlot(_pathB, w->json, w->b);
    _active = chooseActiveSlot(aValid, w->a.sequence, bValid, w->b.sequence);
    switch (_active) {
    case SettingsSlot::A: out = w->a; break;
    case SettingsSlot::B: out = w->b; break;
    default:              logSettingsDefaults(out); break;
    }
    delete w;
    _activeSequence = out.sequence;
    return _active;
}

bool LogSettingsStore::save(LogSettings& s) {
    LogWork* w = new (std::nothrow) LogWork;
    if (w == nullptr) return false;
    SettingsSlot target = nextWriteSlot(_active);
    const char* path = pathFor(target);
    LogSettings& pending = w->a;
    LogSettings& verify = w->b;
    pending = s;
    pending.version = LOG_SETTINGS_VERSION;
    pending.sequence = (_active == SettingsSlot::None) ? 1 : _activeSequence + 1;

    size_t len = logSettingsToJson(pending, w->json, sizeof(w->json));
    bool ok = len != 0 && abWriteFile("Time and syslog settings", path, w->json, len);
    if (ok && (!readSlot(path, w->json, verify) || !logSettingsEqual(verify, pending) ||
               verify.sequence != pending.sequence)) {
        Serial.printf("Time and syslog settings %s: verification failed, keeping previous copy\r\n", path);
        ok = false;
    }
    if (ok) {
        _active = target;
        _activeSequence = pending.sequence;
        s = pending;
    }
    delete w;
    return ok;
}

bool LogSettingsStore::clear() {
    bool ok = true;
    const char* paths[] = {_pathA, _pathB};
    for (const char* path : paths) {
        if (LittleFS.exists(path) && !LittleFS.remove(path)) ok = false;
    }
    _active = SettingsSlot::None;
    _activeSequence = 0;
    return ok;
}

void loadLogSettings() {
    SettingsSlot slot = defaultStore.load(currentLogSettings);
    if (slot != SettingsSlot::None) {
        Serial.printf("Time and syslog settings: loaded (sequence %lu)\r\n",
                      (unsigned long)currentLogSettings.sequence);
    }
}

bool saveLogSettings() {
    return defaultStore.save(currentLogSettings);
}

bool resetLogSettings(LogSettingsStore& store, LogSettings& out) {
    logSettingsDefaults(out);
    return store.clear();
}

bool factoryResetLogSettings() {
    return resetLogSettings(defaultStore, currentLogSettings);
}
