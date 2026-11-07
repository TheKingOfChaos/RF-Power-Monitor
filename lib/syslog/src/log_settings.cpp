#include "log_settings.h"

#include <ArduinoJson.h>
#include <netconfig.h>
#include <string.h>

static const size_t JSON_FIELDS = 10;  // 6 used
static const size_t JSON_DOC_CAPACITY = LOG_SETTINGS_JSON_MAX + JSON_OBJECT_SIZE(JSON_FIELDS);

namespace {

const char* modeName(ServerMode m) {
    switch (m) {
        case ServerMode::Typed: return "typed";
        case ServerMode::None: return "none";
        case ServerMode::Dhcp: break;
    }
    return "dhcp";
}

bool modeFromName(const char* name, ServerMode& out) {
    if (strcmp(name, "dhcp") == 0) {
        out = ServerMode::Dhcp;
    } else if (strcmp(name, "typed") == 0) {
        out = ServerMode::Typed;
    } else if (strcmp(name, "none") == 0) {
        out = ServerMode::None;
    } else {
        return false;
    }
    return true;
}

/// A decimal port 1-65535, the whole string
bool parsePort(const char* s, uint16_t& out) {
    if (*s == '\0') return false;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    out = (uint16_t)v;
    return true;
}

bool isLabelChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

/// One server field: Typed needs a valid value, the others an empty one
bool serverFieldValid(ServerMode mode, const char* value, bool ntp) {
    if (mode != ServerMode::Typed) return value[0] == '\0';
    if (ntp) return ntpServerTextIsValid(value);
    SyslogTarget t;
    return parseSyslogTarget(value, t);
}

}  // namespace

void logSettingsDefaults(LogSettings& s) {
    memset(&s, 0, sizeof(s));
    s.ntpMode = ServerMode::Dhcp;
    s.syslogMode = ServerMode::Dhcp;
    s.version = LOG_SETTINGS_VERSION;
}

size_t logSettingsToJson(const LogSettings& s, char* out, size_t size) {
    if (out == nullptr || size == 0) return 0;
    StaticJsonDocument<JSON_OBJECT_SIZE(JSON_FIELDS)> doc;  // values are referenced, not copied
    doc["version"] = LOG_SETTINGS_VERSION;
    doc["sequence"] = s.sequence;
    doc["ntpMode"] = modeName(s.ntpMode);
    doc["ntp"] = (const char*)s.ntpServer;
    doc["syslogMode"] = modeName(s.syslogMode);
    doc["syslog"] = (const char*)s.syslogServer;
    if (measureJson(doc) + 1 > size) {
        out[0] = '\0';
        return 0;
    }
    return serializeJson(doc, out, size);
}

bool logSettingsFromJson(const char* json, LogSettings& out) {
    logSettingsDefaults(out);
    if (json == nullptr) return false;
    DynamicJsonDocument doc(JSON_DOC_CAPACITY);  // heap: keeps core 0's stack small
    if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
    if (!doc["version"].is<uint32_t>() || doc["version"].as<uint32_t>() != LOG_SETTINGS_VERSION) return false;
    if (!doc["sequence"].is<uint32_t>() || !doc["ntpMode"].is<const char*>() || !doc["ntp"].is<const char*>() ||
        !doc["syslogMode"].is<const char*>() || !doc["syslog"].is<const char*>()) {
        return false;
    }
    LogSettings s;
    logSettingsDefaults(s);
    const char* ntp = doc["ntp"];
    const char* syslog = doc["syslog"];
    if (!modeFromName(doc["ntpMode"], s.ntpMode) || !modeFromName(doc["syslogMode"], s.syslogMode)) return false;
    if (strlen(ntp) > LOG_HOST_MAX || strlen(syslog) > LOG_HOST_MAX) return false;
    if (!serverFieldValid(s.ntpMode, ntp, true) || !serverFieldValid(s.syslogMode, syslog, false)) return false;
    strcpy(s.ntpServer, ntp);
    strcpy(s.syslogServer, syslog);
    s.sequence = doc["sequence"];
    out = s;
    return true;
}

