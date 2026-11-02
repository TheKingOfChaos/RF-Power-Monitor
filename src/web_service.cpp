#include "web_service.h"

#include <LittleFS.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <mains_input.h>
#include <settings.h>
#include <web_app.h>
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "dhcp6_service.h"
#include "hostname.h"
#include "lldp_service.h"
#include "net_stats.h"
#include "snmp_service.h"
#include "source_key.h"
#include "version.h"

extern CountingW5500lwIP eth;  // main.cpp

static const char DEFAULT_PASSWORD_FILE[] = "data/web_default.txt";
static const uint32_t REBOOT_DELAY_MS = 300;  // let the last response leave before rebooting

static WiFiServer server(80);
static WiFiClient client;
static HttpRequest request;  // 2 KB, static: keep large buffers off core 0's stack
static WebApp app;           // response buffers, static for the same reason
static char defaultPassword[WEB_DEFAULT_PASSWORD_LEN + 1];
static bool started = false;
static Settings settingsBackup;  // restored when a save fails; static: too big for core 0's stack

enum class Conn : uint8_t { Idle, Receiving, Checking, Sending };

// A login with an own password needs WEB_HASH_ITERATIONS SHA-256 rounds (~0.45 s).
// Run them a few at a time (~5 ms per loop pass) so SNMP keeps answering.
static const uint32_t HASH_ROUNDS_PER_POLL = 20;
static WebPasswordCheck passwordCheck;
static Conn conn = Conn::Idle;
static uint32_t connStartMs = 0;
static SourceKey peer;
static const char* out = nullptr;
static size_t outLen = 0, outPos = 0;
static const uint8_t* staticBody = nullptr;  // sent after out (the stylesheet, from flash)
static size_t staticLen = 0;
static bool rebootRequested = false;
static uint32_t rebootAtMs = 0;
static bool listening = false;
static bool stopAfterResponse = false;  // the page itself turned the web interface off

static bool wantListening = false;     // web interface on, port 80 may still be opening
static uint32_t lastListenTryMs = 0;

/// Try to open port 80; webPoll() retries every second until it works
static void tryListening() {
    lastListenTryMs = millis();
    server.begin();
    listening = server.status() == LISTEN;
    if (!listening) Serial.println("\r\nWeb: cannot open TCP port 80 yet, retrying");
}

static void startListening() {
    wantListening = true;
    if (!listening) tryListening();
}

/// Close port 80 and end every session (after the current response, if one is being sent)
static void stopListening() {
    wantListening = false;
    if (listening) server.stop();
    listening = false;
    app.logoutAll();
}

