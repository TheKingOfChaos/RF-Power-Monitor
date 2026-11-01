// Host unit tests for lib/snmp_agent: MIB contents and order, GetRequest/GetNextRequest/SetRequest
// handling, error responses, community check and statistics.
// Run: pio test -e native -f native/test_snmp_agent

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <snmp_agent.h>

static SnmpDeviceState state;
static uint8_t resp[SNMP_MAX_MESSAGE];

void setUp() {
    state.uptimeTicks = 123456;
    state.mainsPresent = true;
    state.mainsChanges = 3;
    state.mainsLastChangeTicks = 100000;
    state.mainsAlarm = false;
    state.sysDescr = "RF-Power-Monitor";
    state.sysContact = "ops@example.com";
    state.sysName = "power-monitor";
    state.sysLocation = "Rack 4";
    memset(&state.iface, 0, sizeof(state.iface));
    state.iface.linkUp = true;
    state.iface.speedBps = 100000000;
    const uint8_t mac[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};
    memcpy(state.iface.mac, mac, 6);
    state.iface.lastChangeTicks = 950;
    state.iface.counters.inOctets = 0x123456789ull;       // above 32 bits
    state.iface.counters.inUcastPkts = 1000;
    state.iface.counters.inMulticastPkts = 20;
    state.iface.counters.inBroadcastPkts = 30;
    state.iface.counters.outOctets = 500000;
    state.iface.counters.outUcastPkts = 900;
    state.iface.counters.outMulticastPkts = 2;
    state.iface.counters.outBroadcastPkts = 3;
    state.iface.counters.outErrors = 4;
    state.iface.counters.inDiscards = 5;
    state.iface.counters.inErrors = 6;
    state.iface.counters.inUnknownProtos = 7;
    state.cpuTempDeciC = 287;
    state.ambientTempValid = true;
    state.ambientTempDeciC = 271;
    state.lldp = nullptr;
}

void tearDown() {}

static void withSwitch(bool present);

// snmpget -v1 -c public 1.3.6.1.2.1.1.1.0 (net-snmp)
static const uint8_t NETSNMP_GET_SYSDESCR[] = {
    0x30, 0x29, 0x02, 0x01, 0x00, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
    0xa0, 0x1c, 0x02, 0x04, 0x46, 0x74, 0x1e, 0x94, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
    0x05, 0x00};

static Oid oid(const char* text) {
    Oid o;
    TEST_ASSERT_TRUE_MESSAGE(oidFromString(text, o), text);
    return o;
}

static const char* str(const Oid& o) {
    static char buf[4][128];
    static int n = 0;
    n = (n + 1) % 4;
    oidToString(o, buf[n], sizeof(buf[n]));
    return buf[n];
}

/// Build a request with the given PDU type and OIDs
static size_t buildRequest(uint8_t* buf, size_t cap, uint8_t pdu, const char* const* oids, int n,
                           const char* community = "public", int32_t version = 0, int32_t requestId = 4242) {
    BerWriter w(buf, cap);
    w.beginSequence();
    w.writeInteger(version);
    w.writeOctetString(community);
    w.beginSequence(pdu);
    w.writeInteger(requestId);
    w.writeInteger(0);
    w.writeInteger(0);
    w.beginSequence();
    for (int i = 0; i < n; i++) {
        w.beginSequence();
        w.writeOid(oid(oids[i]));
        if (pdu == BerTag::SetRequest) {
            w.writeInteger(1);
        } else {
            w.writeNull();
        }
        w.endSequence();
    }
    w.endSequence();
    w.endSequence();
    w.endSequence();
    TEST_ASSERT_FALSE(w.failed());
    return w.length();
}

/// A decoded GetResponse
struct Reply {
    int32_t version;
    int32_t requestId, errorStatus, errorIndex;
    char community[32];
    uint8_t count;
    Oid names[SNMP_MAX_RESPONSE_VARBINDS];
    uint8_t tags[SNMP_MAX_RESPONSE_VARBINDS];
    int32_t ints[SNMP_MAX_RESPONSE_VARBINDS];
    uint32_t uints[SNMP_MAX_RESPONSE_VARBINDS];
    char strings[SNMP_MAX_RESPONSE_VARBINDS][64];
    Oid oids[SNMP_MAX_RESPONSE_VARBINDS];
};

static Reply decode(const uint8_t* buf, size_t len) {
    Reply r;
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, len, "agent sent no response");
    BerReader top(buf, len), msg, pdu, list, vb;
    const uint8_t* c;
    size_t clen;
    TEST_ASSERT_TRUE(top.readSequence(BerTag::Sequence, msg));
    TEST_ASSERT_TRUE(top.atEnd());
    TEST_ASSERT_TRUE(msg.readInteger(r.version));
    TEST_ASSERT_TRUE(msg.readOctetString(c, clen));
    memcpy(r.community, c, clen < 31 ? clen : 31);
    TEST_ASSERT_TRUE(msg.readSequence(BerTag::GetResponse, pdu));
    TEST_ASSERT_TRUE(pdu.readInteger(r.requestId));
    TEST_ASSERT_TRUE(pdu.readInteger(r.errorStatus));
    TEST_ASSERT_TRUE(pdu.readInteger(r.errorIndex));
    TEST_ASSERT_TRUE(pdu.readSequence(BerTag::Sequence, list));
    while (list.remaining() > 0) {
        TEST_ASSERT_TRUE_MESSAGE(r.count < SNMP_MAX_RESPONSE_VARBINDS, "more varbinds than the agent may send");
        uint8_t i = r.count++;
        TEST_ASSERT_TRUE(list.readSequence(BerTag::Sequence, vb));
        TEST_ASSERT_TRUE(vb.readOid(r.names[i]));
        TEST_ASSERT_TRUE(vb.peekTag(r.tags[i]));
        const uint8_t* s;
        size_t slen;
        switch (r.tags[i]) {
        case BerTag::Integer: TEST_ASSERT_TRUE(vb.readInteger(r.ints[i])); break;
        case BerTag::Counter32:
        case BerTag::Gauge32:
        case BerTag::TimeTicks: TEST_ASSERT_TRUE(vb.readUnsigned(r.tags[i], r.uints[i])); break;
        case BerTag::OctetString:
            TEST_ASSERT_TRUE(vb.readOctetString(s, slen));
            memcpy(r.strings[i], s, slen < 63 ? slen : 63);
            break;
        case BerTag::ObjectId: TEST_ASSERT_TRUE(vb.readOid(r.oids[i])); break;
        case BerTag::NoSuchObject:
        case BerTag::NoSuchInstance:
        case BerTag::EndOfMibView:
            TEST_ASSERT_TRUE(vb.readOctetString(s, slen, r.tags[i]));
            TEST_ASSERT_EQUAL_size_t_MESSAGE(0, slen, "exception value must be empty");
            break;
        default: TEST_ASSERT_TRUE(vb.skip()); break;
        }
        TEST_ASSERT_TRUE(vb.atEnd());
    }
    return r;
}

static Reply request(SnmpAgent& agent, uint8_t pdu, const char* const* oids, int n) {
    uint8_t req[1024];
    size_t len = buildRequest(req, sizeof(req), pdu, oids, n);
    return decode(resp, agent.handle(req, len, resp, sizeof(resp), state));
}

