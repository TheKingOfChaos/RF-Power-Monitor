#pragma once

/**
 * @file dhcp_option.h
 * @brief Request and read one extra DHCP option by editing and reading Ethernet frames.
 *
 * lwIP is compiled with the core's lwipopts.h, which sets no DHCP hooks and no
 * extra request options, so it neither asks for
 * nor keeps site-specific options. The firmware sees every frame at the
 * driver (src/net_stats.cpp), so it
 *  - adds the option code to the Parameter Request List (option 55) of every
 *    outgoing DISCOVER/REQUEST/INFORM, in place, using a padding byte after
 *    the END option (lwIP always pads), and fixes the UDP checksum;
 *  - reads the option from incoming DHCPACKs addressed to this device.
 *
 * Pure C++, host tested with frames captured from the device and from Kea.
 */

#include <stddef.h>
#include <stdint.h>

/// Site-specific option (RFC 2132 range 224-254) carrying the SNMP trap destination (IPv4)
static const uint8_t DHCP_OPTION_TRAP_TARGET = 224;

/// Site-specific option carrying the update server URL (text, see dhcpAckText)
static const uint8_t DHCP_OPTION_UPDATE_URL = 225;

/// Result of dhcpRequestOption()
enum class DhcpEdit : uint8_t {
    NotDhcpRequest,    ///< Not a well-formed client DISCOVER/REQUEST/INFORM with option 55: unchanged
    AlreadyRequested,  ///< The code is already in option 55: unchanged
    Added,             ///< Code appended to option 55, UDP checksum updated
    NoRoom             ///< No padding byte after END (or option 55 full): unchanged
};

/**
 * @brief Add code to the Parameter Request List of a DHCP client message, in place
 * @param frame Ethernet frame (destination MAC first); the length does not change
 */
DhcpEdit dhcpRequestOption(uint8_t* frame, size_t len, uint8_t code);

/// Result of dhcpReadAck()
enum class DhcpAck : uint8_t {
    NotDhcpReply,  ///< Not a well-formed DHCP server message (bad checksums count as malformed)
    NotForUs,      ///< chaddr is another device's MAC
    NotAck,        ///< OFFER, NAK, ...
    NoOption,      ///< DHCPACK without the option (or with a wrong length)
    Option         ///< DHCPACK with a 4-byte option: value in out
};

/// What a DHCPACK said (filled for the NoOption and Option results)
struct DhcpAckInfo {
    uint8_t value[4];     ///< The option value (Option only)
    uint8_t yiaddr[4];    ///< The address the ACK assigns
    uint32_t xid;         ///< Transaction id: must be the client's current one to be genuine
    bool hasServerId;     ///< Option 54 present
    uint8_t serverId[4];  ///< Option 54: the server that sent it
};

/**
 * @brief Read a 4-byte option (an IPv4 address) from a DHCPACK for this device
 *
 * Anyone on the LAN can send a frame like this. The caller must only use the
 * value if the DHCP client accepted this ACK (same xid and server, and it moved
 * to BOUND because of it); see src/net_stats.cpp.
 * @param mac This device's MAC; the ACK's chaddr must match
 */
DhcpAck dhcpReadAck(const uint8_t* frame, size_t len, const uint8_t mac[6], uint8_t code, DhcpAckInfo& info);

/**
 * @brief Read a text option (the update server URL) from a DHCPACK
 *
 * Checks the frame as dhcpReadAck() does (checksums, a DHCPACK from a server);
 * the caller must also check the ACK was for this device and accepted by lwIP
 * (see src/net_stats.cpp) before using the value.
 * @param out Receives the text, NUL terminated; empty on failure
 * @param outSize The option must be 1..outSize-1 printable ASCII bytes: longer
 *        or non-printable values are rejected, never truncated
 * @return true if the option was present and acceptable
 */
bool dhcpAckText(const uint8_t* frame, size_t len, uint8_t code, char* out, size_t outSize);
