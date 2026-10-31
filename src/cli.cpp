#include "cli.h"
#include "diag.h"

#include <mains_input.h>
#include <netconfig.h>
#include <settings.h>
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "hostname.h"
#include "net_stats.h"
#include "snmp_service.h"
#include "stack_watch.h"
#include "web_service.h"
#include <web_auth.h>

// Menu state machine
enum CliState {
    CLI_MENU,           // Waiting for a menu choice
    CLI_STATIC_IP,      // Entering static IP
    CLI_STATIC_SUBNET,  // Entering subnet mask
    CLI_STATIC_GATEWAY, // Entering gateway
    CLI_CONFIRM,        // Confirming network change, save and reboot
    CLI_SNMP_COMMUNITY, // Entering SNMP community
    CLI_SNMP_NAME,      // Entering sysName
    CLI_SNMP_CONTACT,   // Entering sysContact
    CLI_SNMP_LOCATION,  // Entering sysLocation
    CLI_SNMP_TRAP,      // Entering trap destination
    CLI_SNMP_TRAP_VER,  // Entering trap version
    CLI_SNMP_CONFIRM,   // Confirming SNMP change, save without reboot
    CLI_WEB_PASSWORD,   // Entering a new web password (echoed as *)
    CLI_WEB_REPEAT,     // Repeating it
    CLI_WEB_REGENERATE, // Confirming a new default web password
    CLI_WEB_TOGGLE,     // Confirming web interface on/off
    CLI_IPV6_ADDRESS,   // Entering the static IPv6 address
    CLI_IPV6_GATEWAY,   // Entering the static IPv6 gateway
    CLI_IPV6_CONFIRM    // Confirming the IPv6 change, save and reboot
};

static CliState state = CLI_MENU;
static char lineBuf[96];
static uint8_t lineLen = 0;
static bool lineOverflow = false;  // More characters were typed than lineBuf holds
static Settings settingsBackup;    // restored when a save fails; static: too big for core 0's stack

// Network values being entered, applied only on confirm
static char newIP[16];
static char newSubnet[16];
static char newGateway[16];
static bool newDhcp;

// SNMP values being entered, applied only on confirm
static char newCommunity[COMMUNITY_MAX_LENGTH + 1];
static char newSysName[SYS_TEXT_MAX_LENGTH + 1];
static char newSysContact[SYS_TEXT_MAX_LENGTH + 1];
static char newSysLocation[SYS_TEXT_MAX_LENGTH + 1];
static char newTrapTarget[16];
static bool newTrapFromDhcp;
static uint8_t newTrapVersion;

// Static IPv6 values being entered, applied only on confirm
static char newIPv6[IPV6_TEXT_MAX];
static char newIPv6Gateway[IPV6_TEXT_MAX];

// Web password being entered; cleared after use
static char newWebPassword[WEB_PASSWORD_MAX + 1];

static const char* trapVersionName(uint8_t v) {
    return v == TRAP_VERSION_V1 ? "1" : "2c";
}

/**
 * @brief Print the main menu
 */
static void printMenu() {
    Serial.println();
    Serial.println("=== RF-UPS-Monitor Menu ===");
    Serial.println("1. Show status");
    Serial.println("2. Use DHCP");
    Serial.println("3. Set static IP");
    Serial.println("4. SNMP settings");
    Serial.println("5. Access log");
    Serial.println("6. Web password");
    Serial.println("7. New default web password");
    Serial.println("8. Web interface on/off");
    Serial.println("9. Static IPv6");
    Serial.println("?. Show this menu");
    Serial.print("> ");
}

/**
 * @brief One status line with a temperature in tenths of a degree, e.g. "-0.5 C"
 */
static void printTemperature(const char* name, int32_t deciC) {
    uint32_t a = (uint32_t)(deciC < 0 ? -deciC : deciC);
    Serial.printf("%s: %s%lu.%lu C\n", name, deciC < 0 ? "-" : "", (unsigned long)(a / 10),
                  (unsigned long)(a % 10));
}

/**
 * @brief Show the stored settings and the live interface addresses
 */
