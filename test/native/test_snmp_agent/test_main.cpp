// Host unit tests for lib/snmp_agent: MIB contents and order, GetRequest/GetNextRequest/SetRequest
// handling, error responses, community check and statistics.
// Run: pio test -e native -f native/test_snmp_agent

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <snmp_agent.h>
#include <snmp_trap.h>

static SnmpDeviceState state;
static SnmpHostState host;
static SnmpProtoCounters proto;
/// The object that sorts last: snmpSetSerialNo (SNMPv2-MIB) const char* const LAST_OID = "1.3.6.1.6.3.1.1.6.1.0";
static uint8_t resp[SNMP_MAX_MESSAGE];
/// The object that sorts last: snmpSetSerialNo (SNMPv2-MIB)
static const char* const LAST_OID = "1.3.6.1.6.3.1.1.6.1.0";

void setUp() {
    state.uptimeTicks = 123456;
    state.mainsPresent = true;
    state.mainsChanges = 3;
    state.mainsLastChangeTicks = 100000;
    state.mainsAlarm = false;
    state.fw = {};
    state.fw.runningSlot = 0;
    strcpy(state.fw.slotVersion[0], "3.4.0");
    strcpy(state.fw.slotVersion[1], "3.3.0");
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
    state.clockValid = true;  // NTP has set the clock: hrSystemDate present (like the ambient sensor)
    static const uint8_t DATE[11] = {0x07, 0xEA, 1, 2, 3, 4, 5, 0, '+', 1, 0};
    memcpy(state.dateAndTime, DATE, sizeof(DATE));
    state.lldp = nullptr;
    state.snmp = nullptr;
    state.snmpOutTraps = 0;
    host.ramTotalBytes = 262144;
    host.ramUsedBytes = 131000;  // 127.9 KB: rounds up to 128
    host.fsValid = true;
    host.fsTotalBytes = 262144;
    host.fsUsedBytes = 16384;
    host.cpuLoad[0] = 7;
    host.cpuLoad[1] = 1;
    host.serialNumber = "E6614103E7452D2F";
    host.firmwareRevision = "3.1.0";
    host.hardwareRevision = "W5500-EVB-Pico-PoE + carrier rev 1.1";
    state.host = &host;
    // Every counter a different value, so a mixed-up mapping shows
    uint32_t* field = &proto.ipInReceives;
    for (uint32_t i = 0; i < SNMP_PROTO_COUNTERS; i++) field[i] = 1000 + i;
    proto.ipDefaultTtl = 255;
    proto.ipReasmTimeoutS = 15;
    proto.tcpRtoMinMs = 1000;
    proto.tcpRtoMaxMs = 60000;
    proto.tcpMaxConn = 5;
    state.proto = &proto;
    // No IP address rows by default: the walk tests compare with the static table (mibEntry order)
    state.ip = nullptr;
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
        case BerTag::IpAddress:  // the 4 bytes, raw, in strings
            TEST_ASSERT_TRUE(vb.readOctetString(s, slen, BerTag::IpAddress));
            TEST_ASSERT_EQUAL_size_t(4, slen);
            memcpy(r.strings[i], s, 4);
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
    for (uint16_t i = 1; i < mibSize(); i++) {
        TEST_ASSERT_LESS_THAN_MESSAGE(0, oidCompare(mibEntry(i - 1).oid, mibEntry(i).oid), str(mibEntry(i).oid));
    }
}

