#include "cli_input.h"

#include <stdio.h>
#include <string.h>

#include <netconfig.h>
#include <settings_codec.h>
#include <update_url.h>

void LineEditor::reset() {
    memset(_buf, 0, sizeof(_buf));
    _len = 0;
    _overflow = false;
    _lastWasCR = false;
    _echo = 0;
}

void LineEditor::wipe() { memset(_buf, 0, sizeof(_buf)); }

LineEvent LineEditor::feed(char c, bool secret) {
    // Ignore the LF of a CR LF pair
    bool skip = c == '\n' && _lastWasCR;
    _lastWasCR = c == '\r';
    if (skip) return LineEvent::None;

    if (c == '\r' || c == '\n') {
        _buf[_len] = '\0';
        _len = 0;
        if (_overflow) {
            _overflow = false;
            wipe();  // never acted on, and may be a password
            return LineEvent::Overflow;
        }
        return LineEvent::Line;
    }
    if (c == '\b' || c == 0x7F) {
        if (_len == 0) return LineEvent::None;
        _len--;
        return LineEvent::Erase;
    }
    if (c < 0x20 || c > 0x7E) return LineEvent::None;  // control characters, bytes above ASCII (char may be signed)
    if (_len >= MAX_CHARS) {
        _overflow = true;
        return LineEvent::None;
    }
    _buf[_len++] = c;
    _echo = secret ? '*' : c;
    return LineEvent::Echo;
}

bool cliConfirmed(const char* input) { return input[0] == 'y' || input[0] == 'Y'; }

bool cliTakeAddress(const char* input, const char* current, char* dest, char* msg, size_t msgSize) {
    const char* value = input[0] ? input : current;
    uint8_t octets[4];
    if (!parseIPv4(value, octets)) {
        snprintf(msg, msgSize, "'%s' is not a valid IPv4 address", value);
        return false;
    }
    snprintf(dest, 16, "%s", value);
    return true;
}

bool cliTakeText(const char* input, const char* current, char* dest, size_t maxLength, bool allowEmpty, char* msg,
                 size_t msgSize) {
    const char* value = input;
    if (input[0] == '\0') {
        value = current;
    } else if (strcmp(input, "-") == 0) {
        value = "";
    }
    if (value[0] == '\0' && !allowEmpty) {
        snprintf(msg, msgSize, "Must not be empty");
        return false;
    }
    if (strlen(value) > maxLength) {
        snprintf(msg, msgSize, "Too long, at most %u characters", (unsigned)maxLength);
        return false;
    }
    if (!settingsTextIsValid(value, maxLength, allowEmpty)) {
        snprintf(msg, msgSize, "Only printable ASCII characters are allowed");
        return false;
    }
    snprintf(dest, maxLength + 1, "%s", value);
    return true;
}

bool cliTakeHostname(const char* input, const char* current, char* dest, char* msg, size_t msgSize) {
    if (input[0] == '\0') {
        snprintf(dest, SYS_TEXT_MAX_LENGTH + 1, "%s", current);  // may be free text from older firmware
        return true;
    }
    if (strcmp(input, "-") == 0) {
        dest[0] = '\0';  // not set: the default name
        return true;
    }
    if (!hostnameIsValid(input)) {
        snprintf(msg, msgSize, "Not a valid hostname: 1-%u letters, digits and '-', not starting or ending with '-'",
                 (unsigned)HOSTNAME_MAX_LENGTH);
        return false;
    }
    snprintf(dest, SYS_TEXT_MAX_LENGTH + 1, "%s", input);
    return true;
}

bool cliTakeTrapTarget(const char* input, const char* currentTarget, bool currentFromDhcp, char* destTarget,
                       bool& destFromDhcp, char* msg, size_t msgSize) {
    if (input[0] == '\0') {
        snprintf(destTarget, IPV6_TEXT_MAX, "%s", currentTarget);
        destFromDhcp = currentFromDhcp;
        return true;
    }
    if (strcmp(input, "dhcp") == 0 || strcmp(input, "DHCP") == 0) {
        // The fixed address is kept while DHCP is chosen, for switching back
        snprintf(destTarget, IPV6_TEXT_MAX, "%s", currentTarget);
        destFromDhcp = true;
        return true;
    }
    if (strcmp(input, "-") == 0) {
        destTarget[0] = '\0';
        destFromDhcp = false;
        return true;
    }
    TrapAddress address;
    TrapTargetError err = parseTrapTarget(input, address);
    if (err != TrapTargetError::None) {
        snprintf(msg, msgSize, "'%s': %s", input, trapTargetErrorMessage(err));
        return false;
    }
    formatTrapAddress(address, destTarget);
    destFromDhcp = false;
    return true;
}

bool cliTakeTrapVersion(const char* input, uint8_t current, uint8_t& dest, char* msg, size_t msgSize) {
    if (input[0] == '\0') {
        dest = current;
    } else if (strcmp(input, "1") == 0) {
        dest = TRAP_VERSION_V1;
    } else if (strcmp(input, "2c") == 0 || strcmp(input, "2C") == 0) {
        dest = TRAP_VERSION_V2C;
    } else {
        snprintf(msg, msgSize, "Enter 1 or 2c");
        return false;
    }
    return true;
}

bool cliTakeIPv6(const char* input, const char* current, char* dest, char* msg, size_t msgSize) {
    const char* value = input[0] == '\0' ? current : strcmp(input, "-") == 0 ? "" : input;
    if (value[0] == '\0') {
        dest[0] = '\0';
        return true;
    }
    uint8_t bytes[16];
    if (!parseIPv6(value, bytes)) {
        snprintf(msg, msgSize, "'%s' is not a valid IPv6 address (write it without /64, e.g. fd00::10)", value);
        return false;
    }
    formatIPv6(bytes, dest);
    return true;
}

bool cliTakeUpdateUrl(const char* input, const char* currentUrl, bool currentFromDhcp, char* destUrl,
                      bool& destFromDhcp, char* msg, size_t msgSize) {
    UpdateUrl parsed;
    if (input[0] == '\0') {
        snprintf(destUrl, UPDATE_URL_MAX + 1, "%s", currentUrl);
        destFromDhcp = currentFromDhcp;
    } else if (strcmp(input, "none") == 0) {
        destUrl[0] = '\0';
        destFromDhcp = false;
    } else if (strcmp(input, "dhcp") == 0) {
        snprintf(destUrl, UPDATE_URL_MAX + 1, "%s", currentUrl);  // kept for later
        destFromDhcp = true;
    } else if (strlen(input) > UPDATE_URL_MAX || !parseUpdateUrl(input, parsed)) {
        snprintf(msg, msgSize, "'%s': not an http://host[:port][/path] URL of at most %u characters", input,
                 (unsigned)UPDATE_URL_MAX);
        return false;
    } else {
        snprintf(destUrl, UPDATE_URL_MAX + 1, "%s", input);
        destFromDhcp = false;
    }
    return true;
}