static void printStatus() {
    Serial.println();

    MainsSnapshot mains;
    if (!mainsRead(mains)) {
        Serial.println("Mains power: unknown (monitor not running)");
    } else if (mains.changes == 0) {
        Serial.printf("Mains power: %s (no changes since boot)\n", mains.present ? "present" : "LOST");
    } else {
        Serial.printf("Mains power: %s (%lu changes, last change %lu s ago)\n",
                      mains.present ? "present" : "LOST", (unsigned long)mains.changes,
                      (unsigned long)((millis() - mains.lastChangeMs) / 1000));
    }

    const SnmpStats& snmp = snmpStats();
    Serial.printf("SNMP: %lu packets, %lu responses (get %lu, getnext %lu, set %lu), "
                  "bad community %lu, bad version %lu, malformed %lu\n",
                  (unsigned long)snmp.inPackets, (unsigned long)snmp.outResponses,
                  (unsigned long)snmp.inGetRequests, (unsigned long)snmp.inGetNextRequests,
                  (unsigned long)snmp.inSetRequests, (unsigned long)snmp.inBadCommunity,
                  (unsigned long)snmp.inBadVersions, (unsigned long)snmp.inParseErrors);
    Serial.printf("SNMP v2c: %lu requests, %lu getbulk\n", (unsigned long)snmp.inV2cRequests,
                  (unsigned long)snmp.inGetBulkRequests);

    printTemperature("CPU temperature", cpuTempDeciC());
    if (ambientTempValid()) {
        printTemperature("Ambient temperature", ambientTempDeciC());
    } else {
        Serial.print("Ambient temperature: no sensor (TMP1075 at 0x48 on GP4/GP5)\n");
    }
    SnmpInterfaceState ifs;
    netStatsCapture(ifs);
    Serial.printf("Ethernet: link %s, %lu Mbit/s, in %llu bytes / %llu packets, out %llu bytes / %llu packets\n",
                  ifs.linkUp ? "up" : "down", (unsigned long)(ifs.speedBps / 1000000),
                  (unsigned long long)ifs.counters.inOctets,
                  (unsigned long long)(ifs.counters.inUcastPkts + ifs.counters.inMulticastPkts +
                                       ifs.counters.inBroadcastPkts),
                  (unsigned long long)ifs.counters.outOctets,
                  (unsigned long long)(ifs.counters.outUcastPkts + ifs.counters.outMulticastPkts +
                                       ifs.counters.outBroadcastPkts));
    Serial.printf("Ethernet drops: %lu discarded (no buffer), %lu errors, %lu unknown protocol, %lu send errors\n",
                  (unsigned long)ifs.counters.inDiscards, (unsigned long)ifs.counters.inErrors,
                  (unsigned long)ifs.counters.inUnknownProtos, (unsigned long)ifs.counters.outErrors);
    const SnmpServiceStats& svc = snmpServiceStats();
    Serial.printf("SNMP security: %lu rate limited, %lu access log events\n",
                  (unsigned long)svc.rateLimited, (unsigned long)snmpAccessLog().total());
    Serial.printf("SNMP community: \"%s\"\n", currentSettings.community);
    Serial.printf("sysName: \"%s\"\n", currentSettings.sysName);
    Serial.printf("sysContact: \"%s\"\n", currentSettings.sysContact);
    Serial.printf("sysLocation: \"%s\"\n", currentSettings.sysLocation);
    // The destination in use first, then where it comes from
    uint8_t trap[4];
    char trapText[16] = "";
    if (snmpTrapTarget(trap)) snprintf(trapText, sizeof(trapText), "%u.%u.%u.%u", trap[0], trap[1], trap[2], trap[3]);
    Serial.printf("Trap destination: \"%s\" (%lu sent, %lu failed)\n", trapText, (unsigned long)svc.trapsSent,
                  (unsigned long)svc.trapsFailed);
    if (!currentSettings.trapFromDhcp) {
        Serial.println("Trap destination source: fixed (menu option 4)");
    } else {
        DhcpTrapInfo d = netStatsDhcpTrap();
        Serial.printf("Trap destination source: DHCP option %u (asked in %lu DHCP messages; ", DHCP_OPTION_TRAP_TARGET,
                      (unsigned long)d.requests);
        if (!currentSettings.dhcpEnabled) {
            Serial.println("static IP: no DHCP)");
        } else if (!d.seen) {
            Serial.println("no DHCP ACK yet)");
        } else if (!d.hasOption) {
            Serial.printf("last ACK %lu s ago had no option %u)\n", (unsigned long)((millis() - d.ackMs) / 1000),
                          DHCP_OPTION_TRAP_TARGET);
        } else {
            Serial.printf("last ACK %lu s ago: %u.%u.%u.%u)\n", (unsigned long)((millis() - d.ackMs) / 1000),
                          d.target[0], d.target[1], d.target[2], d.target[3]);
        }
        if (d.ignored) {
            Serial.printf("DHCP: %lu ACK-like frames ignored (not accepted by the DHCP client, possibly forged)\n",
                          (unsigned long)d.ignored);
        }
    }
    Serial.printf("Trap version: %s\n", currentSettings.trapVersion == TRAP_VERSION_V1 ? "v1" : "v2c");
    Serial.printf("Hostname: %s (DHCP, mDNS %s.local)\n", hostnameCurrent(), hostnameCurrent());
    Serial.printf("Web interface: %s\n", !webEnabled()   ? "off (option 8 turns it on)"
                                         : webListening() ? "on (TCP port 80)"
                                                          : "on, opening TCP port 80 (retrying)");
    if (webUsesDefaultPassword()) {
        Serial.printf("Web login: user admin, default password \"%s\" (change with option 6)\n", webDefaultPassword());
    } else {
        Serial.println("Web login: user admin, own password (option 6)");
    }
    Serial.printf("Core 0 stack: %u bytes never used\n", (unsigned)stackWatchNeverUsed());
    if (diagReport()) Serial.println(diagReport());

    Serial.printf("Mode: %s\n", currentSettings.dhcpEnabled ? "DHCP" : "Static");
    Serial.printf("Stored static IP: %s / %s, gateway %s\n",
                  currentSettings.staticIP, currentSettings.subnetMask, currentSettings.gateway);
    if (currentSettings.ipv6Address[0]) {
        Serial.printf("Stored static IPv6: %s/64, gateway %s\n", currentSettings.ipv6Address,
                      currentSettings.ipv6Gateway[0] ? currentSettings.ipv6Gateway : "from router advertisements");
    } else {
        Serial.println("Stored static IPv6: none (option 9)");
    }
    Serial.println("Live interface:");
    printNetworkInfo();
}

