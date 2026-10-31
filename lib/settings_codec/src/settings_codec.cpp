#include "settings_codec.h"

#include <ArduinoJson.h>
#include <string.h>

#include "netconfig.h"

// Factory defaults
static const char* const DEFAULT_COMMUNITY = "public";
static const char* const DEFAULT_STATIC_IP = "192.168.1.100";
static const char* const DEFAULT_SUBNET_MASK = "255.255.255.0";
static const char* const DEFAULT_GATEWAY = "192.168.1.1";
static const char* const DEFAULT_SYS_NAME = "rf-ups-monitor";

// Room for the parsed document: the object, its members and copies of all strings
static const size_t JSON_FIELDS = 20;  // 17 used, room for a few more
static const size_t JSON_DOC_CAPACITY = SETTINGS_JSON_MAX + JSON_OBJECT_SIZE(JSON_FIELDS);

/**
 * @brief Copy a string into a fixed buffer, failing instead of truncating
 */
static bool copyField(char* dest, size_t size, const char* src) {
    size_t len = strlen(src);
    if (len >= size) return false;
    memcpy(dest, src, len + 1);
    return true;
}

bool settingsTextIsValid(const char* text, size_t maxLength, bool allowEmpty) {
    if (text == nullptr) return false;
    size_t len = 0;
    for (const char* c = text; *c; c++, len++) {
        if (len >= maxLength) return false;
        if (*c < 0x20 || *c > 0x7E) return false;
    }
    return allowEmpty || len > 0;
}

void settingsDefaults(Settings& s) {
    memset(&s, 0, sizeof(s));
    copyField(s.community, sizeof(s.community), DEFAULT_COMMUNITY);
    s.dhcpEnabled = true;
    copyField(s.staticIP, sizeof(s.staticIP), DEFAULT_STATIC_IP);
    copyField(s.subnetMask, sizeof(s.subnetMask), DEFAULT_SUBNET_MASK);
    copyField(s.gateway, sizeof(s.gateway), DEFAULT_GATEWAY);
    copyField(s.sysName, sizeof(s.sysName), DEFAULT_SYS_NAME);
    s.trapVersion = TRAP_VERSION_V2C;
    s.trapFromDhcp = false;
    s.webEnabled = true;
    s.version = SETTINGS_VERSION;
    s.sequence = 0;
}

size_t settingsToJson(const Settings& s, char* buf, size_t size) {
    // Strings are stored by pointer, so the document only needs the object
    StaticJsonDocument<JSON_OBJECT_SIZE(JSON_FIELDS)> doc;
    doc["community"] = (const char*)s.community;
    doc["dhcpEnabled"] = s.dhcpEnabled;
    doc["staticIP"] = (const char*)s.staticIP;
    doc["subnetMask"] = (const char*)s.subnetMask;
    doc["gateway"] = (const char*)s.gateway;
    doc["ipv6Address"] = (const char*)s.ipv6Address;
    doc["ipv6Gateway"] = (const char*)s.ipv6Gateway;
    doc["sysName"] = (const char*)s.sysName;
    doc["sysContact"] = (const char*)s.sysContact;
    doc["sysLocation"] = (const char*)s.sysLocation;
    doc["trapTarget"] = (const char*)s.trapTarget;
    doc["trapVersion"] = s.trapVersion;
    doc["trapFromDhcp"] = s.trapFromDhcp;
    doc["webEnabled"] = s.webEnabled;
    doc["webPassword"] = (const char*)s.webPassword;
    doc["version"] = s.version;
    doc["sequence"] = s.sequence;

    if (buf == nullptr || doc.overflowed() || measureJson(doc) + 1 > size) return 0;
    return serializeJson(doc, buf, size);
}

/**
 * @brief "<32 lower-case hex>$<64 lower-case hex>", the form web_auth stores
 */
static bool webPasswordHashFormatOk(const char* text) {
    if (strlen(text) != SETTINGS_WEB_PASSWORD_LEN) return false;
    for (size_t i = 0; i < SETTINGS_WEB_PASSWORD_LEN; i++) {
        char c = text[i];
        bool ok = i == 32 ? c == '$' : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!ok) return false;
    }
    return true;
}

/**
 * @brief Read an optional text field into dest, keeping dest's current value if absent
 */
