// Host unit tests for lib/snmp_agent message layer: SNMP v1 request parsing and response building.
// Requests marked "net-snmp" were captured from real snmpget/snmpgetnext/snmpset/v2c tools.
// Run: pio test -e native -f native/test_snmp_message

#include <unity.h>
#include <string.h>
#include <snmp_message.h>

void setUp() {}
void tearDown() {}

// snmpget -v1 -c public 1.3.6.1.2.1.1.1.0
static const uint8_t GET_V1[] = {
    0x30, 0x29, 0x02, 0x01, 0x00, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa0, 0x1c, 0x02, 0x04, 0x46, 0x74, 0x1e, 0x94, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
    0x05, 0x00};

// snmpgetnext -v1 -c public 1.3.6.1.4.1.63050
static const uint8_t GETNEXT_V1[] = {
    0x30, 0x29, 0x02, 0x01, 0x00, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa1, 0x1c, 0x02, 0x04, 0x67, 0xf6, 0x52, 0xc1, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x83, 0xec, 0x4a,
    0x05, 0x00};

// snmpget -v1 -c public 1.3.6.1.2.1.1.3.0 1.3.6.1.4.1.63050.1.0
static const uint8_t GET_TWO_V1[] = {
    0x30, 0x39, 0x02, 0x01, 0x00, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa0, 0x2c, 0x02, 0x04, 0x10, 0xd6, 0xf5, 0x95, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x30, 0x1e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x03, 0x00,
    0x05, 0x00, 0x30, 0x0e, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x83, 0xec, 0x4a,
    0x01, 0x00, 0x05, 0x00};

// net-snmp v2c requests
// snmpget -v2c -c public 1.3.6.1.2.1.1.1.0
static const uint8_t GET_V2C[] = {
    0x30, 0x29, 0x02, 0x01, 0x01, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa0, 0x1c, 0x02, 0x04, 0x4d, 0x40, 0xf4, 0x5f, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
    0x05, 0x00};

// snmpbulkget -v2c -c public -Cn1 -Cr3 1.3.6.1.2.1.1.3.0 1.3.6.1.2.1.1
static const uint8_t BULK_V2C[] = {
    0x30, 0x35, 0x02, 0x01, 0x01, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa5, 0x28, 0x02, 0x04, 0x5d, 0x1b, 0x27, 0x63, 0x02, 0x01, 0x01, 0x02, 0x01, 0x03,
    0x30, 0x1a, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x03, 0x00,
    0x05, 0x00, 0x30, 0x0a, 0x06, 0x06, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x05, 0x00};

static Oid oid(const char* text) {
    Oid o;
    TEST_ASSERT_TRUE_MESSAGE(oidFromString(text, o), text);
    return o;
}

