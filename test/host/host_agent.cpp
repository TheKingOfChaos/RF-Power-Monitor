// Host SNMP agent: runs lib/snmp_agent on a UDP socket on Linux/WSL so the real
// net-snmp tools can be pointed at it before anything is flashed.
// Built and driven by scripts/host_agent_check.sh.
//
// Usage: host_agent <port> [community]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <snmp_agent.h>

static uint32_t uptimeTicks() {
    static timespec start = {0, 0};
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (start.tv_sec == 0 && start.tv_nsec == 0) start = now;
    return (uint32_t)((now.tv_sec - start.tv_sec) * 100 + (now.tv_nsec - start.tv_nsec) / 10000000);
}

/// The neighbour the device sees on a Juniper EX2200-C (as in test_lldp's captured frame)
static SnmpLldpState lldpState() {
    SnmpLldpState l = {};
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(l.localMac, mac, 6);
    l.present = true;
    LldpNeighbour& n = l.neighbour;
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
    n.capsSupported = n.capsEnabled = 0x0014;
    n.ttl = 120;
    return l;
}
static const SnmpLldpState LLDP = lldpState();

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [community]\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);
    const char* community = argc > 2 ? argv[2] : "public";

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    uptimeTicks();
    printf("host agent listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    static SnmpAgent agent(community);
    static uint8_t in[SNMP_MAX_MESSAGE], out[SNMP_MAX_MESSAGE];
    for (;;) {
        sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        ssize_t n = recvfrom(fd, in, sizeof(in), 0, (sockaddr*)&peer, &peerLen);
        if (n < 0) continue;

        SnmpDeviceState state = {};
        state.uptimeTicks = uptimeTicks();
        state.mainsPresent = true;
        state.mainsChanges = 2;
        state.mainsLastChangeTicks = 42;
        state.mainsAlarm = false;
        state.sysDescr = "RF.Power-Monitor host test";
        state.sysContact = "ops@example.com";
        state.sysName = "host-agent";
        state.sysLocation = "WSL";
        state.iface.linkUp = true;
        state.iface.speedBps = 100000000;
        const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(state.iface.mac, mac, 6);
        state.iface.counters.inOctets = 5000000000ull;   // past 32 bits: ifInOctets wraps, ifHCInOctets not
        state.iface.counters.inUcastPkts = 1234;
        state.iface.counters.inDiscards = 3;
        state.iface.counters.inErrors = 2;
        state.iface.counters.inUnknownProtos = 17;
        state.cpuTempDeciC = 250;
        state.ambientTempValid = true;
        state.ambientTempDeciC = 215;
        state.lldp = &LLDP;

        size_t len = agent.handle(in, (size_t)n, out, sizeof(out), state);
        if (len) sendto(fd, out, len, 0, (sockaddr*)&peer, peerLen);
    }
}