static Reply requestV2c(SnmpAgent& agent, uint8_t pdu, const char* const* oids, int n) {
    uint8_t req[1024];
    size_t len = buildRequest(req, sizeof(req), pdu, oids, n, "public", SNMP_VERSION_2C);
    return decode(resp, agent.handle(req, len, resp, sizeof(resp), state));
}

/// GetBulkRequest (v2c) with the given non-repeaters and max-repetitions
static size_t buildBulk(uint8_t* buf, size_t cap, int32_t nonRepeaters, int32_t maxRepetitions,
                        const char* const* oids, int n) {
    BerWriter w(buf, cap);
    w.beginSequence();
    w.writeInteger(SNMP_VERSION_2C);
    w.writeOctetString("public");
    w.beginSequence(BerTag::GetBulkRequest);
    w.writeInteger(77);
    w.writeInteger(nonRepeaters);
    w.writeInteger(maxRepetitions);
    w.beginSequence();
    for (int i = 0; i < n; i++) {
        w.beginSequence();
        w.writeOid(oid(oids[i]));
        w.writeNull();
        w.endSequence();
    }
    w.endSequence();
    w.endSequence();
    w.endSequence();
    TEST_ASSERT_FALSE(w.failed());
    return w.length();
}

static Reply bulk(SnmpAgent& agent, int32_t nonRepeaters, int32_t maxRepetitions, const char* const* oids, int n,
                  size_t capacity = sizeof(resp)) {
    uint8_t req[512];
    size_t len = buildBulk(req, sizeof(req), nonRepeaters, maxRepetitions, oids, n);
    return decode(resp, agent.handle(req, len, resp, capacity, state));
}

// ---- MIB table ----

static void test_mib_is_strictly_sorted() {
    for (uint8_t i = 1; i < mibSize(); i++) {
        TEST_ASSERT_LESS_THAN_MESSAGE(0, oidCompare(mibEntry(i - 1).oid, mibEntry(i).oid), str(mibEntry(i).oid));
    }
}

static void test_mib_contains_expected_objects() {
    // lldpLocPortTable 2-4, lldpRemTable 4-12, system 1-7, ifNumber, ifTable 1-22, ifXTable 1-19, enterprise 1-6
    char expected[80][48];
    uint8_t n = 0;
    for (int i = 2; i <= 4; i++) snprintf(expected[n++], 48, "1.0.8802.1.1.2.1.3.7.1.%d.1", i);
    for (int i = 4; i <= 12; i++) snprintf(expected[n++], 48, "1.0.8802.1.1.2.1.4.1.1.%d.0.1.1", i);
    for (int i = 1; i <= 7; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.1.%d.0", i);
    snprintf(expected[n++], 48, "1.3.6.1.2.1.2.1.0");
    for (int i = 1; i <= 22; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.2.2.1.%d.1", i);
    for (int i = 1; i <= 19; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.31.1.1.1.%d.1", i);
    for (int i = 1; i <= 6; i++) snprintf(expected[n++], 48, "1.3.6.1.4.1.63050.%d.0", i);
    TEST_ASSERT_EQUAL_UINT8(n, mibSize());
    for (uint8_t i = 0; i < n; i++) TEST_ASSERT_EQUAL_STRING(expected[i], str(mibEntry(i).oid));
}

// ---- IF-MIB and temperature ----

static void test_if_table_values() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.2.1.0",       "1.3.6.1.2.1.2.2.1.1.1",  "1.3.6.1.2.1.2.2.1.2.1",
                          "1.3.6.1.2.1.2.2.1.3.1",   "1.3.6.1.2.1.2.2.1.4.1",  "1.3.6.1.2.1.2.2.1.5.1",
                          "1.3.6.1.2.1.2.2.1.7.1",   "1.3.6.1.2.1.2.2.1.8.1",  "1.3.6.1.2.1.2.2.1.9.1",
                          "1.3.6.1.2.1.2.2.1.10.1",  "1.3.6.1.2.1.2.2.1.11.1", "1.3.6.1.2.1.2.2.1.12.1",
                          "1.3.6.1.2.1.2.2.1.16.1",  "1.3.6.1.2.1.2.2.1.20.1"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 14);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);                       // ifNumber
    TEST_ASSERT_EQUAL_INT32(1, r.ints[1]);                       // ifIndex
    TEST_ASSERT_EQUAL_STRING("W5500 10/100 Ethernet", r.strings[2]);
    TEST_ASSERT_EQUAL_INT32(6, r.ints[3]);                       // ethernetCsmacd
    TEST_ASSERT_EQUAL_INT32(1500, r.ints[4]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Gauge32, r.tags[5]);
    TEST_ASSERT_EQUAL_UINT32(100000000, r.uints[5]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[6]);                       // admin up
    TEST_ASSERT_EQUAL_INT32(1, r.ints[7]);                       // oper up
    TEST_ASSERT_EQUAL_UINT32(950, r.uints[8]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Counter32, r.tags[9]);
    TEST_ASSERT_EQUAL_UINT32(0x23456789u, r.uints[9]);           // ifInOctets wraps at 32 bits
    TEST_ASSERT_EQUAL_UINT32(1000, r.uints[10]);
    TEST_ASSERT_EQUAL_UINT32(50, r.uints[11]);                   // non-unicast = multicast + broadcast
    TEST_ASSERT_EQUAL_UINT32(500000, r.uints[12]);
    TEST_ASSERT_EQUAL_UINT32(4, r.uints[13]);                    // ifOutErrors
}

static void test_if_receive_discards_errors_and_unknown_protocols() {
    SnmpAgent agent("public");
    // ifInDiscards, ifInErrors, ifInUnknownProtos, ifOutDiscards
    const char* oids[] = {"1.3.6.1.2.1.2.2.1.13.1", "1.3.6.1.2.1.2.2.1.14.1", "1.3.6.1.2.1.2.2.1.15.1",
                          "1.3.6.1.2.1.2.2.1.19.1"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 4);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(BerTag::Counter32, r.tags[i]);
    TEST_ASSERT_EQUAL_UINT32(5, r.uints[0]);
    TEST_ASSERT_EQUAL_UINT32(6, r.uints[1]);
    TEST_ASSERT_EQUAL_UINT32(7, r.uints[2]);
    TEST_ASSERT_EQUAL_UINT32(0, r.uints[3]);                     // not visible to the firmware
}

static void test_if_phys_address_is_the_mac() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.2.2.1.6.1"};
    uint8_t req[128];
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", SNMP_VERSION_2C);
    size_t n = agent.handle(req, len, resp, sizeof(resp), state);
    const uint8_t mac[] = {0x04, 0x06, 0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, resp + n - sizeof(mac), sizeof(mac));
}