/**
 * @brief Prompt for a value, showing the current one as default
 */
static void prompt(const char* name, const char* current) {
    Serial.printf("%s [%s]: ", name, current);
}

/**
 * @brief Accept an IPv4 address line, empty keeps the current value
 * @return true if dest now holds a valid address
 */
static bool takeAddress(const char* input, const char* current, char* dest) {
    const char* value = input[0] ? input : current;
    uint8_t octets[4];
    if (!parseIPv4(value, octets)) {
        Serial.printf("'%s' is not a valid IPv4 address\n", value);
        return false;
    }
    strlcpy(dest, value, 16);
    return true;
}

/**
 * @brief Start entering a static configuration from the IP prompt
 */
static void startStaticEntry() {
    prompt("IP address", currentSettings.staticIP);
    state = CLI_STATIC_IP;
}

/**
 * @brief Accept a text line for an SNMP setting
 *
 * Empty input keeps the current value; "-" clears it when empty is allowed.
 * The menu only accepts printable characters, so length is the main check.
 *
 * @return true if dest now holds a valid value
 */
static bool takeText(const char* input, const char* current, char* dest, size_t maxLength, bool allowEmpty) {
    const char* value = input;
    if (input[0] == '\0') {
        value = current;
    } else if (strcmp(input, "-") == 0) {
        value = "";
    }
    if (value[0] == '\0' && !allowEmpty) {
        Serial.println("Must not be empty");
        return false;
    }
    if (strlen(value) > maxLength) {
        Serial.printf("Too long, at most %u characters\n", (unsigned)maxLength);
        return false;
    }
    if (!settingsTextIsValid(value, maxLength, allowEmpty)) {
        Serial.println("Only printable ASCII characters are allowed");
        return false;
    }
    strlcpy(dest, value, maxLength + 1);
    return true;
}

