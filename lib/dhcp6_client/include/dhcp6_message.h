#pragma once

/**
 * @file dhcp6_message.h
 * @brief DHCPv6 (RFC 8415) messages of a client that wants one address (IA_NA):
 *        building Solicit/Request/Renew/Rebind/Decline and reading Advertise/Reply.
 *
 * The client identifier is DUID-LL (type 3, hardware type 1) made from the MAC, the
 * IAID is always 1, and the hostname goes in the Client FQDN option (RFC 4704) as a
 * partial name with the S flag, so the server may register it in DNS. Everything
 * received is untrusted: every read is bounds-checked. Pure C++, host tested with
 * frames captured from Kea and dhcpcd (test/native/dhcp6_frames/frames.h).
 */

#include <stddef.h>
#include <stdint.h>

/// Message types (RFC 8415 section 7.3)
constexpr uint8_t DHCP6_SOLICIT = 1;
constexpr uint8_t DHCP6_ADVERTISE = 2;
constexpr uint8_t DHCP6_REQUEST = 3;
constexpr uint8_t DHCP6_RENEW = 5;
constexpr uint8_t DHCP6_REBIND = 6;
constexpr uint8_t DHCP6_REPLY = 7;
constexpr uint8_t DHCP6_DECLINE = 9;

/// Status codes (RFC 8415 section 21.13)
constexpr uint16_t DHCP6_STATUS_SUCCESS = 0;
constexpr uint16_t DHCP6_STATUS_NOADDRSAVAIL = 2;
constexpr uint16_t DHCP6_STATUS_NOBINDING = 3;
constexpr uint16_t DHCP6_STATUS_NOTONLINK = 4;

/// Longest server DUID kept (a DUID is at most 128 bytes, RFC 8415 section 11.1)
constexpr size_t DHCP6_SERVER_ID_MAX = 130;

/// The one IA_NA used by this client
constexpr uint32_t DHCP6_IAID = 1;

/**
 * @brief An address lease: lifetimes and timers in seconds as the server sent them
 */
struct Dhcp6Lease {
    uint8_t addr[16];
    uint32_t preferred;
    uint32_t valid;
    uint32_t t1;
    uint32_t t2;
};

/**
 * @brief A message to build
 */
struct Dhcp6Out {
    uint8_t type;              ///< DHCP6_SOLICIT, _REQUEST, _RENEW, _REBIND or _DECLINE
    uint32_t xid;              ///< Transaction id, low 24 bits used
    uint16_t elapsedCs;        ///< Elapsed Time option, hundredths of a second
    const uint8_t* serverId;   ///< Server DUID (Request, Renew, Decline), nullptr if absent
    uint8_t serverIdLen;
    const uint8_t* addr;       ///< 16-byte address in the IA_NA (all but Solicit), nullptr if absent
    const char* hostname;      ///< Client FQDN as a partial name, nullptr or "" for none; not in Decline
};

/**
 * @brief Build a client message
 * @param mac This device's MAC (the DUID-LL)
 * @return Length written, 0 if out is too small or the hostname is not one DNS label (1-63 bytes)
 */
size_t dhcp6Build(const Dhcp6Out& m, const uint8_t mac[6], uint8_t* out, size_t size);

/**
 * @brief What a server message said
 */
struct Dhcp6In {
    uint8_t type;                          ///< DHCP6_ADVERTISE or DHCP6_REPLY
    uint32_t xid;
    uint8_t serverId[DHCP6_SERVER_ID_MAX];
    uint8_t serverIdLen;
    uint8_t preference;                    ///< Preference option, 0 when absent
    uint16_t status;                       ///< Message-level Status Code, Success when absent
    uint16_t iaStatus;                     ///< Status Code inside our IA_NA, Success when absent
    bool hasLease;                         ///< Our IA_NA held a usable address
    Dhcp6Lease lease;                      ///< Valid only with hasLease
};

/**
 * @brief Result of dhcp6Parse()
 */
enum class Dhcp6Parse : uint8_t {
    Ok,                ///< An Advertise or Reply for this client
    Malformed,         ///< Truncated, bad option lengths, missing Client ID or Server ID
    NotForUs,          ///< Client ID is not this device's DUID-LL
    NotServerMessage   ///< Any other message type
};

/**
 * @brief Read an Advertise or Reply
 *
 * The lease is taken from the IA_NA with IAID 1 only; an address whose preferred
 * lifetime is above its valid lifetime, or whose valid lifetime is 0, is not usable
 * (hasLease false, RFC 8415 section 21.6).
 *
 * @param msg The DHCPv6 message (UDP payload)
 * @param mac This device's MAC
 * @param out Written only when the result is Ok
 */
Dhcp6Parse dhcp6Parse(const uint8_t* msg, size_t len, const uint8_t mac[6], Dhcp6In& out);