static void test_mib_contains_expected_objects() {
    // lldpLocPortTable 2-4, lldpRemTable 4-12, system 1-8, sysORTable, ifNumber, ifTable 1-22, snmp group,
    // ifXTable 1-19, enterprise 1-6, snmpSetSerialNo
    static char expected[320][48];
    uint16_t n = 0;
    for (int i = 2; i <= 4; i++) snprintf(expected[n++], 48, "1.0.8802.1.1.2.1.3.7.1.%d.1", i);
    for (int i = 4; i <= 12; i++) snprintf(expected[n++], 48, "1.0.8802.1.1.2.1.4.1.1.%d.0.1.1", i);
    for (int i = 1; i <= 8; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.1.%d.0", i);
    for (int c = 2; c <= 4; c++) {
        for (int r = 1; r <= 9; r++) snprintf(expected[n++], 48, "1.3.6.1.2.1.1.9.1.%d.%d", c, r);
    }
    snprintf(expected[n++], 48, "1.3.6.1.2.1.2.1.0");
    for (int i = 1; i <= 22; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.2.2.1.%d.1", i);
    // ip 1-19 and 23, icmp 1-26, tcp 1-12, 14, 15, udp 1-4 (MIB-II scalars)
    for (int i = 1; i <= 19; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.4.%d.0", i);
    snprintf(expected[n++], 48, "1.3.6.1.2.1.4.23.0");
    for (int i = 1; i <= 26; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.5.%d.0", i);
    for (int i = 1; i <= 15; i++) {
        if (i != 13) snprintf(expected[n++], 48, "1.3.6.1.2.1.6.%d.0", i);  // 13 is tcpConnTable
    }
    for (int i = 1; i <= 4; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.7.%d.0", i);
    for (int i = 1; i <= 32; i++) {
        if (i != 7 && i != 23) snprintf(expected[n++], 48, "1.3.6.1.2.1.11.%d.0", i);  // 7 and 23 never assigned
    }
    // HOST-RESOURCES-MIB: hrSystem, hrMemorySize, hrStorageTable (RAM 1, LittleFS 2),
    // hrDeviceTable and hrProcessorTable (cores 768 and 769)
    static const int HR_SYSTEM[] = {1, 2, 5, 6, 7};  // 2: hrSystemDate (absent until NTP sets the clock)
    for (int i : HR_SYSTEM) snprintf(expected[n++], 48, "1.3.6.1.2.1.25.1.%d.0", i);
    snprintf(expected[n++], 48, "1.3.6.1.2.1.25.2.2.0");
    for (int c = 1; c <= 7; c++) {
        for (int r = 1; r <= 2; r++) snprintf(expected[n++], 48, "1.3.6.1.2.1.25.2.3.1.%d.%d", c, r);
    }
    for (int c = 1; c <= 6; c++) {
        for (int r = 768; r <= 769; r++) snprintf(expected[n++], 48, "1.3.6.1.2.1.25.3.2.1.%d.%d", c, r);
    }
    for (int c = 1; c <= 2; c++) {
        for (int r = 768; r <= 769; r++) snprintf(expected[n++], 48, "1.3.6.1.2.1.25.3.3.1.%d.%d", c, r);
    }
    for (int i = 1; i <= 19; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.31.1.1.1.%d.1", i);
    // ENTITY-MIB: entPhysicalTable columns 2-16 of the one entity, entLastChangeTime
    for (int i = 2; i <= 16; i++) snprintf(expected[n++], 48, "1.3.6.1.2.1.47.1.1.1.1.%d.1", i);
    snprintf(expected[n++], 48, "1.3.6.1.2.1.47.1.4.1.0");
    for (int i = 1; i <= 6; i++) snprintf(expected[n++], 48, "1.3.6.1.4.1.63050.%d.0", i);
    for (int i = 1; i <= 5; i++) snprintf(expected[n++], 48, "1.3.6.1.4.1.63050.7.%d.0", i);  // fwUpdate
    snprintf(expected[n++], 48, "1.3.6.1.6.3.1.1.6.1.0");
    TEST_ASSERT_EQUAL_UINT16(n, mibSize());
    for (uint16_t i = 0; i < n; i++) TEST_ASSERT_EQUAL_STRING(expected[i], str(mibEntry(i).oid));
}

// ---- SNMPv2-MIB (RFC 3418) ----

/// Value of one counter of the snmp group, 1.3.6.1.2.1.11.<n>.0, read with a v2c Get
static uint32_t snmpCounter(SnmpAgent& agent, int n) {
    char text[32];
    snprintf(text, sizeof(text), "1.3.6.1.2.1.11.%d.0", n);
    const char* oids[] = {text};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BerTag::Counter32, r.tags[0], text);
    return r.uints[0];
}

static void test_sysortable_lists_nine_mibs() {
    SnmpAgent agent("public");
    const char* ids[] = {"1.3.6.1.6.3.1",      "1.3.6.1.2.1.31", "1.3.6.1.2.1.48", "1.3.6.1.2.1.50",
                         "1.3.6.1.2.1.49",     "1.3.6.1.2.1.25.7.1", "1.3.6.1.2.1.47", "1.0.8802.1.1.2",
                         "1.3.6.1.4.1.63050"};
    const char* descrs[] = {"SNMPv2-MIB", "IF-MIB", "IP-MIB", "UDP-MIB", "TCP-MIB", "HOST-RESOURCES-MIB",
                            "ENTITY-MIB", "LLDP-MIB", "RF-POWER-MONITOR-MIB"};
    for (int row = 1; row <= 9; row++) {
        char id[32], descr[32], up[32];
        snprintf(id, sizeof(id), "1.3.6.1.2.1.1.9.1.2.%d", row);
        snprintf(descr, sizeof(descr), "1.3.6.1.2.1.1.9.1.3.%d", row);
        snprintf(up, sizeof(up), "1.3.6.1.2.1.1.9.1.4.%d", row);
        const char* oids[] = {id, descr, up};
        Reply r = request(agent, BerTag::GetRequest, oids, 3);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        TEST_ASSERT_EQUAL_STRING(ids[row - 1], str(r.oids[0]));
        TEST_ASSERT_EQUAL_STRING(descrs[row - 1], r.strings[1]);
        TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[2]);
        TEST_ASSERT_EQUAL_UINT32(0, r.uints[2]);
    }
    const char* last[] = {"1.3.6.1.2.1.1.8.0"};  // sysORLastChange
    Reply r = request(agent, BerTag::GetRequest, last, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[0]);
    TEST_ASSERT_EQUAL_UINT32(0, r.uints[0]);
}

static void test_snmp_group_counts_requests() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    const char* three[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.1.5.0"};
    const char* one[] = {"1.3.6.1.2.1.1.1.0"};
    request(agent, BerTag::GetRequest, three, 3);
    request(agent, BerTag::GetNextRequest, one, 1);
    const char* counters[] = {"1.3.6.1.2.1.11.1.0", "1.3.6.1.2.1.11.15.0", "1.3.6.1.2.1.11.16.0",
                              "1.3.6.1.2.1.11.13.0", "1.3.6.1.2.1.11.28.0"};
    Reply r = request(agent, BerTag::GetRequest, counters, 5);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT32(3, r.uints[0]);  // snmpInPkts: this request included
    TEST_ASSERT_EQUAL_UINT32(2, r.uints[1]);  // snmpInGetRequests
    TEST_ASSERT_EQUAL_UINT32(1, r.uints[2]);  // snmpInGetNexts
    TEST_ASSERT_EQUAL_UINT32(4, r.uints[3]);  // snmpInTotalReqVars: 3 + 1, this response not yet
    TEST_ASSERT_EQUAL_UINT32(2, r.uints[4]);  // snmpOutGetResponses
}

static void test_snmp_in_pkts_includes_the_current_request() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 1));
}

static void test_snmp_total_req_vars_skips_errors_and_exceptions() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    const char* bad[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.9"};
    request(agent, BerTag::GetRequest, bad, 2);    // v1 noSuchName: nothing retrieved
    requestV2c(agent, BerTag::GetRequest, bad, 2);  // v2c: sysDescr retrieved, noSuchObject not
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 13));
}

static void test_snmp_bad_version_community_and_parse_errors_are_counted() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    uint8_t req[256];
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    size_t len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "public", 3);  // SNMPv3
    agent.handle(req, len, resp, sizeof(resp), state);
    len = buildRequest(req, sizeof(req), BerTag::GetRequest, oids, 1, "wrong");
    agent.handle(req, len, resp, sizeof(resp), state);
    agent.handle(req, 5, resp, sizeof(resp), state);  // truncated
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 3));  // snmpInBadVersions
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 4));  // snmpInBadCommunityNames
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 6));  // snmpInASNParseErrs
}

static void test_snmp_in_traps_counts_trap_pdus() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    static const uint8_t addr[4] = {10, 0, 0, 2};
    uint8_t trap[256];
    size_t len = snmpBuildColdStartTrapV2c("public", addr, 100, 7, trap, sizeof(trap));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(trap, len, resp, sizeof(resp), state));
    len = snmpBuildColdStartTrap("public", addr, 100, trap, sizeof(trap));
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(trap, len, resp, sizeof(resp), state));
    const char* oids[] = {"1.3.6.1.2.1.1.1.0"};
    len = buildRequest(trap, sizeof(trap), BerTag::GetResponse, oids, 1);
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(trap, len, resp, sizeof(resp), state));
    TEST_ASSERT_EQUAL_UINT32(2, snmpCounter(agent, 19));  // snmpInTraps
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 18));  // snmpInGetResponses
}

