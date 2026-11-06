#include "update_settings.h"

#include <ArduinoJson.h>
#include <string.h>

static const size_t JSON_FIELDS = 8;  // 5 used
static const size_t JSON_DOC_CAPACITY = UPDATE_SETTINGS_JSON_MAX + JSON_OBJECT_SIZE(JSON_FIELDS);

void updateSettingsDefaults(UpdateSettings& s) {
    memset(&s, 0, sizeof(s));
    s.version = UPDATE_SETTINGS_VERSION;
}

size_t updateSettingsToJson(const UpdateSettings& s, char* out, size_t size) {
    if (out == nullptr || size == 0) return 0;
    char keyHex[2 * UPDATE_KEY_LEN + 1] = "";
    if (s.hasKey) {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < UPDATE_KEY_LEN; i++) {
            keyHex[2 * i] = digits[s.key[i] >> 4];
            keyHex[2 * i + 1] = digits[s.key[i] & 0x0F];
        }
        keyHex[2 * UPDATE_KEY_LEN] = '\0';
    }
    StaticJsonDocument<JSON_OBJECT_SIZE(JSON_FIELDS)> doc;  // values are referenced, not copied
    doc["version"] = UPDATE_SETTINGS_VERSION;
    doc["sequence"] = s.sequence;
    doc["url"] = (const char*)s.url;
    doc["fromDhcp"] = s.fromDhcp;
    doc["key"] = (const char*)keyHex;
    if (measureJson(doc) + 1 > size) {
        out[0] = '\0';
        return 0;
    }
    return serializeJson(doc, out, size);
}

bool updateSettingsFromJson(const char* json, UpdateSettings& out) {
    updateSettingsDefaults(out);
    if (json == nullptr) return false;
    DynamicJsonDocument doc(JSON_DOC_CAPACITY);  // heap: keeps core 0's stack small
    if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
    if (!doc["version"].is<uint32_t>() || doc["version"].as<uint32_t>() != UPDATE_SETTINGS_VERSION) return false;
    if (!doc["sequence"].is<uint32_t>() || !doc["url"].is<const char*>() || !doc["fromDhcp"].is<bool>() ||
        !doc["key"].is<const char*>()) {
        return false;
    }
    const char* url = doc["url"];
    const char* key = doc["key"];
    if (strlen(url) > UPDATE_URL_MAX) return false;
    UpdateUrl parsed;
    if (url[0] != '\0' && !parseUpdateUrl(url, parsed)) return false;
    if (key[0] != '\0' && !parseKeyHex(key, out.key)) return false;
    strcpy(out.url, url);
    out.hasKey = key[0] != '\0';
    out.fromDhcp = doc["fromDhcp"];
    out.sequence = doc["sequence"];
    return true;
}

bool updateSettingsApply(UpdateSettings& s, const char* url, bool fromDhcp, const char* keyHex, bool clearKey,
                         const char*& error) {
    UpdateUrl parsed;
    if (url == nullptr || strlen(url) > UPDATE_URL_MAX || (url[0] != '\0' && !parseUpdateUrl(url, parsed))) {
        error = "The URL must be empty or http://host[:port][/path], at most 127 characters.";
        return false;
    }
    uint8_t key[UPDATE_KEY_LEN];
    bool newKey = keyHex != nullptr && keyHex[0] != '\0';
    if (keyHex == nullptr || (newKey && !clearKey && !parseKeyHex(keyHex, key))) {
        error = "The key must be 64 hex characters from the update site.";
        return false;
    }
    strcpy(s.url, url);
    s.fromDhcp = fromDhcp;
    if (clearKey) {
        memset(s.key, 0, sizeof(s.key));
        s.hasKey = false;
    } else if (newKey) {
        memcpy(s.key, key, sizeof(s.key));
        s.hasKey = true;
    }
    memset(key, 0, sizeof(key));
    return true;
}

bool updateSettingsEqual(const UpdateSettings& a, const UpdateSettings& b) {
    if (strcmp(a.url, b.url) != 0 || a.fromDhcp != b.fromDhcp || a.hasKey != b.hasKey) return false;
    return !a.hasKey || memcmp(a.key, b.key, UPDATE_KEY_LEN) == 0;
}
