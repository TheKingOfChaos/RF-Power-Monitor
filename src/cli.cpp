#include "cli.h"
#include "diag.h"

#include <mains_input.h>
#include <netconfig.h>
#include <settings.h>
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "dhcp6_service.h"
#include "fw_update_service.h"
#include "hostname.h"
#include "http_fetch.h"
#include "update_service.h"
#include <update_settings_store.h>
#include "lldp_service.h"
#include "net_stats.h"
#include "snmp_service.h"
#include "host_stats.h"
#include "stack_watch.h"
#include "watchdog.h"
#include "web_service.h"
#include <web_auth.h>
#include <bench_info.h>
#include <monocypher.h>
#include <new>
#include "version.h"
#include "time_service.h"

extern CountingW5500lwIP eth;  // main.cpp

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
    CLI_IPV6_CONFIRM,   // Confirming the IPv6 change, save and reboot
    CLI_UPDATE_URL,     // Entering the update server URL (or dhcp / none)
    CLI_UPDATE_KEY      // Entering the shared key (echoed as *), then save
};

static CliState state = CLI_MENU;
static char lineBuf[144];  // room for a 127-character URL after a command word
static uint8_t lineLen = 0;
static bool lineOverflow = false;  // More characters were typed than lineBuf holds
static Settings settingsBackup;    // restored when a save fails; static: too big for core 0's stack

// Update server values being entered (option 10), saved after the key
static char newUpdateUrl[UPDATE_URL_MAX + 1];
static bool newUpdateFromDhcp = false;

// Hidden command fetchtest <url>: a GET in the background, its result printed when it ends
static bool fetchTestActive = false;
static uint32_t fetchTestBytes = 0;
static uint32_t fetchTestStartMs = 0;

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
static char newTrapTarget[IPV6_TEXT_MAX];
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

/// "Update server: <url> (fixed|DHCP option 225), key set|not set, <check-ins>" or "Update server: off"
static void printUpdateServerLine() {
    if (!currentUpdateSettings.fromDhcp && currentUpdateSettings.url[0] == '\0') {
        Serial.println("Update server: off (option 10)");
        return;
    }
    char state[96];
    updateServiceStateText(state, sizeof(state));
    UpdateServiceStatus st;
    updateServiceStatus(st);
    Serial.printf("Update server: %s (%s), key %s, %s\r\n", st.urlInUse[0] ? st.urlInUse : "none yet",
                  currentUpdateSettings.fromDhcp ? "DHCP option 225" : "fixed",
                  currentUpdateSettings.hasKey ? "set" : "not set", state);
}

/// "Update server default: <url>, key set|not set (a factory reset restores it)", only when the bench set one
static void printUpdateDefaultLine() {
    UpdateSettings* d = new (std::nothrow) UpdateSettings;  // heap: core 0's stack is small
    if (d == nullptr) return;
    if (loadUpdateBenchDefaults(*d)) {
        Serial.printf("Update server default: %s, key %s (a factory reset restores it)\r\n",
                      d->fromDhcp ? "dhcp" : d->url[0] ? d->url : "none", d->hasKey ? "set" : "not set");
    }
    crypto_wipe(d, sizeof(*d));  // it held the shared key
    delete d;
}

/// Ask for the update server URL (option 10)
static void promptUpdateUrl() {
    const char* current = currentUpdateSettings.fromDhcp ? "dhcp"
                          : currentUpdateSettings.url[0] ? currentUpdateSettings.url
                                                         : "none";
    Serial.printf("Update server URL (http://host[:port][/path]), dhcp or none [%s]: ", current);
    state = CLI_UPDATE_URL;
}

/// Ask for the shared key (option 10, second step)
static void promptUpdateKey() {
    Serial.printf("Shared key (64 hex characters from the update site), Enter keeps, clear removes [%s]: ",
                  currentUpdateSettings.hasKey ? "set" : "not set");
    state = CLI_UPDATE_KEY;
}

/**
 * @brief Print the main menu
 */
static void printMenu() {
    Serial.println();
    Serial.println("=== RF-Power-Monitor Menu ===");
    Serial.println("1. Show status");
    Serial.println("2. Use DHCP");
    Serial.println("3. Set static IP");
    Serial.println("4. SNMP settings");
    Serial.println("5. Access log");
    Serial.println("6. Web password");
    Serial.println("7. New default web password");
    Serial.println("8. Web interface on/off");
    Serial.println("9. Static IPv6");
    Serial.println("10. Update server");
    Serial.println("?. Show this menu");
    Serial.print("> ");
}

