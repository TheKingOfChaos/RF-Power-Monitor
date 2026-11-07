#include "web_service.h"

#include <LittleFS.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <mains_input.h>
#include <settings.h>
#include <web_app.h>
#include <default_password.h>
#include "ambient_temp.h"
#include "cpu_temp.h"
#include "dhcp6_service.h"
#include "fw_update_service.h"
#include "hostname.h"
#include "lldp_service.h"
#include "net_stats.h"
#include "snmp_service.h"
#include "source_key.h"
#include "update_service.h"
#include "version.h"
#include <update_settings_store.h>

static_assert(WEB_UPDATE_URL_MAX == UPDATE_URL_MAX, "the web form and the setting take the same URL length");

extern CountingW5500lwIP eth;  // main.cpp

// Default password copy A (the file older firmware reads), copy B, and the bench lock
static const char DEFAULT_PASSWORD_FILE[] = "data/web_default.txt";
static const char DEFAULT_PASSWORD_FILE_B[] = "data/web_default_b.txt";
static const char DEFAULT_PASSWORD_LOCK_FILE[] = "data/web_default.lock";
// Written by the boot that lost a locked password; removed when the bench locks again
static const char DEFAULT_PASSWORD_LOST_FILE[] = "data/web_default.lost";
static const uint32_t REBOOT_DELAY_MS = 300;  // let the last response leave before rebooting

static WiFiServer server(80);
static WiFiClient client;
static HttpRequest request;  // 2 KB, static: keep large buffers off core 0's stack
static WebApp app;           // response buffers, static for the same reason
static char defaultPassword[WEB_DEFAULT_PASSWORD_LEN + 1];
static bool passwordLocked = false;  // the lock file exists (default password on the device label)
static bool passwordLost = false;    // locked, but no valid copy was found at boot
static bool started = false;
static Settings settingsBackup;  // restored when a save fails; static: too big for core 0's stack

enum class Conn : uint8_t { Idle, Receiving, Checking, Uploading, Sending };

// Firmware upload (POST /firmware/upload): the body is streamed into the update
// service, never buffered whole; StreamBody ends a silent or endless one
// Reads per webPoll() pass: 4 KB at most, so SNMP keeps answering
static const int UPLOAD_READS_PER_POLL = 8;
static uint8_t uploadBuf[512];  // static: kept off core 0's stack
static StreamBody uploadBody;   // body bytes still to come, timeouts
static bool uploadAccepted = false;  // false once the file was refused: the rest is read and dropped
// Refused before the body (session, CSRF, busy, too large): the body is still read and dropped,
// then this answer goes out. Closing with unread data would reset the connection, and the
// client would never see why
static int refusalCode = 0;  // 0: the update service has the upload
static const char* refusalText = nullptr;

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
        strlcpy(st.sysContact, currentSettings.sysContact, sizeof(st.sysContact));
        strlcpy(st.sysLocation, currentSettings.sysLocation, sizeof(st.sysLocation));
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

    bool saveDeviceName(const char* hostname, const char* contact, const char* location) override {
        settingsBackup = currentSettings;
        strlcpy(currentSettings.sysName, hostname, sizeof(currentSettings.sysName));
        strlcpy(currentSettings.sysContact, contact, sizeof(currentSettings.sysContact));
        strlcpy(currentSettings.sysLocation, location, sizeof(currentSettings.sysLocation));
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

    void firmware(WebFirmware& out) override {
        fwUpdateStatus(out);
        strlcpy(out.updateUrl, currentUpdateSettings.url, sizeof(out.updateUrl));
        out.updateFromDhcp = currentUpdateSettings.fromDhcp;
        out.updateHasKey = currentUpdateSettings.hasKey;
        updateServiceStateText(out.updateState, sizeof(out.updateState));
    }
    bool saveUpdateServer(const char* url, bool fromDhcp, const char* keyHex, bool clearKey,
                          const char*& error) override {
        return updateServiceSave(url, fromDhcp, keyHex, clearKey, error);
    }

    bool switchSlot() override { return fwUpdateSwitchSlot(); }

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
        } else if (event == WebEvent::UploadRefused) {
            accessLogRecord(source, AccessEvent::WebUploadRefused);
        } else {
            char text[SOURCE_TEXT_MAX];
            formatSource(source, text);
            Serial.printf("\r\nWeb: admin logged in from %s\r\n", text);
        }
    }
};

static DeviceBackend backend;