static SettingsParseError readOptionalText(JsonVariantConst value, char* dest, size_t size) {
    if (value.isNull()) return SettingsParseError::None;
    if (!value.is<const char*>()) return SettingsParseError::MissingField;
    const char* text = value.as<const char*>();
    if (!copyField(dest, size, text)) return SettingsParseError::FieldTooLong;
    if (!settingsTextIsValid(dest, size - 1, true)) return SettingsParseError::BadSysText;
    return SettingsParseError::None;
}

SettingsParseError settingsFromJson(const char* json, Settings& out) {
    if (json == nullptr || json[0] == '\0') return SettingsParseError::Empty;

    // On the heap: the document holds copies of every string (~1 KB)
    DynamicJsonDocument doc(JSON_DOC_CAPACITY);
    if (deserializeJson(doc, json) || !doc.is<JsonObject>()) return SettingsParseError::InvalidJson;

    if (!doc["version"].is<uint32_t>() || doc["version"].as<uint32_t>() != SETTINGS_VERSION) {
        return SettingsParseError::WrongVersion;
    }

    if (!doc["community"].is<const char*>() || !doc["dhcpEnabled"].is<bool>() ||
        !doc["staticIP"].is<const char*>() || !doc["subnetMask"].is<const char*>() ||
        !doc["gateway"].is<const char*>()) {
        return SettingsParseError::MissingField;
    }
    if (!doc["sequence"].isNull() && !doc["sequence"].is<uint32_t>()) {
        return SettingsParseError::MissingField;
    }

    // Start from defaults so optional fields missing in older files get them.
    // Static: ~400 bytes is too much for core 0's stack. Not reentrant, but
    // settings are only ever parsed on core 0.
    static Settings s;
    settingsDefaults(s);
    if (!copyField(s.community, sizeof(s.community), doc["community"]) ||
        !copyField(s.staticIP, sizeof(s.staticIP), doc["staticIP"]) ||
        !copyField(s.subnetMask, sizeof(s.subnetMask), doc["subnetMask"]) ||
        !copyField(s.gateway, sizeof(s.gateway), doc["gateway"])) {
        return SettingsParseError::FieldTooLong;
    }

    if (!settingsTextIsValid(s.community, COMMUNITY_MAX_LENGTH, false)) return SettingsParseError::BadCommunity;

    SettingsParseError err;
    if ((err = readOptionalText(doc["sysName"], s.sysName, sizeof(s.sysName))) != SettingsParseError::None ||
        (err = readOptionalText(doc["sysContact"], s.sysContact, sizeof(s.sysContact))) != SettingsParseError::None ||
        (err = readOptionalText(doc["sysLocation"], s.sysLocation, sizeof(s.sysLocation))) != SettingsParseError::None ||
        (err = readOptionalText(doc["trapTarget"], s.trapTarget, sizeof(s.trapTarget))) != SettingsParseError::None ||
        (err = readOptionalText(doc["ipv6Address"], s.ipv6Address, sizeof(s.ipv6Address))) != SettingsParseError::None ||
        (err = readOptionalText(doc["ipv6Gateway"], s.ipv6Gateway, sizeof(s.ipv6Gateway))) != SettingsParseError::None) {
        return err;
    }
    TrapAddress trapAddr;
    if (s.trapTarget[0] != '\0' && parseTrapTarget(s.trapTarget, trapAddr) != TrapTargetError::None) {
        return SettingsParseError::BadTrapTarget;
    }
    if (validateStaticIPv6(s.ipv6Address, s.ipv6Gateway) != StaticIPv6Error::None) return SettingsParseError::BadStaticIPv6;

    JsonVariantConst webPassword = doc["webPassword"];
    if (!webPassword.isNull()) {
        if (!webPassword.is<const char*>()) return SettingsParseError::MissingField;
        if (!copyField(s.webPassword, sizeof(s.webPassword), webPassword)) return SettingsParseError::FieldTooLong;
        if (s.webPassword[0] != '\0' && !webPasswordHashFormatOk(s.webPassword)) return SettingsParseError::BadWebPassword;
    }

    JsonVariantConst trapFromDhcp = doc["trapFromDhcp"], webEnabled = doc["webEnabled"];
    if ((!trapFromDhcp.isNull() && !trapFromDhcp.is<bool>()) || (!webEnabled.isNull() && !webEnabled.is<bool>())) {
        return SettingsParseError::MissingField;
    }
    if (!trapFromDhcp.isNull()) s.trapFromDhcp = trapFromDhcp.as<bool>();
    if (!webEnabled.isNull()) s.webEnabled = webEnabled.as<bool>();

    JsonVariantConst trapVersion = doc["trapVersion"];
    if (!trapVersion.isNull()) {
        if (!trapVersion.is<int>()) return SettingsParseError::BadTrapVersion;
        int v = trapVersion.as<int>();
        if (v != TRAP_VERSION_V1 && v != TRAP_VERSION_V2C) return SettingsParseError::BadTrapVersion;
        s.trapVersion = (uint8_t)v;
    }
    if (!trapVersionFitsTarget(s.trapTarget, s.trapFromDhcp, s.trapVersion)) return SettingsParseError::TrapIPv6NeedsV2c;

    s.dhcpEnabled = doc["dhcpEnabled"];
    if (!s.dhcpEnabled &&
        validateStaticConfig(s.staticIP, s.subnetMask, s.gateway) != StaticConfigError::None) {
        return SettingsParseError::BadStaticConfig;
    }

    s.version = SETTINGS_VERSION;
    s.sequence = doc["sequence"] | 0u;

    out = s;
    return SettingsParseError::None;
}