static void test_snmp_silent_drops_counts_no_room() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    uint8_t tiny[10];
    TEST_ASSERT_EQUAL_size_t(0, agent.handle(NETSNMP_GET_SYSDESCR, sizeof(NETSNMP_GET_SYSDESCR), tiny,
                                             sizeof(tiny), state));
    TEST_ASSERT_EQUAL_UINT32(1, agent.stats().silentDrops);
    TEST_ASSERT_EQUAL_UINT32(1, snmpCounter(agent, 31));
}

static void test_snmp_out_pkts_adds_traps() {
    SnmpAgent agent("public");
    state.snmp = &agent.stats();
    state.snmpOutTraps = 5;
    TEST_ASSERT_EQUAL_UINT32(5, snmpCounter(agent, 29));  // snmpOutTraps
    TEST_ASSERT_EQUAL_UINT32(6, snmpCounter(agent, 2));   // snmpOutPkts: 1 response so far + 5 traps
}

static void test_snmp_enable_authen_traps_is_enabled() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.11.30.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
}

static void test_snmp_counters_are_zero_without_stats() {
    SnmpAgent agent("public");
    state.snmp = nullptr;
    TEST_ASSERT_EQUAL_UINT32(0, snmpCounter(agent, 1));
}

static void test_snmp_set_serial_no_is_zero_and_read_only() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.6.3.1.1.6.1.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(0, r.ints[0]);
    r = requestV2c(agent, BerTag::SetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NotWritable, r.errorStatus);
}

// ---- HOST-RESOURCES-MIB (RFC 2790) ----

static void test_hr_system_values() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.25.1.1.0", "1.3.6.1.2.1.25.1.5.0", "1.3.6.1.2.1.25.1.6.0",
                          "1.3.6.1.2.1.25.1.7.0", "1.3.6.1.2.1.25.2.2.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 5);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[0]);  // hrSystemUptime = sysUpTime
    TEST_ASSERT_EQUAL_UINT32(123456, r.uints[0]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Gauge32, r.tags[1]);    // hrSystemNumUsers
    TEST_ASSERT_EQUAL_UINT32(0, r.uints[1]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Gauge32, r.tags[2]);    // hrSystemProcesses: the two core loops
    TEST_ASSERT_EQUAL_UINT32(2, r.uints[2]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[3]);    // hrSystemMaxProcesses
    TEST_ASSERT_EQUAL_INT32(2, r.ints[3]);
    TEST_ASSERT_EQUAL_INT32(256, r.ints[4]);               // hrMemorySize in KB
}

static void test_hr_system_date_served_when_clock_valid() {
    SnmpAgent agent("public");
    const uint8_t when[11] = {0x07, 0xEA, 11, 7, 19, 42, 10, 0, '+', 1, 0};  // 2026-11-07 19:42:10 CET
    state.clockValid = true;
    memcpy(state.dateAndTime, when, sizeof(when));
    const char* oids[] = {"1.3.6.1.2.1.25.1.2.0"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[0]);
    TEST_ASSERT_EQUAL_MEMORY(when, r.strings[0], sizeof(when));
}

static void test_hr_system_date_absent_without_clock() {
    SnmpAgent agent("public");
    state.clockValid = false;
    const char* oids[] = {"1.3.6.1.2.1.25.1.2.0"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
    const char* before[] = {"1.3.6.1.2.1.25.1.1.0"};
    r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.1.5.0", str(r.names[0]));  // skipped by a walk
    state.clockValid = true;
    r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.1.2.0", str(r.names[0]));
}

static void test_hr_storage_reports_ram_and_littlefs() {
    SnmpAgent agent("public");
    const char* ram[] = {"1.3.6.1.2.1.25.2.3.1.1.1", "1.3.6.1.2.1.25.2.3.1.2.1", "1.3.6.1.2.1.25.2.3.1.3.1",
                         "1.3.6.1.2.1.25.2.3.1.4.1", "1.3.6.1.2.1.25.2.3.1.5.1", "1.3.6.1.2.1.25.2.3.1.6.1",
                         "1.3.6.1.2.1.25.2.3.1.7.1"};
    Reply r = request(agent, BerTag::GetRequest, ram, 7);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.2.1.2", str(r.oids[1]));  // hrStorageRam
    TEST_ASSERT_EQUAL_STRING("RAM", r.strings[2]);
    TEST_ASSERT_EQUAL_INT32(1024, r.ints[3]);
    TEST_ASSERT_EQUAL_INT32(256, r.ints[4]);
    TEST_ASSERT_EQUAL_INT32(128, r.ints[5]);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Counter32, r.tags[6]);
    const char* fs[] = {"1.3.6.1.2.1.25.2.3.1.1.2", "1.3.6.1.2.1.25.2.3.1.2.2", "1.3.6.1.2.1.25.2.3.1.3.2",
                        "1.3.6.1.2.1.25.2.3.1.5.2", "1.3.6.1.2.1.25.2.3.1.6.2"};
    r = request(agent, BerTag::GetRequest, fs, 5);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.2.1.9", str(r.oids[1]));  // hrStorageFlashMemory
    TEST_ASSERT_EQUAL_STRING("LittleFS (settings)", r.strings[2]);
    TEST_ASSERT_EQUAL_INT32(256, r.ints[3]);
    TEST_ASSERT_EQUAL_INT32(16, r.ints[4]);
}

static void test_hr_storage_used_rounds_up_to_whole_kb() {
    SnmpAgent agent("public");
    host.fsUsedBytes = 1;
    host.ramUsedBytes = 1024;
    const char* oids[] = {"1.3.6.1.2.1.25.2.3.1.6.2", "1.3.6.1.2.1.25.2.3.1.6.1"};
    Reply r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[1]);
}

