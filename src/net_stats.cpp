#include "net_stats.h"

#include <LwipEthernet.h>
#include <SPI.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <string.h>
#include <lwip/dhcp.h>
#include <lwip/prot/dhcp.h>
#include "dhcp6_service.h"
#include "lldp_service.h"
#include "diag.h"
#include "pins.h"

// W5500 common register with link, speed and duplex
static const uint16_t W5500_PHYCFGR = 0x002E;
static const uint8_t W5500_COMMON_READ = 0x00;  // block 0 (common), read, variable length

static const uint32_t PHY_POLL_MS = 500;

// Updated from lwIP context, read under the lwIP lock
static IfCounters counters = {};
static netif_input_fn originalInput = nullptr;
static netif_linkoutput_fn originalLinkOutput = nullptr;

// Updated by netStatsPoll() on core 0
static PhyStatus phy = {false, false, false};
static uint32_t lastChangeTicks = 0;
static uint8_t mac[6] = {0};
static bool started = false;
static netif* statsNetif = nullptr;
// DHCP option 224 (trap destination): requested in our DHCP messages, read from ACKs to us
static volatile bool dhcpRequestOn = false;
static DhcpTrapInfo dhcpTrap = {};

static bool isWaitingForAck(uint8_t state) {
    return state == DHCP_STATE_REQUESTING || state == DHCP_STATE_REBOOTING || state == DHCP_STATE_RENEWING ||
           state == DHCP_STATE_REBINDING;
}

/**
 * @brief Use a DHCPACK's option 224 only if lwIP accepted that ACK
 *
 * Anyone on the LAN can send a frame that looks like an ACK for this MAC, and
 * traps carry the SNMP community, so a forged option 224 would leak it. The
 * value is taken only when lwIP was waiting for an ACK, the frame has lwIP's
 * current transaction id and, if lwIP already knows the server, its server id,
 * and lwIP went to BOUND while processing this very frame.
 */
static err_t inputWithDhcpAck(struct pbuf* p, struct netif* nif) {
    DhcpAckInfo info;
    DhcpAck r = dhcpReadAck((const uint8_t*)p->payload, p->len, mac, DHCP_OPTION_TRAP_TARGET, info);
    struct dhcp* dhcp = netif_dhcp_data(nif);
    if ((r != DhcpAck::Option && r != DhcpAck::NoOption) || dhcp == nullptr) return originalInput(p, nif);

    bool candidate = isWaitingForAck(dhcp->state) && info.xid == dhcp->xid;
    if (candidate && info.hasServerId && !ip_addr_isany_val(dhcp->server_ip_addr)) {
        candidate = memcmp(info.serverId, &ip_2_ip4(&dhcp->server_ip_addr)->addr, 4) == 0;
    }
    err_t err = originalInput(p, nif);  // lwIP handles the ACK now (p belongs to lwIP afterwards)
    if (!candidate || dhcp->state != DHCP_STATE_BOUND) {
        dhcpTrap.ignored++;
        return err;
    }
    dhcpTrap.acks++;
    dhcpTrap.seen = true;
    dhcpTrap.hasOption = r == DhcpAck::Option;
    if (dhcpTrap.hasOption) memcpy(dhcpTrap.target, info.value, 4);
    memcpy(dhcpTrap.yiaddr, info.yiaddr, 4);
    dhcpTrap.ackMs = millis();
    return err;
}

/// Received frame: count, then hand to lwIP as before (it drops what it cannot use)
static err_t countingInput(struct pbuf* p, struct netif* nif) {
    // The driver allocates one contiguous pbuf per frame (PBUF_RAM), so payload holds the header
    if (p == nullptr) return originalInput(p, nif);
    DIAG_HOOK(100);
    ifCountReceived(counters, (const uint8_t*)p->payload, p->len, LWIP_IPV6 != 0);
    dhcp6InspectFrame((const uint8_t*)p->payload, p->len);  // router advertisements, DHCPv6 replies; lwIP still gets it
    lldpInspectFrame((const uint8_t*)p->payload, p->len);   // the switch's LLDPDUs; lwIP drops them
    err_t err = inputWithDhcpAck(p, nif);
    DIAG_HOOK(101);
    return err;
}

void netStatsCountDiscard(uint16_t length) {
    ifCountDiscard(counters, length);
}

