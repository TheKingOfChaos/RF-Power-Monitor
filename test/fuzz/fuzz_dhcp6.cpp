/**
 * @file fuzz_dhcp6.cpp
 * @brief Fuzz DHCPv6 (lib/dhcp6_client): frames, messages, router advertisements and the client
 *
 * Input: byte 0 (bit 0 set: keep checksums as given), then pieces of
 * [length byte][bytes]. Each piece is read as an Ethernet frame (DHCPv6 reply,
 * router advertisement) and as a bare DHCPv6 message; a message dhcp6Parse
 * accepts goes to a running client with the client's own transaction id, so the
 * fuzzer drives Solicit -> Request -> Bound -> Renew without guessing ids.
 * Checks the client never hands out a lease whose preferred lifetime exceeds its valid one.
 */
#include <dhcp6_client.h>
#include <dhcp6_frame.h>
#include <dhcp6_message.h>

#include "fuzz_common.h"

static const uint8_t MAC[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};

static uint32_t fixedRandom() {
    static uint32_t x = 12345;
    x = x * 1103515245u + 12345u;
    return x;
}

/// Run the client's actions; returns the transaction id of the last message it sent
static uint32_t drain(Dhcp6Client& c, uint32_t now, uint32_t xid) {
    uint8_t buf[256];
    for (int i = 0; i < 16; i++) {
        Dhcp6Action a = c.poll(now, buf, sizeof(buf));
        if (a.kind == Dhcp6Action::None) break;
        if (a.kind == Dhcp6Action::Send) {
            if (a.len < 4 || a.len > sizeof(buf)) __builtin_trap();
            xid = (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        }
        if (a.kind == Dhcp6Action::AddAddress && a.preferred > a.valid) __builtin_trap();
    }
    return xid;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);
    bool raw = in.byte() & 1;
    Dhcp6Client client(MAC, fixedRandom);
    client.setHostname("rf-power-monitor");
    uint32_t now = 1000;
    client.start(now);
    now += 1100;
    uint32_t xid = drain(client, now, 0);

    while (in.left() > 0) {
        size_t len;
        const uint8_t* piece = in.take(in.byte(), len);
        std::vector<uint8_t> f(piece, piece + len);  // exact size for ASan
        if (!raw) fuzzFixIPv6(f.data(), f.size());

        RouterAdvert ra;
        raRead(f.data(), f.size(), ra);
        const uint8_t* payload;
        size_t payloadLen;
        if (dhcp6FromFrame(f.data(), f.size(), MAC, &payload, &payloadLen)) {
            if (payload < f.data() || payload + payloadLen > f.data() + f.size()) __builtin_trap();
            Dhcp6In m;
            dhcp6Parse(payload, payloadLen, MAC, m);
        }

        Dhcp6In m;
        if (dhcp6Parse(f.data(), f.size(), MAC, m) == Dhcp6Parse::Ok) {
            if (m.serverIdLen > DHCP6_SERVER_ID_MAX || m.ntpCount > DHCP6_NTP_MAX) __builtin_trap();
            m.xid = xid;
            client.received(m, now);
        }
        now += 500;
        xid = drain(client, now, xid);
        if (client.hasLease() && client.preferredLeftS(now) > client.validLeftS(now)) __builtin_trap();
    }
    return 0;
}