static void test_hr_storage_littlefs_row_absent_when_invalid() {
    SnmpAgent agent("public");
    host.fsValid = false;
    const char* oids[] = {"1.3.6.1.2.1.25.2.3.1.5.2"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
    const char* after[] = {"1.3.6.1.2.1.25.2.3.1.5.1"};  // the RAM row: next is RAM's next column
    r = requestV2c(agent, BerTag::GetNextRequest, after, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.2.3.1.6.1", str(r.names[0]));
}

static void test_hr_processor_load_per_core() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.25.3.3.1.2.768", "1.3.6.1.2.1.25.3.3.1.2.769", "1.3.6.1.2.1.25.3.3.1.1.768"};
    Reply r = request(agent, BerTag::GetRequest, oids, 3);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(7, r.ints[0]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[1]);
    TEST_ASSERT_EQUAL_STRING("0.0", str(r.oids[2]));  // hrProcessorFrwID: unknown
}

static void test_hr_device_rows_describe_both_cores() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.25.3.2.1.1.768", "1.3.6.1.2.1.25.3.2.1.2.768", "1.3.6.1.2.1.25.3.2.1.3.768",
                          "1.3.6.1.2.1.25.3.2.1.3.769", "1.3.6.1.2.1.25.3.2.1.5.769", "1.3.6.1.2.1.25.3.2.1.6.769"};
    Reply r = request(agent, BerTag::GetRequest, oids, 6);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(768, r.ints[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.25.3.1.3", str(r.oids[1]));  // hrDeviceProcessor
    TEST_ASSERT_EQUAL_STRING("RP2040 core 0 (network, SNMP)", r.strings[2]);
    TEST_ASSERT_EQUAL_STRING("RP2040 core 1 (mains input)", r.strings[3]);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[4]);  // running
    TEST_ASSERT_EQUAL_HEX8(BerTag::Counter32, r.tags[5]);
}

static void test_host_resources_absent_without_host_state() {
    SnmpAgent agent("public");
    state.host = nullptr;
    const char* oids[] = {"1.3.6.1.2.1.25.3.3.1.2.768", "1.3.6.1.2.1.25.2.3.1.5.1", "1.3.6.1.2.1.47.1.1.1.1.11.1"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 3);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[i]);
    const char* before[] = {"1.3.6.1.2.1.11.32.0"};  // snmpProxyDrops: then HOST-RESOURCES, all skipped
    r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.31.1.1.1.1.1", str(r.names[0]));
}

// ---- IP, ICMP, TCP and UDP scalars (MIB-II counters from lwIP) ----

/// Get one scalar with a v2c Get; returns the reply with its single varbind
static Reply getOne(SnmpAgent& agent, const char* text) {
    const char* oids[] = {text};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, r.errorStatus, text);
    return r;
}

static void test_ip_scalars_from_state() {
    SnmpAgent agent("public");
    Reply r = getOne(agent, "1.3.6.1.2.1.4.1.0");
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[0]);  // ipForwarding: notForwarding
    TEST_ASSERT_EQUAL_INT32(255, getOne(agent, "1.3.6.1.2.1.4.2.0").ints[0]);
    TEST_ASSERT_EQUAL_INT32(15, getOne(agent, "1.3.6.1.2.1.4.13.0").ints[0]);  // ipReasmTimeout
    // Counters .3-.12 and .14-.19 in MIB order: ipInReceives (.3) is the struct's first field
    static const int COUNTERS[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19};
    for (uint32_t i = 0; i < 16; i++) {
        char text[32];
        snprintf(text, sizeof(text), "1.3.6.1.2.1.4.%d.0", COUNTERS[i]);
        r = getOne(agent, text);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(BerTag::Counter32, r.tags[0], text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000 + i, r.uints[0], text);
    }
    r = getOne(agent, "1.3.6.1.2.1.4.23.0");  // ipRoutingDiscards
    TEST_ASSERT_EQUAL_UINT32(1016, r.uints[0]);
}

static void test_icmp_scalars_map_in_and_out_arrays() {
    SnmpAgent agent("public");
    for (int i = 1; i <= 26; i++) {
        char text[32];
        snprintf(text, sizeof(text), "1.3.6.1.2.1.5.%d.0", i);
        Reply r = getOne(agent, text);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(BerTag::Counter32, r.tags[0], text);
        uint32_t expected = i <= 13 ? proto.icmpIn[i - 1] : proto.icmpOut[i - 14];
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, r.uints[0], text);
    }
}

