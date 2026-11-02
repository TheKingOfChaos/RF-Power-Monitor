#pragma once

/**
 * @file settings_codec.h
 * @brief Settings structure, JSON (de)serialization and A/B copy selection.
 *
 * Pure C++ plus ArduinoJson, no Arduino core or filesystem dependency, so it
 * is unit tested on the host (`pio test -e native`). The flash storage layer
 * in lib/settings uses it.
 *
 * Storage model: two copies (slot A and slot B) are kept. Every save writes
 * the slot that is NOT currently active, with sequence = active sequence + 1.
 * On load, the valid copy with the newest sequence wins. A power cut during a
 * save therefore leaves the previous copy intact.
 */

#include <stddef.h>
#include <stdint.h>

#include "netconfig.h"

/// Increment only when the JSON layout changes incompatibly
constexpr uint32_t SETTINGS_VERSION = 1;

/// Largest serialized settings document, including the terminating NUL.
/// Worst case: every text field at maximum length and fully JSON-escaped.
constexpr size_t SETTINGS_JSON_MAX = 1024;

/// Longest SNMP community string
constexpr size_t COMMUNITY_MAX_LENGTH = 31;

/// Longest sysName / sysContact / sysLocation
constexpr size_t SYS_TEXT_MAX_LENGTH = 63;

/// Stored web password: 32 hex salt + '$' + 64 hex SHA-256 (see lib/web_ui/web_auth.h)
constexpr size_t SETTINGS_WEB_PASSWORD_LEN = 97;

/// Trap formats
constexpr uint8_t TRAP_VERSION_V1 = 1;   ///< SNMPv1 Trap-PDU
constexpr uint8_t TRAP_VERSION_V2C = 2;  ///< SNMPv2-Trap (default)

/**
 * @brief Persistent device settings
 */
struct Settings {
    char community[COMMUNITY_MAX_LENGTH + 1];  ///< SNMP community string, 1-31 printable characters
    bool dhcpEnabled;     ///< true = DHCP, false = static configuration below
    char staticIP[16];    ///< Static IP address, dotted quad
    char subnetMask[16];  ///< Static subnet mask, dotted quad
    char gateway[16];     ///< Static gateway, dotted quad
    char ipv6Address[IPV6_TEXT_MAX];  ///< Static IPv6 address (/64, RFC 5952 text), empty = none; independent of dhcpEnabled
    char ipv6Gateway[IPV6_TEXT_MAX];  ///< Static IPv6 gateway, empty = routers from router advertisements
    char sysName[SYS_TEXT_MAX_LENGTH + 1];      ///< SNMP sysName, 0-63 printable characters
    char sysContact[SYS_TEXT_MAX_LENGTH + 1];   ///< SNMP sysContact, 0-63 printable characters
    char sysLocation[SYS_TEXT_MAX_LENGTH + 1];  ///< SNMP sysLocation, 0-63 printable characters
    char trapTarget[IPV6_TEXT_MAX];  ///< SNMP trap destination (IPv4 or IPv6, UDP 162), empty = traps off
    uint8_t trapVersion;  ///< TRAP_VERSION_V1 or TRAP_VERSION_V2C
    bool trapFromDhcp;    ///< true: trap destination from DHCP option 224 instead of trapTarget
    bool webEnabled;      ///< false: no web server on TCP 80 (serial menu only)
    char webPassword[SETTINGS_WEB_PASSWORD_LEN + 1];  ///< Salted hash of the web password, empty = device default
    uint32_t version;     ///< Layout version, must equal SETTINGS_VERSION
    uint32_t sequence;    ///< Save counter, newest valid copy wins on load
};

/**
 * @brief Check a text setting: printable ASCII (0x20-0x7E), at most maxLength characters
 * @param allowEmpty false for the community string, true for sysName/sysContact/sysLocation
 */
bool settingsTextIsValid(const char* text, size_t maxLength, bool allowEmpty);

/**
 * @brief Why a settings document was rejected
 */
enum class SettingsParseError : uint8_t {
    None = 0,         ///< Parsed and valid
    Empty,            ///< Input is nullptr or empty
    InvalidJson,      ///< Not a JSON object
    WrongVersion,     ///< "version" missing or not SETTINGS_VERSION
    MissingField,     ///< A required field is missing or has the wrong type
    FieldTooLong,     ///< A string does not fit its buffer
    BadCommunity,     ///< Community string empty or contains non-printable characters
    BadStaticConfig,  ///< DHCP disabled but static configuration is invalid
    BadSysText,       ///< sysName/sysContact/sysLocation contains non-printable characters
    BadTrapTarget,    ///< trapTarget is neither empty nor accepted by parseTrapTarget()
    BadTrapVersion,   ///< trapVersion is not 1 or 2
    BadWebPassword,   ///< webPassword is neither empty nor a well-formed salted hash
    BadStaticIPv6,    ///< ipv6Address / ipv6Gateway fail validateStaticIPv6()
    TrapIPv6NeedsV2c  ///< A fixed IPv6 trapTarget with trapVersion 1 (see trapVersionFitsTarget)
};

/**
 * @brief Whether the trap version can be sent to the destination: a v1 Trap-PDU carries an
 *        IPv4 agent-addr, so a fixed IPv6 destination needs v2c. With trapFromDhcp the
 *        fixed address is only kept, not used, so anything fits.
 * @param trapTarget Destination text, assumed already checked (empty = traps off)
 */
bool trapVersionFitsTarget(const char* trapTarget, bool trapFromDhcp, uint8_t trapVersion);

/**
 * @brief Which stored copy to use
 */
enum class SettingsSlot : uint8_t { None, A, B };

/**
 * @brief Fill settings with factory defaults (DHCP, community "public",
 *        empty sysName (the device then uses RF-Power-<MAC tail>), empty sysContact and sysLocation)
 */
void settingsDefaults(Settings& s);

/**
 * @brief Serialize settings to compact JSON
 * @param buf  Output buffer
 * @param size Buffer size in bytes, SETTINGS_JSON_MAX is always enough
 * @return Length written excluding NUL, or 0 if the buffer is too small
 */
size_t settingsToJson(const Settings& s, char* buf, size_t size);

/**
 * @brief Parse and validate a settings document
 *
 * Unknown fields are ignored. "sequence", "sysName", "sysContact",
 * "sysLocation", "trapTarget", "trapVersion", "trapFromDhcp", "webEnabled", "webPassword", "ipv6Address" and
 * "ipv6Gateway" are optional and take their
 * defaults, so files written by older firmware still load.
 *
 * @param json NUL-terminated JSON text
 * @param out  Receives the settings only when the result is None
 */
SettingsParseError settingsFromJson(const char* json, Settings& out);

/**
 * @brief Human-readable message for a parse error, for log output
 */
const char* settingsParseErrorMessage(SettingsParseError error);

/**
 * @brief Compare two settings field by field (ignores bytes after string terminators)
 */
bool settingsEqual(const Settings& a, const Settings& b);

/**
 * @brief true if sequence a is newer than b, handling 32-bit wrap-around
 */
bool sequenceIsNewer(uint32_t a, uint32_t b);

/**
 * @brief Choose the copy to load
 * @return The valid slot with the newest sequence, A on a tie, None if neither is valid
 */
SettingsSlot chooseActiveSlot(bool aValid, uint32_t aSequence, bool bValid, uint32_t bSequence);

/**
 * @brief The slot the next save writes to: the opposite of the active one, A if none
 */
SettingsSlot nextWriteSlot(SettingsSlot active);
