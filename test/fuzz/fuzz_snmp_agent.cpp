/**
 * @file fuzz_snmp_agent.cpp
 * @brief Fuzz the SNMP agent (lib/snmp_agent): request parsing, the MIB lookups and the response
 *
 * Input: one byte selecting which optional MIB parts exist (bit 0 LLDP neighbour,
 * bit 1 IP address rows, bit 2 host and protocol groups), then the UDP payload
 * as it arrives on port 161. Community "public".
 */
#include <snmp_agent.h>

#include "fuzz_common.h"

static SnmpDeviceState state;
static SnmpLldpState lldp;
static SnmpHostState host;
static SnmpProtoCounters proto;
static SnmpIpState ip;

/// Values as on a running device (as in test/native/test_snmp_agent), every optional part filled in
static void setUpState() {
    state.uptimeTicks = 123456;
    state.mainsPresent = true;
    state.mainsChanges = 3;
    state.mainsLastChangeTicks = 100000;
    state.update.result = 2;
    state.update.minutesSinceGood = 12;
    strcpy(state.fw.slotVersion[0], "3.4.0");
    strcpy(state.fw.slotVersion[1], "3.3.0");
    state.sysDescr = "RF-Power-Monitor";
    state.sysContact = "ops@example.com";
    state.sysName = "power-monitor";
    state.sysLocation = "Rack 4";
    state.iface.linkUp = true;
    state.iface.speedBps = 100000000;
    const uint8_t mac[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};
    memcpy(state.iface.mac, mac, 6);
    state.iface.counters.inOctets = 0x123456789ull;
    state.cpuTempDeciC = 287;
    state.ambientTempValid = true;
    state.ambientTempDeciC = 271;
    state.clockValid = true;
    static const uint8_t DATE[11] = {0x07, 0xEA, 1, 2, 3, 4, 5, 0, '+', 1, 0};
    memcpy(state.dateAndTime, DATE, sizeof(DATE));

    memcpy(lldp.localMac, mac, 6);
    lldp.present = true;
    LldpNeighbour& n = lldp.neighbour;
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

    host.ramTotalBytes = 262144;
    host.ramUsedBytes = 131000;
    host.fsValid = true;
    host.fsTotalBytes = 262144;
    host.fsUsedBytes = 16384;
    host.cpuLoad[0] = 7;
    host.cpuLoad[1] = 1;
    host.serialNumber = "E6614103E7452D2F";
    host.firmwareRevision = "3.1.0";
    host.hardwareRevision = "W5500-EVB-Pico-PoE + carrier rev 1.1";
    memset(&proto, 0x11, sizeof(proto));

    ip.reasmMaxSize = 1500;
    ip.count = 3;
    const uint8_t v4[4] = {10, 132, 0, 77};
    memcpy(ip.addr[0].addr, v4, 4);
    ip.addr[0].prefixLen = 24;
    ip.addr[0].origin = SnmpAddrOrigin::Dhcp;
    ip.addr[0].preferred = true;
    const uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x1c, 0xb2, 0xcf, 0xff, 0xfe, 0x4e, 0xa2, 0x2c};
    for (int i = 1; i < 3; i++) {
        ip.addr[i].ipv6 = true;
        memcpy(ip.addr[i].addr, ll, 16);
        ip.addr[i].prefixLen = 64;
        ip.addr[i].origin = i == 1 ? SnmpAddrOrigin::LinkLayer : SnmpAddrOrigin::Random;
        ip.addr[i].preferred = i == 1;
    }
    ip.addr[2].addr[0] = 0xfd;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool ready = false;
    if (!ready) {
        setUpState();
        ready = true;
    }
    static SnmpAgent* agent = new SnmpAgent("public");  // large members: not on the stack
    static uint8_t response[SNMP_MAX_MESSAGE];

    FuzzInput in(data, size);
    uint8_t parts = in.byte();
    state.lldp = (parts & 1) ? &lldp : nullptr;
    state.ip = (parts & 2) ? &ip : nullptr;
    state.host = (parts & 4) ? &host : nullptr;
    state.proto = (parts & 4) ? &proto : nullptr;
    state.snmp = &agent->stats();

    size_t len;
    const uint8_t* packet = in.rest(len);
    // The exact size, so ASan sees any read past the datagram
    std::vector<uint8_t> copy(packet, packet + len);
    size_t n = agent->handle(copy.data(), copy.size(), response, sizeof(response), state);
    if (n > sizeof(response)) __builtin_trap();
    if (n > 0) {
        // Whatever goes on the wire is exactly one well-formed message
        BerReader r(response, n);
        BerReader msg;
        if (!r.readSequence(BerTag::Sequence, msg) || !r.atEnd()) __builtin_trap();
    }
    return 0;
}