/// Frame to send: ask for option 224 in our DHCP messages, hand to the driver, count the result
static err_t countingLinkOutput(struct netif* nif, struct pbuf* p) {
    DIAG_HOOK(200);
    // lwIP builds each DHCP message in one pbuf (LWIP_NETIF_TX_SINGLE_PBUF), freed after sending
    if (dhcpRequestOn && p->next == nullptr &&
        dhcpRequestOption((uint8_t*)p->payload, p->len, DHCP_OPTION_TRAP_TARGET) == DhcpEdit::Added) {
        dhcpTrap.requests++;
    }
    err_t err = originalLinkOutput(nif, p);
    if (err == ERR_OK) {
        ifCountFrame(counters, (const uint8_t*)p->payload, p->tot_len, true);
    } else {
        counters.outErrors++;
    }
    DIAG_HOOK(201);
    return err;
}

/**
 * @brief Read PHYCFGR over SPI. The driver keeps this register private, so it
 *        is read directly, holding the same lock the driver uses for its own access.
 */
static uint8_t readPhyCfgr() {
    DriverLock lock;
    digitalWrite(W5500_CS_PIN, LOW);
    SPI.transfer((uint8_t)(W5500_PHYCFGR >> 8));
    SPI.transfer((uint8_t)(W5500_PHYCFGR & 0xFF));
    SPI.transfer(W5500_COMMON_READ);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(W5500_CS_PIN, HIGH);
    return value;
}

void netStatsBegin(netif* nif) {
    if (nif == nullptr || started) return;
    DriverLock lock;
    originalInput = nif->input;
    originalLinkOutput = nif->linkoutput;
    nif->input = countingInput;
    nif->linkoutput = countingLinkOutput;
    memcpy(mac, nif->hwaddr, 6);
    statsNetif = nif;
    started = true;
}

bool netStatsSendFrame(const uint8_t* frame, size_t len) {
    DriverLock lock;
    if (!started || len == 0 || len > 0xFFFF) return false;
    pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (p == nullptr) return false;
    pbuf_take(p, frame, (u16_t)len);
    err_t err = countingLinkOutput(statsNetif, p);  // counted like every other frame
    pbuf_free(p);
    return err == ERR_OK;
}

void netStatsPoll(uint32_t uptimeTicks) {
    static uint32_t lastPoll = 0;
    if (!started || millis() - lastPoll < PHY_POLL_MS) return;
    lastPoll = millis();

    PhyStatus now = decodePhyCfgr(readPhyCfgr());
    if (now.linkUp != phy.linkUp) {
        lastChangeTicks = uptimeTicks;
        Serial.printf("\r\nEthernet link %s", now.linkUp ? "up" : "down");
        if (now.linkUp) Serial.printf(", %s Mbit/s %s duplex", now.speed100 ? "100" : "10",
                                      now.fullDuplex ? "full" : "half");
        Serial.println();
    }
    phy = now;
}

void netStatsCapture(SnmpInterfaceState& out) {
    {
        DriverLock lock;  // counters are written from lwIP context
        out.counters = counters;
    }
    out.linkUp = phy.linkUp;
    out.speedBps = phySpeedBps(phy);
    memcpy(out.mac, mac, 6);
    out.lastChangeTicks = lastChangeTicks;
}

bool netStatsHasRoutableIPv6() {
    if (statsNetif == nullptr) return false;
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (ip6_addr_isvalid(netif_ip6_addr_state(statsNetif, i)) && !ip6_addr_islinklocal(netif_ip6_addr(statsNetif, i))) {
            return true;
        }
    }
    return false;
}

PhyStatus netStatsPhy() {
    return phy;
}

void netStatsRequestDhcpTrapTarget(bool on) {
    DriverLock lock;
    bool changed = dhcpRequestOn != on;
    dhcpRequestOn = on;
    if (!changed || !on) return;
    // Ask now instead of at the next renewal (lease time / 2)
    for (netif* n = netif_list; n != nullptr; n = n->next) {
        if (dhcp_supplied_address(n)) dhcp_renew(n);  // only with a bound lease
    }
}

DhcpTrapInfo netStatsDhcpTrap() {
    DriverLock lock;  // written from lwIP context
    return dhcpTrap;
}