static void assertOid(const char* expected, const Oid& actual) {
    char buf[128];
    oidToString(actual, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

/// Copy a request and patch one byte, for building variants of a captured packet
static size_t patched(uint8_t* dst, const uint8_t* src, size_t len, size_t pos, uint8_t value) {
    memcpy(dst, src, len);
    dst[pos] = value;
    return len;
}

/// Decoded response fields
struct Response {
    int32_t version;
    char community[32];
    uint8_t pduType;
    int32_t requestId, errorStatus, errorIndex;
    uint8_t count;
    Oid names[SNMP_MAX_VARBINDS];
    uint8_t valueTags[SNMP_MAX_VARBINDS];
    int32_t ints[SNMP_MAX_VARBINDS];
};

static Response decodeResponse(const uint8_t* buf, size_t len) {
    Response r;
    memset(&r, 0, sizeof(r));
    BerReader top(buf, len), msg, pdu, list, vb;
    const uint8_t* comm;
    size_t commLen;
    TEST_ASSERT_TRUE(top.readSequence(BerTag::Sequence, msg));
    TEST_ASSERT_TRUE(top.atEnd());
    TEST_ASSERT_TRUE(msg.readInteger(r.version));
    TEST_ASSERT_TRUE(msg.readOctetString(comm, commLen));
    TEST_ASSERT_TRUE(commLen < sizeof(r.community));
    memcpy(r.community, comm, commLen);
    TEST_ASSERT_TRUE(msg.peekTag(r.pduType));
    TEST_ASSERT_TRUE(msg.readSequence(r.pduType, pdu));
    TEST_ASSERT_TRUE(msg.atEnd());
    TEST_ASSERT_TRUE(pdu.readInteger(r.requestId));
    TEST_ASSERT_TRUE(pdu.readInteger(r.errorStatus));
    TEST_ASSERT_TRUE(pdu.readInteger(r.errorIndex));
    TEST_ASSERT_TRUE(pdu.readSequence(BerTag::Sequence, list));
    TEST_ASSERT_TRUE(pdu.atEnd());
    while (list.remaining() > 0) {
        TEST_ASSERT_TRUE(list.readSequence(BerTag::Sequence, vb));
        TEST_ASSERT_TRUE(vb.readOid(r.names[r.count]));
        TEST_ASSERT_TRUE(vb.peekTag(r.valueTags[r.count]));
        if (r.valueTags[r.count] == BerTag::Integer) {
            TEST_ASSERT_TRUE(vb.readInteger(r.ints[r.count]));
        } else {
            TEST_ASSERT_TRUE(vb.skip());
        }
        TEST_ASSERT_TRUE(vb.atEnd());
        r.count++;
    }
    return r;
}

// ---- parsing ----

static void test_parse_netsnmp_get() {
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(GET_V1, sizeof(GET_V1), req));
    TEST_ASSERT_EQUAL_INT32(0, req.version);
    TEST_ASSERT_EQUAL_size_t(6, req.communityLength);
    TEST_ASSERT_EQUAL_MEMORY("public", req.community, 6);
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetRequest, req.pduType);
    TEST_ASSERT_EQUAL_HEX32(0x46741e94, (uint32_t)req.requestId);
    TEST_ASSERT_EQUAL_UINT8(1, req.varbindCount);
    assertOid("1.3.6.1.2.1.1.1.0", req.names[0]);
    TEST_ASSERT_EQUAL_size_t(2, req.valueLengths[0]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Null, req.values[0][0]);
}

static void test_parse_netsnmp_getnext() {
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(GETNEXT_V1, sizeof(GETNEXT_V1), req));
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetNextRequest, req.pduType);
    assertOid("1.3.6.1.4.1.63050", req.names[0]);
}

static void test_parse_two_varbinds_in_order() {
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(GET_TWO_V1, sizeof(GET_TWO_V1), req));
    TEST_ASSERT_EQUAL_UINT8(2, req.varbindCount);
    assertOid("1.3.6.1.2.1.1.3.0", req.names[0]);
    assertOid("1.3.6.1.4.1.63050.1.0", req.names[1]);
}

static void test_parse_set_request_is_accepted_for_the_agent_to_reject() {
    uint8_t pkt[sizeof(GET_V1)];
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 13, BerTag::SetRequest), req));
    TEST_ASSERT_EQUAL_HEX8(BerTag::SetRequest, req.pduType);
}

static void test_parse_rejects_other_versions() {
    uint8_t pkt[sizeof(GET_V1)];
    SnmpRequest req;
    // 2 is unassigned, 3 = SNMPv3
    TEST_ASSERT_EQUAL(SnmpParseResult::BadVersion,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 4, 2), req));
    TEST_ASSERT_EQUAL(SnmpParseResult::BadVersion,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 4, 3), req));
}

// ---- v2c ----

static void test_parse_netsnmp_v2c_get() {
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(GET_V2C, sizeof(GET_V2C), req));
    TEST_ASSERT_EQUAL_INT32(SNMP_VERSION_2C, req.version);
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetRequest, req.pduType);
    assertOid("1.3.6.1.2.1.1.1.0", req.names[0]);
}

static void test_parse_netsnmp_getbulk() {
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(BULK_V2C, sizeof(BULK_V2C), req));
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetBulkRequest, req.pduType);
    TEST_ASSERT_EQUAL_INT32(1, req.nonRepeaters);
    TEST_ASSERT_EQUAL_INT32(3, req.maxRepetitions);
    TEST_ASSERT_EQUAL_UINT8(2, req.varbindCount);
    assertOid("1.3.6.1.2.1.1.3.0", req.names[0]);
    assertOid("1.3.6.1.2.1.1", req.names[1]);
}

