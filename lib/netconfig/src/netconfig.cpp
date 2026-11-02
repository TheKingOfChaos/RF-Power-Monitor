#include "netconfig.h"

#include <stdio.h>
#include <string.h>

bool parseIPv4(const char* text, uint8_t out[4]) {
    if (text == nullptr) return false;

    uint8_t octets[4];
    const char* p = text;

    for (int i = 0; i < 4; i++) {
        // Each octet is 1-3 decimal digits with a value of 0-255
        int value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (*p - '0');
            p++;
        }
        if (digits == 0 || value > 255) return false;
        octets[i] = (uint8_t)value;

        // Octets are separated by '.', and the string must end after the fourth
        if (i < 3) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p != '\0') return false;

    for (int i = 0; i < 4; i++) out[i] = octets[i];
    return true;
}

uint32_t ipv4ToU32(const uint8_t addr[4]) {
    return ((uint32_t)addr[0] << 24) | ((uint32_t)addr[1] << 16) |
           ((uint32_t)addr[2] << 8) | (uint32_t)addr[3];
}

bool isValidSubnetMask(const uint8_t mask[4]) {
    uint32_t m = ipv4ToU32(mask);
    if (m == 0) return false;
    // Inverted host bits must be of the form 0...01...1
    uint32_t hostBits = ~m;
    return (hostBits & (hostBits + 1)) == 0;
}

bool isSameSubnet(const uint8_t a[4], const uint8_t b[4], const uint8_t mask[4]) {
    uint32_t m = ipv4ToU32(mask);
    return (ipv4ToU32(a) & m) == (ipv4ToU32(b) & m);
}

StaticConfigError validateStaticConfig(const char* ip, const char* mask, const char* gateway) {
    uint8_t ipOct[4], maskOct[4], gwOct[4];

    if (!parseIPv4(ip, ipOct)) return StaticConfigError::InvalidIP;
    if (!parseIPv4(mask, maskOct)) return StaticConfigError::InvalidMask;
    if (!isValidSubnetMask(maskOct)) return StaticConfigError::NonContiguousMask;
    if (!parseIPv4(gateway, gwOct)) return StaticConfigError::InvalidGateway;

    uint32_t m = ipv4ToU32(maskOct);
    uint32_t host = ipv4ToU32(ipOct);
    // /31 and /32 have no separate network and broadcast addresses
    if (m < 0xFFFFFFFEu) {
        uint32_t hostPart = host & ~m;
        if (hostPart == 0 || hostPart == ~m) return StaticConfigError::HostIsNetworkOrBroadcast;
    }

    if (!isSameSubnet(ipOct, gwOct, maskOct)) return StaticConfigError::GatewayNotInSubnet;
    if (ipv4ToU32(gwOct) == host) return StaticConfigError::GatewayIsHost;

    return StaticConfigError::None;
}

const char* staticConfigErrorMessage(StaticConfigError error) {
    switch (error) {
    case StaticConfigError::None:                     return "OK";
    case StaticConfigError::InvalidIP:                return "IP address is not a valid IPv4 address";
    case StaticConfigError::InvalidMask:              return "Subnet mask is not a valid IPv4 address";
    case StaticConfigError::NonContiguousMask:        return "Subnet mask must be contiguous, e.g. 255.255.255.0";
    case StaticConfigError::InvalidGateway:           return "Gateway is not a valid IPv4 address";
    case StaticConfigError::HostIsNetworkOrBroadcast: return "IP address is the network or broadcast address of the subnet";
    case StaticConfigError::GatewayNotInSubnet:       return "Gateway is not in the same subnet as the IP address";
    case StaticConfigError::GatewayIsHost:            return "Gateway must differ from the IP address";
    }
    return "Unknown error";
}

static bool isHostnameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

bool hostnameIsValid(const char* name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '-') return false;
    size_t len = 0;
    for (const char* c = name; *c; c++, len++) {
        if (len >= HOSTNAME_MAX_LENGTH || !isHostnameChar(*c)) return false;
    }
    return name[len - 1] != '-';
}