/**
 * @brief Accept the hostname (also sysName): Enter keeps the current value, else it must be a valid hostname
 */
static bool takeHostname(const char* input, const char* current, char* dest) {
    if (input[0] == '\0') {
        strlcpy(dest, current, SYS_TEXT_MAX_LENGTH + 1);  // may be free text from older firmware
        return true;
    }
    if (!hostnameIsValid(input)) {
        Serial.printf("Not a valid hostname: 1-%u letters, digits and '-', not starting or ending with '-'\n",
                      (unsigned)HOSTNAME_MAX_LENGTH);
        return false;
    }
    strlcpy(dest, input, SYS_TEXT_MAX_LENGTH + 1);
    return true;
}

/**
 * @brief Prompt for the SNMP setting that belongs to the current state
 */
static void promptSnmp() {
    switch (state) {
    case CLI_SNMP_COMMUNITY: prompt("Community", currentSettings.community); break;
    case CLI_SNMP_NAME:      prompt("Hostname (sysName)", currentSettings.sysName); break;
    case CLI_SNMP_CONTACT:   prompt("sysContact", currentSettings.sysContact); break;
    case CLI_SNMP_LOCATION:  prompt("sysLocation", currentSettings.sysLocation); break;
    case CLI_SNMP_TRAP:
        prompt("Trap destination IP or dhcp", currentSettings.trapFromDhcp ? "dhcp" : currentSettings.trapTarget);
        break;
    case CLI_SNMP_TRAP_VER:  prompt("Trap version (1/2c)", trapVersionName(currentSettings.trapVersion)); break;
    default: break;
    }
}

/**
 * @brief Accept the trap destination: empty keeps, "-" turns traps off, "dhcp" takes it from
 *        DHCP option 224, else an IPv4 address
 */
static bool takeTrapTarget(const char* input) {
    // The fixed address is kept while DHCP is chosen, for switching back
    strlcpy(newTrapTarget, currentSettings.trapTarget, sizeof(newTrapTarget));
    if (input[0] == '\0') {
        newTrapFromDhcp = currentSettings.trapFromDhcp;
        return true;
    }
    if (strcmp(input, "dhcp") == 0 || strcmp(input, "DHCP") == 0) {
        newTrapFromDhcp = true;
        return true;
    }
    newTrapFromDhcp = false;
    if (strcmp(input, "-") == 0) {
        newTrapTarget[0] = '\0';
        return true;
    }
    uint8_t octets[4];
    if (strlen(input) >= sizeof(newTrapTarget) || !parseIPv4(input, octets)) {
        Serial.printf("'%s' is not a valid IPv4 address\n", input);
        return false;
    }
    strlcpy(newTrapTarget, input, sizeof(newTrapTarget));
    return true;
}

/**
 * @brief Show the access log, newest first
 */
static void printAccessLog() {
    const AccessLog& log = snmpAccessLog();
    uint32_t now = millis();
    Serial.println();
    Serial.printf("Access log: %u entries, %lu events since boot\n", log.size(), (unsigned long)log.total());
    for (uint8_t i = 0; i < log.size(); i++) {
        const AccessLog::Entry& e = log.newest(i);
        char ip[SOURCE_TEXT_MAX];
        formatSource(e.source, ip);
        Serial.printf("  %-15s %-14s x%-6lu last %lu s ago\n", ip, accessEventName(e.event),
                      (unsigned long)e.count, (unsigned long)((now - e.lastMs) / 1000));
    }
    Serial.print("> ");
}

/**
 * @brief Show the pending SNMP change and ask for confirmation
 */