static void test_tcp_scalars_from_state() {
    SnmpAgent agent("public");
    TEST_ASSERT_EQUAL_INT32(4, getOne(agent, "1.3.6.1.2.1.6.1.0").ints[0]);  // vanj
    TEST_ASSERT_EQUAL_INT32(1000, getOne(agent, "1.3.6.1.2.1.6.2.0").ints[0]);
    TEST_ASSERT_EQUAL_INT32(60000, getOne(agent, "1.3.6.1.2.1.6.3.0").ints[0]);
    TEST_ASSERT_EQUAL_INT32(5, getOne(agent, "1.3.6.1.2.1.6.4.0").ints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpActiveOpens, getOne(agent, "1.3.6.1.2.1.6.5.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpPassiveOpens, getOne(agent, "1.3.6.1.2.1.6.6.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpAttemptFails, getOne(agent, "1.3.6.1.2.1.6.7.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpEstabResets, getOne(agent, "1.3.6.1.2.1.6.8.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpInSegs, getOne(agent, "1.3.6.1.2.1.6.10.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpOutSegs, getOne(agent, "1.3.6.1.2.1.6.11.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpRetransSegs, getOne(agent, "1.3.6.1.2.1.6.12.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpInErrs, getOne(agent, "1.3.6.1.2.1.6.14.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.tcpOutRsts, getOne(agent, "1.3.6.1.2.1.6.15.0").uints[0]);
}

static void test_tcp_curr_estab_is_gauge32() {
    SnmpAgent agent("public");
    proto.tcpCurrEstab = 2;
    Reply r = getOne(agent, "1.3.6.1.2.1.6.9.0");
    TEST_ASSERT_EQUAL_HEX8(BerTag::Gauge32, r.tags[0]);
    TEST_ASSERT_EQUAL_UINT32(2, r.uints[0]);
}

static void test_udp_scalars_from_state() {
    SnmpAgent agent("public");
    TEST_ASSERT_EQUAL_UINT32(proto.udpInDatagrams, getOne(agent, "1.3.6.1.2.1.7.1.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.udpNoPorts, getOne(agent, "1.3.6.1.2.1.7.2.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.udpInErrors, getOne(agent, "1.3.6.1.2.1.7.3.0").uints[0]);
    TEST_ASSERT_EQUAL_UINT32(proto.udpOutDatagrams, getOne(agent, "1.3.6.1.2.1.7.4.0").uints[0]);
}

static void test_protocol_counters_absent_without_proto_state() {
    SnmpAgent agent("public");
    state.proto = nullptr;
    const char* oids[] = {"1.3.6.1.2.1.4.3.0", "1.3.6.1.2.1.5.1.0", "1.3.6.1.2.1.6.9.0", "1.3.6.1.2.1.7.1.0"};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 4);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[i]);
    const char* after[] = {"1.3.6.1.2.1.2.2.1.22.1"};  // ifSpecific: then ip..udp, all skipped
    r = requestV2c(agent, BerTag::GetNextRequest, after, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.11.1.0", str(r.names[0]));
}

// ---- IP-MIB addresses: rows that change at run time ----

static SnmpIpState ips;

static void setIpv6(SnmpIpAddress& a, const char* hex16, SnmpAddrOrigin origin, bool preferred = true) {
    a.ipv6 = true;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex16 + 2 * i, "%2x", &v);
        a.addr[i] = (uint8_t)v;
    }
    a.prefixLen = 64;
    a.origin = origin;
    a.preferred = preferred;
}

/// The device's addresses, deliberately not in index order: a global IPv6, IPv4, link-local
static void withAddresses() {
    memset(&ips, 0, sizeof(ips));
    setIpv6(ips.addr[0], "fd16d8334f0e3ece1cb2cffffe4ea22c", SnmpAddrOrigin::LinkLayer);
    ips.addr[1].ipv6 = false;
    const uint8_t v4[4] = {10, 132, 0, 58};
    memcpy(ips.addr[1].addr, v4, 4);
    ips.addr[1].prefixLen = 22;
    ips.addr[1].origin = SnmpAddrOrigin::Dhcp;
    ips.addr[1].preferred = true;
    setIpv6(ips.addr[2], "fe800000000000001cb2cffffe4ea22c", SnmpAddrOrigin::LinkLayer);
    ips.count = 3;
    ips.reasmMaxSize = 14820;  // lwIP: 10 fragments of 1480 bytes plus the header
    state.ip = &ips;
}

static const char* const V4_ROW = "1.4.10.132.0.58";  // ipAddressAddrType ipv4(1), length 4, address
static const char* const LL_ROW = "2.16.254.128.0.0.0.0.0.0.28.178.207.255.254.78.162.44";
static const char* const GLOBAL_ROW = "2.16.253.22.216.51.79.14.62.206.28.178.207.255.254.78.162.44";

/// The OID of ipAddressTable column c, row
static const char* addrOid(int column, const char* row) {
    static char buf[8][128];  // up to 8 in one request
    static int n = 0;
    n = (n + 1) % 8;
    snprintf(buf[n], sizeof(buf[n]), "1.3.6.1.2.1.4.34.1.%d.%s", column, row);
    return buf[n];
}

static void test_ip_tables_empty_without_addresses() {
    SnmpAgent agent("public");
    const char* before[] = {"1.3.6.1.2.1.4.19.0"};  // ipFragCreates: the tables would follow
    Reply r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.4.23.0", str(r.names[0]));
    const char* routing[] = {"1.3.6.1.2.1.4.23.0"};
    r = requestV2c(agent, BerTag::GetNextRequest, routing, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.5.1.0", str(r.names[0]));
    // An address the device no longer has: v1 noSuchName, v2c noSuchInstance (the column exists)
    const char* old[] = {"1.3.6.1.2.1.4.20.1.1.10.132.0.58"};
    r = request(agent, BerTag::GetRequest, old, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    r = requestV2c(agent, BerTag::GetRequest, old, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchInstance, r.tags[0]);
    withAddresses();
    ips.count = 0;  // a state with no rows behaves the same
    r = requestV2c(agent, BerTag::GetNextRequest, before, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.4.23.0", str(r.names[0]));
}

static void test_ipaddrtable_row_for_ipv4() {
    SnmpAgent agent("public");
    withAddresses();
    const char* oids[] = {"1.3.6.1.2.1.4.20.1.1.10.132.0.58", "1.3.6.1.2.1.4.20.1.2.10.132.0.58",
                          "1.3.6.1.2.1.4.20.1.3.10.132.0.58", "1.3.6.1.2.1.4.20.1.4.10.132.0.58",
                          "1.3.6.1.2.1.4.20.1.5.10.132.0.58"};
    Reply r = request(agent, BerTag::GetRequest, oids, 5);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    const uint8_t addr[4] = {10, 132, 0, 58}, mask[4] = {255, 255, 252, 0};
    TEST_ASSERT_EQUAL_HEX8(BerTag::IpAddress, r.tags[0]);
    TEST_ASSERT_EQUAL_MEMORY(addr, r.strings[0], 4);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[1]);  // ifIndex
    TEST_ASSERT_EQUAL_MEMORY(mask, r.strings[2], 4);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[3]);  // broadcast: host part all ones
    TEST_ASSERT_EQUAL_INT32(14820, r.ints[4]);  // what the stack can reassemble, from the state
    // Only IPv4 rows: the column ends after the one address
    const char* next[] = {"1.3.6.1.2.1.4.20.1.1.10.132.0.58"};
    r = requestV2c(agent, BerTag::GetNextRequest, next, 1);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.4.20.1.2.10.132.0.58", str(r.names[0]));
}

static void test_ipaddresstable_rows_sorted_ipv4_then_ipv6() {
    SnmpAgent agent("public");
    withAddresses();
    char current[128];
    strcpy(current, "1.3.6.1.2.1.4.34.1.3");  // ipAddressIfIndex
    const char* expected[] = {addrOid(3, V4_ROW), addrOid(3, GLOBAL_ROW), addrOid(3, LL_ROW)};
    static char want[3][128];
    for (int i = 0; i < 3; i++) strcpy(want[i], expected[i]);
    for (int i = 0; i < 3; i++) {
        const char* oids[] = {current};
        Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
        TEST_ASSERT_EQUAL_STRING(want[i], str(r.names[0]));
        TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
        strcpy(current, str(r.names[0]));
    }
    // fd16:: sorts before fe80:: (index bytes compare as numbers)
    const char* oids[] = {current};
    Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_STRING(addrOid(4, V4_ROW), str(r.names[0]));  // next column, first row
}

static void test_ipaddress_row_values() {
    SnmpAgent agent("public");
    withAddresses();
    ips.addr[0].preferred = false;  // a deprecated SLAAC address
    const char* oids[] = {addrOid(4, V4_ROW), addrOid(6, V4_ROW), addrOid(7, V4_ROW), addrOid(7, GLOBAL_ROW),
                          addrOid(6, LL_ROW), addrOid(8, LL_ROW), addrOid(10, LL_ROW), addrOid(11, LL_ROW)};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 8);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);  // unicast
    TEST_ASSERT_EQUAL_INT32(4, r.ints[1]);  // origin dhcp
    TEST_ASSERT_EQUAL_INT32(1, r.ints[2]);  // preferred
    TEST_ASSERT_EQUAL_INT32(2, r.ints[3]);  // deprecated
    TEST_ASSERT_EQUAL_INT32(5, r.ints[4]);  // linklayer
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[5]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[6]);  // rowStatus active
    TEST_ASSERT_EQUAL_INT32(2, r.ints[7]);  // storageType volatile
}

