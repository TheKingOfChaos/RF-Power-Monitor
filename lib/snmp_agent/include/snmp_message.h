#pragma once

/**
 * @file snmp_message.h
 * @brief SNMP v1 (RFC 1157) and v2c (RFC 1901, 3416) message parsing and response building.
 *
 * Pure C++ on top of lib/snmp_ber, unit tested on the host.
 *
 *   Message ::= SEQUENCE { version INTEGER (0 = v1, 1 = v2c), community OCTET STRING, data PDU }
 *   PDU     ::= [tag] SEQUENCE { request-id, error-status, error-index, VarBindList }
 *   GetBulk ::= [5]   SEQUENCE { request-id, non-repeaters, max-repetitions, VarBindList }
 *
 * The response always uses the version of the request.
 */

#include <snmp_ber.h>

/// Most variable bindings accepted in one request
constexpr uint8_t SNMP_MAX_VARBINDS = 16;

/// version field values
constexpr int32_t SNMP_VERSION_1 = 0;
constexpr int32_t SNMP_VERSION_2C = 1;

/// error-status values (RFC 1157 4.1.1; v2c adds 6-18, RFC 3416)
enum class SnmpError : int32_t {
    NoError = 0,
    TooBig = 1,
    NoSuchName = 2,   ///< v1 only
    BadValue = 3,
    ReadOnly = 4,
    GenErr = 5,
    NoAccess = 6,     ///< v2c
    NoCreation = 11,  ///< v2c: the object does not exist and cannot be created
    NotWritable = 17  ///< v2c: the object exists but is read-only
};

/// Why a request could not be parsed
enum class SnmpParseResult : uint8_t {
    Ok = 0,
    Malformed,         ///< Not valid BER / not an SNMP message
    BadVersion,        ///< Valid message but neither v1 nor v2c (e.g. v3)
    UnsupportedPdu,    ///< Response, trap or unknown PDU type
    TooManyVarbinds    ///< More than SNMP_MAX_VARBINDS
};

/**
 * @brief A parsed request. Pointers refer into the received packet.
 */
struct SnmpRequest {
    int32_t version;           ///< SNMP_VERSION_1 or SNMP_VERSION_2C
    const uint8_t* community;
    size_t communityLength;
    uint8_t pduType;           ///< GetRequest, GetNextRequest, SetRequest, or GetBulkRequest (v2c)
    int32_t requestId;
    int32_t nonRepeaters;      ///< GetBulk only (the error-status field)
    int32_t maxRepetitions;    ///< GetBulk only (the error-index field)
    uint8_t varbindCount;
    Oid names[SNMP_MAX_VARBINDS];
    /// Encoded value TLV of each request varbind, echoed back in error responses
    const uint8_t* values[SNMP_MAX_VARBINDS];
    size_t valueLengths[SNMP_MAX_VARBINDS];
};

/**
 * @brief A value returned by the MIB
 */
struct SnmpValue {
    uint8_t type;              ///< BerTag of the value
    int32_t integer;           ///< Integer
    uint32_t unsigned32;       ///< Counter32, Gauge32, TimeTicks
    uint64_t unsigned64;       ///< Counter64 (v2c only)
    const uint8_t* data;       ///< OctetString (must stay valid until encoded)
    size_t length;
    Oid oid;                   ///< ObjectId
    uint8_t ip[4];             ///< IpAddress

    static SnmpValue makeInteger(int32_t v);
    static SnmpValue makeUnsigned(uint8_t type, uint32_t v);
    static SnmpValue makeCounter64(uint64_t v);
    /// OCTET STRING with binary content (e.g. a MAC address); data must stay valid until encoded
    static SnmpValue makeBytes(const uint8_t* data, size_t length);
    static SnmpValue makeString(const char* s);
    static SnmpValue makeIpAddress(const uint8_t addr[4]);  ///< Copies the 4 bytes
    static SnmpValue makeOid(const Oid& o);
    static SnmpValue makeNull();
    /// v2c exception: BerTag::NoSuchObject, NoSuchInstance or EndOfMibView
    static SnmpValue makeException(uint8_t tag);
};

/**
 * @brief Parse an SNMP v1 request
 *
 * The version is checked before the PDU, so a v2c/v3 packet reports
 * BadVersion even if the rest would not parse as v1.
 */
SnmpParseResult snmpParseRequest(const uint8_t* packet, size_t length, SnmpRequest& out);

/**
 * @brief Encode one value
 */
bool snmpWriteValue(BerWriter& w, const SnmpValue& value);

/**
 * @brief Build a GetResponse whose varbinds are the request's own (used for errors)
 *
 * RFC 1157: on error the response carries the request's variable bindings
 * unchanged, with error-status and error-index set. With echoVarbinds false
 * the list is empty, as RFC 3416 requires for a v2c tooBig.
 *
 * @return Response length, or 0 if it does not fit
 */
size_t snmpBuildErrorResponse(const SnmpRequest& req, SnmpError error, uint8_t errorIndex,
                              uint8_t* out, size_t capacity, bool echoVarbinds = true);

/**
 * @brief Incrementally builds a successful GetResponse
 *
 *   SnmpResponseBuilder b(buf, sizeof(buf), req);
 *   b.add(name, value); ...
 *   size_t len = b.finish();
 */
class SnmpResponseBuilder {
public:
    SnmpResponseBuilder(uint8_t* out, size_t capacity, const SnmpRequest& req);

    /// Append a variable binding; false if it does not fit (response is then tooBig)
    bool add(const Oid& name, const SnmpValue& value);

    /**
     * @brief Append a variable binding if it fits; otherwise leave the response as it was
     *
     * Used for GetBulk, which returns as many varbinds as fit (RFC 3416 4.2.3).
     */
    bool tryAdd(const Oid& name, const SnmpValue& value);

    /// Variable bindings added so far
    uint8_t count() const { return _count; }

    /// Close the message; returns its length, or 0 on failure
    size_t finish();

private:
    BerWriter _w;
    uint8_t _count = 0;
};