static void test_getbulk_is_not_a_v1_pdu() {
    uint8_t pkt[sizeof(BULK_V2C)];
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::UnsupportedPdu,
                      snmpParseRequest(pkt, patched(pkt, BULK_V2C, sizeof(BULK_V2C), 4, 0), req));
}

static void test_v2c_rejects_inform_trap_and_report_pdus() {
    uint8_t pkt[sizeof(GET_V2C)];
    SnmpRequest req;
    const uint8_t tags[] = {BerTag::GetResponse, BerTag::InformRequest, BerTag::SNMPv2Trap, BerTag::Report,
                            BerTag::Trap};
    for (uint8_t t : tags) {
        TEST_ASSERT_EQUAL(SnmpParseResult::UnsupportedPdu,
                          snmpParseRequest(pkt, patched(pkt, GET_V2C, sizeof(GET_V2C), 13, t), req));
    }
}

static void test_v2c_response_carries_version_1_and_exceptions() {
    SnmpRequest req;
    snmpParseRequest(GET_V2C, sizeof(GET_V2C), req);
    uint8_t buf[128];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    TEST_ASSERT_TRUE(b.add(req.names[0], SnmpValue::makeException(BerTag::NoSuchObject)));
    size_t len = b.finish();
    // Same layout as the request; version 1 is kept, the value is noSuchObject (80 00)
    const uint8_t expected[] = {
        0x30, 0x29, 0x02, 0x01, 0x01, 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',
        0xa2, 0x1c, 0x02, 0x04, 0x4d, 0x40, 0xf4, 0x5f, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
        0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
        0x80, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof(expected));
}

static void test_try_add_drops_a_varbind_that_does_not_fit_and_keeps_the_rest() {
    SnmpRequest req;
    snmpParseRequest(GET_V2C, sizeof(GET_V2C), req);
    uint8_t buf[70];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    TEST_ASSERT_TRUE(b.tryAdd(req.names[0], SnmpValue::makeInteger(1)));
    char big[100];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_FALSE(b.tryAdd(req.names[0], SnmpValue::makeString(big)));
    TEST_ASSERT_EQUAL_UINT8(1, b.count());
    size_t len = b.finish();
    TEST_ASSERT_GREATER_THAN(0, len);
    Response r = decodeResponse(buf, len);
    TEST_ASSERT_EQUAL_UINT8(1, r.count);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
}

static void test_v2c_error_codes() {
    TEST_ASSERT_EQUAL_INT32(6, (int32_t)SnmpError::NoAccess);
    TEST_ASSERT_EQUAL_INT32(11, (int32_t)SnmpError::NoCreation);
    TEST_ASSERT_EQUAL_INT32(17, (int32_t)SnmpError::NotWritable);
}

static void test_bulk_every_single_byte_corruption_is_safe() {
    uint8_t pkt[sizeof(BULK_V2C)];
    SnmpRequest req;
    for (size_t i = 0; i < sizeof(pkt); i++) {
        for (int v = 0; v < 256; v++) {
            if (snmpParseRequest(pkt, patched(pkt, BULK_V2C, sizeof(pkt), i, (uint8_t)v), req) ==
                SnmpParseResult::Ok) {
                TEST_ASSERT_TRUE(req.varbindCount <= SNMP_MAX_VARBINDS);
            }
        }
    }
    TEST_PASS();
}

static void test_parse_rejects_response_and_trap_pdus() {
    uint8_t pkt[sizeof(GET_V1)];
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::UnsupportedPdu,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 13, BerTag::GetResponse), req));
    TEST_ASSERT_EQUAL(SnmpParseResult::UnsupportedPdu,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 13, BerTag::Trap), req));
    TEST_ASSERT_EQUAL(SnmpParseResult::UnsupportedPdu,
                      snmpParseRequest(pkt, patched(pkt, GET_V1, sizeof(GET_V1), 13, 0xA5), req));
}