static void askSnmpConfirm() {
    Serial.println();
    Serial.printf("New SNMP settings: community \"%s\", sysName \"%s\", sysContact \"%s\", sysLocation \"%s\", "
                  "trap destination \"%s\", trap version %s\n",
                  newCommunity, newSysName, newSysContact, newSysLocation, newTrapFromDhcp ? "dhcp" : newTrapTarget,
                  newTrapVersion == TRAP_VERSION_V1 ? "v1" : "v2c");
    Serial.print("Save? (y/n): ");
    state = CLI_SNMP_CONFIRM;
}

/**
 * @brief Show the pending change and ask for confirmation
 */
static void askConfirm() {
    Serial.println();
    if (newDhcp) {
        Serial.println("New mode: DHCP");
    } else {
        Serial.printf("New mode: Static %s / %s, gateway %s\n", newIP, newSubnet, newGateway);
    }
    Serial.print("Save and reboot? (y/n): ");
    state = CLI_CONFIRM;
}

/**
 * @brief Accept an IPv6 line: Enter keeps current, "-" clears, else IPv6 text stored in RFC 5952 form
 * @return true if dest now holds an empty or well-formed value (usability is checked by the caller)
 */
static bool takeIPv6(const char* input, const char* current, char* dest) {
    const char* value = input[0] == '\0' ? current : strcmp(input, "-") == 0 ? "" : input;
    if (value[0] == '\0') {
        dest[0] = '\0';
        return true;
    }
    uint8_t bytes[16];
    if (!parseIPv6(value, bytes)) {
        Serial.printf("'%s' is not a valid IPv6 address (write it without /64, e.g. fd00::10)\n", value);
        return false;
    }
    formatIPv6(bytes, dest);
    return true;
}

static void promptIPv6Address() {
    prompt("Static IPv6 address, '-' for none", currentSettings.ipv6Address);
    state = CLI_IPV6_ADDRESS;
}

static void promptIPv6Gateway() {
    prompt("IPv6 gateway, '-' for routers that announce themselves", currentSettings.ipv6Gateway);
    state = CLI_IPV6_GATEWAY;
}

static void askIPv6Confirm() {
    Serial.println();
    if (newIPv6[0]) {
        Serial.printf("New static IPv6: %s/64, gateway %s\n", newIPv6,
                      newIPv6Gateway[0] ? newIPv6Gateway : "from router advertisements");
    } else {
        Serial.println("New static IPv6: none");
    }
    Serial.print("Save and reboot? (y/n): ");
    state = CLI_IPV6_CONFIRM;
}

static void promptWebPassword() {
    Serial.printf("New web password (%u-%u characters, no spaces), '-' for the device default, Enter to cancel: ",
                  (unsigned)WEB_PASSWORD_MIN, (unsigned)WEB_PASSWORD_MAX);
    state = CLI_WEB_PASSWORD;
}

/// Leave the web password entry, wiping what was typed
static void endWebPassword(const char* message) {
    memset(newWebPassword, 0, sizeof(newWebPassword));
    memset(lineBuf, 0, sizeof(lineBuf));
    Serial.println(message);
    state = CLI_MENU;
    printMenu();
}

/**
 * @brief Handle one complete input line according to the current state
 */