const char* settingsParseErrorMessage(SettingsParseError error) {
    switch (error) {
    case SettingsParseError::None:            return "OK";
    case SettingsParseError::Empty:           return "empty file";
    case SettingsParseError::InvalidJson:     return "invalid JSON";
    case SettingsParseError::WrongVersion:    return "wrong or missing version";
    case SettingsParseError::MissingField:    return "missing or mistyped field";
    case SettingsParseError::FieldTooLong:    return "field too long";
    case SettingsParseError::BadCommunity:    return "invalid community string";
    case SettingsParseError::BadStaticConfig: return "invalid static IP configuration";
    case SettingsParseError::BadSysText:      return "invalid sysName/sysContact/sysLocation";
    case SettingsParseError::BadTrapTarget:   return "invalid trap destination";
    case SettingsParseError::BadTrapVersion:  return "invalid trap version";
    case SettingsParseError::BadWebPassword:  return "invalid web password hash";
    case SettingsParseError::BadStaticIPv6:   return "invalid static IPv6 configuration";
    case SettingsParseError::TrapIPv6NeedsV2c: return "IPv6 trap destination needs trap version 2c";
    }
    return "unknown error";
}

bool trapVersionFitsTarget(const char* trapTarget, bool trapFromDhcp, uint8_t trapVersion) {
    TrapAddress t;
    bool fixedIPv6 = !trapFromDhcp && parseTrapTarget(trapTarget, t) == TrapTargetError::None && t.ipv6;
    return !fixedIPv6 || trapVersion == TRAP_VERSION_V2C;
}

bool settingsEqual(const Settings& a, const Settings& b) {
    return strcmp(a.community, b.community) == 0 && a.dhcpEnabled == b.dhcpEnabled &&
           strcmp(a.staticIP, b.staticIP) == 0 && strcmp(a.subnetMask, b.subnetMask) == 0 &&
           strcmp(a.gateway, b.gateway) == 0 && strcmp(a.ipv6Address, b.ipv6Address) == 0 &&
           strcmp(a.ipv6Gateway, b.ipv6Gateway) == 0 && strcmp(a.sysName, b.sysName) == 0 &&
           strcmp(a.sysContact, b.sysContact) == 0 && strcmp(a.sysLocation, b.sysLocation) == 0 &&
           strcmp(a.trapTarget, b.trapTarget) == 0 && a.trapVersion == b.trapVersion &&
           a.trapFromDhcp == b.trapFromDhcp && a.webEnabled == b.webEnabled &&
           strcmp(a.webPassword, b.webPassword) == 0 &&
           a.version == b.version && a.sequence == b.sequence;
}

bool sequenceIsNewer(uint32_t a, uint32_t b) {
    // Serial number arithmetic: a is newer if it is ahead by less than half the range
    return (int32_t)(a - b) > 0;
}

SettingsSlot chooseActiveSlot(bool aValid, uint32_t aSequence, bool bValid, uint32_t bSequence) {
    if (aValid && bValid) return sequenceIsNewer(bSequence, aSequence) ? SettingsSlot::B : SettingsSlot::A;
    if (aValid) return SettingsSlot::A;
    if (bValid) return SettingsSlot::B;
    return SettingsSlot::None;
}

SettingsSlot nextWriteSlot(SettingsSlot active) {
    return active == SettingsSlot::A ? SettingsSlot::B : SettingsSlot::A;
}
