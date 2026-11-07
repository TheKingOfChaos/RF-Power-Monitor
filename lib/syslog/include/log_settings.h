// NTP and syslog server settings: codec for their own A/B files
// (lib/settings log_settings_store.h; the main settings file is full) and the
// checks for what the serial menu and the web page accept.
//
// NTP server: IPv4 or IPv6 address or a DNS name. Syslog server: IPv4 or IPv6
// address with an optional port (a.b.c.d[:port], v6, [v6][:port]; 514 when
// none). Each is Dhcp (from DHCP: option 42 / DHCPv6 56, option 7), Typed or
// None; a factory reset sets both to Dhcp.
#pragma once
#include <stddef.h>
#include <stdint.h>

static const uint32_t LOG_SETTINGS_VERSION = 1;
static const size_t LOG_SETTINGS_JSON_MAX = 384;
static const size_t LOG_HOST_MAX = 63;
static const uint16_t SYSLOG_DEFAULT_PORT = 514;

enum class ServerMode : uint8_t { Dhcp, Typed, None };

struct LogSettings {
    ServerMode ntpMode;
    char ntpServer[LOG_HOST_MAX + 1];  // set when ntpMode is Typed
    ServerMode syslogMode;
    char syslogServer[LOG_HOST_MAX + 1];  // set when syslogMode is Typed
    uint32_t version;
    uint32_t sequence;
};

/// Both servers from DHCP, no typed values
void logSettingsDefaults(LogSettings& s);
/// JSON for the settings file; the length, or 0 when it does not fit
size_t logSettingsToJson(const LogSettings& s, char* out, size_t size);
/// Read a settings file; false (and defaults in `out`) for anything missing or invalid
bool logSettingsFromJson(const char* json, LogSettings& out);
/// Same modes and servers (version and sequence are not compared)
bool logSettingsEqual(const LogSettings& a, const LogSettings& b);

struct SyslogTarget {
    bool ipv6;
    uint8_t addr[16];  // IPv4 in the first 4 bytes
    uint16_t port;
};

/// Parse a typed syslog server; DNS names and link-local IPv6 addresses (fe80::/10) are not accepted
bool parseSyslogTarget(const char* text, SyslogTarget& out);
/// An IPv4 or IPv6 address (not link-local fe80::/10), or a DNS name (labels of 1-63 [A-Za-z0-9-] not
/// starting or ending with '-', one trailing dot allowed, at most LOG_HOST_MAX characters)
bool ntpServerTextIsValid(const char* text);

/// What a line typed for a server setting means: empty keeps the old value
enum class ServerInput : uint8_t { Keep, Dhcp, None, Value, Invalid };
ServerInput classifyServerInput(const char* line);

/**
 * @brief Apply what was typed for the two servers (serial menu option 11, web page)
 *
 * Each input: empty keeps the value, "dhcp", "none" (or "-"), or a server
 * (NTP: address or name; syslog: address with optional port). All or nothing:
 * with an invalid input, `s` is unchanged and `error` says why.
 */
bool logSettingsApplyInput(LogSettings& s, const char* ntpInput, const char* syslogInput, const char*& error);