static void test_if_x_table_counter64_values() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.31.1.1.1.1.1", "1.3.6.1.2.1.31.1.1.1.6.1", "1.3.6.1.2.1.31.1.1.1.10.1",
                          "1.3.6.1.2.1.31.1.1.1.15.1"};
    uint8_t req[256];
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 4, "public", SNMP_VERSION_2C);
    size_t n = agent.handle(req, len, resp, sizeof(resp), state);
    // Decode the Counter64 values directly
    BerReader top(resp, n), msg, pdu, list, vb;
    int32_t x;
    const uint8_t* s;
    size_t sl;
    top.readSequence(BerTag::Sequence, msg);
    msg.readInteger(x);
    msg.readOctetString(s, sl);
    msg.readSequence(BerTag::GetResponse, pdu);
    pdu.readInteger(x);
    pdu.readInteger(x);
    TEST_ASSERT_EQUAL_INT32(0, x);
    pdu.readInteger(x);
    pdu.readSequence(BerTag::Sequence, list);
    Oid name;
    list.readSequence(BerTag::Sequence, vb);
    vb.readOid(name);
    TEST_ASSERT_TRUE(vb.readOctetString(s, sl));
    TEST_ASSERT_EQUAL_MEMORY("eth0", s, 4);
    uint64_t v64;
    list.readSequence(BerTag::Sequence, vb);
    vb.readOid(name);
    TEST_ASSERT_TRUE(vb.readUnsigned64(BerTag::Counter64, v64));
    TEST_ASSERT_TRUE(v64 == 0x123456789ull);                      // ifHCInOctets keeps all 64 bits
    list.readSequence(BerTag::Sequence, vb);
    vb.readOid(name);
    TEST_ASSERT_TRUE(vb.readUnsigned64(BerTag::Counter64, v64));
    TEST_ASSERT_TRUE(v64 == 500000);
    uint32_t mbps;
    list.readSequence(BerTag::Sequence, vb);
    vb.readOid(name);
    TEST_ASSERT_TRUE(vb.readUnsigned(BerTag::Gauge32, mbps));
    TEST_ASSERT_EQUAL_UINT32(100, mbps);                           // ifHighSpeed
    TEST_ASSERT_FALSE(top.failed());
}

static void test_link_down_shows_down_and_speed_zero() {
    SnmpAgent agent("public");
    state.iface.linkUp = false;
    const char* oids[] = {"1.3.6.1.2.1.2.2.1.8.1", "1.3.6.1.2.1.2.2.1.5.1", "1.3.6.1.2.1.31.1.1.1.15.1"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 3);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[0]);
    TEST_ASSERT_EQUAL_UINT32(0, r.uints[1]);
    TEST_ASSERT_EQUAL_UINT32(0, r.uints[2]);
}

static void test_counter64_does_not_exist_for_v1() {
    SnmpAgent agent("public");
    const char* hc[] = {"1.3.6.1.2.1.31.1.1.1.6.1"};
    Reply r = request(agent, BerTag::GetRequest, hc, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    // GetNext from ifOutBroadcastPkts skips the eight ifHC* columns
    const char* before[] = {"1.3.6.1.2.1.31.1.1.1.5.1"};
    r = request(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.31.1.1.1.14.1", str(r.names[0]));
}

static void test_v1_walk_has_every_object_except_counter64() {
    SnmpAgent agent("public");
    withSwitch(true);  // every object has an instance, LLDP-MIB included
    char current[128] = "1.0";
    uint8_t visited = 0;
    for (;;) {
        const char* oids[] = {current};
        Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
        if (r.errorStatus != 0) break;
        TEST_ASSERT_NOT_EQUAL(BerTag::Counter64, r.tags[0]);
        strcpy(current, str(r.names[0]));
        visited++;
        TEST_ASSERT_TRUE_MESSAGE(visited <= mibSize(), "walk did not terminate");
    }
    TEST_ASSERT_EQUAL_UINT8(mibSize() - 8, visited);
}

static void test_cpu_temperature() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.4.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(287, r.ints[0]);
    state.cpuTempDeciC = -55;
    r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(-55, r.ints[0]);
}

static void test_ambient_temperature_when_the_sensor_answers() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.5.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(271, r.ints[0]);
    state.ambientTempDeciC = -400;
    r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(-400, r.ints[0]);
}

static void test_ambient_temperature_absent_v1_get_is_no_such_name() {
    SnmpAgent agent("public");
    state.ambientTempValid = false;
    const char* oids[] = {"1.3.6.1.4.1.63050.4.0", "1.3.6.1.4.1.63050.5.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(2, r.errorIndex);
}

static void test_ambient_temperature_absent_v2c_get_is_no_such_instance() {
    SnmpAgent agent("public");
    state.ambientTempValid = false;
    const char* oids[] = {"1.3.6.1.4.1.63050.5.0", "1.3.6.1.4.1.63050.4.0"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.5.0", str(r.names[0]));
    TEST_ASSERT_EQUAL_INT32(287, r.ints[1]);  // the CPU temperature is still there
}

static void test_ambient_temperature_absent_is_skipped_by_getnext() {
    SnmpAgent agent("public");
    state.ambientTempValid = false;
    const char* oids[] = {"1.3.6.1.4.1.63050.4.0"};
    Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[0]));
    r = request(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[0]));
    const char* before[] = {"1.3.6.1.4.1.63050.3.0"};
    r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.4.0", str(r.names[0]));
}

static void test_ambient_temperature_absent_is_skipped_by_getbulk() {
    SnmpAgent agent("public");
    state.ambientTempValid = false;
    const char* oids[] = {"1.3.6.1.4.1.63050.3.0", "1.3.6.1.4.1.63050.4.0"};
    Reply r = bulk(agent, 1, 2, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.4.0", str(r.names[0]));  // non-repeater
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[1]));  // repeater skips .5.0
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[2]);              // then ends
    for (uint8_t i = 0; i < r.count; i++) TEST_ASSERT_NOT_EQUAL(BerTag::NoSuchInstance, r.tags[i]);
}

// ---- LLDP-MIB (lldpLocPortTable, lldpRemTable) ----

static SnmpLldpState lldpState;

/// The neighbour the device sees on a Juniper EX2200-C (values from test_lldp's captured frame)
static void withSwitch(bool present) {
    memset(&lldpState, 0, sizeof(lldpState));
    const uint8_t mac[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};
    memcpy(lldpState.localMac, mac, 6);
    lldpState.present = present;
    LldpNeighbour& n = lldpState.neighbour;
    n.chassisIdSubtype = 4;
    const uint8_t chassis[6] = {0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xc0};
    memcpy(n.chassisIdRaw, chassis, 6);
    n.chassisIdLen = 6;
    n.portIdSubtype = 7;
    memcpy(n.portIdRaw, "523", 3);
    n.portIdLen = 3;
    strcpy(n.portDescr, "ge-0/0/9.0");
    strcpy(n.sysName, "r169");
    strcpy(n.sysDescr, "Juniper Networks, Inc. ex2200-c-12p-2g");
    n.capsSupported = 0x0014;
    n.capsEnabled = 0x0014;
    n.ttl = 120;
    state.lldp = &lldpState;
}

static const char* const LLDP_LOC[] = {"1.0.8802.1.1.2.1.3.7.1.2.1", "1.0.8802.1.1.2.1.3.7.1.3.1",
                                       "1.0.8802.1.1.2.1.3.7.1.4.1"};