static void test_parse_rejects_trailing_bytes() {
    uint8_t pkt[sizeof(GET_V1) + 1];
    memcpy(pkt, GET_V1, sizeof(GET_V1));
    pkt[sizeof(GET_V1)] = 0x00;
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Malformed, snmpParseRequest(pkt, sizeof(pkt), req));
}

static void test_parse_rejects_every_truncation() {
    SnmpRequest req;
    for (size_t n = 0; n < sizeof(GET_TWO_V1); n++) {
        TEST_ASSERT_NOT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(GET_TWO_V1, n, req));
    }
}

static void test_parse_rejects_varbind_without_value() {
    // Message with a varbind that holds only an OID
    uint8_t buf[64];
    BerWriter w(buf, sizeof(buf));
    w.beginSequence();
    w.writeInteger(0);
    w.writeOctetString("public");
    w.beginSequence(BerTag::GetRequest);
    w.writeInteger(1);
    w.writeInteger(0);
    w.writeInteger(0);
    w.beginSequence();
    w.beginSequence();
    w.writeOid(oid("1.3.6.1"));
    w.endSequence();
    w.endSequence();
    w.endSequence();
    w.endSequence();
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Malformed, snmpParseRequest(buf, w.length(), req));
}

/// Build a GetRequest with n varbinds of 1.3.6.1.2.1.1.<i>.0
static size_t buildGet(uint8_t* buf, size_t cap, int n, const char* community = "public") {
    BerWriter w(buf, cap);
    w.beginSequence();
    w.writeInteger(0);
    w.writeOctetString(community);
    w.beginSequence(BerTag::GetRequest);
    w.writeInteger(7);
    w.writeInteger(0);
    w.writeInteger(0);
    w.beginSequence();
    for (int i = 0; i < n; i++) {
        Oid o = oid("1.3.6.1.2.1.1.1.0");
        o.arcs[7] = (uint32_t)i + 1;
        w.beginSequence();
        w.writeOid(o);
        w.writeNull();
        w.endSequence();
    }
    w.endSequence();
    w.endSequence();
    w.endSequence();
    TEST_ASSERT_FALSE(w.failed());
    return w.length();
}

static void test_parse_varbind_limit() {
    uint8_t buf[1024];
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(buf, buildGet(buf, sizeof(buf), SNMP_MAX_VARBINDS), req));
    TEST_ASSERT_EQUAL_UINT8(SNMP_MAX_VARBINDS, req.varbindCount);
    TEST_ASSERT_EQUAL(SnmpParseResult::TooManyVarbinds,
                      snmpParseRequest(buf, buildGet(buf, sizeof(buf), SNMP_MAX_VARBINDS + 1), req));
}

static void test_parse_empty_varbind_list() {
    uint8_t buf[64];
    SnmpRequest req;
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(buf, buildGet(buf, sizeof(buf), 0), req));
    TEST_ASSERT_EQUAL_UINT8(0, req.varbindCount);
}

static void test_parse_garbage_is_malformed() {
    SnmpRequest req;
    const uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL(SnmpParseResult::Malformed, snmpParseRequest(junk, sizeof(junk), req));
    TEST_ASSERT_EQUAL(SnmpParseResult::Malformed, snmpParseRequest(nullptr, 0, req));
}

static void test_parse_every_single_byte_corruption_is_safe() {
    uint8_t pkt[sizeof(GET_TWO_V1)];
    SnmpRequest req;
    int ok = 0;
    for (size_t i = 0; i < sizeof(pkt); i++) {
        for (int v = 0; v < 256; v++) {
            if (snmpParseRequest(pkt, patched(pkt, GET_TWO_V1, sizeof(pkt), i, (uint8_t)v), req) ==
                SnmpParseResult::Ok) {
                ok++;
                TEST_ASSERT_TRUE(req.varbindCount <= SNMP_MAX_VARBINDS);
            }
        }
    }
    // Changing e.g. the request-id or community bytes still parses
    TEST_ASSERT_GREATER_THAN(0, ok);
}

// ---- responses ----