/**
 * @brief One status line with a temperature in tenths of a degree, e.g. "-0.5 C"
 */
static void printTemperature(const char* name, int32_t deciC) {
    uint32_t a = (uint32_t)(deciC < 0 ? -deciC : deciC);
    Serial.printf("%s: %s%lu.%lu C\r\n", name, deciC < 0 ? "-" : "", (unsigned long)(a / 10),
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
        Serial.printf("Mains power: %s (no changes since boot)\r\n", mains.present ? "present" : "LOST");
    } else {
        Serial.printf("Mains power: %s (%lu changes, last change %lu s ago)\r\n",
                      mains.present ? "present" : "LOST", (unsigned long)mains.changes,
                      (unsigned long)((millis() - mains.lastChangeMs) / 1000));
    }
    Serial.printf("Mains alarm: %s\r\n", snmpMainsAlarm() ? "lost over 60 s" : "none");

    const SnmpStats& snmp = snmpStats();
    Serial.printf("SNMP: %lu packets, %lu responses (get %lu, getnext %lu, set %lu), "
                  "bad community %lu, bad version %lu, malformed %lu\r\n",
                  (unsigned long)snmp.inPackets, (unsigned long)snmp.outResponses,
                  (unsigned long)snmp.inGetRequests, (unsigned long)snmp.inGetNextRequests,
                  (unsigned long)snmp.inSetRequests, (unsigned long)snmp.inBadCommunity,
                  (unsigned long)snmp.inBadVersions, (unsigned long)snmp.inParseErrors);
    Serial.printf("SNMP v2c: %lu requests, %lu getbulk\r\n", (unsigned long)snmp.inV2cRequests,
                  (unsigned long)snmp.inGetBulkRequests);

    printTemperature("CPU temperature", cpuTempDeciC());
    if (ambientTempValid()) {
        printTemperature("Ambient temperature", ambientTempDeciC());
    } else {
        Serial.print("Ambient temperature: no sensor (TMP1075 at 0x48 on GP4/GP5)\r\n");
    }
    SnmpInterfaceState ifs;
    netStatsCapture(ifs);
    Serial.printf("Ethernet: link %s, %lu Mbit/s, in %llu bytes / %llu packets, out %llu bytes / %llu packets\r\n",
                  ifs.linkUp ? "up" : "down", (unsigned long)(ifs.speedBps / 1000000),
                  (unsigned long long)ifs.counters.inOctets,
                  (unsigned long long)(ifs.counters.inUcastPkts + ifs.counters.inMulticastPkts +
                                       ifs.counters.inBroadcastPkts),
                  (unsigned long long)ifs.counters.outOctets,
                  (unsigned long long)(ifs.counters.outUcastPkts + ifs.counters.outMulticastPkts +
                                       ifs.counters.outBroadcastPkts));
    Serial.printf("Ethernet drops: %lu discarded (no buffer), %lu errors, %lu unknown protocol, %lu send errors\r\n",
                  (unsigned long)ifs.counters.inDiscards, (unsigned long)ifs.counters.inErrors,
                  (unsigned long)ifs.counters.inUnknownProtos, (unsigned long)ifs.counters.outErrors);
    const SnmpServiceStats& svc = snmpServiceStats();
    Serial.printf("SNMP security: %lu rate limited, %lu access log events\r\n",
                  (unsigned long)svc.rateLimited, (unsigned long)snmpAccessLog().total());
    Serial.printf("SNMP community: \"%s\"\r\n", currentSettings.community);
    Serial.printf("sysName: \"%s\"\r\n", hostnameCurrent());  // the name in use, also when the setting is empty
    Serial.printf("sysContact: \"%s\"\r\n", currentSettings.sysContact);
    Serial.printf("sysLocation: \"%s\"\r\n", currentSettings.sysLocation);
    // The destination in use first, then where it comes from
    TrapAddress trap;
    char trapText[IPV6_TEXT_MAX] = "";
    if (snmpTrapTarget(trap)) formatTrapAddress(trap, trapText);
    Serial.printf("Trap destination: \"%s\" (%lu sent, %lu failed)\r\n", trapText, (unsigned long)svc.trapsSent,
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
            Serial.printf("last ACK %lu s ago had no option %u)\r\n", (unsigned long)((millis() - d.ackMs) / 1000),
                          DHCP_OPTION_TRAP_TARGET);
        } else {
            Serial.printf("last ACK %lu s ago: %u.%u.%u.%u)\r\n", (unsigned long)((millis() - d.ackMs) / 1000),
                          d.target[0], d.target[1], d.target[2], d.target[3]);
        }
        if (d.ignored) {
            Serial.printf("DHCP: %lu ACK-like frames ignored (not accepted by the DHCP client, possibly forged)\r\n",
                          (unsigned long)d.ignored);
        }
    }
    Serial.printf("Trap version: %s\r\n", currentSettings.trapVersion == TRAP_VERSION_V1 ? "v1" : "v2c");
    Serial.printf("Hostname: %s (DHCP, mDNS %s.local)\r\n", hostnameCurrent(), hostnameCurrent());
    {
        static char sw[144], addr[46];  // static: core 0's stack is small
        lldpServiceSwitchText(sw, sizeof(sw));
        lldpServiceSwitchAddress(addr, sizeof(addr));
        if (addr[0]) {
            Serial.printf("Switch: %s (%s)\r\n", sw, addr);
        } else {
            Serial.printf("Switch: %s\r\n", sw);
        }
    }
    Serial.printf("Web interface: %s\r\n", !webEnabled()   ? "off (option 8 turns it on)"
                                         : webListening() ? "on (TCP port 80)"
                                                          : "on, opening TCP port 80 (retrying)");
    if (webUsesDefaultPassword()) {
        Serial.printf("Web login: user admin, default password \"%s\" (change with option 6)\r\n", webDefaultPassword());
    } else {
        Serial.println("Web login: user admin, own password (option 6)");
    }
    Serial.println(webDefaultPasswordLocked() ? "Default web password: locked (on the label)"
                                              : "Default web password: unlocked");
    if (webDefaultPasswordLost()) Serial.println(WEB_PASSWORD_LOST_TEXT);
    Serial.printf("Core 0 stack: %u bytes never used\r\n", (unsigned)stackWatchNeverUsed());
    {
        const SnmpHostState& h = hostStatsSnapshot();
        Serial.printf("CPU load: core 0 %u %%, core 1 %u %% (last minute)\r\n", h.cpuLoad[0], h.cpuLoad[1]);
        Serial.printf("RAM used: %lu of %lu bytes\r\n", (unsigned long)h.ramUsedBytes, (unsigned long)h.ramTotalBytes);
        if (h.fsValid) {
            Serial.printf("LittleFS used: %lu of %lu bytes\r\n", (unsigned long)h.fsUsedBytes,
                          (unsigned long)h.fsTotalBytes);
        }
        Serial.printf("Serial number: %s\r\n", h.serialNumber);
    }
    Serial.printf("Last restart: %s (%lu watchdog restarts since power-on)\r\n",
                  restartCauseName(watchdogRestartCause()), (unsigned long)watchdogRestarts());
    if (diagReport()) Serial.println(diagReport());
    {
        static WebFirmware fw;  // static: not on core 0's small stack
        memset(&fw, 0, sizeof(fw));
        fwUpdateStatus(fw);
        char other = fw.runningSlot == 0 ? 'B' : 'A';
        Serial.printf("Firmware: slot %c, %s, %s\r\n", fw.runningSlot == 0 ? 'A' : 'B', fw.runningVersion,
                      fw.trial ? "trial" : "kept");
        if (fw.otherVersion[0]) {
            Serial.printf("Other slot: %c, %s\r\n", other, fw.otherVersion);
        } else {
            Serial.printf("Other slot: %c, %s\r\n", other, fw.otherProblem[0] ? fw.otherProblem : "unknown");
        }
        Serial.printf("Last update: %s\r\n", fw.lastResult[0] ? fw.lastResult : "none");
        Serial.printf("Firmware key: %s\r\n", fw.keyFingerprint);
    }
    printUpdateServerLine();
    printUpdateDefaultLine();
    char timeLine[160];
    timeServiceStatusText(timeLine, sizeof(timeLine));
    Serial.printf("Time: %s\r\n", timeLine);

    Serial.printf("Mode: %s\r\n", currentSettings.dhcpEnabled ? "DHCP" : "Static");
    Serial.printf("Stored static IP: %s / %s, gateway %s\r\n",
                  currentSettings.staticIP, currentSettings.subnetMask, currentSettings.gateway);
    if (currentSettings.ipv6Address[0]) {
        Serial.printf("Stored static IPv6: %s/64, gateway %s\r\n", currentSettings.ipv6Address,
                      currentSettings.ipv6Gateway[0] ? currentSettings.ipv6Gateway : "from router advertisements");
    } else {
        Serial.println("Stored static IPv6: none (option 9)");
    }
    Dhcp6Status d6;
    dhcp6GetStatus(d6);
    if (d6.state == Dhcp6State::Idle) {
        Serial.println("DHCPv6: off (no router asked for it)");
    } else {
        Serial.printf("DHCPv6: %s", dhcp6StateName(d6.state));
        if (d6.address[0]) {
            if (d6.validLeftS == 0xFFFFFFFFu) {
                Serial.printf(", %s (valid forever)", d6.address);
            } else {
                Serial.printf(", %s (valid %lu s)", d6.address, (unsigned long)d6.validLeftS);
            }
        }
        Serial.printf(", %lu sent, %lu accepted, %lu ignored\r\n", (unsigned long)d6.sent, (unsigned long)d6.accepted,
                      (unsigned long)d6.ignored);
        if (d6.problem[0]) Serial.printf("DHCPv6: address not in use (%s)\r\n", d6.problem);
    }
    Serial.println("Live interface:");
    printNetworkInfo();
}