static void test_lldp_loc_port_table() {
    SnmpAgent agent("public");
    withSwitch(false);
    for (int v = 0; v < 2; v++) {
        Reply r = v ? requestV2c(agent, BerTag::GetRequest, LLDP_LOC, 3) : request(agent, BerTag::GetRequest, LLDP_LOC, 3);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        TEST_ASSERT_EQUAL_INT32(3, r.ints[0]);  // macAddress
        TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[1]);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(lldpState.localMac, r.strings[1], 6);
        TEST_ASSERT_EQUAL_STRING("W5500 10/100 Ethernet", r.strings[2]);
    }
}

static void test_lldp_rem_table_with_neighbour() {
    SnmpAgent agent("public");
    withSwitch(true);
    char oids[9][48];
    const char* names[9];
    for (int i = 0; i < 9; i++) {
        snprintf(oids[i], sizeof(oids[i]), "1.0.8802.1.1.2.1.4.1.1.%d.0.1.1", i + 4);
        names[i] = oids[i];
    }
    for (int v = 0; v < 2; v++) {
        Reply r = v ? requestV2c(agent, BerTag::GetRequest, names, 9) : request(agent, BerTag::GetRequest, names, 9);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        TEST_ASSERT_EQUAL_INT32(4, r.ints[0]);  // chassis: macAddress
        const uint8_t chassis[6] = {0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xc0};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(chassis, r.strings[1], 6);
        TEST_ASSERT_EQUAL_INT32(5, r.ints[2]);  // port: interfaceName, not Junos' locally assigned ifIndex
        TEST_ASSERT_EQUAL_STRING("ge-0/0/9.0", r.strings[3]);
        TEST_ASSERT_EQUAL_STRING("ge-0/0/9.0", r.strings[4]);
        TEST_ASSERT_EQUAL_STRING("r169", r.strings[5]);
        TEST_ASSERT_EQUAL_STRING("Juniper Networks, Inc. ex2200-c-12p-2g", r.strings[6]);
        // BITS (RFC 2578 7.1.4): bit 0 is the top bit of the first byte, LLDP TLVs count from the bottom bit;
        // bridge(2) + router(4), net-snmp with LLDP-MIB decodes it as "bridge(2) router(4)"
        const uint8_t caps[2] = {0x28, 0x00};
        TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[7]);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(caps, r.strings[7], 2);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(caps, r.strings[8], 2);
    }
}

/// GetNext chain from start (v1 or v2c), names of the first n objects
static void walkNames(SnmpAgent& agent, bool v2c, const char* start, int n, char out[][40]) {
    char current[128];
    strcpy(current, start);
    for (int i = 0; i < n; i++) {
        const char* oids[] = {current};
        Reply r = v2c ? requestV2c(agent, BerTag::GetNextRequest, oids, 1) : request(agent, BerTag::GetNextRequest, oids, 1);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        strcpy(out[i], str(r.names[0]));
        strcpy(current, out[i]);
    }
}

static void test_walk_without_neighbour_skips_rem_table() {
    SnmpAgent agent("public");
    withSwitch(false);
    for (int v = 0; v < 2; v++) {
        char names[4][40];
        walkNames(agent, v, "1.0", 4, names);
        for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_STRING(LLDP_LOC[i], names[i]);
        TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", names[3]);
    }
    const char* oids[] = {"1.0"};
    Reply r = bulk(agent, 0, 4, oids, 1);
    TEST_ASSERT_EQUAL_UINT8(4, r.count);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", str(r.names[3]));
}

static void test_walk_with_neighbour_lists_rem_columns_in_order() {
    SnmpAgent agent("public");
    withSwitch(true);
    char expected[14][48];
    for (int i = 0; i < 3; i++) strcpy(expected[i], LLDP_LOC[i]);
    for (int i = 0; i < 9; i++) snprintf(expected[3 + i], 48, "1.0.8802.1.1.2.1.4.1.1.%d.0.1.1", i + 4);
    strcpy(expected[12], "1.3.6.1.2.1.1.1.0");
    for (int v = 0; v < 2; v++) {
        char names[13][40];
        walkNames(agent, v, "1.0", 13, names);
        for (int i = 0; i < 13; i++) TEST_ASSERT_EQUAL_STRING(expected[i], names[i]);
    }
    const char* oids[] = {"1.0"};
    Reply r = bulk(agent, 0, 13, oids, 1);
    TEST_ASSERT_EQUAL_UINT8(13, r.count);
    for (int i = 0; i < 13; i++) TEST_ASSERT_EQUAL_STRING(expected[i], str(r.names[i]));
}

static void test_rem_get_without_neighbour() {
    SnmpAgent agent("public");
    withSwitch(false);
    const char* oids[] = {"1.0.8802.1.1.2.1.4.1.1.9.0.1.1"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
}

static void test_rem_column_other_index_is_no_such_instance() {
    SnmpAgent agent("public");
    withSwitch(true);
    const char* oids[] = {"1.0.8802.1.1.2.1.4.1.1.9.0.1.2", "1.0.8802.1.1.2.1.4.1.1.13.0.1.1", "1.0.8802.1.1.2.1.4.1.1.9.5.1.1",
                          "1.0.8802.1.1.2.1.3.7.1.4.2"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 4);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);  // lldpRemSysName exists, not this row
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchObject, r.tags[1]);    // no column 13
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[2]);  // another time mark: still lldpRemSysName
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[3]);  // lldpLocPortDesc, port 2
}

static void test_lldp_absent_without_state() {
    SnmpAgent agent("public");
    state.lldp = nullptr;  // host tools and the tests above: no LLDP data
    char names[1][40];
    walkNames(agent, true, "1.0", 1, names);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", names[0]);
}

static void test_mib2_walk_unchanged() {
    SnmpAgent agent("public");
    withSwitch(true);
    char names[1][40];
    walkNames(agent, false, "1.3.6.1.2.1", 1, names);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", names[0]);
}

static void test_get_mains_alarm_reports_state() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.6.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(0, r.ints[0]);
    state.mainsAlarm = true;
    r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
}

static void test_getnext_after_ambient_temperature_is_mains_alarm() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.5.0"};
    Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[0]));
}

static void test_getnext_after_mains_alarm_is_end_of_mib() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.6.0"};
    Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[0]);
}

static void test_no_such_instance_for_wrong_if_index() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.2.2.1.10.2"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
}