static void formatIP(const IPAddress& ip, char* buf, size_t size) {
    snprintf(buf, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/// The device as the web page sees it
class DeviceBackend : public WebBackend {
public:
    void status(WebStatus& st) override {
        st.version = FIRMWARE_VERSION;
        st.uptimeSeconds = millis() / 1000;
        MainsSnapshot mains;
        st.mainsKnown = mainsRead(mains);
        st.mainsPresent = st.mainsKnown && mains.present;
        st.mainsChanges = st.mainsKnown ? mains.changes : 0;
        st.mainsAlarm = snmpMainsAlarm();
        st.cpuTempDeciC = cpuTempDeciC();
        st.ambientTempValid = ambientTempValid();
        st.ambientTempDeciC = ambientTempDeciC();
        SnmpInterfaceState ifs;
        netStatsCapture(ifs);
        st.linkUp = ifs.linkUp;
        st.linkMbps = ifs.speedBps / 1000000;
        formatIP(eth.connected() ? eth.localIP() : IPAddress(0, 0, 0, 0), st.ip, sizeof(st.ip));
        if (eth.connected()) {
            formatIP(eth.subnetMask(), st.liveMask, sizeof(st.liveMask));
            formatIP(eth.gatewayIP(), st.liveGateway, sizeof(st.liveGateway));
            formatIP(eth.dnsIP(), st.dns, sizeof(st.dns));
        }
#if LWIP_IPV6
        netif* nif = eth.getNetIf();
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
            if (!ip6_addr_isvalid(netif_ip6_addr_state(nif, i)) || dhcp6IsSlot(i)) continue;  // DHCPv6 has its own row
            const ip6_addr_t* a = netif_ip6_addr(nif, i);
            char* dest = ip6_addr_islinklocal(a) ? st.ipv6LinkLocal : st.ipv6;
            if (dest[0] == '\0') ip6addr_ntoa_r(a, dest, sizeof(st.ipv6));
        }
        for (char* c = st.ipv6; *c; c++) *c = (char)tolower(*c);
        for (char* c = st.ipv6LinkLocal; *c; c++) *c = (char)tolower(*c);
        if (st.ipv6[0] == '\0') strlcpy(st.ipv6, st.ipv6LinkLocal, sizeof(st.ipv6));
        Dhcp6Status d6;
        dhcp6GetStatus(d6);
        if (d6.hasAddress) {
            strlcpy(st.ipv6Dhcp, d6.address, sizeof(st.ipv6Dhcp));
            st.ipv6DhcpValidS = d6.validLeftS;
        } else {
            strlcpy(st.ipv6DhcpProblem, d6.problem, sizeof(st.ipv6DhcpProblem));
        }
#endif
        snprintf(st.mac, sizeof(st.mac), "%02X:%02X:%02X:%02X:%02X:%02X", ifs.mac[0], ifs.mac[1], ifs.mac[2],
                 ifs.mac[3], ifs.mac[4], ifs.mac[5]);
        st.dhcp = currentSettings.dhcpEnabled;
        strlcpy(st.staticIP, currentSettings.staticIP, sizeof(st.staticIP));
        strlcpy(st.subnetMask, currentSettings.subnetMask, sizeof(st.subnetMask));
        strlcpy(st.gateway, currentSettings.gateway, sizeof(st.gateway));
        strlcpy(st.staticIPv6, currentSettings.ipv6Address, sizeof(st.staticIPv6));
        strlcpy(st.ipv6Gateway, currentSettings.ipv6Gateway, sizeof(st.ipv6Gateway));
        strlcpy(st.community, currentSettings.community, sizeof(st.community));
        strlcpy(st.hostname, hostnameCurrent(), sizeof(st.hostname));
        lldpServiceSwitchText(st.lldpSwitch, sizeof(st.lldpSwitch));
        lldpServiceSwitchAddress(st.lldpSwitchAddr, sizeof(st.lldpSwitchAddr));
    }

    bool checkPassword(const char* password) override {
        if (currentSettings.webPassword[0] == '\0') return webConstantTimeEquals(password, defaultPassword);
        // webPoll() ran the slow hash for this login in steps before handing it to the app
        if (passwordCheck.done()) return passwordCheck.matches();
        return webCheckPassword(password, currentSettings.webPassword);
    }

    bool saveNetwork(bool dhcp, const char* ip, const char* mask, const char* gateway) override {
        settingsBackup = currentSettings;
        currentSettings.dhcpEnabled = dhcp;
        strlcpy(currentSettings.staticIP, ip, sizeof(currentSettings.staticIP));
        strlcpy(currentSettings.subnetMask, mask, sizeof(currentSettings.subnetMask));
        strlcpy(currentSettings.gateway, gateway, sizeof(currentSettings.gateway));
        if (saveSettings()) {
            Serial.printf("\r\nWeb: network settings saved (%s), rebooting\r\n", dhcp ? "DHCP" : ip);
            return true;
        }
        currentSettings = settingsBackup;
        return false;
    }

    bool saveIPv6(const char* address, const char* gateway) override {
        settingsBackup = currentSettings;
        strlcpy(currentSettings.ipv6Address, address, sizeof(currentSettings.ipv6Address));
        strlcpy(currentSettings.ipv6Gateway, gateway, sizeof(currentSettings.ipv6Gateway));
        if (saveSettings()) {
            Serial.printf("\r\nWeb: static IPv6 saved (%s), rebooting\r\n", address[0] ? address : "none");
            return true;
        }
        currentSettings = settingsBackup;
        return false;
    }

    bool saveCommunity(const char* community) override {
        settingsBackup = currentSettings;
        strlcpy(currentSettings.community, community, sizeof(currentSettings.community));
        if (saveSettings()) {
            snmpCommunityChanged();
            Serial.println("\r\nWeb: SNMP community changed");
            return true;
        }
        currentSettings = settingsBackup;
        return false;
    }

    bool saveHostname(const char* hostname) override {
        settingsBackup = currentSettings;
        strlcpy(currentSettings.sysName, hostname, sizeof(currentSettings.sysName));
        if (saveSettings()) {
            hostnameApply();
            return true;
        }
        currentSettings = settingsBackup;
        return false;
    }

    bool disableWeb() override {
        settingsBackup = currentSettings;
        currentSettings.webEnabled = false;
        if (!saveSettings()) {
            currentSettings = settingsBackup;
            return false;
        }
        Serial.println("\r\nWeb: interface turned off from the web page (serial menu option 8 turns it on)");
        stopAfterResponse = true;  // the confirmation page still goes out
        hostnameAdvertiseWeb(false);
        return true;
    }

    void requestReboot() override { rebootRequested = true; }

    void random(uint8_t* buf, size_t len) override {
        for (size_t i = 0; i < len; i += 4) {
            uint32_t r = rp2040.hwrand32();
            for (size_t j = 0; j < 4 && i + j < len; j++) buf[i + j] = (uint8_t)(r >> (8 * j));
        }
    }

    void log(const SourceKey& source, WebEvent event) override {
        if (event == WebEvent::LoginFailed) {
            accessLogRecord(source, AccessEvent::WebLoginFailed);
        } else if (event == WebEvent::LoginLocked) {
            accessLogRecord(source, AccessEvent::WebLoginLocked);
        } else {
            char text[SOURCE_TEXT_MAX];
            formatSource(source, text);
            Serial.printf("\r\nWeb: admin logged in from %s\r\n", text);
        }
    }
};

static DeviceBackend backend;

/// Read the stored default password, or create and store a new one
/// Read the stored default password into out; false if missing or invalid
static bool readDefaultPassword(char* out) {
    File f = LittleFS.open(DEFAULT_PASSWORD_FILE, "r");
    if (!f) return false;
    size_t n = f.readBytes(out, WEB_DEFAULT_PASSWORD_LEN);
    bool extra = f.available() > 0;
    f.close();
    out[n] = '\0';
    return n == WEB_DEFAULT_PASSWORD_LEN && !extra && webPasswordIsValid(out);
}

/// Generate a new random default password, store it and read it back
static bool generateDefaultPassword() {
    uint8_t random[WEB_DEFAULT_PASSWORD_LEN + 2];
    backend.random(random, sizeof(random));
    webMakeDefaultPassword(random, defaultPassword);
    File f = LittleFS.open(DEFAULT_PASSWORD_FILE, "w");
    bool ok = f && f.write((const uint8_t*)defaultPassword, WEB_DEFAULT_PASSWORD_LEN) == WEB_DEFAULT_PASSWORD_LEN;
    if (f) f.close();
    char check[WEB_DEFAULT_PASSWORD_LEN + 1];
    return ok && readDefaultPassword(check) && strcmp(check, defaultPassword) == 0;
}

static void loadDefaultPassword() {
    if (readDefaultPassword(defaultPassword)) return;
    bool ok = generateDefaultPassword();
    Serial.printf("Web: new default password generated%s\r\n", ok ? "" : " (NOT saved, will change at reboot)");
}

bool webRegenerateDefaultPassword() {
    bool ok = generateDefaultPassword();
    // Browsers logged in with the old default must log in again
    if (webUsesDefaultPassword()) app.logoutAll();
    return ok;
}

void webBegin() {
    loadDefaultPassword();
    started = true;
    hostnameAdvertiseWeb(currentSettings.webEnabled);  // before mDNS starts with the network
    if (currentSettings.webEnabled) {
        startListening();
    } else {
        Serial.println("Web interface off (serial menu option 8 turns it on)");
    }
}

bool webEnabled() { return currentSettings.webEnabled; }

bool webListening() { return listening; }

bool webSetEnabled(bool on) {
    settingsBackup = currentSettings;
    currentSettings.webEnabled = on;
    if (!saveSettings()) {
        currentSettings = settingsBackup;
        return false;
    }
    hostnameAdvertiseWeb(on);
    if (on) {
        stopAfterResponse = false;
        startListening();
    } else if (conn == Conn::Idle) {
        stopListening();
    } else {
        stopAfterResponse = true;  // let the connection in progress finish first
    }
    return true;
}

/// A login while an own (hashed) password is set: check it in steps first
static bool needsHashedCheck() {
    return request.state() == HttpParse::Complete && request.method() == HttpMethod::Post &&
           strcmp(request.path(), "/login") == 0 && currentSettings.webPassword[0] != '\0';
}

/// Build the response to the received request and start sending it
static void respond() {
    out = app.handle(request, peer, millis(), backend, outLen);
    staticBody = app.staticBody(staticLen);
    outPos = 0;
    conn = Conn::Sending;
}

static void closeConnection() {
    client.stop(1);  // do not wait for the last ACK; lwIP still delivers the queued data
    conn = Conn::Idle;
    out = nullptr;
    if (rebootRequested && rebootAtMs == 0) rebootAtMs = millis() + REBOOT_DELAY_MS;
    if (stopAfterResponse) {
        stopAfterResponse = false;
        stopListening();
    }
}

void webPoll() {
    if (!started) return;

    if (rebootRequested && rebootAtMs != 0 && (int32_t)(millis() - rebootAtMs) >= 0) {
        Serial.println("\r\nWeb: rebooting");
        Serial.flush();
        rp2040.reboot();
    }

    if (conn == Conn::Idle) {
        if (!listening) {
            if (wantListening && millis() - lastListenTryMs >= 1000) tryListening();
            return;
        }
        client = server.accept();
        if (!client) return;
        client.setNoDelay(true);
        client.setSync(false);  // writes never wait for ACKs
        client.setTimeout(50);
        request.reset();
        if (!sourceKeyOf(client.remoteIP(), peer)) {
            client.stop(1);  // IPv6 source in ::/32: cannot be a real LAN host
            return;
        }
        connStartMs = millis();
        conn = Conn::Receiving;
    }

    if (conn == Conn::Receiving) {
        uint8_t buf[128];
        while (client.available() > 0 && request.state() == HttpParse::Incomplete) {
            int n = client.read(buf, sizeof(buf));
            if (n <= 0) break;
            request.feed(buf, (size_t)n);
        }
        if (request.state() != HttpParse::Incomplete) {
            if (needsHashedCheck()) {
                char password[WEB_PASSWORD_MAX + 2];
                if (!formValue(request.body(), request.bodyLength(), "password", password, sizeof(password))) {
                    password[0] = '\0';  // missing or too long: checked anyway, fails
                }
                passwordCheck.begin(password, currentSettings.webPassword);
                memset(password, 0, sizeof(password));
                conn = Conn::Checking;
            } else {
                respond();
            }
        } else if (!client.connected() || millis() - connStartMs > WEB_CONNECTION_TIMEOUT_MS) {
            closeConnection();  // gave up or too slow: no answer
            return;
        }
    }

    if (conn == Conn::Checking && passwordCheck.step(HASH_ROUNDS_PER_POLL)) {
        respond();
        passwordCheck.clear();
    }

    if (conn == Conn::Sending) {
        // The response (headers + page), then for the stylesheet its body straight from flash
        size_t total = outLen + staticLen;
        size_t room = (size_t)client.availableForWrite();
        if (room > 0 && outPos < total) {
            const uint8_t* src;
            size_t left;
            if (outPos < outLen) {
                src = (const uint8_t*)out + outPos;
                left = outLen - outPos;
            } else {
                src = staticBody + (outPos - outLen);
                left = total - outPos;
            }
            outPos += client.write(src, left < room ? left : room);
        }
        if (outPos >= total || !client.connected() || millis() - connStartMs > 2 * WEB_CONNECTION_TIMEOUT_MS) {
            closeConnection();
        }
    }
}

const char* webDefaultPassword() { return defaultPassword; }

bool webUsesDefaultPassword() { return currentSettings.webPassword[0] == '\0'; }

bool webSetPassword(const char* password) {
    char hashed[WEB_PASSWORD_HASH_LEN + 1] = "";
    if (password != nullptr) {
        if (!webPasswordIsValid(password)) return false;
        uint8_t salt[WEB_SALT_LEN];
        backend.random(salt, sizeof(salt));
        webHashPassword(password, salt, hashed);
    }
    char saved[sizeof(currentSettings.webPassword)];
    strlcpy(saved, currentSettings.webPassword, sizeof(saved));
    strlcpy(currentSettings.webPassword, hashed, sizeof(currentSettings.webPassword));
    if (!saveSettings()) {
        strlcpy(currentSettings.webPassword, saved, sizeof(currentSettings.webPassword));
        return false;
    }
    app.logoutAll();
    return true;
}
