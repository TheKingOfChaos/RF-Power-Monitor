#include <Arduino.h>
#include <settings.h>
#include <LittleFS.h>
#include <W5500lwIP.h>
#include <mains_input.h>
#include "cli.h"
#include "diag.h"
#include "watchdog.h"
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "dhcp6_service.h"
#include "factory_reset.h"
#include "host_stats.h"
#include "hostname.h"
#include "ipv6_static.h"
#include "lldp_service.h"
#include "net_stats.h"
#include "pins.h"
#include "snmp_service.h"
#include "stack_watch.h"
#include "web_service.h"
#include "version.h"

// Give each core its own 8 KB stack (core 1's from the heap) instead of splitting
// the 8 KB scratch stack into two 4 KB halves. With IPv6, lwIP's processing on
// core 0 left only ~660 bytes of a 4 KB stack after a burst of web page loads,
// and core 0 overflowing would silently overwrite core 1's stack (Arduino-Pico
// docs, multicore.rst). Costs 8 KB of RAM.
bool core1_separate_stack = true;

// W5500 on SPI0 (MISO 16, CS 17, SCK 18, MOSI 19) with interrupt on GPIO21;
// the core's Wiznet5500lwIP driver, counting dropped frames for IF-MIB
CountingW5500lwIP eth(W5500_CS_PIN, SPI, W5500_INT_PIN);

// false if the W5500 did not respond at boot
static bool ethPresent = false;

/**
 * @brief Print the interface state and addresses
 *
 * DHCP runs in the background (lwIP keeps retrying until a server answers),
 * so this can be called at any time, before or after a lease is obtained.
 */
void printNetworkInfo() {
    if (!ethPresent) {
        Serial.println("State: W5500 not detected, check wiring");
    } else if (!eth.isLinked()) {
        Serial.println("State: no Ethernet link, check cable");
    } else if (!eth.connected()) {
        Serial.println(currentSettings.dhcpEnabled ? "State: link up, waiting for DHCP lease"
                                                   : "State: link up, no address");
    } else {
        Serial.println("State: connected");
    }

    uint8_t mac[6];
    eth.macAddress(mac);
    Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print("IP: ");      Serial.println(eth.localIP());
    Serial.print("Subnet: ");  Serial.println(eth.subnetMask());
    Serial.print("Gateway: "); Serial.println(eth.gatewayIP());
#if LWIP_IPV6
    // Link-local, router-advertised (SLAAC), static and DHCPv6 addresses; "tentative" while duplicate detection runs
    netif* nif = eth.getNetIf();
    bool any = false;
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        u8_t state = netif_ip6_addr_state(nif, i);
        if (ip6_addr_isinvalid(state)) continue;
        any = true;
        Serial.printf("IPv6: %s%s%s\r\n", ip6addr_ntoa(netif_ip6_addr(nif, i)),
                      ipv6StaticIsSlot(i) ? " (static)" : dhcp6IsSlot(i) ? " (dhcpv6)" : "",
                      ip6_addr_istentative(state)    ? " (tentative)"
                      : ip6_addr_isdeprecated(state) ? " (deprecated)"
                      : ip6_addr_isduplicated(state) ? " (DUPLICATE: another host uses it, not in use)"
                                                     : "");
    }
    if (!any) Serial.println("IPv6: none");
    if (ipv6StaticProblem()[0]) {
        Serial.printf("IPv6: static %s not in use (%s)\r\n", currentSettings.ipv6Address, ipv6StaticProblem());
    }
    if (currentSettings.ipv6Address[0]) {
        Serial.printf("IPv6 gateway: %s\r\n", currentSettings.ipv6Gateway[0] ? currentSettings.ipv6Gateway
                                                                          : "from router advertisements");
    }
#endif
    Serial.print("DNS: ");     Serial.println(eth.dnsIP());
}

/**
 * @brief Reset the W5500 and bring up the interface using DHCP or static settings
 *
 * Does not wait for a DHCP lease: lwIP obtains it in the background and
 * loop() reports when the address changes.
 *
 * @return true if the W5500 was detected
 */
bool startEthernet() {
    // Hardware reset of the W5500
    digitalWrite(W5500_RST_PIN, LOW);
    delay(1);
    digitalWrite(W5500_RST_PIN, HIGH);
    delay(50);

    SPI.setRX(W5500_MISO_PIN);
    SPI.setCS(W5500_CS_PIN);
    SPI.setSCK(W5500_SCK_PIN);
    SPI.setTX(W5500_MOSI_PIN);

    if (!currentSettings.dhcpEnabled) {
        IPAddress ip, subnet, gateway;
        if (ip.fromString(currentSettings.staticIP) &&
            subnet.fromString(currentSettings.subnetMask) &&
            gateway.fromString(currentSettings.gateway)) {
            eth.config(ip, gateway, subnet, gateway);
        } else {
            Serial.println("Invalid static IP settings, falling back to DHCP");
        }
    }

    if (!eth.begin()) {
        Serial.println("W5500 not detected - check wiring");
        return false;
    }
    eth.setDefault();
    ethPresent = true;

    Serial.println(currentSettings.dhcpEnabled ? "Ethernet started, requesting DHCP lease in background"
                                               : "Ethernet started with static IP");
    return true;
}

