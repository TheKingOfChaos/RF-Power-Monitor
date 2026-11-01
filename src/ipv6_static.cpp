#include "ipv6_static.h"

#include <Arduino.h>
#include "net_stats.h"  // DriverLock
#include <lwip/netif.h>
#include <lwip_hooks.h>
#include <netconfig.h>
#include <settings.h>
#include <string.h>

#if LWIP_IPV6
static netif* staticNetif = nullptr;
static ip6_addr_t staticAddress;
static ip6_addr_t staticGateway;
static bool haveAddress = false;
static bool haveGateway = false;
static const char* problem = "";

/// lwIP address from 16 network-order bytes, with the interface zone for link-local addresses
static void toLwip(const uint8_t bytes[16], ip6_addr_t& out, netif* nif) {
    uint32_t w[4];
    memcpy(w, bytes, sizeof(w));  // ip6_addr_t words are in network order
    IP6_ADDR(&out, w[0], w[1], w[2], w[3]);
    ip6_addr_assign_zone(&out, IP6_UNICAST, nif);
}

void ipv6StaticBegin(netif* nif) {
    // The settings were validated when loaded; check again so a bad value cannot reach lwIP
    if (currentSettings.ipv6Address[0] == '\0') return;
    uint8_t a[16], g[16];
    if (validateStaticIPv6(currentSettings.ipv6Address, currentSettings.ipv6Gateway) != StaticIPv6Error::None ||
        !parseIPv6(currentSettings.ipv6Address, a)) {
        problem = "invalid setting";
        return;
    }
    staticNetif = nif;
    toLwip(a, staticAddress, nif);

    DriverLock lock;
    s8_t slot = -1;
    if (netif_add_ip6_address(nif, &staticAddress, &slot) != ERR_OK || slot < 0) {
        problem = "no free IPv6 address slot";
        return;
    }
    // Static: no lifetime, router advertisements never expire it
    netif_ip6_addr_set_valid_life(nif, slot, IP6_ADDR_LIFE_STATIC);
    netif_ip6_addr_set_pref_life(nif, slot, IP6_ADDR_LIFE_STATIC);
    haveAddress = true;

    if (currentSettings.ipv6Gateway[0] != '\0' && parseIPv6(currentSettings.ipv6Gateway, g)) {
        toLwip(g, staticGateway, nif);
        haveGateway = true;
    }
}

bool ipv6StaticIsSlot(int i) {
    return haveAddress && ip6_addr_cmp(netif_ip6_addr(staticNetif, i), &staticAddress);
}

const char* ipv6StaticProblem() {
    return problem;
}

extern "C" const ip6_addr_t* rfpowerIPv6Gateway(struct netif* nif, const ip6_addr_t* dest) {
    (void)dest;
    return haveGateway && nif == staticNetif ? &staticGateway : nullptr;
}
#else
void ipv6StaticBegin(netif*) {}
bool ipv6StaticIsSlot(int) { return false; }
const char* ipv6StaticProblem() { return ""; }
#endif