static void test_mib_find_and_next() {
    TEST_ASSERT_NOT_NULL(mibFind(oid("1.3.6.1.2.1.1.3.0")));
    TEST_ASSERT_NULL(mibFind(oid("1.3.6.1.2.1.1.3")));        // not a leaf instance
    TEST_ASSERT_NULL(mibFind(oid("1.3.6.1.2.1.1.3.0.0")));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", str(mibNext(oid("1.3"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", str(mibNext(oid("1.3.6.1.2.1.1.3.0"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", str(mibNext(oid("1.3.6.1.2.1.1.3.0.5"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.2.1.0", str(mibNext(oid("1.3.6.1.2.1.1.7.0"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.1.0", str(mibNext(oid("1.3.6.1.4.1.63050"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.4.0", str(mibNext(oid("1.3.6.1.4.1.63050.3.0"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.5.0", str(mibNext(oid("1.3.6.1.4.1.63050.4.0"))->oid));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(mibNext(oid("1.3.6.1.4.1.63050.5.0"))->oid));
    TEST_ASSERT_NULL(mibNext(oid("1.3.6.1.4.1.63050.6.0")));
    TEST_ASSERT_NULL(mibNext(oid("2.0")));
}

// ---- GetRequest ----

static void test_get_netsnmp_request_returns_sysdescr() {
    SnmpAgent agent("public");
    size_t len = agent.handle(NETSNMP_GET_SYSDESCR, sizeof(NETSNMP_GET_SYSDESCR), resp, sizeof(resp), state);
    Reply r = decode(resp, len);
    TEST_ASSERT_EQUAL_HEX32(0x46741e94, (uint32_t)r.requestId);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(1, r.count);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", str(r.names[0]));
    TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[0]);
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor", r.strings[0]);
    TEST_ASSERT_EQUAL_STRING("public", r.community);
}

static void test_get_every_object_value_and_type() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.2.0", "1.3.6.1.2.1.1.3.0",
                          "1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.6.0",
                          "1.3.6.1.2.1.1.7.0", "1.3.6.1.4.1.63050.1.0", "1.3.6.1.4.1.63050.2.0",
                          "1.3.6.1.4.1.63050.3.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 10);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(10, r.count);
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor", r.strings[0]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::ObjectId, r.tags[1]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050", str(r.oids[1]));
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[2]);
    TEST_ASSERT_EQUAL_UINT32(123456, r.uints[2]);
    TEST_ASSERT_EQUAL_STRING("ops@example.com", r.strings[3]);
    TEST_ASSERT_EQUAL_STRING("power-monitor", r.strings[4]);
    TEST_ASSERT_EQUAL_STRING("Rack 4", r.strings[5]);
    TEST_ASSERT_EQUAL_INT32(72, r.ints[6]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[7]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[7]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Counter32, r.tags[8]);
    TEST_ASSERT_EQUAL_UINT32(3, r.uints[8]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[9]);
    TEST_ASSERT_EQUAL_UINT32(100000, r.uints[9]);
}

static void test_get_reports_mains_lost() {
    SnmpAgent agent("public");
    state.mainsPresent = false;
    const char* oids[] = {"1.3.6.1.4.1.63050.1.0"};
    TEST_ASSERT_EQUAL_INT32(0, request(agent, BerTag::GetRequest, oids, 1).ints[0]);
}

static void test_get_empty_strings_are_allowed() {
    SnmpAgent agent("public");
    state.sysContact = "";
    state.sysLocation = nullptr;
    const char* oids[] = {"1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.6.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("", r.strings[0]);
    TEST_ASSERT_EQUAL_STRING("", r.strings[1]);
}

static void test_get_unknown_oid_is_no_such_name_with_index() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.99.0", "1.3.6.1.2.1.1.3.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 3);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(2, r.errorIndex);
    // v1 error responses echo the request varbinds with their NULL values
    TEST_ASSERT_EQUAL_UINT8(3, r.count);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.99.0", str(r.names[1]));
    TEST_ASSERT_EQUAL_HEX8(BerTag::Null, r.tags[0]);
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().outNoSuchName);
}

static void test_get_non_leaf_is_no_such_name() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1"};
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, request(agent, BerTag::GetRequest, oids, 1).errorStatus);
}

static void test_get_with_no_varbinds_returns_empty_response() {
    SnmpAgent agent("public");
    Reply r = request(agent, BerTag::GetRequest, nullptr, 0);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(0, r.count);
}

// ---- GetNextRequest ----

static void test_walk_from_root_visits_every_object_in_order_then_ends() {
    SnmpAgent agent("public");
    withSwitch(true);  // every object has an instance, LLDP-MIB included
    char current[128] = "1.0";
    uint8_t visited = 0;
    uint8_t expectedIndex = 0;
    for (;;) {
        const char* oids[] = {current};
        Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
        if (r.errorStatus != 0) {
            TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
            TEST_ASSERT_EQUAL_INT32(1, r.errorIndex);
            TEST_ASSERT_EQUAL_STRING(current, str(r.names[0]));  // echoed unchanged
            break;
        }
        TEST_ASSERT_TRUE_MESSAGE(visited < mibSize(), "walk did not terminate");
        // v1 sees every object except the Counter64 ones, in MIB order
        while (mibEntry(expectedIndex).get(state).type == BerTag::Counter64) expectedIndex++;
        TEST_ASSERT_EQUAL_STRING(str(mibEntry(expectedIndex).oid), str(r.names[0]));
        expectedIndex++;
        strcpy(current, str(r.names[0]));
        visited++;
    }
    TEST_ASSERT_EQUAL_UINT8(mibSize(), expectedIndex);
    TEST_ASSERT_EQUAL_UINT32(visited + 1, agent.stats().inGetNextRequests);
}

static void test_walk_of_enterprise_subtree() {
    SnmpAgent agent("public");
    const char* start[] = {"1.3.6.1.4.1.63050"};
    Reply r = request(agent, BerTag::GetNextRequest, start, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.1.0", str(r.names[0]));
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
}

static void test_getnext_with_several_varbinds() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1", "1.3.6.1.2.1.1.3.0", "1.3.6.1.4.1.63050.1.0"};
    Reply r = request(agent, BerTag::GetNextRequest, oids, 3);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", str(r.names[0]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", str(r.names[1]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.2.0", str(r.names[2]));
}

static void test_getnext_past_end_reports_index_of_that_varbind() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.4.1.63050.6.0"};
    Reply r = request(agent, BerTag::GetNextRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(2, r.errorIndex);
}

// ---- SetRequest ----

static void test_set_is_rejected_with_no_such_name() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.5.0"};
    Reply r = request(agent, BerTag::SetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(1, r.errorIndex);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);  // the set value is echoed
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().inSetRequests);
}

// ---- requests that get no response ----

static void test_wrong_community_gets_no_response() {
    SnmpAgent agent("secret");
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    const char* wrong[] = {"public", "secre", "secrets", "Secret", ""};
    for (const char* c : wrong) {
        size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, c);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, agent.handle(req, len, resp, sizeof(resp), state), c);
    }
    TEST_ASSERT_EQUAL_UINT32(5, agent.stats().inBadCommunity);
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "secret");
    TEST_ASSERT_GREATER_THAN(0, agent.handle(req, len, resp, sizeof(resp), state));
}

static void test_community_follows_the_configured_string() {
    char community[32] = "public";
    SnmpAgent agent(community);
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public");
    TEST_ASSERT_GREATER_THAN(0, agent.handle(req, len, resp, sizeof(resp), state));
    strcpy(community, "changed");
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(req, len, resp, sizeof(resp), state));
}

static void test_v3_and_unknown_versions_get_no_response() {
    SnmpAgent agent("public");
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", 3);
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(req, len, resp, sizeof(resp), state));
    len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", 2);
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(req, len, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_UINT32(2, agent.stats().inBadVersions);
}

// ---- v2c ----

static void test_v2c_get_answers_in_v2c() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.4.1.63050.1.0"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(SNMP_VERSION_2C, r.version);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor", r.strings[0]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[1]);
}

