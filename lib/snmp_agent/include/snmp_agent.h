#pragma once

/**
 * @file snmp_agent.h
 * @brief SNMP v1 and v2c request handling: Get, GetNext, GetBulk (v2c), Set (rejected).
 *
 * Pure C++, unit tested on the host. The firmware passes each received UDP
 * payload to SnmpAgent::handle() and sends back whatever it returns. Each
 * request is answered in its own version.
 *
 * Both versions:
 *   - Not v1/v2c, malformed, wrong community, or not a request: no response
 *     (counted in SnmpStats)
 *
 * v1 (RFC 1157):
 *   - Get: every OID must name an object, else noSuchName with the 1-based
 *     index of the first unknown varbind
 *   - GetNext: the next object after each OID, noSuchName past the end
 *   - Set: all objects are read-only, answered with noSuchName
 *   - Response larger than the buffer: tooBig with the request varbinds
 *
 * v2c (RFC 3416):
 *   - Get: noSuchObject / noSuchInstance per varbind, error-status stays 0
 *   - GetNext: endOfMibView per varbind past the end
 *   - GetBulk: non-repeaters once, then up to max-repetitions rows of the
 *     repeaters; stops early when every repeater is at endOfMibView, and
 *     returns as many varbinds as fit (at most SNMP_MAX_BULK_VARBINDS, to
 *     limit reflection) instead of tooBig
 *   - Set: notWritable for existing objects, noCreation for others
 *   - Response larger than the buffer: tooBig with an empty varbind list
 */

#include "snmp_mib.h"

/// Largest response the agent builds (RFC 1157 requires at least 484)
constexpr size_t SNMP_MAX_MESSAGE = 1472;

/// Most variable bindings in one response (decoders size their buffers by this)
constexpr uint8_t SNMP_MAX_RESPONSE_VARBINDS = 64;

/// Most variable bindings in one GetBulk response. Kept small so a short
/// request (possibly with a spoofed source) cannot be turned into a large
/// reflected reply; managers simply send more GetBulk requests.
constexpr uint8_t SNMP_MAX_BULK_VARBINDS = 16;

/**
 * @brief What handle() did with the last request, for access logging and traps
 */
enum class SnmpOutcome : uint8_t {
    Responded,     ///< Get/GetNext answered (including noSuchName / tooBig errors)
    RespondedBulk, ///< GetBulk answered; does not prove a round trip, so it earns no trust
    SetRejected,   ///< SetRequest answered with an error (all objects are read-only)
    ParseError,    ///< Dropped: malformed
    BadVersion,    ///< Dropped: not SNMPv1
    BadCommunity,  ///< Dropped: wrong community
    Unsupported,   ///< Dropped: trap/response PDU or too many varbinds
    NoRoom         ///< Dropped: not even an error response fits the buffer
};

class SnmpAgent {
public:
    /**
     * @param community NUL-terminated community string; the pointer is kept,
     *                  so it must stay valid (e.g. currentSettings.community)
     */
    explicit SnmpAgent(const char* community) : _community(community) {}

    /**
     * @brief Handle one request
     * @param request  Received UDP payload
     * @param response Output buffer
     * @param capacity Size of the output buffer
     * @param state    Values for the MIB, captured for this request
     * @return Length of the response to send, 0 to send nothing
     */
    size_t handle(const uint8_t* request, size_t length, uint8_t* response, size_t capacity,
                  const SnmpDeviceState& state);

    const SnmpStats& stats() const { return _stats; }

    /// Outcome of the most recent handle() call
    SnmpOutcome lastOutcome() const { return _outcome; }

private:
    bool communityMatches(const SnmpRequest& req) const;
    size_t respondError(const SnmpRequest& req, SnmpError error, uint8_t index, uint8_t* out, size_t cap);
    bool resolveV1(const Oid& name, bool getNext, const SnmpDeviceState& state);
    size_t handleV1(uint8_t* response, size_t capacity, const SnmpDeviceState& state);
    size_t handleV2c(uint8_t* response, size_t capacity, const SnmpDeviceState& state);
    size_t handleGetBulk(uint8_t* response, size_t capacity, const SnmpDeviceState& state);
    size_t finishResponse(SnmpResponseBuilder& builder, uint8_t* response, size_t capacity);

    const char* _community;
    SnmpStats _stats = {};
    SnmpOutcome _outcome = SnmpOutcome::Responded;
    SnmpRequest _req;                   // ~1 KB, kept out of the stack
    Oid _cursor[SNMP_MAX_VARBINDS];     // GetBulk repeater positions, ~2 KB, kept out of the stack
    MibObject _found;                   // Last lookup result, ~300 bytes, kept out of the stack
    uint8_t _retrieved = 0;             // Varbinds with a value in the response being built (snmpInTotalReqVars)
};
