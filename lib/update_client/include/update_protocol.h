#pragma once

/**
 * @file update_protocol.h
 * @brief The update server's wire format: the check-in body the device sends,
 *        the reply it accepts, and the keyed BLAKE2b MACs that authenticate both.
 *
 * Check-in: POST <path>/api/checkin, text/plain, header X-RFPower-Auth: MAC(key, body).
 * The body is LF-terminated key=value lines in a fixed order (buildCheckinBody).
 * Reply: "status=ok" or "status=update", "version=", "path=" lines, then
 * "auth=" MAC(key, nonce + "\n" + every byte before the auth line). The nonce is
 * the one this device sent, so an old reply cannot be replayed.
 *
 * The site (update-server/rfupdate/protocol.py) implements the same format; both
 * are tested against update-server/tests/vectors.json.
 * Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>
#include <netconfig.h>

/// Shared key length in bytes (entered as 64 hex characters)
static const size_t UPDATE_KEY_LEN = 32;
/// A MAC as lowercase hex (BLAKE2b, 32-byte output)
static const size_t UPDATE_MAC_HEX = 64;
/// The check-in nonce as lowercase hex (16 random bytes)
static const size_t UPDATE_NONCE_HEX = 32;
/// Largest reply body the device accepts
static const size_t UPDATE_REPLY_MAX = 512;
/// Longest version string (as in the .upd header, which holds 16 bytes)
static const size_t UPDATE_VERSION_MAX = 16;
/// Longest download path in a reply, including the leading '/'
static const size_t UPDATE_PATH_MAX = 127;
/// IPv6 addresses a check-in reports at most
static const size_t UPDATE_IPV6_MAX = 4;

/// What a check-in reports (all strings NUL terminated)
struct CheckinInfo {
    char serial[17];                                 ///< Flash unique id, 16 upper hex
    char hostname[64];                               ///< hostnameCurrent()
    char ipv4[16];                                   ///< Dotted quad, empty without an address
    char ipv6[UPDATE_IPV6_MAX][IPV6_TEXT_MAX];       ///< RFC 5952 text
    uint8_t ipv6Count;                               ///< Entries used in ipv6
    char mac[18];                                    ///< aa:bb:cc:dd:ee:ff
    char version[UPDATE_VERSION_MAX + 1];            ///< FIRMWARE_VERSION
    char slot;                                       ///< 'A' or 'B'
    bool trial;                                      ///< Running firmware is on trial
    bool downloading;                                ///< A download from the site is in progress
    char fail[96];                                   ///< "<v> refused <why>" / "<v> download <why>", empty = none
    char last[48];                                   ///< "none", "installed <v>", "rolledback-crash <v>", ...
    char nonce[UPDATE_NONCE_HEX + 1];                ///< Fresh random nonce, lowercase hex
};

/**
 * @brief Write the check-in body
 * @return Its length (out is NUL terminated), or 0 when it does not fit or a
 *         field holds a control character (which could forge a line)
 */
size_t buildCheckinBody(const CheckinInfo& in, char* out, size_t size);

/// BLAKE2b keyed with key (32-byte output) over data, as 64 lowercase hex characters plus NUL
void updateMacHex(const uint8_t key[UPDATE_KEY_LEN], const uint8_t* data, size_t len, char out[UPDATE_MAC_HEX + 1]);

/// Parse a shared key: exactly 64 hex characters (either case); false leaves key unspecified
bool parseKeyHex(const char* text, uint8_t key[UPDATE_KEY_LEN]);

/// Result of parseCheckinReply()
enum class ReplyKind : uint8_t {
    Ok,         ///< Authenticated, nothing to do
    Update,     ///< Authenticated, download version from path
    Malformed,  ///< Too long, or authenticated but not a reply this device understands
    BadAuth     ///< No auth line, a wrong MAC, or a bad nonce: not from the site holding the key
};

/// The fields of an Update reply
struct CheckinReply {
    char version[UPDATE_VERSION_MAX + 1];
    char path[UPDATE_PATH_MAX + 1];
};

/**
 * @brief Check a reply's MAC against the nonce this check-in sent, then read it
 *
 * Nothing in the reply is read before its MAC is verified (in constant time).
 * The final LF after the auth line is optional; CR and NUL bytes are refused.
 * @param nonce The check-in's nonce (32 hex characters)
 */
ReplyKind parseCheckinReply(const char* body, size_t len, const char* nonce, const uint8_t key[UPDATE_KEY_LEN],
                            CheckinReply& out);

/**
 * @brief The word after "status=" in an (unauthenticated) error reply, e.g. "badauth"
 *
 * Only [a-z0-9-] is kept: the word ends at the first other byte, so nothing from
 * the network can put control or escape sequences on the serial console.
 * @param out Receives the word, "" when there is none; cut to size - 1 characters
 */
void replyStatusWord(const char* body, size_t len, char* out, size_t size);