/// One copy of the default password: false if the file is missing; out holds up to
/// WEB_DEFAULT_PASSWORD_LEN + 1 characters, so a longer file does not pass as valid
static bool readPasswordCopy(const char* path, char* out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t n = f.readBytes(out, WEB_DEFAULT_PASSWORD_LEN + 1);
    f.close();
    out[n] = '\0';
    return true;
}

/// Write one copy of defaultPassword and read it back
static bool writePasswordCopy(const char* path) {
    File f = LittleFS.open(path, "w");
    bool ok = f && f.write((const uint8_t*)defaultPassword, WEB_DEFAULT_PASSWORD_LEN) == WEB_DEFAULT_PASSWORD_LEN;
    if (f) f.close();
    char check[WEB_DEFAULT_PASSWORD_LEN + 2];
    return ok && readPasswordCopy(path, check) && strcmp(check, defaultPassword) == 0;
}

/// Write both copies, A first (a valid A is the newer one, see default_password.h)
static bool writePasswordCopies() {
    bool a = writePasswordCopy(DEFAULT_PASSWORD_FILE);
    bool b = writePasswordCopy(DEFAULT_PASSWORD_FILE_B);
    return a && b;
}

/// Generate a new random default password and store both copies
static bool generateDefaultPassword() {
    uint8_t random[WEB_DEFAULT_PASSWORD_LEN + 2];
    backend.random(random, sizeof(random));
    webMakeDefaultPassword(random, defaultPassword);
    return writePasswordCopies();
}

void webLoadDefaultPassword() {
    static char a[WEB_DEFAULT_PASSWORD_LEN + 2], b[WEB_DEFAULT_PASSWORD_LEN + 2];  // static: core 0's stack
    bool haveA = readPasswordCopy(DEFAULT_PASSWORD_FILE, a);
    bool haveB = readPasswordCopy(DEFAULT_PASSWORD_FILE_B, b);
    passwordLocked = LittleFS.exists(DEFAULT_PASSWORD_LOCK_FILE);
    bool lostBefore = LittleFS.exists(DEFAULT_PASSWORD_LOST_FILE);
    DefaultPasswordChoice c = chooseDefaultPassword(haveA ? a : nullptr, haveB ? b : nullptr, passwordLocked, lostBefore);
    passwordLost = c.lostWhileLocked;
    if (passwordLost && !lostBefore) {
        // The new copies will be valid at the next boot: keep the loss reported until the bench relabels
        File f = LittleFS.open(DEFAULT_PASSWORD_LOST_FILE, "w");
        if (f) {
            f.print("lost\n");
            f.close();
        }
    }
    if (passwordLost) Serial.printf("Web: %s\r\n", WEB_PASSWORD_LOST_TEXT);
    if (c.source == DefaultPasswordSource::None) {
        bool ok = generateDefaultPassword();
        Serial.printf("Web: new default password generated%s\r\n", ok ? "" : " (NOT saved, will change at reboot)");
        return;
    }
    strlcpy(defaultPassword, c.source == DefaultPasswordSource::CopyA ? a : b, sizeof(defaultPassword));
    bool ok = (!c.writeA || writePasswordCopy(DEFAULT_PASSWORD_FILE)) &&
              (!c.writeB || writePasswordCopy(DEFAULT_PASSWORD_FILE_B));
    if (!ok) Serial.println("Web: could not repair a copy of the default password");
}

bool webRegenerateDefaultPassword() {
    if (passwordLocked) return false;
    bool ok = generateDefaultPassword();
    // Browsers logged in with the old default must log in again
    if (webUsesDefaultPassword()) app.logoutAll();
    return ok;
}

bool webDefaultPasswordLocked() { return passwordLocked; }

bool webDefaultPasswordLost() { return passwordLost; }

bool webSetDefaultPasswordLocked(bool locked) {
    if (locked) {
        File f = LittleFS.open(DEFAULT_PASSWORD_LOCK_FILE, "w");
        bool ok = f && f.print("locked\n") == 7;
        if (f) f.close();
        if (!ok) LittleFS.remove(DEFAULT_PASSWORD_LOCK_FILE);
    } else if (LittleFS.exists(DEFAULT_PASSWORD_LOCK_FILE)) {
        LittleFS.remove(DEFAULT_PASSWORD_LOCK_FILE);
    }
    passwordLocked = LittleFS.exists(DEFAULT_PASSWORD_LOCK_FILE);
    if (locked && passwordLocked) {
        // The label printed now has the password in use
        LittleFS.remove(DEFAULT_PASSWORD_LOST_FILE);
        passwordLost = false;
    }
    return passwordLocked == locked;
}