bool logSettingsEqual(const LogSettings& a, const LogSettings& b) {
    return a.ntpMode == b.ntpMode && a.syslogMode == b.syslogMode && strcmp(a.ntpServer, b.ntpServer) == 0 &&
           strcmp(a.syslogServer, b.syslogServer) == 0;
}

bool parseSyslogTarget(const char* text, SyslogTarget& out) {
    if (text == nullptr || text[0] == '\0' || strlen(text) > LOG_HOST_MAX) return false;
    SyslogTarget t;
    memset(&t, 0, sizeof(t));
    t.port = SYSLOG_DEFAULT_PORT;
    char host[LOG_HOST_MAX + 1];
    if (text[0] == '[') {
        const char* close = strchr(text, ']');
        if (close == nullptr) return false;
        size_t n = (size_t)(close - text - 1);
        memcpy(host, text + 1, n);
        host[n] = '\0';
        if (close[1] == ':') {
            if (!parsePort(close + 2, t.port)) return false;
        } else if (close[1] != '\0') {
            return false;
        }
        t.ipv6 = true;
        if (!parseIPv6(host, t.addr)) return false;
    } else if (strchr(text, ':') != strrchr(text, ':')) {  // two or more colons: a bare IPv6 address
        t.ipv6 = true;
        if (!parseIPv6(text, t.addr)) return false;
    } else {
        const char* colon = strchr(text, ':');
        size_t n = colon ? (size_t)(colon - text) : strlen(text);
        memcpy(host, text, n);
        host[n] = '\0';
        if (colon && !parsePort(colon + 1, t.port)) return false;
        if (!parseIPv4(host, t.addr)) return false;
    }
    out = t;
    return true;
}

bool ntpServerTextIsValid(const char* text) {
    if (text == nullptr) return false;
    size_t len = strlen(text);
    if (len == 0 || len > LOG_HOST_MAX) return false;
    uint8_t addr[16];
    if (parseIPv4(text, addr) || parseIPv6(text, addr)) return true;
    if (text[len - 1] == '.') len--;  // one trailing dot: a fully qualified name
    size_t label = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? text[i] : '.';
        if (c == '.') {
            if (label == 0 || label > 63 || text[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!isLabelChar(c) || (label == 0 && c == '-')) return false;
            label++;
        }
    }
    return true;
}

ServerInput classifyServerInput(const char* line) {
    if (line == nullptr) return ServerInput::Invalid;
    if (line[0] == '\0') return ServerInput::Keep;
    if (strcmp(line, "dhcp") == 0 || strcmp(line, "DHCP") == 0) return ServerInput::Dhcp;
    if (strcmp(line, "none") == 0 || strcmp(line, "-") == 0) return ServerInput::None;
    return ServerInput::Value;
}

/// One server input into mode/value; false if invalid
static bool applyOne(const char* input, bool ntp, ServerMode& mode, char* value) {
    switch (classifyServerInput(input)) {
    case ServerInput::Keep:
        return true;
    case ServerInput::Dhcp:
        mode = ServerMode::Dhcp;
        value[0] = '\0';
        return true;
    case ServerInput::None:
        mode = ServerMode::None;
        value[0] = '\0';
        return true;
    case ServerInput::Value:
        if (!serverFieldValid(ServerMode::Typed, input, ntp)) return false;
        mode = ServerMode::Typed;
        strcpy(value, input);  // valid values are at most LOG_HOST_MAX characters
        return true;
    case ServerInput::Invalid:
        break;
    }
    return false;
}

bool logSettingsApplyInput(LogSettings& s, const char* ntpInput, const char* syslogInput, const char*& error) {
    LogSettings next = s;
    if (!applyOne(ntpInput, true, next.ntpMode, next.ntpServer)) {
        error = "NTP server: an IPv4 or IPv6 address, a name (e.g. pool.ntp.org), dhcp or none";
        return false;
    }
    if (!applyOne(syslogInput, false, next.syslogMode, next.syslogServer)) {
        error = "Syslog server: an IPv4 address[:port] or [IPv6 address][:port], dhcp or none";
        return false;
    }
    s = next;
    return true;
}
