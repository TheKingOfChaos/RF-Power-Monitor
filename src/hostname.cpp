#include "hostname.h"

#include <lwip/apps/mdns.h>
#include <netconfig.h>
#include <settings.h>
#include "net_stats.h"

extern CountingW5500lwIP eth;  // main.cpp

static char current[HOSTNAME_MAX_LENGTH + 1] = "";
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
    hostnameFromText(currentSettings.sysName, current);
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
            Serial.printf("\nmDNS: %s.local\n", current);
        } else {
            Serial.println("\nmDNS: could not start");
        }
    } else {
        mdns_resp_announce(nif);  // new address: tell the network
    }
}

void hostnameApply() {
    char next[HOSTNAME_MAX_LENGTH + 1];
    hostnameFromText(currentSettings.sysName, next);
    if (strcmp(next, current) == 0) return;
    strcpy(current, next);
    DriverLock lock;
    eth.setHostname(current);  // also renews a DHCP lease, so the server learns the new name
    if (mdnsStarted) mdns_resp_rename_netif(eth.getNetIf(), current);
    Serial.printf("\nHostname: %s (DHCP, mDNS %s.local)\n", current, current);
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