void hostnameFromText(const char* text, char* out) {
    if (hostnameIsValid(text)) {
        strcpy(out, text);
        return;
    }
    size_t len = 0;
    bool pendingDash = false;
    for (const char* c = text ? text : ""; *c && len < HOSTNAME_MAX_LENGTH; c++) {
        if (isHostnameChar(*c) && *c != '-') {
            if (pendingDash && len > 0) {
                if (len + 2 > HOSTNAME_MAX_LENGTH) break;  // no room for '-' and a letter: stop at the word end
                out[len++] = '-';
            }
            pendingDash = false;
            out[len++] = *c;
        } else {
            pendingDash = true;  // '-' and anything invalid: at most one '-' between letters
        }
    }
    out[len] = '\0';
    if (!hostnameIsValid(out)) {
        strncpy(out, HOSTNAME_FALLBACK, HOSTNAME_MAX_LENGTH);
        out[HOSTNAME_MAX_LENGTH] = '\0';
    }
}

void hostnameDefault(const uint8_t mac[6], char* out) {
    snprintf(out, HOSTNAME_MAX_LENGTH + 1, "RF-Power-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void hostnameForDevice(const char* sysName, const uint8_t mac[6], char* out) {
    bool unset = sysName == nullptr || sysName[0] == '\0' || strcmp(sysName, HOSTNAME_OLD_DEFAULT) == 0;
    if (!unset) {
        hostnameFromText(sysName, out);
        // hostnameFromText falls back to the project name when nothing usable is left: use the device's own
        if (strcmp(out, HOSTNAME_FALLBACK) != 0) return;
    }
    hostnameDefault(mac, out);
}

// ---- IPv6 ----

/// Value of a hex digit, -1 for anything else
static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseIPv6(const char* text, uint8_t out[16]) {
    if (text == nullptr) return false;
    size_t len = strnlen(text, IPV6_TEXT_MAX);
    if (len == 0 || len >= IPV6_TEXT_MAX) return false;

    uint16_t groups[8];
    int count = 0;
    int gap = -1;  // index in groups where "::" stands, -1 if there is none
    const char* p = text;
    if (p[0] == ':') {
        if (p[1] != ':') return false;  // a single leading ':'
        gap = 0;
        p += 2;
    }
    while (*p) {
        if (count == 8) return false;
        uint32_t value = 0;
        int digits = 0;
        for (int v; (v = hexValue(*p)) >= 0; p++) {
            if (++digits > 4) return false;
            value = value * 16 + (uint32_t)v;
        }
        if (digits == 0) return false;  // ":::", stray characters, IPv4 dots, '/', '%'
        groups[count++] = (uint16_t)value;
        if (*p == '\0') break;
        if (*p != ':') return false;
        p++;
        if (*p == ':') {
            if (gap >= 0) return false;  // a second "::"
            gap = count;
            p++;
        } else if (*p == '\0') {
            return false;  // a single trailing ':'
        }
    }
    // Without "::" all eight groups are written; "::" stands for at least one zero group
    if (gap < 0 ? count != 8 : count > 7) return false;

    uint16_t full[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (gap < 0) {
        memcpy(full, groups, sizeof(full));
    } else {
        for (int i = 0; i < gap; i++) full[i] = groups[i];
        int tail = count - gap;
        for (int i = 0; i < tail; i++) full[8 - tail + i] = groups[gap + i];
    }
    for (int i = 0; i < 8; i++) {
        out[2 * i] = (uint8_t)(full[i] >> 8);
        out[2 * i + 1] = (uint8_t)full[i];
    }
    return true;
}

void formatIPv6(const uint8_t addr[16], char* out) {
    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = (uint16_t)(addr[2 * i] << 8 | addr[2 * i + 1]);
    // The longest run of two or more zero groups (the first if tied) becomes "::"
    int bestStart = -1, bestLen = 1;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && g[j] == 0) j++;
        if (j - i > bestLen) {
            bestStart = i;
            bestLen = j - i;
        }
        i = j;
    }
    static const char HEX[] = "0123456789abcdef";
    char* p = out;
    for (int i = 0; i < 8; i++) {
        if (i == bestStart) {
            *p++ = ':';
            if (i == 0) *p++ = ':';
            i += bestLen - 1;
            continue;
        }
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            int d = g[i] >> shift & 0xF;
            if (d || started || shift == 0) {
                *p++ = HEX[d];
                started = true;
            }
        }
        if (i < 7) *p++ = ':';
    }
    *p = '\0';
}

