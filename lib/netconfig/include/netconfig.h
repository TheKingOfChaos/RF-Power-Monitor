#pragma once

/**
 * @file netconfig.h
 * @brief IPv4 address parsing and static network configuration validation.
 *
 * Pure C++ with no Arduino dependency, so it is unit tested on the host
 * (`pio test -e native`). Used by the serial menu and by settings loading.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Result of validating a static IP configuration
 */
enum class StaticConfigError : uint8_t {
    None = 0,            ///< Configuration is valid
    InvalidIP,           ///< IP address is not a dotted-quad IPv4 address
    InvalidMask,         ///< Subnet mask is not a dotted-quad IPv4 address
    NonContiguousMask,   ///< Subnet mask one-bits are not contiguous, or mask is 0.0.0.0
    InvalidGateway,      ///< Gateway is not a dotted-quad IPv4 address
    HostIsNetworkOrBroadcast, ///< IP is the network or broadcast address of its subnet
    GatewayNotInSubnet,  ///< Gateway is outside the subnet of the IP address
    GatewayIsHost        ///< Gateway equals the device IP address
};

/**
 * @brief Parse a strict dotted-quad IPv4 address such as "10.10.6.111"
 *
 * Accepts exactly four decimal octets 0-255 separated by '.', each with 1-3
 * digits. Rejects signs, spaces, empty octets, and trailing characters.
 *
 * @param text NUL-terminated input, may be nullptr
 * @param out  Receives the four octets on success, untouched on failure
 * @return true if text is a valid address
 */
bool parseIPv4(const char* text, uint8_t out[4]);

/**
 * @brief Convert four octets to a host-order 32-bit value
 */
uint32_t ipv4ToU32(const uint8_t addr[4]);

/**
 * @brief Check that a subnet mask is non-zero and its one-bits are contiguous
 * @param mask Four octets, e.g. {255, 255, 252, 0}
 * @return true for masks /1 through /32
 */
bool isValidSubnetMask(const uint8_t mask[4]);

/**
 * @brief Check whether two addresses are in the same subnet
 */
bool isSameSubnet(const uint8_t a[4], const uint8_t b[4], const uint8_t mask[4]);

/**
 * @brief Validate a complete static configuration given as strings
 *
 * Checks are applied in the order listed in StaticConfigError, and the first
 * failure is returned. /31 and /32 masks have no network/broadcast address,
 * so that check is skipped for them.
 *
 * @return StaticConfigError::None if the configuration can be applied
 */
StaticConfigError validateStaticConfig(const char* ip, const char* mask, const char* gateway);

/**
 * @brief Human-readable message for a validation error, for the serial menu
 */
const char* staticConfigErrorMessage(StaticConfigError error);

/// Longest hostname: the DHCP hostname of the lwIP driver takes at most 32 characters
static const size_t HOSTNAME_MAX_LENGTH = 32;

/// Used when no valid hostname can be derived
static const char HOSTNAME_FALLBACK[] = "rf-ups-monitor";

/**
 * @brief A valid device hostname (one DNS label, RFC 1123): 1-32 letters,
 *        digits and '-', not starting or ending with '-'
 */
bool hostnameIsValid(const char* name);

/**
 * @brief Make a valid hostname from free text, e.g. a sysName set by older firmware
 *
 * A valid name is kept as it is. Otherwise every run of other characters
 * becomes one '-', leading and trailing '-' are dropped and the result is cut
 * to HOSTNAME_MAX_LENGTH ("Rack 4 / UPS" -> "Rack-4-UPS"). Falls back to
 * HOSTNAME_FALLBACK when nothing usable remains.
 * @param out At least HOSTNAME_MAX_LENGTH + 1 bytes
 */
void hostnameFromText(const char* text, char* out);

// ---- IPv6 ----

/// Longest IPv6 text accepted or produced, including the NUL ("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
static const size_t IPV6_TEXT_MAX = 40;

/**
 * @brief Parse an IPv6 address in RFC 4291 text form, e.g. "fd16:d833:4f0e:3ece::249"
 *
 * Eight groups of 1-4 hex digits (either case) separated by ':', with at most
 * one "::" standing for one or more zero groups. Rejects a prefix length
 * ("/64"), a zone ("%eth0"), an embedded IPv4 tail ("::ffff:1.2.3.4"),
 * spaces and anything longer than IPV6_TEXT_MAX - 1 characters.
 *
 * @param text NUL-terminated input, may be nullptr
 * @param out  Receives the 16 bytes (network order) on success, untouched on failure
 */
bool parseIPv6(const char* text, uint8_t out[16]);

/**
 * @brief Format an IPv6 address the RFC 5952 way: lower case, no leading zeros,
 *        the longest run of two or more zero groups (the first if tied) as "::"
 * @param out At least IPV6_TEXT_MAX bytes
 */
void formatIPv6(const uint8_t addr[16], char* out);

/**
 * @brief Result of validating a static IPv6 configuration
 */
enum class StaticIPv6Error : uint8_t {
    None = 0,             ///< Valid (or both empty: no static IPv6)
    InvalidAddress,       ///< Address is not IPv6 text (see parseIPv6)
    AddressNotUsable,     ///< Address is in ::/8, link-local, multicast, or its interface id is zero
    InvalidGateway,       ///< Gateway is not IPv6 text
    GatewayNotUsable,     ///< Gateway is in ::/8 or multicast
    GatewayNotOnLink,     ///< Gateway is neither link-local nor in the address's /64
    GatewayIsHost,        ///< Gateway equals the address
    GatewayWithoutAddress ///< A gateway was given without a static address
};

/**
 * @brief Validate a static IPv6 address (always /64) and optional gateway
 *
 * Empty address and empty gateway mean "no static IPv6". The checks run in
 * the order of StaticIPv6Error and the first failure is returned.
 */
StaticIPv6Error validateStaticIPv6(const char* address, const char* gateway);

/**
 * @brief Human-readable message for a static IPv6 error, for the serial menu and web page
 */
const char* staticIPv6ErrorMessage(StaticIPv6Error error);

// ---- Trap destination ----

/**
 * @brief A trap destination: IPv4 (addr[0..3]) or IPv6 (addr[0..15], network order)
 */
struct TrapAddress {
    bool ipv6;
    uint8_t addr[16];
};

/**
 * @brief Result of parsing a trap destination
 */
enum class TrapTargetError : uint8_t {
    None = 0,   ///< Valid
    Invalid,    ///< Neither IPv4 nor IPv6 text (empty included)
    NotUsable   ///< IPv6 in ::/8, link-local or multicast
};

/**
 * @brief Parse a trap destination: any IPv4 address (as parseIPv4), or a global or
 *        ULA unicast IPv6 address (as parseIPv6; link-local would need a zone)
 * @param out Receives the address only when the result is None
 */
TrapTargetError parseTrapTarget(const char* text, TrapAddress& out);

/**
 * @brief Format a trap destination: dotted quad, or RFC 5952 text for IPv6
 * @param out At least IPV6_TEXT_MAX bytes
 */
void formatTrapAddress(const TrapAddress& address, char* out);

/**
 * @brief Human-readable message for a trap destination error, for the serial menu
 */
const char* trapTargetErrorMessage(TrapTargetError error);