static void test_ipaddress_prefix_pointer_carries_prefix_length() {
    SnmpAgent agent("public");
    withAddresses();
    const char* oids[] = {addrOid(5, V4_ROW), addrOid(5, GLOBAL_ROW)};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    // ipAddressPrefixOrigin.ifIndex.type.length.prefix.prefixLength: the network, then its length
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.4.32.1.5.1.1.4.10.132.0.0.22", str(r.oids[0]));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.4.32.1.5.1.2.16.253.22.216.51.79.14.62.206.0.0.0.0.0.0.0.0.64",
                             str(r.oids[1]));
}

static void test_dynamic_rows_getnext_continues_after_a_removed_row() {
    SnmpAgent agent("public");
    withAddresses();
    // The manager is at the global address's row; then the address goes away
    const char* at[] = {addrOid(3, GLOBAL_ROW)};
    ips.addr[0] = ips.addr[2];  // fe80:: takes its place
    ips.count = 2;
    Reply r = requestV2c(agent, BerTag::GetNextRequest, at, 1);
    TEST_ASSERT_EQUAL_STRING(addrOid(3, LL_ROW), str(r.names[0]));
    // And from a row past every remaining one: the next column
    const char* last[] = {addrOid(3, "2.16.255.0.0.0.0.0.0.0.0.0.0.0.0.0.0.1")};
    r = requestV2c(agent, BerTag::GetNextRequest, last, 1);
    TEST_ASSERT_EQUAL_STRING(addrOid(4, V4_ROW), str(r.names[0]));
}

static void test_dynamic_rows_getnext_takes_an_address_added_mid_walk() {
    SnmpAgent agent("public");
    withAddresses();
    // A DHCPv6 address fd00::1 arrives between two GetNexts; its index sorts between IPv4 and fd16::
    static const char* const NEW_ROW = "2.16.253.0.0.0.0.0.0.0.0.0.0.0.0.0.0.1";
    setIpv6(ips.addr[3], "fd000000000000000000000000000001", SnmpAddrOrigin::Dhcp);
    ips.count = 4;
    // From the IPv4 row the walk goes on to the new row, which sorts next
    const char* fromV4[] = {addrOid(3, V4_ROW)};
    Reply r = requestV2c(agent, BerTag::GetNextRequest, fromV4, 1);
    TEST_ASSERT_EQUAL_STRING(addrOid(3, NEW_ROW), str(r.names[0]));
    // A walk already past it does not go back: no repeat, no loop
    const char* fromGlobal[] = {addrOid(3, GLOBAL_ROW)};
    r = requestV2c(agent, BerTag::GetNextRequest, fromGlobal, 1);
    TEST_ASSERT_EQUAL_STRING(addrOid(3, LL_ROW), str(r.names[0]));
}

static void test_dynamic_rows_getbulk_walks_columns_in_order() {
    SnmpAgent agent("public");
    withAddresses();
    const char* oids[] = {"1.3.6.1.2.1.4.34.1"};
    Reply r = bulk(agent, 0, 16, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_UINT8(16, r.count);
    for (uint8_t i = 1; i < r.count; i++) {
        TEST_ASSERT_LESS_THAN_MESSAGE(0, oidCompare(r.names[i - 1], r.names[i]), str(r.names[i]));
    }
    TEST_ASSERT_EQUAL_STRING(addrOid(3, V4_ROW), str(r.names[0]));
    TEST_ASSERT_EQUAL_STRING(addrOid(4, V4_ROW), str(r.names[3]));  // 3 rows per column
}

static void test_ipv6_index_is_28_arcs() {
    SnmpAgent agent("public");
    withAddresses();
    const char* oids[] = {addrOid(3, LL_ROW)};
    Reply r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);
    TEST_ASSERT_EQUAL_UINT8(28, r.names[0].length);
}

static void test_ip_address_walk_in_v1_sees_every_row() {
    SnmpAgent agent("public");
    withAddresses();
    char current[128] = "1.3.6.1.2.1.4.19.0";
    int rows = 0;
    for (;;) {
        const char* oids[] = {current};
        Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
        TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
        strcpy(current, str(r.names[0]));
        if (strncmp(current, "1.3.6.1.2.1.4.", 14) != 0 || strcmp(current, "1.3.6.1.2.1.4.34.2") > 0) break;
        if (strncmp(current, "1.3.6.1.2.1.4.20.", 17) == 0 || strncmp(current, "1.3.6.1.2.1.4.34.", 17) == 0) rows++;
        TEST_ASSERT_TRUE_MESSAGE(rows < 100, "walk did not end");
    }
    TEST_ASSERT_EQUAL_INT(5 + 9 * 3, rows);  // ipAddrTable: 5 columns x 1; ipAddressTable: 9 columns x 3
}

// ---- ENTITY-MIB (RFC 6933) ----