static void test_v2c_get_reports_exceptions_per_varbind_without_error_status() {
    SnmpAgent agent("public");
    // existing value, wrong instance of an existing object, an object that does not exist
    const char* oids[] = {"1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.5.7", "1.3.6.1.2.1.1.99.0", "1.3.6.1.2.1.1.1"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 4);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(0, r.errorIndex);
    TEST_ASSERT_EQUAL_UINT8(4, r.count);
    TEST_ASSERT_EQUAL_STRING("power-monitor", r.strings[0]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[1]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchObject, r.tags[2]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[3]);   // the object itself, no instance
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.99.0", str(r.names[2]));
}

static void test_v2c_getnext_past_end_is_end_of_mib_view() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.7.0", "1.3.6.1.4.1.63050.6.0"};
    Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.2.1.0", str(r.names[0]));   // ifNumber follows the system group
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[1]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[1]));
}

static void test_v2c_walk_ends_with_end_of_mib_view() {
    SnmpAgent agent("public");
    withSwitch(true);  // every object has an instance, LLDP-MIB included
    char current[128] = "1.0";
    uint8_t visited = 0;
    for (;;) {
        const char* oids[] = {current};
        Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        if (r.tags[0] == BerTag::EndOfMibView) break;
        TEST_ASSERT_TRUE_MESSAGE(visited < mibSize(), "walk did not terminate");
        TEST_ASSERT_EQUAL_STRING(str(mibEntry(visited).oid), str(r.names[0]));
        strcpy(current, str(r.names[0]));
        visited++;
    }
    TEST_ASSERT_EQUAL_UINT8(mibSize(), visited);
}

static void test_v2c_set_is_not_writable_or_no_creation() {
    SnmpAgent agent("public");
    const char* existing[] = {"1.3.6.1.2.1.1.5.0"};
    Reply r = requestV2c(agent, BerTag::SetRequest, existing, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NotWritable, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(1, r.errorIndex);
    const char* missing[] = {"1.3.6.1.2.1.1.99.0"};
    r = requestV2c(agent, BerTag::SetRequest, missing, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoCreation, r.errorStatus);
    TEST_ASSERT_EQUAL(SnmpOutcome::SetRejected, agent.lastOutcome());
}

static void test_v1_behaviour_is_unchanged_for_the_same_requests() {
    SnmpAgent agent("public");
    const char* unknown[] = {"1.3.6.1.2.1.1.99.0"};
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, request(agent, BerTag::GetRequest, unknown, 1).errorStatus);
    const char* last[] = {"1.3.6.1.4.1.63050.6.0"};
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, request(agent, BerTag::GetNextRequest, last, 1).errorStatus);
    const char* name[] = {"1.3.6.1.2.1.1.5.0"};
    Reply r = request(agent, BerTag::SetRequest, name, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(SNMP_VERSION_1, r.version);
}

// ---- GetBulk ----

static void test_getbulk_matches_netsnmp_request() {
    // snmpbulkget -v2c -Cn1 -Cr3 1.3.6.1.2.1.1.3.0 1.3.6.1.2.1.1 (captured from net-snmp)
    static const uint8_t NETSNMP_BULK[] = {
        0x30, 0x35, 0x02, 0x01, 0x01, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,
        0xa5, 0x28, 0x02, 0x04, 0x5d, 0x1b, 0x27, 0x63, 0x02, 0x01, 0x01, 0x02, 0x01, 0x03,
        0x30, 0x1a, 0x30, 0x0c, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x03, 0x00,
        0x05, 0x00, 0x30, 0x0a, 0x06, 0x06, 0x2b, 0x06, 0x01, 0x02, 0x01, 0x01, 0x05, 0x00};
    SnmpAgent agent("public");
    Reply r = decode(resp, agent.handle(NETSNMP_BULK, sizeof(NETSNMP_BULK), resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(4, r.count);
    // non-repeater: next after sysUpTime.0
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", str(r.names[0]));
    // repeater: three objects after 1.3.6.1.2.1.1
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", str(r.names[1]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.2.0", str(r.names[2]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.3.0", str(r.names[3]));
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().inGetBulkRequests);
}

static void test_getbulk_interleaves_several_repeaters() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.4.1.63050"};
    Reply r = bulk(agent, 0, 2, oids, 2);
    TEST_ASSERT_EQUAL_UINT8(4, r.count);
    // Row by row: repeater A, repeater B, repeater A, repeater B
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.2.0", str(r.names[0]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.1.0", str(r.names[1]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.3.0", str(r.names[2]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.2.0", str(r.names[3]));
}

static void test_getbulk_walk_returns_whole_mib_then_end_of_mib_view() {
    // Like snmpbulkwalk: repeat GetBulk from the last OID until endOfMibView
    SnmpAgent agent("public");
    withSwitch(true);
    char current[128] = "1.0";
    uint16_t seen = 0;
    bool ended = false;
    for (int requests = 0; requests < 50 && !ended; requests++) {
        const char* oids[] = {current};
        Reply r = bulk(agent, 0, 50, oids, 1);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        TEST_ASSERT_GREATER_THAN(0, r.count);
        TEST_ASSERT_LESS_OR_EQUAL(SNMP_MAX_BULK_VARBINDS, r.count);
        for (uint8_t i = 0; i < r.count; i++) {
            if (r.tags[i] == BerTag::EndOfMibView) {
                ended = true;
                break;
            }
            TEST_ASSERT_TRUE(seen < mibSize());
            TEST_ASSERT_EQUAL_STRING(str(mibEntry((uint8_t)seen).oid), str(r.names[i]));
            seen++;
        }
        strcpy(current, str(r.names[r.count - 1]));
    }
    TEST_ASSERT_TRUE_MESSAGE(ended, "walk did not reach endOfMibView");
    TEST_ASSERT_EQUAL_UINT16(mibSize(), seen);
}

static void test_getbulk_response_is_capped_against_amplification() {
    // A small request must not produce a huge response (UDP reflection)
    SnmpAgent agent("public");
    const char* oids[] = {"1.3", "1.3", "1.3", "1.3", "1.3", "1.3", "1.3", "1.3"};
    uint8_t req[256];
    size_t reqLen = buildBulk(req, sizeof(req), 0, INT32_MAX, oids, 8);
    size_t len = agent.handle(req, reqLen, resp, sizeof(resp), state);
    Reply r = decode(resp, len);
    TEST_ASSERT_EQUAL_UINT8(SNMP_MAX_BULK_VARBINDS, r.count);
    // Amplification well below a full 1472-byte packet
    TEST_ASSERT_LESS_THAN(SNMP_MAX_MESSAGE / 2, len);
}

static void test_getbulk_outcome_does_not_count_as_trust() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3"};
    bulk(agent, 0, 3, oids, 1);
    TEST_ASSERT_EQUAL(SnmpOutcome::RespondedBulk, agent.lastOutcome());
    const char* get[] = {"1.3.6.1.2.1.1.1.0"};
    requestV2c(agent, BerTag::GetRequest, get, 1);
    TEST_ASSERT_EQUAL(SnmpOutcome::Responded, agent.lastOutcome());
}