void listFiles(const char * dirname, uint8_t levels) {
    Serial.printf("\r\nListing directory: %s\r\n", dirname);

    File root = LittleFS.open(dirname, "r");
    if (!root) {
        Serial.println("Failed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println("Not a directory");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if (levels) {
                listFiles(file.name(), levels - 1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void setup() {
    // First, so the high-water mark covers everything after it
    stackWatchBegin();

    // Initialize serial for debugging
    Serial.begin(115200);
    // Wait briefly for a terminal, but don't block boot when none is attached
    uint32_t serialWait = millis();
    while (!Serial && millis() - serialWait < 3000) delay(10);
    Serial.println("\r\nRF-Power-Monitor " FIRMWARE_VERSION " starting...");
    watchdogBegin();  // restart cause, before diagBegin() arms its own early watchdog
    diagBegin();  // only with -DRFPOWER_DIAG
    cpuTempBegin();
    ambientTempBegin();

    // Initialize pins (POWER_PIN and FACTORY_RESET_PIN are configured on core 1)
    pinMode(LED_PIN, OUTPUT);
    pinMode(W5500_INT_PIN, INPUT);
    pinMode(W5500_RST_PIN, OUTPUT);
    digitalWrite(W5500_RST_PIN, HIGH);

    // Initialize filesystem
    if (!LittleFS.begin()) {
        Serial.println("LittleFS Mount Failed");
        // Flash error pattern on LED
        while (1) {
            digitalWrite(LED_PIN, HIGH);
            delay(200);
            digitalWrite(LED_PIN, LOW);
            delay(200);
        }
    }
    Serial.println("LittleFS Mounted Successfully");
    
    // Show filesystem contents
    listFiles("/", 1);
    
    // Print filesystem info
    FSInfo fs_info;
    LittleFS.info(fs_info);
    Serial.printf("\r\nFilesystem Info:\r\n");
    Serial.printf("Total space: %u bytes\r\n", fs_info.totalBytes);
    Serial.printf("Used space: %u bytes\r\n", fs_info.usedBytes);
    Serial.printf("Block size: %u bytes\r\n", fs_info.blockSize);
    Serial.printf("Page size: %u bytes\r\n", fs_info.pageSize);

    hostStatsBegin();  // serial number and LittleFS figures for HOST-RESOURCES-MIB / ENTITY-MIB

    // Load settings
    settingsLoaded = loadSettings();
    hostnameBegin();  // before Ethernet, so the first DHCP request carries the name

    // Bring up Ethernet and the SNMP agent
    if (startEthernet()) {
        netStatsBegin(eth.getNetIf());
        ipv6StaticBegin(eth.getNetIf());
        dhcp6Begin(eth.getNetIf());  // waits for a router advertisement with the M flag
        lldpBegin(eth.getNetIf());
        netStatsRequestDhcpTrapTarget(currentSettings.trapFromDhcp);  // before the first DISCOVER
        snmpBegin();
        webBegin();
    }

    // Serial configuration menu
    cliBegin();

    // From here on a hang on either core restarts the device
    watchdogArm();
}

void loop() {
    hostStatsLoopPass();  // core 0 load: how much longer this pass takes than an idle one
    watchdogPoll();
    diagPoll();
    DIAG_MARK(1);
    // Report when the address is obtained, changed or lost
    static uint32_t lastIP = 0;
    uint32_t ip = eth.connected() ? (uint32_t)eth.localIP() : 0;
    if (ip != lastIP) {
        lastIP = ip;
        if (ip) {
            Serial.printf("\r\nNetwork up after %lu ms\r\n", (unsigned long)millis());
            printNetworkInfo();
            snmpNetworkUp(eth.localIP());
            hostnameNetworkUp();
        } else {
            Serial.println("\r\nNetwork down, retrying");
            snmpNetworkDown();
        }
    }

    DIAG_MARK(2);
    // Report mains power changes detected by core 1
    static uint32_t lastMainsChanges = 0;
    MainsSnapshot mains;
    if (mainsRead(mains) && mains.changes != lastMainsChanges) {
        lastMainsChanges = mains.changes;
        Serial.printf("\r\nMains power %s at %lu ms\r\n", mains.present ? "RESTORED" : "LOST",
                      (unsigned long)mains.lastChangeMs);
        snmpNotifyMainsChange();
    }
    snmpPollMainsAlarm();

    // Link state for IF-MIB, CPU and ambient temperature
    DIAG_MARK(3);
    netStatsPoll(snmpUptimeTicks());
    dhcp6Poll();
    lldpPoll();
    DIAG_MARK(4);
    cpuTempPoll();
    ambientTempPoll();
    hostStatsPoll();

    // SNMP requests
    DIAG_MARK(5);
    snmpPoll();

    // Configuration web page (non-blocking)
    DIAG_MARK(6);
    webPoll();

    // Serial configuration menu
    DIAG_MARK(7);
    cliProcess();

    // Factory reset requested by core 1
    DIAG_MARK(8);
    factoryResetPoll();
    DIAG_MARK(9);
}

// Core 1: mains power monitoring, reset button and LED
// (.clinerules: core 1 handles power monitoring and GPIO)
void setup1() {
    mainsBegin();
    factoryResetBegin();
}

void loop1() {
    uint32_t start = time_us_32();
    mainsSample();
    factoryResetSample();
    watchdogCore1Beat();
    // Core 1 load: the time outside delay(1)
    uint32_t end = time_us_32();
    hostStatsCore1Busy(end, end - start);
    delay(1);
}