static void test_response_builder_produces_get_response() {
    SnmpRequest req;
    snmpParseRequest(GET_V1, sizeof(GET_V1), req);
    uint8_t buf[256];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    TEST_ASSERT_TRUE(b.add(req.names[0], SnmpValue::makeString("RF.Power-Monitor")));
    size_t len = b.finish();
    TEST_ASSERT_GREATER_THAN(0, len);

    Response r = decodeResponse(buf, len);
    TEST_ASSERT_EQUAL_INT32(0, r.version);
    TEST_ASSERT_EQUAL_STRING("public", r.community);
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetResponse, r.pduType);
    TEST_ASSERT_EQUAL_HEX32(0x46741e94, (uint32_t)r.requestId);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(0, r.errorIndex);
    TEST_ASSERT_EQUAL_UINT8(1, r.count);
    assertOid("1.3.6.1.2.1.1.1.0", r.names[0]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.valueTags[0]);
}

static void test_response_exact_bytes() {
    // GetResponse for GET_V1 with sysDescr = "Pico": byte-exact expected encoding
    const uint8_t expected[] = {
        0x30, 0x2d, 0x02, 0x01, 0x00, 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',
        0xa2, 0x20, 0x02, 0x04, 0x46, 0x74, 0x1e, 0x94, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
        0x30, 0x12, 0x30, 0x10, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
        0x04, 0x04, 'P', 'i', 'c', 'o'};
    SnmpRequest req;
    snmpParseRequest(GET_V1, sizeof(GET_V1), req);
    uint8_t buf[128];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    b.add(req.names[0], SnmpValue::makeString("Pico"));
    size_t len = b.finish();
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof(expected));
}

static void test_response_encodes_every_value_type() {
    SnmpRequest req;
    snmpParseRequest(GET_V1, sizeof(GET_V1), req);
    uint8_t buf[512];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    SnmpValue ip = SnmpValue::makeNull();
    ip.type = BerTag::IpAddress;
    ip.ip[0] = 10; ip.ip[1] = 10; ip.ip[2] = 6; ip.ip[3] = 111;
    TEST_ASSERT_TRUE(b.add(oid("1.3.1"), SnmpValue::makeInteger(-5)));
    TEST_ASSERT_TRUE(b.add(oid("1.3.2"), SnmpValue::makeUnsigned(BerTag::Counter32, 7)));
    TEST_ASSERT_TRUE(b.add(oid("1.3.3"), SnmpValue::makeUnsigned(BerTag::Gauge32, 8)));
    TEST_ASSERT_TRUE(b.add(oid("1.3.4"), SnmpValue::makeUnsigned(BerTag::TimeTicks, 9)));
    TEST_ASSERT_TRUE(b.add(oid("1.3.5"), SnmpValue::makeOid(oid("1.3.6.1.4.1.63050"))));
    TEST_ASSERT_TRUE(b.add(oid("1.3.6"), ip));
    TEST_ASSERT_TRUE(b.add(oid("1.3.7"), SnmpValue::makeNull()));
    TEST_ASSERT_TRUE(b.add(oid("1.3.8"), SnmpValue::makeString(nullptr)));
    size_t len = b.finish();
    Response r = decodeResponse(buf, len);
    const uint8_t tags[] = {BerTag::Integer, BerTag::Counter32, BerTag::Gauge32, BerTag::TimeTicks,
                            BerTag::ObjectId, BerTag::IpAddress, BerTag::Null, BerTag::OctetString};
    TEST_ASSERT_EQUAL_UINT8(8, r.count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tags, r.valueTags, 8);
    TEST_ASSERT_EQUAL_INT32(-5, r.ints[0]);
}

static void test_response_builder_rejects_unknown_value_type() {
    SnmpRequest req;
    snmpParseRequest(GET_V1, sizeof(GET_V1), req);
    uint8_t buf[128];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    SnmpValue bad = SnmpValue::makeNull();
    bad.type = 0x7E;
    TEST_ASSERT_FALSE(b.add(req.names[0], bad));
    TEST_ASSERT_EQUAL_size_t(0, b.finish());
}

static void test_response_builder_reports_full_buffer() {
    SnmpRequest req;
    snmpParseRequest(GET_V1, sizeof(GET_V1), req);
    uint8_t buf[60];
    SnmpResponseBuilder b(buf, sizeof(buf), req);
    char big[100];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_FALSE(b.add(req.names[0], SnmpValue::makeString(big)));
    TEST_ASSERT_EQUAL_size_t(0, b.finish());
}