static void test_getbulk_zero_and_negative_parameters() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0"};
    // max-repetitions 0: only the non-repeaters
    Reply r = bulk(agent, 1, 0, oids, 2);
    TEST_ASSERT_EQUAL_UINT8(1, r.count);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.2.0", str(r.names[0]));
    // negative values count as 0 (RFC 3416)
    r = bulk(agent, -5, -5, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(0, r.count);
    // non-repeaters larger than the varbind count: all are non-repeaters
    r = bulk(agent, 99, 10, oids, 2);
    TEST_ASSERT_EQUAL_UINT8(2, r.count);
}

static void test_getbulk_huge_max_repetitions_is_bounded() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3", "1.3", "1.3", "1.3"};
    Reply r = bulk(agent, 0, INT32_MAX, oids, 4);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    // Stops at the varbind cap, or when every repeater reached endOfMibView
    uint16_t untilEnd = (uint16_t)(mibSize() + 1) * 4;
    TEST_ASSERT_EQUAL_UINT8(untilEnd < SNMP_MAX_BULK_VARBINDS ? untilEnd : SNMP_MAX_BULK_VARBINDS, r.count);
}

static void test_getbulk_truncates_to_fit_instead_of_too_big() {
    SnmpAgent agent("public");
    withSwitch(true);
    const char* oids[] = {"1.0"};
    uint8_t small[120];
    uint8_t req[128];
    size_t len = buildBulk(req, sizeof(req), 0, 50, oids, 1);
    Reply r = decode(small, agent.handle(req, len, small, sizeof(small), state));
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_GREATER_THAN(0, r.count);
    TEST_ASSERT_LESS_THAN(mibSize(), r.count);
    // What is there is the start of the MIB, in order
    for (uint8_t i = 0; i < r.count; i++) TEST_ASSERT_EQUAL_STRING(str(mibEntry(i).oid), str(r.names[i]));
}

static void test_getbulk_too_big_when_not_even_the_non_repeaters_fit() {
    SnmpAgent agent("public");
    char longDescr[300];
    memset(longDescr, 'd', sizeof(longDescr) - 1);
    longDescr[sizeof(longDescr) - 1] = '\0';
    state.sysDescr = longDescr;
    const char* oids[] = {"1.3"};
    uint8_t small[120];
    uint8_t req[128];
    size_t len = buildBulk(req, sizeof(req), 1, 5, oids, 1);
    Reply r = decode(small, agent.handle(req, len, small, sizeof(small), state));
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::TooBig, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(0, r.count);   // v2c tooBig carries no varbinds
}

static void test_v2c_get_too_big_has_empty_varbind_list() {
    SnmpAgent agent("public");
    char longDescr[300];
    memset(longDescr, 'd', sizeof(longDescr) - 1);
    longDescr[sizeof(longDescr) - 1] = '\0';
    state.sysDescr = longDescr;
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", SNMP_VERSION_2C);
    uint8_t small[100];
    Reply r = decode(small, agent.handle(req, len, small, sizeof(small), state));
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::TooBig, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(0, r.count);
}

static void test_getbulk_every_single_byte_corruption_is_handled_safely() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.1"};
    uint8_t base[128];
    size_t n = buildBulk(base, sizeof(base), 1, 3, oids, 2);
    uint8_t pkt[128];
    for (size_t i = 0; i < n; i++) {
        for (int v = 0; v < 256; v++) {
            memcpy(pkt, base, n);
            pkt[i] = (uint8_t)v;
            size_t len = agent.handle(pkt, n, resp, sizeof(resp), state);
            if (len) {
                BerReader top(resp, len), msg;
                TEST_ASSERT_TRUE(top.readSequence(BerTag::Sequence, msg));
                TEST_ASSERT_TRUE(top.atEnd());
            }
        }
    }
    TEST_PASS();
}

static void test_responses_and_traps_get_no_response() {
    SnmpAgent agent("public");
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetResponse, oids, 1);
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(req, len, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().inUnsupported);
}

static void test_malformed_packets_get_no_response() {
    SnmpAgent agent("public");
    const uint8_t junk[] = {0x30, 0x03, 0x02, 0x01};
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(junk, sizeof(junk), resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(nullptr, 0, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_UINT32(2, agent.stats().inParseErrors);
    TEST_ASSERT_EQUAL_UINT32(2, agent.stats().inPackets);
}

// ---- tooBig ----

static void test_response_too_big_for_buffer_is_too_big_error() {
    SnmpAgent agent("public");
    char longDescr[300];
    memset(longDescr, 'd', sizeof(longDescr) - 1);
    longDescr[sizeof(longDescr) - 1] = '\0';
    state.sysDescr = longDescr;
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1);
    uint8_t small[100];
    Reply r = decode(small, agent.handle(req, len, small, sizeof(small), state));
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::TooBig, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(0, r.errorIndex);
    TEST_ASSERT_EQUAL_UINT8(1, r.count);
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().outTooBig);
}

static void test_no_response_when_even_the_error_does_not_fit() {
    SnmpAgent agent("public");
    uint8_t tiny[10];
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(NETSNMP_GET_SYSDESCR, sizeof(NETSNMP_GET_SYSDESCR), tiny,
                                             sizeof(tiny), state));
}

static void test_max_varbinds_fit_in_max_message() {
    SnmpAgent agent("public");
    const char* oids[SNMP_MAX_VARBINDS];
    for (int i = 0; i < SNMP_MAX_VARBINDS; i++) oids[i] = "1.3.6.1.2.1.1.1.0";
    Reply r = request(agent, BerTag::GetRequest, oids, SNMP_MAX_VARBINDS);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(SNMP_MAX_VARBINDS, r.count);
}

// ---- statistics and robustness ----

static void test_statistics_count_each_kind() {
    SnmpAgent agent("public");
    const char* get[] = {"1.3.6.1.2.1.1.1.0"};
    request(agent, BerTag::GetRequest, get, 1);
    request(agent, BerTag::GetNextRequest, get, 1);
    request(agent, BerTag::SetRequest, get, 1);
    const SnmpStats& s = agent.stats();
    TEST_ASSERT_EQUAL_UINT32(3, s.inPackets);
    TEST_ASSERT_EQUAL_UINT32(1, s.inGetRequests);
    TEST_ASSERT_EQUAL_UINT32(1, s.inGetNextRequests);
    TEST_ASSERT_EQUAL_UINT32(1, s.inSetRequests);
    TEST_ASSERT_EQUAL_UINT32(3, s.outResponses);
}

static void test_request_id_is_echoed() {
    SnmpAgent agent("public");
    const int32_t ids[] = {0, 1, -1, INT32_MAX, INT32_MIN};
    const char* oids[] = {"1.3.6.1.2.1.1.3.0"};
    for (int32_t id : ids) {
        uint8_t req[128];
        size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", 0, id);
        TEST_ASSERT_EQUAL_INT32(id, decode(resp, agent.handle(req, len, resp, sizeof(resp), state)).requestId);
    }
}

static void test_every_single_byte_corruption_is_handled_safely() {
    // Any response produced from a corrupted request must itself be a valid GetResponse
    SnmpAgent agent("public");
    uint8_t pkt[sizeof(NETSNMP_GET_SYSDESCR)];
    int responses = 0;
    for (size_t i = 0; i < sizeof(pkt); i++) {
        for (int v = 0; v < 256; v++) {
            memcpy(pkt, NETSNMP_GET_SYSDESCR, sizeof(pkt));
            pkt[i] = (uint8_t)v;
            size_t len = agent.handle(pkt, sizeof(pkt), resp, sizeof(resp), state);
            if (len) {
                responses++;
                BerReader top(resp, len), msg;
                TEST_ASSERT_TRUE(top.readSequence(BerTag::Sequence, msg));
                TEST_ASSERT_TRUE(top.atEnd());
            }
        }
    }
    TEST_ASSERT_GREATER_THAN(0, responses);
}