static void test_entity_row_names_the_device() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.2.1.47.1.1.1.1.2.1",  "1.3.6.1.2.1.47.1.1.1.1.3.1",  "1.3.6.1.2.1.47.1.1.1.1.5.1",
                          "1.3.6.1.2.1.47.1.1.1.1.7.1",  "1.3.6.1.2.1.47.1.1.1.1.8.1",  "1.3.6.1.2.1.47.1.1.1.1.9.1",
                          "1.3.6.1.2.1.47.1.1.1.1.11.1", "1.3.6.1.2.1.47.1.1.1.1.12.1", "1.3.6.1.2.1.47.1.1.1.1.13.1",
                          "1.3.6.1.2.1.47.1.1.1.1.16.1", "1.3.6.1.2.1.47.1.1.1.1.6.1",  "1.3.6.1.2.1.47.1.4.1.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 12);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor mains power monitor", r.strings[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050", str(r.oids[1]));
    TEST_ASSERT_EQUAL_INT32(3, r.ints[2]);                                 // chassis
    TEST_ASSERT_EQUAL_STRING("power-monitor", r.strings[3]);               // = sysName
    TEST_ASSERT_EQUAL_STRING("W5500-EVB-Pico-PoE + carrier rev 1.1", r.strings[4]);
    TEST_ASSERT_EQUAL_STRING("3.1.0", r.strings[5]);
    TEST_ASSERT_EQUAL_STRING("E6614103E7452D2F", r.strings[6]);
    TEST_ASSERT_EQUAL_STRING("Kenneth Foli J\xC3\xB8rgensen", r.strings[7]);  // UTF-8
    TEST_ASSERT_EQUAL_STRING("RF-Power-Monitor", r.strings[8]);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[9]);                                 // isFRU false
    TEST_ASSERT_EQUAL_INT32(-1, r.ints[10]);                               // parentRelPos: no parent
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, r.tags[11]);
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
    uint16_t visited = 0;
    for (;;) {
        const char* oids[] = {current};
        Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
        if (r.errorStatus != 0) break;
        TEST_ASSERT_NOT_EQUAL(BerTag::Counter64, r.tags[0]);
        strcpy(current, str(r.names[0]));
        visited++;
        TEST_ASSERT_TRUE_MESSAGE(visited <= mibSize(), "walk did not terminate");
    }
    TEST_ASSERT_EQUAL_UINT16(mibSize() - 8, visited);
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
    Reply r = bulk(agent, 1, 8, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.4.0", str(r.names[0]));  // non-repeater
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", str(r.names[1]));  // repeater skips .5.0
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.7.1.0", str(r.names[2]));  // fwUpdate group follows
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.7.5.0", str(r.names[6]));
    TEST_ASSERT_EQUAL_STRING(LAST_OID, str(r.names[7]));                  // snmpSetSerialNo
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[8]);              // then ends
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

// ---- fwUpdate group (63050.7) ----

static void test_get_fw_running_slot() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.7.1.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::Integer, r.tags[0]);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);  // slot A
    state.fw.runningSlot = 1;
    r = requestV2c(agent, BerTag::GetRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[0]);  // slot B
}

static void test_get_fw_slot_versions_empty_when_invalid() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.7.2.0", "1.3.6.1.4.1.63050.7.3.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[0]);
    TEST_ASSERT_EQUAL_STRING("3.4.0", r.strings[0]);
    TEST_ASSERT_EQUAL_STRING("3.3.0", r.strings[1]);
    state.fw.slotVersion[1][0] = '\0';  // the other slot does not verify
    r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_HEX8(BerTag::OctetString, r.tags[1]);
    TEST_ASSERT_EQUAL_STRING("", r.strings[1]);
}