static void handleLine(const char* line) {
    switch (state) {
    case CLI_MENU:
        if (strcmp(line, "1") == 0) {
            printStatus();
            Serial.print("> ");
        } else if (strcmp(line, "2") == 0) {
            newDhcp = true;
            strlcpy(newIP, currentSettings.staticIP, sizeof(newIP));
            strlcpy(newSubnet, currentSettings.subnetMask, sizeof(newSubnet));
            strlcpy(newGateway, currentSettings.gateway, sizeof(newGateway));
            askConfirm();
        } else if (strcmp(line, "3") == 0) {
            newDhcp = false;
            Serial.println("Press Enter to keep the value in brackets.");
            startStaticEntry();
        } else if (strcmp(line, "4") == 0) {
            Serial.println("Press Enter to keep the value in brackets, '-' to clear it.");
            state = CLI_SNMP_COMMUNITY;
            promptSnmp();
        } else if (strcmp(line, "5") == 0) {
            printAccessLog();
        } else if (strcmp(line, "6") == 0) {
            Serial.printf("Web page login: user admin, %s.\n",
                          webUsesDefaultPassword() ? "currently the device default password" : "own password set");
            promptWebPassword();
        } else if (strcmp(line, "7") == 0) {
            Serial.printf("Replace the device default web password \"%s\" with a new random one? (y/n): ",
                          webDefaultPassword());
            state = CLI_WEB_REGENERATE;
        } else if (strcmp(line, "8") == 0) {
            Serial.printf(webEnabled() ? "The web interface is on. Turn it off? (y/n): "
                                       : "The web interface is off. Turn it on? (y/n): ");
            state = CLI_WEB_TOGGLE;
        } else if (strcmp(line, "9") == 0) {
            Serial.println("The address is used as a /64 next to the automatic (SLAAC) address.");
            Serial.println("Press Enter to keep the value in brackets.");
            promptIPv6Address();
        } else if (line[0] == '\0') {
            Serial.print("> ");
        } else {
            printMenu();
        }
        break;

    case CLI_IPV6_ADDRESS: {
        if (!takeIPv6(line, currentSettings.ipv6Address, newIPv6)) {
            promptIPv6Address();
            break;
        }
        StaticIPv6Error err = validateStaticIPv6(newIPv6, "");
        if (err != StaticIPv6Error::None) {
            Serial.println(staticIPv6ErrorMessage(err));
            promptIPv6Address();
        } else if (newIPv6[0] == '\0') {
            newIPv6Gateway[0] = '\0';  // no gateway without an address
            askIPv6Confirm();
        } else {
            promptIPv6Gateway();
        }
        break;
    }

    case CLI_IPV6_GATEWAY: {
        if (!takeIPv6(line, currentSettings.ipv6Gateway, newIPv6Gateway)) {
            promptIPv6Gateway();
            break;
        }
        StaticIPv6Error err = validateStaticIPv6(newIPv6, newIPv6Gateway);
        if (err != StaticIPv6Error::None) {
            Serial.println(staticIPv6ErrorMessage(err));
            promptIPv6Gateway();
        } else {
            askIPv6Confirm();
        }
        break;
    }

    case CLI_IPV6_CONFIRM:
        if (line[0] == 'y' || line[0] == 'Y') {
            settingsBackup = currentSettings;
            strlcpy(currentSettings.ipv6Address, newIPv6, sizeof(currentSettings.ipv6Address));
            strlcpy(currentSettings.ipv6Gateway, newIPv6Gateway, sizeof(currentSettings.ipv6Gateway));
            if (saveSettings()) {
                Serial.println("Settings saved. Rebooting...");
                Serial.flush();
                delay(200);
                rp2040.reboot();
            }
            currentSettings = settingsBackup;
            Serial.println("Failed to save settings, nothing changed");
        } else {
            Serial.println("Cancelled");
        }
        state = CLI_MENU;
        printMenu();
        break;

    case CLI_STATIC_IP:
        if (takeAddress(line, currentSettings.staticIP, newIP)) {
            prompt("Subnet mask", currentSettings.subnetMask);
            state = CLI_STATIC_SUBNET;
        } else {
            prompt("IP address", currentSettings.staticIP);
        }
        break;

    case CLI_STATIC_SUBNET: {
        uint8_t mask[4];
        if (!takeAddress(line, currentSettings.subnetMask, newSubnet)) {
            prompt("Subnet mask", currentSettings.subnetMask);
        } else if (parseIPv4(newSubnet, mask) && !isValidSubnetMask(mask)) {
            Serial.println(staticConfigErrorMessage(StaticConfigError::NonContiguousMask));
            prompt("Subnet mask", currentSettings.subnetMask);
        } else {
            prompt("Gateway", currentSettings.gateway);
            state = CLI_STATIC_GATEWAY;
        }
        break;
    }

    case CLI_STATIC_GATEWAY: {
        if (!takeAddress(line, currentSettings.gateway, newGateway)) {
            prompt("Gateway", currentSettings.gateway);
            break;
        }
        StaticConfigError err = validateStaticConfig(newIP, newSubnet, newGateway);
        if (err == StaticConfigError::None) {
            askConfirm();
        } else if (err == StaticConfigError::GatewayNotInSubnet || err == StaticConfigError::GatewayIsHost) {
            Serial.println(staticConfigErrorMessage(err));
            prompt("Gateway", currentSettings.gateway);
        } else {
            // IP and mask combination is unusable, start over
            Serial.println(staticConfigErrorMessage(err));
            startStaticEntry();
        }
        break;
    }

    case CLI_CONFIRM:
        if (line[0] == 'y' || line[0] == 'Y') {
            settingsBackup = currentSettings;
            currentSettings.dhcpEnabled = newDhcp;
            strlcpy(currentSettings.staticIP, newIP, sizeof(currentSettings.staticIP));
            strlcpy(currentSettings.subnetMask, newSubnet, sizeof(currentSettings.subnetMask));
            strlcpy(currentSettings.gateway, newGateway, sizeof(currentSettings.gateway));
            if (saveSettings()) {
                Serial.println("Settings saved. Rebooting...");
                Serial.flush();
                delay(200);
                rp2040.reboot();
            }
            currentSettings = settingsBackup;
            Serial.println("Failed to save settings, nothing changed");
        } else {
            Serial.println("Cancelled");
        }
        state = CLI_MENU;
        printMenu();
        break;

    case CLI_SNMP_COMMUNITY:
        if (takeText(line, currentSettings.community, newCommunity, COMMUNITY_MAX_LENGTH, false)) {
            state = CLI_SNMP_NAME;
        }
        promptSnmp();
        break;

    case CLI_SNMP_NAME:
        if (takeHostname(line, currentSettings.sysName, newSysName)) {
            state = CLI_SNMP_CONTACT;
        }
        promptSnmp();
        break;

    case CLI_SNMP_CONTACT:
        if (takeText(line, currentSettings.sysContact, newSysContact, SYS_TEXT_MAX_LENGTH, true)) {
            state = CLI_SNMP_LOCATION;
        }
        promptSnmp();
        break;

    case CLI_SNMP_LOCATION:
        if (takeText(line, currentSettings.sysLocation, newSysLocation, SYS_TEXT_MAX_LENGTH, true)) {
            state = CLI_SNMP_TRAP;
        }
        promptSnmp();
        break;

    case CLI_SNMP_TRAP:
        if (takeTrapTarget(line)) {
            state = CLI_SNMP_TRAP_VER;
        }
        promptSnmp();
        break;

    case CLI_SNMP_TRAP_VER:
        if (line[0] == '\0') {
            newTrapVersion = currentSettings.trapVersion;
        } else if (strcmp(line, "1") == 0) {
            newTrapVersion = TRAP_VERSION_V1;
        } else if (strcmp(line, "2c") == 0 || strcmp(line, "2C") == 0) {
            newTrapVersion = TRAP_VERSION_V2C;
        } else {
            Serial.println("Enter 1 or 2c");
            promptSnmp();
            break;
        }
        askSnmpConfirm();
        break;

    case CLI_SNMP_CONFIRM:
        if (line[0] == 'y' || line[0] == 'Y') {
            settingsBackup = currentSettings;
            bool communityChanged = strcmp(currentSettings.community, newCommunity) != 0;
            strlcpy(currentSettings.community, newCommunity, sizeof(currentSettings.community));
            strlcpy(currentSettings.sysName, newSysName, sizeof(currentSettings.sysName));
            strlcpy(currentSettings.sysContact, newSysContact, sizeof(currentSettings.sysContact));
            strlcpy(currentSettings.sysLocation, newSysLocation, sizeof(currentSettings.sysLocation));
            strlcpy(currentSettings.trapTarget, newTrapTarget, sizeof(currentSettings.trapTarget));
            currentSettings.trapFromDhcp = newTrapFromDhcp;
            currentSettings.trapVersion = newTrapVersion;
            // The SNMP agent reads these on every request, so no reboot is needed
            if (saveSettings()) {
                if (communityChanged) snmpCommunityChanged();
                hostnameApply();  // no-op when sysName did not change
                netStatsRequestDhcpTrapTarget(currentSettings.trapFromDhcp);  // renews when switched on
                Serial.println("SNMP settings saved and active");
            } else {
                currentSettings = settingsBackup;
                Serial.println("Failed to save settings, nothing changed");
            }
        } else {
            Serial.println("Cancelled");
        }
        state = CLI_MENU;
        printMenu();
        break;

    case CLI_WEB_PASSWORD:
        if (line[0] == '\0') {
            endWebPassword("Cancelled");
        } else if (strcmp(line, "-") == 0) {
            endWebPassword(webSetPassword(nullptr) ? "Web password reset to the device default; web sessions logged out"
                                                   : "Failed to save settings, nothing changed");
        } else if (!webPasswordIsValid(line)) {
            Serial.println("Not accepted: use 8-32 printable characters without spaces");
            promptWebPassword();
        } else {
            strlcpy(newWebPassword, line, sizeof(newWebPassword));
            Serial.print("Repeat the password: ");
            state = CLI_WEB_REPEAT;
        }
        break;

    case CLI_WEB_REPEAT:
        if (strcmp(line, newWebPassword) != 0) {
            endWebPassword("The passwords differ, nothing changed");
        } else {
            endWebPassword(webSetPassword(newWebPassword) ? "Web password saved; web sessions logged out"
                                                          : "Failed to save settings, nothing changed");
        }
        break;

    case CLI_WEB_REGENERATE:
        if (line[0] == 'y' || line[0] == 'Y') {
            bool saved = webRegenerateDefaultPassword();
            Serial.printf("New default web password: \"%s\"%s\n", webDefaultPassword(),
                          saved ? "" : " (NOT saved, a new one is generated at the next boot)");
            if (!webUsesDefaultPassword()) {
                Serial.println("Your own password stays in use; option 6 with '-' switches to the new default");
            }
        } else {
            Serial.println("Cancelled");
        }
        state = CLI_MENU;
        printMenu();
        break;

    case CLI_WEB_TOGGLE:
        if (line[0] == 'y' || line[0] == 'Y') {
            bool on = !webEnabled();
            if (webSetEnabled(on)) {
                Serial.println(on ? "Web interface on" : "Web interface off; browsers logged out");
            } else {
                Serial.println("Failed to save settings, nothing changed");
            }
        } else {
            Serial.println("Cancelled");
        }
        state = CLI_MENU;
        printMenu();
        break;
    }
}

