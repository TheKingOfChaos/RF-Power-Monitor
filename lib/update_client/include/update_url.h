#pragma once

/**
 * @file update_url.h
 * @brief The update server URL: http://host[:port][/path] (no HTTPS, no user:password@).
 * Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

/// Longest update server URL (setting, web form, DHCP option 225)
static const size_t UPDATE_URL_MAX = 127;

/// A parsed update server URL
struct UpdateUrl {
    char host[64];    ///< DNS name, IPv4 dotted quad, or IPv6 text without the brackets
    uint16_t port;    ///< 1-65535, 80 when the URL has none
    char path[64];    ///< Prefix for the site's paths, "" or "/..." without a trailing '/'
    bool hostIsIPv6;  ///< host was written as [IPv6]
};

/**
 * @brief Parse an update server URL
 *
 * Accepts "http://" (any case), then a host of letters, digits, '.' and '-'
 * (at most 63) or a bracketed IPv6 address, an optional port 1-65535 and an
 * optional path (at most 63 characters, printable, no spaces). At most
 * UPDATE_URL_MAX characters in all.
 * @return false for anything else (out is then unspecified)
 */
bool parseUpdateUrl(const char* text, UpdateUrl& out);