/// fe80::/10
static bool isLinkLocal6(const uint8_t a[16]) {
    return a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
}

StaticIPv6Error validateStaticIPv6(const char* address, const char* gateway) {
    bool hasAddress = address != nullptr && address[0] != '\0';
    bool hasGateway = gateway != nullptr && gateway[0] != '\0';
    if (!hasAddress) return hasGateway ? StaticIPv6Error::GatewayWithoutAddress : StaticIPv6Error::None;

    uint8_t a[16];
    if (!parseIPv6(address, a)) return StaticIPv6Error::InvalidAddress;
    // ::/8 holds unspecified, loopback and IPv4-mapped/compatible addresses; link-local
    // addresses are made automatically; an all-zero interface id is the subnet-router anycast address
    bool idIsZero = true;
    for (int i = 8; i < 16; i++) idIsZero = idIsZero && a[i] == 0;
    if (a[0] == 0 || a[0] == 0xFF || isLinkLocal6(a) || idIsZero) return StaticIPv6Error::AddressNotUsable;
    if (!hasGateway) return StaticIPv6Error::None;

    uint8_t g[16];
    if (!parseIPv6(gateway, g)) return StaticIPv6Error::InvalidGateway;
    if (g[0] == 0 || g[0] == 0xFF) return StaticIPv6Error::GatewayNotUsable;
    if (!isLinkLocal6(g) && memcmp(a, g, 8) != 0) return StaticIPv6Error::GatewayNotOnLink;
    if (memcmp(a, g, 16) == 0) return StaticIPv6Error::GatewayIsHost;
    return StaticIPv6Error::None;
}

const char* staticIPv6ErrorMessage(StaticIPv6Error error) {
    switch (error) {
    case StaticIPv6Error::None:             return "OK";
    case StaticIPv6Error::InvalidAddress:   return "IPv6 address is not valid (write it without /64, e.g. fd00::10)";
    case StaticIPv6Error::AddressNotUsable: return "IPv6 address must be a global or ULA unicast address with a non-zero interface id";
    case StaticIPv6Error::InvalidGateway:   return "IPv6 gateway is not a valid IPv6 address";
    case StaticIPv6Error::GatewayNotUsable: return "IPv6 gateway must be a unicast address";
    case StaticIPv6Error::GatewayNotOnLink: return "IPv6 gateway must be link-local (fe80::...) or in the address's /64";
    case StaticIPv6Error::GatewayIsHost:    return "IPv6 gateway must differ from the address";
    case StaticIPv6Error::GatewayWithoutAddress: return "An IPv6 gateway needs a static IPv6 address";
    }
    return "Unknown error";
}

TrapTargetError parseTrapTarget(const char* text, TrapAddress& out) {
    uint8_t a[16];
    if (parseIPv4(text, a)) {
        out.ipv6 = false;
        memcpy(out.addr, a, 4);
        return TrapTargetError::None;
    }
    if (!parseIPv6(text, a)) return TrapTargetError::Invalid;
    // ::/8 holds unspecified, loopback and IPv4-mapped addresses; link-local would need a zone
    if (a[0] == 0 || a[0] == 0xFF || isLinkLocal6(a)) return TrapTargetError::NotUsable;
    out.ipv6 = true;
    memcpy(out.addr, a, 16);
    return TrapTargetError::None;
}

void formatTrapAddress(const TrapAddress& address, char* out) {
    if (address.ipv6) {
        formatIPv6(address.addr, out);
    } else {
        snprintf(out, IPV6_TEXT_MAX, "%u.%u.%u.%u", address.addr[0], address.addr[1], address.addr[2], address.addr[3]);
    }
}

const char* trapTargetErrorMessage(TrapTargetError error) {
    switch (error) {
    case TrapTargetError::None:      return "OK";
    case TrapTargetError::Invalid:   return "not a valid IPv4 or IPv6 address";
    case TrapTargetError::NotUsable: return "IPv6 trap destination must be a global or ULA unicast address";
    }
    return "Unknown error";
}
