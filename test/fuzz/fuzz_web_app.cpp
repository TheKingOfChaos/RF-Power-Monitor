/**
 * @file fuzz_web_app.cpp
 * @brief Fuzz the web page (lib/web_ui WebApp): routing, login, forms, CSRF checks and the pages
 *
 * Input:
 *   byte 0  flags: bit 0 log in first, bit 1 saves fail, bit 2 firmware on trial,
 *           bit 3 an upload is running
 *   byte 1  n, then n bytes (at most 143) for the LLDP switch name on the status page:
 *           text from the network, which the page must escape
 *   byte 2  minutes after the login the request comes (sessions expire)
 *   rest    the HTTP request; "@S" becomes the session token and "@C" the CSRF token
 *
 * Checks that every response is one complete text response that fits WEB_RESPONSE_MAX.
 */
#include <string>

#include <web_app.h>

#include "fuzz_common.h"

/// Accepts everything (the firmware's checks are in other libraries), counts what it is asked
class Backend : public WebBackend {
public:
    bool saveOk = true;
    WebStatus st;
    WebFirmware fw;
    uint8_t counter = 0;

    void status(WebStatus& out) override { out = st; }
    void firmware(WebFirmware& out) override { out = fw; }
    bool checkPassword(const char* p) override { return strcmp(p, "goodpasswd1") == 0; }
    bool saveNetwork(bool, const char*, const char*, const char*) override { return saveOk; }
    bool saveIPv6(const char*, const char*) override { return saveOk; }
    bool saveCommunity(const char*) override { return saveOk; }
    bool saveDeviceName(const char*, const char*, const char*) override { return saveOk; }
    bool disableWeb() override { return saveOk; }
    bool saveTimeLog(const char*, const char*, const char*& error) override {
        if (!saveOk) error = "Saving failed, nothing changed.";
        return saveOk;
    }
    bool saveUpdateServer(const char*, bool, const char*, bool, const char*& error) override {
        if (!saveOk) error = "Saving failed, nothing changed.";
        return saveOk;
    }
    void requestReboot() override {}
    bool switchSlot() override { return saveOk; }
    void random(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(counter * 31 + i);
        counter++;
    }
    void log(const SourceKey&, WebEvent) override {}
};

static void setUpBackend(Backend& b) {
    memset(&b.st, 0, sizeof(b.st));
    memset(&b.fw, 0, sizeof(b.fw));
    b.st.version = "3.6.1";
    b.st.uptimeSeconds = 90061;
    b.st.mainsKnown = true;
    b.st.mainsPresent = true;
    b.st.ambientTempValid = true;
    b.st.ambientTempDeciC = -55;
    b.st.linkUp = true;
    b.st.linkMbps = 100;
    strcpy(b.st.ip, "10.132.0.77");
    strcpy(b.st.ipv6, "fd16:d833:4f0e:3ece:1cb2:cfff:fe4e:a22c");
    strcpy(b.st.mac, "1E:B2:CF:4E:A2:2C");
    b.st.dhcp = true;
    strcpy(b.st.community, "public");
    strcpy(b.st.hostname, "rf-power-monitor");
    strcpy(b.st.sysContact, "\"ops\" <ops@example.com>");
    strcpy(b.st.sysLocation, "Rack 4 & 5");
    strcpy(b.st.time, "not synced");
    strcpy(b.st.syslog, "off");
    strcpy(b.st.ntpSetting, "dhcp");
    strcpy(b.st.syslogSetting, "none");
    strcpy(b.fw.runningVersion, "3.6.1");
    strcpy(b.fw.otherVersion, "3.6.0");
    strcpy(b.fw.keyFingerprint, "0123456789abcdef");
    strcpy(b.fw.updateUrl, "http://10.132.0.45:8074");
    strcpy(b.fw.updateState, "off");
}

static std::string between(const std::string& s, const char* start, const char* end) {
    size_t a = s.find(start);
    if (a == std::string::npos) return "";
    a += strlen(start);
    size_t b = s.find(end, a);
    return b == std::string::npos ? "" : s.substr(a, b - a);
}

/// Parse raw as one request and answer it; checks the response
static std::string send(WebApp& app, Backend& b, HttpRequest& req, const std::string& raw, uint32_t now) {
    req.reset();
    req.streamPath("/firmware/upload");
    HttpParse st = req.feed((const uint8_t*)raw.data(), raw.size());
    if (st == HttpParse::Incomplete) return "";  // the firmware waits for more bytes
    size_t len = 0;
    const char* resp;
    if (st == HttpParse::Streaming) {
        int code = app.authorizeUpload(req, 0x0A840045, now, b);
        if (code != 0 && (code < 400 || code > 599)) __builtin_trap();
        resp = app.uploadResult(code == 0 ? 200 : code, code == 0 ? "3.6.2" : "refused", len);
    } else {
        resp = app.handle(req, 0x0A840045, now, b, len);
    }
    if (resp == nullptr || len > WEB_RESPONSE_MAX || strlen(resp) != len) __builtin_trap();
    if (strncmp(resp, "HTTP/1.1 ", 9) != 0) __builtin_trap();
    return std::string(resp, len);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static HttpRequest* req = new HttpRequest();
    static Backend* b = new Backend();
    setUpBackend(*b);
    WebApp* app = new WebApp();  // fresh sessions and login throttle for each input

    FuzzInput in(data, size);
    uint8_t flags = in.byte();
    b->saveOk = !(flags & 2);
    b->fw.trial = (flags & 4) != 0;
    b->fw.uploading = (flags & 8) != 0;
    size_t nameLen;
    uint8_t want = in.byte();
    const uint8_t* name = in.take(want < sizeof(b->st.lldpSwitch) ? want : sizeof(b->st.lldpSwitch) - 1, nameLen);
    if (nameLen > 0) {
        memcpy(b->st.lldpSwitch, name, nameLen);
        b->st.lldpSwitch[nameLen] = '\0';  // a NUL inside just ends it early, as strcpy would
    } else {
        strcpy(b->st.lldpSwitch, "none seen");
    }
    uint32_t now = 1000 + (uint32_t)in.byte() * 60000u;

    std::string token, csrf;
    if (flags & 1) {
        std::string body = "user=admin&password=goodpasswd1";
        std::string resp = send(*app, *b, *req,
                                "POST /login HTTP/1.1\r\nHost: d\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body,
                                1000);
        token = between(resp, "Set-Cookie: sid=", ";");
        std::string page = send(*app, *b, *req, "GET / HTTP/1.1\r\nHost: d\r\nCookie: sid=" + token + "\r\n\r\n", 1000);
        csrf = between(page, "name=\"csrf\" value=\"", "\"");
    }

    size_t len;
    const uint8_t* rest = in.rest(len);
    std::string raw;
    raw.reserve(len + 128);
    for (size_t i = 0; i < len; i++) {
        if (rest[i] == '@' && i + 1 < len && (rest[i + 1] == 'S' || rest[i + 1] == 'C')) {
            raw += rest[i + 1] == 'S' ? token : csrf;
            i++;
        } else {
            raw += (char)rest[i];
        }
    }
    send(*app, *b, *req, raw, now);
    delete app;
    return 0;
}