void webBegin() {
    request.streamPath("/firmware/upload");  // its body goes to the update service (Conn::Uploading)
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
    if (app.authenticated()) fwUpdateNoteValidRequest();  // reachable: a firmware on trial is kept
    request.reset();  // answered: wipe it now (passwords, the update server key), not at the next connection
}

static void closeConnection();

/// HTTP status for an upload the update service did not accept
static int uploadStatus(FwUploadFault fault) {
    switch (fault) {
    case FwUploadFault::BadFile:     return 400;
    case FwUploadFault::WrongKey:    return 403;
    case FwUploadFault::TooLarge:    return 413;
    case FwUploadFault::DeviceError: return 500;
    case FwUploadFault::Busy:
    case FwUploadFault::None:        break;
    }
    return 409;
}

/// Answer an upload (text/plain) and start sending it; a success reboots after it went out
static void respondUpload(int code, const char* text) {
    out = app.uploadResult(code, text, outLen);
    staticBody = nullptr;
    staticLen = 0;
    outPos = 0;
    connStartMs = millis();  // the send timeout starts now, not when the upload began
    conn = Conn::Sending;
    if (code == 200) rebootRequested = true;  // planned restart into the new slot, on trial
}

/// The head of POST /firmware/upload is in: check the session, then start feeding the update service.
/// A refusal is answered once the body was read (refusalCode)
static void startUpload() {
    uint32_t length = (uint32_t)request.contentLength();
    size_t prefixLen = 0;
    const uint8_t* prefix = request.bodyPrefix(prefixLen);
    if (prefixLen > length) prefixLen = length;
    uploadBody.begin(length - (uint32_t)prefixLen, millis());
    uploadAccepted = false;
    refusalCode = 0;
    conn = Conn::Uploading;

    int code = app.authorizeUpload(request, peer, millis(), backend);
    if (app.authenticated()) fwUpdateNoteValidRequest();
    if (code != 0) {
        refusalCode = code;
        refusalText = code == 401 ? "not logged in" : code == 403 ? "bad CSRF token" : "busy or on trial";
        // Without a login the client gets seconds, not the upload's 5 minutes, to send what it
        // announced: otherwise anyone could hold the single web connection that long
        if (code == 401 || code == 403) uploadBody.drainOnly();
        return;
    }
    const char* error = nullptr;
    FwUploadFault fault;
    if (!fwUpdateUploadBegin(length, error, fault)) {
        refusalCode = uploadStatus(fault);
        refusalText = error;
        return;
    }
    uploadAccepted = prefixLen == 0 || fwUpdateUploadFeed(prefix, prefixLen);
}

/// Read the next part of the upload body; answer when all of it is in, give up on a silent client
static void pollUpload() {
    for (int i = 0; i < UPLOAD_READS_PER_POLL && !uploadBody.done() && client.available() > 0; i++) {
        size_t want = uploadBody.left() < sizeof(uploadBuf) ? uploadBody.left() : sizeof(uploadBuf);
        int n = client.read(uploadBuf, want);  // never more than the buffer it goes into
        if (n <= 0) break;
        uploadBody.consumed((uint32_t)n, millis());
        if (uploadAccepted) uploadAccepted = fwUpdateUploadFeed(uploadBuf, (size_t)n);
    }
    if (uploadBody.done() && refusalCode != 0) {
        respondUpload(refusalCode, refusalText);  // refused before the body: now the client listens
        return;
    }
    if (uploadBody.done()) {
        char version[17];
        const char* error = nullptr;
        FwUploadFault fault;
        if (fwUpdateUploadFinish(version, error, fault)) {
            respondUpload(200, version);
        } else {
            // An image signed with another key is a refusal like a missing session: on the access log
            if (fault == FwUploadFault::WrongKey) accessLogRecord(peer, AccessEvent::WebUploadRefused);
            respondUpload(uploadStatus(fault), error);
        }
        return;
    }
    if ((!client.connected() && client.available() <= 0) || uploadBody.expired(millis())) {
        if (refusalCode == 0) fwUpdateUploadAbort();
        closeConnection();  // the client is gone, silent or too slow: no answer
    }
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
        if (request.state() == HttpParse::Streaming) {
            startUpload();
        } else if (request.state() != HttpParse::Incomplete) {
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

    if (conn == Conn::Uploading) pollUpload();

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
