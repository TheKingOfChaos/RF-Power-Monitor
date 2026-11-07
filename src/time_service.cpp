#include "time_service.h"

#include <Arduino.h>
#include <clock.h>
#include <log_settings_store.h>
#include <lwip/apps/sntp.h>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <sys/time.h>

#include "net_stats.h"

extern "C" void __setSystemTime(unsigned long long sec, unsigned long usec);  // the core (posix.cpp)

// Written from lwIP's context, read on core 0
static volatile bool synced = false;
static volatile uint32_t lastSyncMs = 0;
// lwIP keeps the pointer: the typed server name must stay put
static char serverName[LOG_HOST_MAX + 1] = "";

extern "C" void timeServiceSetTime(unsigned long sec, unsigned long us) {
    __setSystemTime(sec, us);
    lastSyncMs = millis();
    synced = sec >= CLOCK_VALID_AFTER;
}

/// Configure lwIP's SNTP client for the current setting (caller holds the driver lock)
static void configure() {
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    switch (currentLogSettings.ntpMode) {
    case ServerMode::Typed:
        snprintf(serverName, sizeof(serverName), "%s", currentLogSettings.ntpServer);
        sntp_servermode_dhcp(0);
        sntp_setserver(1, nullptr);
        sntp_setservername(0, serverName);
        sntp_init();
        break;
    case ServerMode::Dhcp:
        serverName[0] = '\0';
        sntp_setservername(0, nullptr);
        sntp_servermode_dhcp(1);  // option 42 / DHCPv6 option 56 fill the servers at the next lease
        sntp_init();
        break;
    case ServerMode::None:
        serverName[0] = '\0';
        sntp_servermode_dhcp(0);
        sntp_setservername(0, nullptr);
        sntp_setserver(0, nullptr);
        sntp_setserver(1, nullptr);
        break;
    }
}

void timeServiceBegin() {
    DriverLock lock;
    configure();
}

void timeServiceApply() {
    {
        DriverLock lock;
        configure();
    }
    // A DHCP server may offer option 42 only in a new lease: renew now, like options 224, 225 and 7
    if (currentLogSettings.ntpMode == ServerMode::Dhcp) {
        DriverLock lock;
        for (netif* n = netif_list; n != nullptr; n = n->next) {
            if (dhcp_supplied_address(n)) dhcp_renew(n);
        }
    }
}

void timeServicePoll() {
    if (currentLogSettings.ntpMode != ServerMode::None) return;
    DriverLock lock;
    if (sntp_enabled()) sntp_stop();  // LwipIntfDev starts it again on every link change
}

bool timeServiceSynced() {
    return synced;
}

uint64_t timeServiceEpochMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

uint32_t timeServiceLastSyncAgeS() {
    return synced ? (millis() - lastSyncMs) / 1000u : 0;
}

void timeServiceLocalText(char* out, size_t size) {
    if (size == 0) return;
    out[0] = '\0';
    if (synced) formatLocalTime(timeServiceEpochMs() / 1000u, out, size);
}

/// "pool.ntp.org", "10.132.0.1 from DHCP", "DHCP, none offered" or "off"
static void sourceText(char* out, size_t size) {
    switch (currentLogSettings.ntpMode) {
    case ServerMode::Typed:
        snprintf(out, size, "%s", currentLogSettings.ntpServer);
        return;
    case ServerMode::None:
        snprintf(out, size, "off");
        return;
    case ServerMode::Dhcp:
        break;
    }
    char addr[IPADDR_STRLEN_MAX] = "";
    {
        DriverLock lock;
        const ip_addr_t* a = sntp_getserver(0);
        if (a != nullptr && !ip_addr_isany(a)) ipaddr_ntoa_r(a, addr, sizeof(addr));
    }
    if (addr[0] != '\0') {
        snprintf(out, size, "%s from DHCP", addr);
    } else {
        snprintf(out, size, "DHCP, none offered");
    }
}

void timeServiceStatusText(char* out, size_t size) {
    char source[80];
    sourceText(source, sizeof(source));
    if (!synced) {
        snprintf(out, size, "not synced (NTP %s)", source);
        return;
    }
    char local[32];
    timeServiceLocalText(local, sizeof(local));
    uint32_t age = timeServiceLastSyncAgeS();
    if (age < 120) {
        snprintf(out, size, "%s (NTP %s, synced %lu s ago)", local, source, (unsigned long)age);
    } else {
        snprintf(out, size, "%s (NTP %s, synced %lu min ago)", local, source, (unsigned long)(age / 60));
    }
}

bool timeServiceDateAndTime(uint8_t out[11]) {
    if (!synced) return false;
    dateAndTime(timeServiceEpochMs() / 1000u, out);
    return true;
}
