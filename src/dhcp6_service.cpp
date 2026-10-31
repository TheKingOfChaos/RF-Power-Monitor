#include "dhcp6_service.h"

#include <Arduino.h>
#include <new>
#include <string.h>
#include <lwip/netif.h>
#include <dhcp6_frame.h>
#include <dhcp6_message.h>
#include "hostname.h"
#include "net_stats.h"

#if LWIP_IPV6
static netif* dhcpNetif = nullptr;
static uint8_t mac[6];

// The client needs the MAC at construction, which is known only in dhcp6Begin()
alignas(Dhcp6Client) static uint8_t clientStorage[sizeof(Dhcp6Client)];
static Dhcp6Client* client = nullptr;

// Written in lwIP context (input hook), read by dhcp6Poll() under the driver lock
static bool managedSeen = false;
static bool inboxFull = false;
static Dhcp6In inbox;

// Owned by dhcp6Poll() on core 0; static, core 0's stack is small
static uint8_t message[256];
static uint8_t frame[DHCP6_FRAME_HEADER + sizeof(message)];
static int slot = -1;
static uint8_t slotAddr[16];
static const char* problem = "";

static uint32_t random32() {
    return rp2040.hwrand32();
}

void dhcp6Begin(netif* nif) {
    if (nif == nullptr || client != nullptr) return;
    dhcpNetif = nif;
    memcpy(mac, nif->hwaddr, 6);
    client = new (clientStorage) Dhcp6Client(mac, random32);
}

void dhcp6InspectFrame(const uint8_t* f, size_t len) {
    if (client == nullptr) return;
    RouterAdvert ra;
    if (raRead(f, len, ra)) {
        if (ra.managed) managedSeen = true;
        return;
    }
    const uint8_t* payload;
    size_t payloadLen;
    if (!dhcp6FromFrame(f, len, mac, &payload, &payloadLen)) return;
    // Parsed here so only messages for this device take the one-entry inbox; a second one
    // before dhcp6Poll() is dropped (the server retransmits)
    static Dhcp6In parsed;
    if (!inboxFull && dhcp6Parse(payload, payloadLen, mac, parsed) == Dhcp6Parse::Ok) {
        inbox = parsed;
        inboxFull = true;
    }
}

static void toLwip(const uint8_t bytes[16], ip6_addr_t& out) {
    uint32_t w[4];
    memcpy(w, bytes, sizeof(w));  // ip6_addr_t words are in network order
    IP6_ADDR(&out, w[0], w[1], w[2], w[3]);
    ip6_addr_assign_zone(&out, IP6_UNICAST, dhcpNetif);  // global or ULA: no zone
}

/**
 * @brief Add the leased address, or refresh its lifetimes after a Renew/Rebind. Never evicts.
 */
static void addAddress(const Dhcp6Action& a) {
    ip6_addr_t addr;
    toLwip(a.addr, addr);
    DriverLock lock;
    bool same = slot >= 0 && memcmp(slotAddr, a.addr, 16) == 0;
    if (!same) {
        uint8_t states[LWIP_IPV6_NUM_ADDRESSES];
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) states[i] = netif_ip6_addr_state(dhcpNetif, i);
        slot = dhcp6PickSlot(states, LWIP_IPV6_NUM_ADDRESSES);
        if (slot < 0) {
            problem = "no free IPv6 address slot";
            return;
        }
        problem = "";
        memcpy(slotAddr, a.addr, 16);
        netif_ip6_addr_set(dhcpNetif, slot, &addr);
        netif_ip6_addr_set_state(dhcpNetif, slot, IP6_ADDR_TENTATIVE);  // lwIP runs duplicate address detection
    }
    netif_ip6_addr_set_valid_life(dhcpNetif, slot, a.valid);  // never 0 here: 0 would mean "static" to lwIP
    netif_ip6_addr_set_pref_life(dhcpNetif, slot, a.preferred);
}

static void removeAddress(const Dhcp6Action& a) {
    DriverLock lock;
    if (slot >= 0 && memcmp(slotAddr, a.addr, 16) == 0) {
        netif_ip6_addr_set_state(dhcpNetif, slot, IP6_ADDR_INVALID);
        slot = -1;
    }
    problem = "";
}

static void send(size_t len) {
    uint8_t ll[16];
    {
        DriverLock lock;
        // Our link-local address must have passed duplicate address detection to be a source
        if (!ip6_addr_isvalid(netif_ip6_addr_state(dhcpNetif, 0))) return;
        memcpy(ll, netif_ip6_addr(dhcpNetif, 0)->addr, 16);
    }
    size_t n = dhcp6Frame(mac, ll, message, len, frame, sizeof(frame));
    if (n > 0) netStatsSendFrame(frame, n);  // the client retransmits if this one is lost
}

void dhcp6Poll() {
    if (client == nullptr) return;
    bool managed, haveMsg;
    static Dhcp6In msg;
    bool duplicate = false;
    {
        DriverLock lock;
        managed = managedSeen;
        haveMsg = inboxFull;
        if (haveMsg) msg = inbox;
        inboxFull = false;
        if (slot >= 0) duplicate = ip6_addr_isduplicated(netif_ip6_addr_state(dhcpNetif, slot));
    }
    uint32_t now = millis();
    client->setHostname(hostnameCurrent());
    if (managed) client->start(now);  // no-op once started
    if (haveMsg) client->received(msg, now);
    if (duplicate) client->addressDuplicate(now);

    for (int i = 0; i < 8; i++) {  // at most a remove, an add and a send per call
        Dhcp6Action a = client->poll(now, message, sizeof(message));
        if (a.kind == Dhcp6Action::None) break;
        if (a.kind == Dhcp6Action::Send) send(a.len);
        if (a.kind == Dhcp6Action::AddAddress) addAddress(a);
        if (a.kind == Dhcp6Action::RemoveAddress) removeAddress(a);
    }
}

bool dhcp6IsSlot(int i) {
    return slot >= 0 && slot == i;
}

void dhcp6GetStatus(Dhcp6Status& out) {
    memset(&out, 0, sizeof(out));
    out.problem = problem;
    if (client == nullptr) return;
    out.state = client->state();
    out.hasAddress = client->hasLease() && slot >= 0;
    if (client->hasLease()) formatIPv6(client->lease().addr, out.address);
    out.validLeftS = client->validLeftS(millis());
    out.sent = client->sent();
    out.accepted = client->accepted();
    out.ignored = client->ignored();
}
#else
void dhcp6Begin(netif*) {}
void dhcp6Poll() {}
void dhcp6InspectFrame(const uint8_t*, size_t) {}
bool dhcp6IsSlot(int) { return false; }
void dhcp6GetStatus(Dhcp6Status& out) { memset(&out, 0, sizeof(out)); out.problem = ""; }
#endif