/**
 * @brief Print the device data block for the bench tool (hidden command benchinfo, lib/bench_info)
 */
static void printBenchInfo() {
    // On the heap while it runs: not on core 0's small stack, and static RAM is at its limit
    struct Work {
        WebFirmware fw;
        UpdateSettings updateDefault;
        char block[BENCH_INFO_MAX];
    };
    Work* w = new (std::nothrow) Work;
    if (w == nullptr) {
        Serial.println("benchinfo: out of memory");
        return;
    }
    memset(&w->fw, 0, sizeof(w->fw));
    fwUpdateStatus(w->fw);
    bool haveDefault = loadUpdateBenchDefaults(w->updateDefault);
    BenchInfo info{};
    info.hostname = hostnameCurrent();
    eth.macAddress(info.mac);
    info.serial = hostStatsSnapshot().serialNumber;
    info.firmware = FIRMWARE_VERSION;
    info.fwKey = w->fw.keyFingerprint;
    info.password = webDefaultPassword();
    info.locked = webDefaultPasswordLocked();
    info.passwordLost = webDefaultPasswordLost();
    if (haveDefault) {
        info.updateDefault = w->updateDefault.fromDhcp ? "dhcp" : w->updateDefault.url[0] ? w->updateDefault.url : "none";
        info.updateDefaultKey = w->updateDefault.hasKey;
    }
    if (formatBenchInfo(info, w->block, sizeof(w->block)) == 0) {
        Serial.println("benchinfo: does not fit");
    } else {
        Serial.print(w->block);
    }
    crypto_wipe(&w->updateDefault, sizeof(w->updateDefault));  // it held the shared key
    delete w;
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
        Serial.printf("'%s' is not a valid IPv4 address\r\n", value);
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
        Serial.printf("Too long, at most %u characters\r\n", (unsigned)maxLength);
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
 * @brief Accept the hostname (also sysName): Enter keeps the current value, '-' goes back to the default
 *        (RF-Power-<MAC tail>), else it must be a valid hostname
 */
static bool takeHostname(const char* input, const char* current, char* dest) {
    if (input[0] == '\0') {
        strlcpy(dest, current, SYS_TEXT_MAX_LENGTH + 1);  // may be free text from older firmware
        return true;
    }
    if (strcmp(input, "-") == 0) {
        dest[0] = '\0';  // not set: the default name
        return true;
    }
    if (!hostnameIsValid(input)) {
        Serial.printf("Not a valid hostname: 1-%u letters, digits and '-', not starting or ending with '-'\r\n",
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
    case CLI_SNMP_NAME:      prompt("Hostname (sysName)", hostnameCurrent()); break;  // Enter keeps the setting
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
 *        DHCP option 224, else an IPv4 or IPv6 address (IPv6 stored in RFC 5952 form)
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
    TrapAddress address;
    TrapTargetError err = parseTrapTarget(input, address);
    if (err != TrapTargetError::None) {
        Serial.printf("'%s': %s\r\n", input, trapTargetErrorMessage(err));
        return false;
    }
    formatTrapAddress(address, newTrapTarget);
    return true;
}

/**
 * @brief Show the access log, newest first
 */
static void printAccessLog() {
    const AccessLog& log = snmpAccessLog();
    uint32_t now = millis();
    Serial.println();
    Serial.printf("Access log: %u entries, %lu events since boot\r\n", log.size(), (unsigned long)log.total());
    for (uint8_t i = 0; i < log.size(); i++) {
        const AccessLog::Entry& e = log.newest(i);
        char ip[SOURCE_TEXT_MAX];
        formatSource(e.source, ip);
        Serial.printf("  %-15s %-14s x%-6lu last %lu s ago\r\n", ip, accessEventName(e.event),
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
                  "trap destination \"%s\", trap version %s\r\n",
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
        Serial.printf("New mode: Static %s / %s, gateway %s\r\n", newIP, newSubnet, newGateway);
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
        Serial.printf("'%s' is not a valid IPv6 address (write it without /64, e.g. fd00::10)\r\n", value);
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
        Serial.printf("New static IPv6: %s/64, gateway %s\r\n", newIPv6,
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
            Serial.printf("Web page login: user admin, %s.\r\n",
                          webUsesDefaultPassword() ? "currently the device default password" : "own password set");
            promptWebPassword();
        } else if (strcmp(line, "7") == 0) {
            if (webDefaultPasswordLocked()) {
                Serial.println("The default web password is locked because it is printed on the device label. "
                               "Unlock it at the bench first.");
                printMenu();
            } else {
                Serial.printf("Replace the device default web password \"%s\" with a new random one? (y/n): ",
                              webDefaultPassword());
                state = CLI_WEB_REGENERATE;
            }
        } else if (strcmp(line, "8") == 0) {
            Serial.printf(webEnabled() ? "The web interface is on. Turn it off? (y/n): "
                                       : "The web interface is off. Turn it on? (y/n): ");
            state = CLI_WEB_TOGGLE;
        } else if (strcmp(line, "10") == 0) {
            printUpdateServerLine();
            Serial.println("Press Enter to keep the value in brackets.");
            promptUpdateUrl();
        } else if (strcmp(line, "9") == 0) {
            Serial.println("The address is used as a /64 next to the automatic (SLAAC) address.");
            Serial.println("Press Enter to keep the value in brackets.");
            promptIPv6Address();
        } else if (strncmp(line, "fetchtest ", 10) == 0) {
            // Hidden test command (docs/TESTING.md): GET a URL with the update client's HTTP code
            UpdateUrl url;
            if (!parseUpdateUrl(line + 10, url)) {
                Serial.println("fetchtest: not an http:// URL");
            } else if (fetchTestActive || httpFetchBusy()) {
                Serial.println("fetchtest: busy (a check-in or download is running), try again");
            } else if (httpFetchStart(url, "GET", url.path[0] != '\0' ? url.path : "/", nullptr, nullptr, 0,
                                      0x7FFFFFFF, millis(), 0)) {
                fetchTestActive = true;
                fetchTestBytes = 0;
                fetchTestStartMs = millis();
                Serial.println("fetchtest: started");
            } else {
                Serial.printf("fetchtest: failed: %s\r\n", httpFetchError());
            }
            Serial.print("> ");
        } else if (strcmp(line, "checkin") == 0) {
            // Hidden test command (docs/TESTING.md): check in with the update server now
            updateServiceForceCheckin();
            Serial.printf("checkin: requested (service %s, fetch %s)\r\n", updateServicePhaseName(),
                          httpFetchBusy() ? "busy" : "free");
            Serial.print("> ");
        } else if (strcmp(line, "benchinfo") == 0) {
            // Hidden bench tool command (tools/bench_provision.py): device data for the label
            printBenchInfo();
            Serial.print("> ");
        } else if (strcmp(line, "updatedefault") == 0) {
            // Hidden bench tool command: keep the update server settings (option 10) through factory resets
            bool any = currentUpdateSettings.url[0] || currentUpdateSettings.fromDhcp || currentUpdateSettings.hasKey;
            if (!saveUpdateBenchDefaults()) {
                Serial.println("Failed to save the update server default");
            } else if (any) {
                Serial.println("Update server default saved (a factory reset restores it)");
            } else {
                Serial.println("Update server default removed (a factory reset turns check-ins off)");
            }
            Serial.print("> ");
        } else if (strcmp(line, "pwlock") == 0 || strcmp(line, "pwunlock") == 0) {
            // Hidden bench tool commands: lock the default web password once it is on the label, or unlock it
            bool lock = line[2] == 'l';
            bool ok = webSetDefaultPasswordLocked(lock);
            if (lock) {
                Serial.println(ok ? "Default web password locked (it is on the device label)" : "Failed to lock");
            } else {
                Serial.println(ok ? "Default web password unlocked (option 7 can replace it)" : "Failed to unlock");
            }
            Serial.print("> ");
        } else if (strcmp(line, "lldpdump") == 0) {
            // Hidden test command (docs/TESTING.md): the switch's last LLDPDU in hex, for test vectors
            lldpDump();
            Serial.print("> ");
        } else if (strcmp(line, "stall0") == 0 || strcmp(line, "stall1") == 0) {
            // Hidden test command (docs/TESTING.md): hang one core so the watchdog restarts the device
            uint8_t core = line[5] - '0';
            Serial.printf("Stalling core %u, the watchdog restarts the device within %lu s\r\n", core,
                          (unsigned long)(WATCHDOG_TIMEOUT_MS + CoreHeartbeatCheck::DEFAULT_STALE_MS) / 1000 + 1);
            Serial.flush();
            delay(200);  // USB is serviced by interrupts, which a core 0 stall switches off
            watchdogStall(core);
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
        if (!trapVersionFitsTarget(newTrapTarget, newTrapFromDhcp, newTrapVersion)) {
            Serial.println("An IPv6 trap destination needs trap version 2c (v1 traps carry an IPv4 address)");
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
            Serial.printf("New default web password: \"%s\"%s\r\n", webDefaultPassword(),
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

    case CLI_UPDATE_URL: {
        UpdateUrl parsed;
        if (line[0] == '\0') {
            strlcpy(newUpdateUrl, currentUpdateSettings.url, sizeof(newUpdateUrl));
            newUpdateFromDhcp = currentUpdateSettings.fromDhcp;
        } else if (strcmp(line, "none") == 0) {
            newUpdateUrl[0] = '\0';
            newUpdateFromDhcp = false;
        } else if (strcmp(line, "dhcp") == 0) {
            strlcpy(newUpdateUrl, currentUpdateSettings.url, sizeof(newUpdateUrl));  // kept for later
            newUpdateFromDhcp = true;
        } else if (strlen(line) > UPDATE_URL_MAX || !parseUpdateUrl(line, parsed)) {
            Serial.printf("'%s': not an http://host[:port][/path] URL of at most %u characters\r\n", line,
                          (unsigned)UPDATE_URL_MAX);
            promptUpdateUrl();
            break;
        } else {
            strlcpy(newUpdateUrl, line, sizeof(newUpdateUrl));
            newUpdateFromDhcp = false;
        }
        promptUpdateKey();
        break;
    }

    case CLI_UPDATE_KEY: {
        bool clear = strcmp(line, "clear") == 0;
        const char* error = "";
        if (updateServiceSave(newUpdateUrl, newUpdateFromDhcp, clear ? "" : line, clear, error)) {
            Serial.println("Update server settings saved");
            printUpdateServerLine();
        } else {
            Serial.println(error);
        }
        memset(lineBuf, 0, sizeof(lineBuf));  // the key was typed here
        state = CLI_MENU;
        printMenu();
        break;
    }

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

/// Drain a running fetchtest and print its result once it ends
static void pollFetchTest() {
    if (!fetchTestActive) return;
    uint8_t buf[128];
    FetchState st = httpFetchPoll(millis());
    if (st == FetchState::Body) {
        size_t n;
        while ((n = httpFetchRead(buf, sizeof(buf))) > 0) fetchTestBytes += n;
        st = httpFetchPoll(millis());
    }
    if (st == FetchState::Done) {
        Serial.printf("\r\nfetchtest: %d %lu bytes in %lu ms\r\n> ", httpFetchStatus(), (unsigned long)fetchTestBytes,
                      (unsigned long)(millis() - fetchTestStartMs));
    } else if (st == FetchState::Failed || st == FetchState::Idle) {
        Serial.printf("\r\nfetchtest: failed: %s after %lu ms\r\n> ", st == FetchState::Idle ? "aborted" : httpFetchError(),
                      (unsigned long)(millis() - fetchTestStartMs));
    } else {
        return;
    }
    fetchTestActive = false;
    httpFetchAbort();
}

void cliBegin() {
    printMenu();
}

void cliProcess() {
    static bool lastWasCR = false;
    pollFetchTest();

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
                Serial.printf("Input too long (max %u characters), ignored\r\n", (unsigned)(sizeof(lineBuf) - 1));
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
                bool secret = state == CLI_WEB_PASSWORD || state == CLI_WEB_REPEAT || state == CLI_UPDATE_KEY;
                Serial.print(secret ? '*' : c);
            } else {
                lineOverflow = true;
            }
        }
    }
}