void cliBegin() {
    printMenu();
}

void cliProcess() {
    static bool lastWasCR = false;

    while (Serial.available()) {
        char c = Serial.read();

        // Ignore the \n of a \r\n pair
        bool skip = (c == '\n' && lastWasCR);
        lastWasCR = (c == '\r');
        if (skip) continue;

        if (c == '\r' || c == '\n') {
            Serial.println();
            lineBuf[lineLen] = '\0';
            lineLen = 0;
            if (lineOverflow) {
                // Never act on a silently truncated value
                lineOverflow = false;
                Serial.printf("Input too long (max %u characters), ignored\n", (unsigned)(sizeof(lineBuf) - 1));
                if (state == CLI_MENU) {
                    Serial.print("> ");
                } else if (state >= CLI_SNMP_COMMUNITY && state <= CLI_SNMP_TRAP_VER) {
                    promptSnmp();
                } else if (state == CLI_WEB_PASSWORD || state == CLI_WEB_REPEAT) {
                    memset(newWebPassword, 0, sizeof(newWebPassword));
                    promptWebPassword();
                } else {
                    state = CLI_MENU;
                    printMenu();
                }
                continue;
            }
            handleLine(lineBuf);
        } else if (c == '\b' || c == 0x7F) {
            if (lineLen > 0) {
                lineLen--;
                Serial.print("\b \b");
            }
        } else if (isprint((unsigned char)c)) {
            if (lineLen < sizeof(lineBuf) - 1) {
                lineBuf[lineLen++] = c;
                // Echo, terminals like PuTTY don't echo locally; never echo a password
                bool secret = state == CLI_WEB_PASSWORD || state == CLI_WEB_REPEAT;
                Serial.print(secret ? '*' : c);
            } else {
                lineOverflow = true;
            }
        }
    }
}
