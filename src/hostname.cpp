#include "hostname.h"

#include <lwip/apps/mdns.h>
#include <lwip/dhcp.h>
#include <netconfig.h>
#include <settings.h>
#include "net_stats.h"

extern CountingW5500lwIP eth;  // main.cpp

static char current[HOSTNAME_MAX_LENGTH + 1] = "";
static uint8_t mac[6] = {0, 0, 0, 0, 0, 0};  // known once Ethernet has started (hostnameSetMac)
static bool mdnsStarted = false;
static bool advertiseWeb = true;  // announce the web page as _http._tcp
static s8_t httpSlot = -1;         // its mDNS service slot, -1 = not announced

/// Add or remove the _http._tcp service to match advertiseWeb (driver lock held)
static void syncWebService(netif* nif) {
    if (advertiseWeb && httpSlot < 0) {
        httpSlot = mdns_resp_add_service(nif, "RF-Power-Monitor", "_http", DNSSD_PROTO_TCP, 80, nullptr, nullptr);
    } else if (!advertiseWeb && httpSlot >= 0) {
        mdns_resp_del_service(nif, httpSlot);
        httpSlot = -1;
    }
}

void hostnameBegin() {
    hostnameForDevice(currentSettings.sysName, mac, current);
    DriverLock lock;
    eth.setHostname(current);  // used by DHCP once Ethernet starts
}

void hostnameNetworkUp() {
    netif* nif = eth.getNetIf();
    DriverLock lock;
    if (!mdnsStarted) {
        mdns_resp_init();
        if (mdns_resp_add_netif(nif, current) == ERR_OK) {
            syncWebService(nif);
            mdnsStarted = true;
            Serial.printf("\r\nmDNS: %s.local\r\n", current);
        } else {
            Serial.println("\r\nmDNS: could not start");
        }
    } else {
        mdns_resp_announce(nif);  // new address: tell the network
    }
}

void hostnameApply() {
    char next[HOSTNAME_MAX_LENGTH + 1];
    hostnameForDevice(currentSettings.sysName, mac, next);
    if (strcmp(next, current) == 0) return;
    strcpy(current, next);
    netif* nif = eth.getNetIf();
    DriverLock lock;
    if (dhcp_supplied_address(nif)) {
        eth.setHostname(current);  // also renews the DHCP lease, so the server learns the new name
    } else {
        // No lease (yet): the core's setHostname() would call dhcp_renew(), which in the middle of
        // DISCOVER/REQUEST sends a renewal for an address the device does not have, and DHCP never
        // gets a lease. lwIP reads the name from the netif for every DHCP message: point it here.
        nif->hostname = current;
    }
    if (mdnsStarted) mdns_resp_rename_netif(eth.getNetIf(), current);
    Serial.printf("\r\nHostname: %s (DHCP, mDNS %s.local)\r\n", current, current);
}

void hostnameSetMac(const uint8_t address[6]) {
    memcpy(mac, address, sizeof(mac));
    hostnameApply();  // the default name has the MAC in it: DHCP renew if it changed
}

const char* hostnameCurrent() {
    return current;
}

void hostnameAdvertiseWeb(bool on) {
    advertiseWeb = on;
    if (!mdnsStarted) return;  // hostnameNetworkUp() adds it when mDNS starts
    netif* nif = eth.getNetIf();
    DriverLock lock;
    syncWebService(nif);
    mdns_resp_announce(nif);
}