static void test_last_outcome_reports_why_a_request_was_dropped() {
    SnmpAgent agent("public");
    uint8_t req[128];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};

    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1);
    agent.handle(req, len, resp, sizeof(resp), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::Responded, agent.lastOutcome());

    len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "wrong");
    agent.handle(req, len, resp, sizeof(resp), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::BadCommunity, agent.lastOutcome());

    len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", 3);  // SNMPv3
    agent.handle(req, len, resp, sizeof(resp), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::BadVersion, agent.lastOutcome());

    const uint8_t junk[] = {0x30, 0x01};
    agent.handle(junk, sizeof(junk), resp, sizeof(resp), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::ParseError, agent.lastOutcome());

    len = buildRequest(req, sizeof(req), BerTag::Trap, oids, 1);
    agent.handle(req, len, resp, sizeof(resp), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::Unsupported, agent.lastOutcome());

    len = buildRequest(req, sizeof(req), BerTag::SetRequest, oids, 1);
    TEST_ASSERT_GREATER_THAN(0, agent.handle(req, len, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL(SnmpOutcome::SetRejected, agent.lastOutcome());

    uint8_t tiny[10];
    len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1);
    agent.handle(req, len, tiny, sizeof(tiny), state);
    TEST_ASSERT_EQUAL(SnmpOutcome::NoRoom, agent.lastOutcome());

    // A noSuchName error is still a response
    const char* unknown[] = {"1.3.6.1.9.9.9.0"};
    len = buildRequest(req, sizeof(req), BerTag::GetRequest, unknown, 1);
    TEST_ASSERT_GREATER_THAN(0, agent.handle(req, len, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL(SnmpOutcome::Responded, agent.lastOutcome());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_mib_is_strictly_sorted);
    RUN_TEST(test_mib_contains_expected_objects);
    RUN_TEST(test_mib_find_and_next);
    RUN_TEST(test_if_table_values);
    RUN_TEST(test_if_receive_discards_errors_and_unknown_protocols);
    RUN_TEST(test_if_phys_address_is_the_mac);
    RUN_TEST(test_if_x_table_counter64_values);
    RUN_TEST(test_link_down_shows_down_and_speed_zero);
    RUN_TEST(test_counter64_does_not_exist_for_v1);
    RUN_TEST(test_v1_walk_has_every_object_except_counter64);
    RUN_TEST(test_cpu_temperature);
    RUN_TEST(test_ambient_temperature_when_the_sensor_answers);
    RUN_TEST(test_ambient_temperature_absent_v1_get_is_no_such_name);
    RUN_TEST(test_ambient_temperature_absent_v2c_get_is_no_such_instance);
    RUN_TEST(test_ambient_temperature_absent_is_skipped_by_getnext);
    RUN_TEST(test_ambient_temperature_absent_is_skipped_by_getbulk);
    RUN_TEST(test_lldp_loc_port_table);
    RUN_TEST(test_lldp_rem_table_with_neighbour);
    RUN_TEST(test_walk_without_neighbour_skips_rem_table);
    RUN_TEST(test_walk_with_neighbour_lists_rem_columns_in_order);
    RUN_TEST(test_rem_get_without_neighbour);
    RUN_TEST(test_rem_column_other_index_is_no_such_instance);
    RUN_TEST(test_lldp_absent_without_state);
    RUN_TEST(test_mib2_walk_unchanged);
    RUN_TEST(test_get_mains_alarm_reports_state);
    RUN_TEST(test_getnext_after_ambient_temperature_is_mains_alarm);
    RUN_TEST(test_getnext_after_mains_alarm_is_end_of_mib);
    RUN_TEST(test_no_such_instance_for_wrong_if_index);
    RUN_TEST(test_get_netsnmp_request_returns_sysdescr);
    RUN_TEST(test_get_every_object_value_and_type);
    RUN_TEST(test_get_reports_mains_lost);
    RUN_TEST(test_get_empty_strings_are_allowed);
    RUN_TEST(test_get_unknown_oid_is_no_such_name_with_index);
    RUN_TEST(test_get_non_leaf_is_no_such_name);
    RUN_TEST(test_get_with_no_varbinds_returns_empty_response);
    RUN_TEST(test_walk_from_root_visits_every_object_in_order_then_ends);
    RUN_TEST(test_walk_of_enterprise_subtree);
    RUN_TEST(test_getnext_with_several_varbinds);
    RUN_TEST(test_getnext_past_end_reports_index_of_that_varbind);
    RUN_TEST(test_set_is_rejected_with_no_such_name);
    RUN_TEST(test_wrong_community_gets_no_response);
    RUN_TEST(test_community_follows_the_configured_string);
    RUN_TEST(test_v3_and_unknown_versions_get_no_response);
    RUN_TEST(test_v2c_get_answers_in_v2c);
    RUN_TEST(test_v2c_get_reports_exceptions_per_varbind_without_error_status);
    RUN_TEST(test_v2c_getnext_past_end_is_end_of_mib_view);
    RUN_TEST(test_v2c_walk_ends_with_end_of_mib_view);
    RUN_TEST(test_v2c_set_is_not_writable_or_no_creation);
    RUN_TEST(test_v1_behaviour_is_unchanged_for_the_same_requests);
    RUN_TEST(test_getbulk_matches_netsnmp_request);
    RUN_TEST(test_getbulk_interleaves_several_repeaters);
    RUN_TEST(test_getbulk_walk_returns_whole_mib_then_end_of_mib_view);
    RUN_TEST(test_getbulk_response_is_capped_against_amplification);
    RUN_TEST(test_getbulk_outcome_does_not_count_as_trust);
    RUN_TEST(test_getbulk_zero_and_negative_parameters);
    RUN_TEST(test_getbulk_huge_max_repetitions_is_bounded);
    RUN_TEST(test_getbulk_truncates_to_fit_instead_of_too_big);
    RUN_TEST(test_getbulk_too_big_when_not_even_the_non_repeaters_fit);
    RUN_TEST(test_v2c_get_too_big_has_empty_varbind_list);
    RUN_TEST(test_getbulk_every_single_byte_corruption_is_handled_safely);
    RUN_TEST(test_responses_and_traps_get_no_response);
    RUN_TEST(test_malformed_packets_get_no_response);
    RUN_TEST(test_response_too_big_for_buffer_is_too_big_error);
    RUN_TEST(test_no_response_when_even_the_error_does_not_fit);
    RUN_TEST(test_max_varbinds_fit_in_max_message);
    RUN_TEST(test_statistics_count_each_kind);
    RUN_TEST(test_request_id_is_echoed);
    RUN_TEST(test_every_single_byte_corruption_is_handled_safely);
    RUN_TEST(test_last_outcome_reports_why_a_request_was_dropped);
    return UNITY_END();
}
