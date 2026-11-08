#include "dhcp6_service.h"

#include <Arduino.h>
#include <new>
#include <string.h>
#include <lwip/netif.h>
#include <lwip/dhcp6.h>
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
static const char* problem = "";
// SNTP servers last handed to lwIP: the client's ntpVersion(); ntpForce hands them over again
static uint32_t ntpApplied = 0;
static volatile bool ntpForce = false;

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

// dhcp6RefreshedState() uses lwIP's values
static_assert(IP6_ADDR_DEPRECATED == 0x10 && IP6_ADDR_PREFERRED == 0x30, "lwIP address states changed");

/// Whether our slot still holds addr in lwIP (lwIP expires addresses itself; SLAAC may reuse the slot). Driver lock held
static bool slotHolds(const uint8_t addr[16]) {
    return slot >= 0 && dhcp6SlotHolds(netif_ip6_addr_state(dhcpNetif, slot),
                                       (const uint8_t*)netif_ip6_addr(dhcpNetif, slot)->addr, addr);
}

/// The slot (after 0) that holds addr, -1 if none. Driver lock held
static int findSlot(const uint8_t addr[16]) {
    for (int i = 1; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (dhcp6SlotHolds(netif_ip6_addr_state(dhcpNetif, i), (const uint8_t*)netif_ip6_addr(dhcpNetif, i)->addr, addr)) {
            return i;
        }
    }
    return -1;
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
    if (!slotHolds(a.addr)) {
        uint8_t states[LWIP_IPV6_NUM_ADDRESSES];
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) states[i] = netif_ip6_addr_state(dhcpNetif, i);
        slot = dhcp6PickSlot(states, LWIP_IPV6_NUM_ADDRESSES);
        if (slot < 0) {
            problem = "no free IPv6 address slot";
            return;
        }
        problem = "";
        netif_ip6_addr_set(dhcpNetif, slot, &addr);
        netif_ip6_addr_set_state(dhcpNetif, slot, IP6_ADDR_TENTATIVE);  // lwIP runs duplicate address detection
    }
    netif_ip6_addr_set_valid_life(dhcpNetif, slot, a.valid);  // never 0 here: 0 would mean "static" to lwIP
    netif_ip6_addr_set_pref_life(dhcpNetif, slot, a.preferred);
    netif_ip6_addr_set_state(dhcpNetif, slot, dhcp6RefreshedState(netif_ip6_addr_state(dhcpNetif, slot), a.preferred));
}

/// Remove addr: the leased address, or one the server moved us away from
static void removeAddress(const Dhcp6Action& a) {
    DriverLock lock;
    int i = findSlot(a.addr);
    if (i >= 0) netif_ip6_addr_set_state(dhcpNetif, i, IP6_ADDR_INVALID);
    if (i == slot || !slotHolds(client->lease().addr)) {
        slot = -1;
        problem = "";
    }
}

/// The server moved us away from addr: no longer preferred, gone after a.valid seconds
static void deprecateAddress(const Dhcp6Action& a) {
    DriverLock lock;
    int i = findSlot(a.addr);
    if (i < 0) return;
    if (a.valid == 0) {
        netif_ip6_addr_set_state(dhcpNetif, i, IP6_ADDR_INVALID);
        return;
    }
    netif_ip6_addr_set_valid_life(dhcpNetif, i, a.valid);  // lwIP expires it; the client removes it then too
    netif_ip6_addr_set_pref_life(dhcpNetif, i, 0);
    netif_ip6_addr_set_state(dhcpNetif, i, dhcp6RefreshedState(netif_ip6_addr_state(dhcpNetif, i), 0));
    if (i == slot) slot = -1;
}

/**
 * @brief Set the leased address's lifetimes and state in lwIP to the client's, once a second: a router
 * advertisement for the same prefix can overwrite them when no SLAAC address sits in a lower slot
 */
static void keepLifetimes(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 1000) return;
    last = now;
    uint32_t valid = client->validLeftS(now), preferred = client->preferredLeftS(now);
    if (valid == 0) return;  // 0 would mean "static" to lwIP; the client removes the address now anyway
    DriverLock lock;
    if (!slotHolds(client->lease().addr)) return;
    netif_ip6_addr_set_valid_life(dhcpNetif, slot, valid);
    netif_ip6_addr_set_pref_life(dhcpNetif, slot, preferred);
    netif_ip6_addr_set_state(dhcpNetif, slot, dhcp6RefreshedState(netif_ip6_addr_state(dhcpNetif, slot), preferred));
}

/**
 * @brief Give lwIP's SNTP the servers from option 31 when they change, the way lwIP's own stateless DHCPv6
 * does (dhcp6_set_ntp_servers: used only while NTP is set to dhcp; the last of DHCPv4 and DHCPv6 wins)
 */
static void applyNtp() {
    bool force = ntpForce;
    ntpForce = false;
    uint8_t n = client->ntpCount();
    if (n == 0 || (client->ntpVersion() == ntpApplied && !force)) return;
    ntpApplied = client->ntpVersion();
    ip_addr_t addrs[DHCP6_NTP_MAX];
    for (uint8_t i = 0; i < n; i++) {
        ip6_addr_t a;
        toLwip(client->ntpServer(i), a);
        ip_addr_copy_from_ip6(addrs[i], a);
    }
    DriverLock lock;
    dhcp6_set_ntp_servers(n, addrs);
}

void dhcp6NtpRefresh() {
    ntpForce = true;
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
        if (slotHolds(client->lease().addr)) duplicate = ip6_addr_isduplicated(netif_ip6_addr_state(dhcpNetif, slot));
    }
    uint32_t now = millis();
    client->setHostname(hostnameCurrent());
    if (managed) client->start(now);  // no-op once started
    // Link back up (the cable may now be on another network): Confirm the address (RFC 8415 18.2.12)
    static bool wasUp = false;
    bool up = netStatsPhy().linkUp;
    if (up && !wasUp) client->linkUp(now);  // no-op before start
    wasUp = up;
    if (haveMsg) client->received(msg, now);
    if (duplicate) client->addressDuplicate(now);

    for (int i = 0; i < 8; i++) {  // at most two removes, a deprecate, an add and a send per call
        Dhcp6Action a = client->poll(now, message, sizeof(message));
        if (a.kind == Dhcp6Action::None) break;
        if (a.kind == Dhcp6Action::Send) send(a.len);
        if (a.kind == Dhcp6Action::AddAddress) addAddress(a);
        if (a.kind == Dhcp6Action::RemoveAddress) removeAddress(a);
        if (a.kind == Dhcp6Action::DeprecateAddress) deprecateAddress(a);
    }
    keepLifetimes(now);
    applyNtp();
}

bool dhcp6IsSlot(int i) {
    // The callers read the netif the same way, without the lock
    return client != nullptr && slot == i && slotHolds(client->lease().addr);
}

void dhcp6GetStatus(Dhcp6Status& out) {
    memset(&out, 0, sizeof(out));
    out.problem = problem;
    if (client == nullptr) return;
    out.state = client->state();
    {
        DriverLock lock;
        out.hasAddress = client->hasLease() && slotHolds(client->lease().addr);
    }
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
void dhcp6NtpRefresh() {}
bool dhcp6IsSlot(int) { return false; }
void dhcp6GetStatus(Dhcp6Status& out) { memset(&out, 0, sizeof(out)); out.problem = ""; }
#endif