static void test_error_response_echoes_request_varbinds() {
    SnmpRequest req;
    snmpParseRequest(GET_TWO_V1, sizeof(GET_TWO_V1), req);
    uint8_t buf[256];
    size_t len = snmpBuildErrorResponse(req, SnmpError::NoSuchName, 2, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    Response r = decodeResponse(buf, len);
    TEST_ASSERT_EQUAL_HEX8(BerTag::GetResponse, r.pduType);
    TEST_ASSERT_EQUAL_INT32(2, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(2, r.errorIndex);
    TEST_ASSERT_EQUAL_UINT8(2, r.count);
    assertOid("1.3.6.1.2.1.1.3.0", r.names[0]);
    assertOid("1.3.6.1.4.1.63050.1.0", r.names[1]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Null, r.valueTags[0]);

    // Apart from the PDU tag and error fields, the response mirrors the request
    TEST_ASSERT_EQUAL_size_t(sizeof(GET_TWO_V1), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GET_TWO_V1 + 27, buf + 27, sizeof(GET_TWO_V1) - 27);
}

static void test_error_response_fails_when_it_does_not_fit() {
    SnmpRequest req;
    snmpParseRequest(GET_TWO_V1, sizeof(GET_TWO_V1), req);
    uint8_t buf[20];
    TEST_ASSERT_EQUAL_size_t(0, snmpBuildErrorResponse(req, SnmpError::TooBig, 0, buf, sizeof(buf)));
}

static void test_community_preserved_byte_for_byte() {
    uint8_t buf[128];
    SnmpRequest req;
    size_t n = buildGet(buf, sizeof(buf), 1, "s3cr\x01t");
    TEST_ASSERT_EQUAL(SnmpParseResult::Ok, snmpParseRequest(buf, n, req));
    uint8_t out[128];
    SnmpResponseBuilder b(out, sizeof(out), req);
    b.add(req.names[0], SnmpValue::makeNull());
    Response r = decodeResponse(out, b.finish());
    TEST_ASSERT_EQUAL_STRING("s3cr\x01t", r.community);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_netsnmp_get);
    RUN_TEST(test_parse_netsnmp_getnext);
    RUN_TEST(test_parse_two_varbinds_in_order);
    RUN_TEST(test_parse_set_request_is_accepted_for_the_agent_to_reject);
    RUN_TEST(test_parse_rejects_other_versions);
    RUN_TEST(test_parse_rejects_response_and_trap_pdus);
    RUN_TEST(test_parse_netsnmp_v2c_get);
    RUN_TEST(test_parse_netsnmp_getbulk);
    RUN_TEST(test_getbulk_is_not_a_v1_pdu);
    RUN_TEST(test_v2c_rejects_inform_trap_and_report_pdus);
    RUN_TEST(test_v2c_response_carries_version_1_and_exceptions);
    RUN_TEST(test_try_add_drops_a_varbind_that_does_not_fit_and_keeps_the_rest);
    RUN_TEST(test_v2c_error_codes);
    RUN_TEST(test_bulk_every_single_byte_corruption_is_safe);
    RUN_TEST(test_parse_rejects_trailing_bytes);
    RUN_TEST(test_parse_rejects_every_truncation);
    RUN_TEST(test_parse_rejects_varbind_without_value);
    RUN_TEST(test_parse_varbind_limit);
    RUN_TEST(test_parse_empty_varbind_list);
    RUN_TEST(test_parse_garbage_is_malformed);
    RUN_TEST(test_parse_every_single_byte_corruption_is_safe);
    RUN_TEST(test_response_builder_produces_get_response);
    RUN_TEST(test_response_exact_bytes);
    RUN_TEST(test_response_encodes_every_value_type);
    RUN_TEST(test_response_builder_rejects_unknown_value_type);
    RUN_TEST(test_response_builder_reports_full_buffer);
    RUN_TEST(test_error_response_echoes_request_varbinds);
    RUN_TEST(test_error_response_fails_when_it_does_not_fit);
    RUN_TEST(test_community_preserved_byte_for_byte);
    return UNITY_END();
}