static void test_get_fw_state_and_last_result() {
    SnmpAgent agent("public");
    const char* oids[] = {"1.3.6.1.4.1.63050.7.4.0", "1.3.6.1.4.1.63050.7.5.0"};
    Reply r = request(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(1, r.ints[0]);  // kept
    TEST_ASSERT_EQUAL_INT32(0, r.ints[1]);  // none
    state.fw.trial = true;
    state.fw.lastResult = 3;
    r = requestV2c(agent, BerTag::GetRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(2, r.ints[0]);  // trial
    TEST_ASSERT_EQUAL_INT32(3, r.ints[1]);
}

static void test_walk_goes_from_mains_alarm_into_fw_group() {
    SnmpAgent agent("public");
    char names[6][40];
    walkNames(agent, false, "1.3.6.1.4.1.63050.5.0", 3, names);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", names[0]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.7.1.0", names[1]);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.7.2.0", names[2]);
}

static void test_getnext_after_fw_group_ends_enterprise_tree() {
    SnmpAgent agent("public");
    // The enterprise tree ends with fwLastResult; the next object is snmpSetSerialNo (SNMPv2-MIB)
    const char* oids[] = {"1.3.6.1.4.1.63050.7.5.0"};
    Reply r = request(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING(LAST_OID, str(r.names[0]));
    r = requestV2c(agent, BerTag::GetNextRequest, oids, 1);
    TEST_ASSERT_EQUAL_STRING(LAST_OID, str(r.names[0]));
    // Nothing past the group inside the enterprise subtree: v1 noSuchName, v2c noSuchObject for the unused 7.6.0
    const char* inside[] = {"1.3.6.1.4.1.63050.7.6.0"};
    r = request(agent, BerTag::GetRequest, inside, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    r = requestV2c(agent, BerTag::GetRequest, inside, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::NoSuchObject, r.tags[0]);
    // ...and past the whole MIB as today
    const char* last[] = {LAST_OID};
    r = request(agent, BerTag::GetNextRequest, last, 1);
    TEST_ASSERT_EQUAL_INT32((int32_t)SnmpError::NoSuchName, r.errorStatus);
    r = requestV2c(agent, BerTag::GetNextRequest, last, 1);
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[0]);
}

static void test_getnext_after_the_last_object_is_end_of_mib() {
    SnmpAgent agent("public");
    const char* oids[] = {LAST_OID};
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

static MibObject found;  // ~300 bytes: not on the stack

/// OID of the next instance after text in the v2c view, or "end"
static const char* nextOf(const char* text) {
    return mibGetNext(oid(text), state, MibView::V2c, found) ? str(found.oid) : "end";
}

static void test_mib_get_and_get_next() {
    TEST_ASSERT_TRUE(mibGet(oid("1.3.6.1.2.1.1.3.0"), state, found));
    TEST_ASSERT_FALSE(mibGet(oid("1.3.6.1.2.1.1.3"), state, found));  // not a leaf instance
    TEST_ASSERT_FALSE(mibGet(oid("1.3.6.1.2.1.1.3.0.0"), state, found));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.1.0", nextOf("1.3"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", nextOf("1.3.6.1.2.1.1.3.0"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.4.0", nextOf("1.3.6.1.2.1.1.3.0.5"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.8.0", nextOf("1.3.6.1.2.1.1.7.0"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.2.1.0", nextOf("1.3.6.1.2.1.1.9.1.4.9"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.1.0", nextOf("1.3.6.1.4.1.63050"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.4.0", nextOf("1.3.6.1.4.1.63050.3.0"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.5.0", nextOf("1.3.6.1.4.1.63050.4.0"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", nextOf("1.3.6.1.4.1.63050.5.0"));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.7.1.0", nextOf("1.3.6.1.4.1.63050.6.0"));
    TEST_ASSERT_EQUAL_STRING(LAST_OID, nextOf("1.3.6.1.4.1.63050.7.5.0"));
    TEST_ASSERT_EQUAL_STRING("end", nextOf(LAST_OID));
    TEST_ASSERT_EQUAL_STRING("end", nextOf("2.0"));
}

static void test_mib_get_returns_the_instance_and_value() {
    TEST_ASSERT_TRUE(mibGet(oid("1.3.6.1.2.1.1.3.0"), state, found));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.3.0", str(found.oid));
    TEST_ASSERT_EQUAL_HEX8(BerTag::TimeTicks, found.value.type);
    TEST_ASSERT_EQUAL_UINT32(123456, found.value.unsigned32);
}

static void test_mib_get_absent_instance_is_false() {
    state.ambientTempValid = false;
    TEST_ASSERT_FALSE(mibGet(oid("1.3.6.1.4.1.63050.5.0"), state, found));
    TEST_ASSERT_TRUE(mibHasObject(oid("1.3.6.1.4.1.63050.5.0")));  // still an object: v2c noSuchInstance
    // GetNext skips it
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.4.1.63050.6.0", nextOf("1.3.6.1.4.1.63050.4.0"));
}

static void test_mib_getnext_v1_skips_counter64() {
    TEST_ASSERT_TRUE(mibGetNext(oid("1.3.6.1.2.1.31.1.1.1.5.1"), state, MibView::V2c, found));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.31.1.1.1.6.1", str(found.oid));
    TEST_ASSERT_EQUAL_HEX8(BerTag::Counter64, found.value.type);
    TEST_ASSERT_TRUE(mibGetNext(oid("1.3.6.1.2.1.31.1.1.1.5.1"), state, MibView::V1, found));
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.31.1.1.1.14.1", str(found.oid));
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
    uint16_t visited = 0;
    uint16_t expectedIndex = 0;
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
    TEST_ASSERT_EQUAL_UINT16(mibSize(), expectedIndex);
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
    const char* oids[] = {"1.3.6.1.2.1.1.1.0", LAST_OID};
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
    const char* oids[] = {"1.3.6.1.2.1.1.7.0", LAST_OID};
    Reply r = requestV2c(agent, BerTag::GetNextRequest, oids, 2);
    TEST_ASSERT_EQUAL_INT32(0, r.errorStatus);
    TEST_ASSERT_EQUAL_STRING("1.3.6.1.2.1.1.8.0", str(r.names[0]));   // sysORLastChange follows sysServices
    TEST_ASSERT_EQUAL_HEX8(BerTag::EndOfMibView, r.tags[1]);
    TEST_ASSERT_EQUAL_STRING(LAST_OID, str(r.names[1]));
}

static void test_v2c_walk_ends_with_end_of_mib_view() {
    SnmpAgent agent("public");
    withSwitch(true);  // every object has an instance, LLDP-MIB included
    char current[128] = "1.0";
    uint16_t visited = 0;
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
    TEST_ASSERT_EQUAL_UINT16(mibSize(), visited);
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
    const char* last[] = {LAST_OID};
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
            TEST_ASSERT_EQUAL_STRING(str(mibEntry((uint16_t)seen).oid), str(r.names[i]));
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
    for (uint16_t i = 0; i < r.count; i++) TEST_ASSERT_EQUAL_STRING(str(mibEntry(i).oid), str(r.names[i]));
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
    RUN_TEST(test_sysortable_lists_nine_mibs);
    RUN_TEST(test_snmp_group_counts_requests);
    RUN_TEST(test_snmp_in_pkts_includes_the_current_request);
    RUN_TEST(test_snmp_total_req_vars_skips_errors_and_exceptions);
    RUN_TEST(test_snmp_bad_version_community_and_parse_errors_are_counted);
    RUN_TEST(test_snmp_in_traps_counts_trap_pdus);
    RUN_TEST(test_snmp_silent_drops_counts_no_room);
    RUN_TEST(test_snmp_out_pkts_adds_traps);
    RUN_TEST(test_snmp_enable_authen_traps_is_enabled);
    RUN_TEST(test_snmp_counters_are_zero_without_stats);
    RUN_TEST(test_snmp_set_serial_no_is_zero_and_read_only);
    RUN_TEST(test_hr_system_values);
    RUN_TEST(test_hr_system_date_served_when_clock_valid);
    RUN_TEST(test_hr_system_date_absent_without_clock);
    RUN_TEST(test_hr_storage_reports_ram_and_littlefs);
    RUN_TEST(test_hr_storage_used_rounds_up_to_whole_kb);
    RUN_TEST(test_hr_storage_littlefs_row_absent_when_invalid);
    RUN_TEST(test_hr_processor_load_per_core);
    RUN_TEST(test_hr_device_rows_describe_both_cores);
    RUN_TEST(test_host_resources_absent_without_host_state);
    RUN_TEST(test_entity_row_names_the_device);
    RUN_TEST(test_ip_scalars_from_state);
    RUN_TEST(test_icmp_scalars_map_in_and_out_arrays);
    RUN_TEST(test_tcp_scalars_from_state);
    RUN_TEST(test_tcp_curr_estab_is_gauge32);
    RUN_TEST(test_udp_scalars_from_state);
    RUN_TEST(test_protocol_counters_absent_without_proto_state);
    RUN_TEST(test_ip_tables_empty_without_addresses);
    RUN_TEST(test_ipaddrtable_row_for_ipv4);
    RUN_TEST(test_ipaddresstable_rows_sorted_ipv4_then_ipv6);
    RUN_TEST(test_ipaddress_row_values);
    RUN_TEST(test_ipaddress_prefix_pointer_carries_prefix_length);
    RUN_TEST(test_dynamic_rows_getnext_continues_after_a_removed_row);
    RUN_TEST(test_dynamic_rows_getnext_takes_an_address_added_mid_walk);
    RUN_TEST(test_dynamic_rows_getbulk_walks_columns_in_order);
    RUN_TEST(test_ipv6_index_is_28_arcs);
    RUN_TEST(test_ip_address_walk_in_v1_sees_every_row);
    RUN_TEST(test_mib_get_and_get_next);
    RUN_TEST(test_mib_get_returns_the_instance_and_value);
    RUN_TEST(test_mib_get_absent_instance_is_false);
    RUN_TEST(test_mib_getnext_v1_skips_counter64);
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
    RUN_TEST(test_get_fw_running_slot);
    RUN_TEST(test_get_fw_slot_versions_empty_when_invalid);
    RUN_TEST(test_get_fw_state_and_last_result);
    RUN_TEST(test_walk_goes_from_mains_alarm_into_fw_group);
    RUN_TEST(test_getnext_after_fw_group_ends_enterprise_tree);
    RUN_TEST(test_getnext_after_the_last_object_is_end_of_mib);
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
