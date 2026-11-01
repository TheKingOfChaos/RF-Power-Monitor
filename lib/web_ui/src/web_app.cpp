#include "web_app.h"

#include <string.h>
#include <netconfig.h>
#include <settings_codec.h>
#include "bootstrap_css.h"
#include "logo_svg.h"

// Headers on every page. The pages have no scripts at all; the policy keeps it that way.
// Bootstrap's form controls use small data: SVG images, the favicon is our own: img-src 'self' data:.
static const char COMMON_HEADERS[] =
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\n"
    "X-Frame-Options: DENY\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
    "form-action 'self'; frame-ancestors 'none'\r\n"
    "Connection: close\r\n";

static void pageStart(TextBuffer& b, const char* title) {
    b.add("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    b.addEscaped(title);
    b.add("</title><link rel=\"icon\" type=\"image/svg+xml\" href=\"" LOGO_FAVICON_PATH "\">"
          "<link rel=\"stylesheet\" href=\"" BOOTSTRAP_CSS_PATH "\">"
          "<style>body{background:#f3f5f8}.container{max-width:1000px}</style></head><body>"
          "<nav class=\"navbar navbar-dark bg-dark mb-4\"><div class=\"container\">"
          "<span class=\"navbar-brand fw-semibold d-flex align-items-center\">"
          "<svg class=\"me-2\" width=\"44\" height=\"28\" viewBox=\"" LOGO_VIEWBOX "\" aria-hidden=\"true\">"
          "<path fill=\"" LOGO_COLOR "\" d=\"" LOGO_PATH "\"/></svg>RF-Power-Monitor</span>");
}

/// Close the navbar and open the page container
static void navEnd(TextBuffer& b) { b.add("</div></nav><main class=\"container pb-5\">"); }

static void pageEnd(TextBuffer& b) { b.add("</main></body></html>"); }

static void message(TextBuffer& b, const char* text, bool error) {
    if (text == nullptr || text[0] == '\0') return;
    b.add(error ? "<div class=\"alert alert-danger\" role=\"alert\">" : "<div class=\"alert alert-success\" role=\"alert\">");
    b.addEscaped(text);
    b.add("</div>");
}

static void csrfField(TextBuffer& b, const char* csrf) {
    b.add("<input type=\"hidden\" name=\"csrf\" value=\"");
    b.addEscaped(csrf);
    b.add("\">");
}

static void row(TextBuffer& b, const char* name, const char* value) {
    b.add("<tr><th scope=\"row\" class=\"ps-3 fw-normal text-secondary\">");
    b.add(name);
    b.add("</th><td>");
    b.addEscaped(value);
    b.add("</td></tr>");
}

/// A status row whose value is a coloured badge (style: success, danger, secondary)
static void badgeRow(TextBuffer& b, const char* name, const char* value, const char* style) {
    b.add("<tr><th scope=\"row\" class=\"ps-3 fw-normal text-secondary\">");
    b.add(name);
    b.add("</th><td><span class=\"badge text-bg-");
    b.add(style);
    b.add("\">");
    b.addEscaped(value);
    b.add("</span></td></tr>");
}

/// A status row with a temperature in tenths of a degree, e.g. "-0.5 °C"
static void tempRow(TextBuffer& b, const char* name, int32_t deciC) {
    char num[16];
    TextBuffer nb(num, sizeof(num));
    if (deciC < 0) nb.add("-");
    uint32_t a = (uint32_t)(deciC < 0 ? -deciC : deciC);
    nb.addUnsigned(a / 10);
    nb.add(".");
    nb.addUnsigned(a % 10);
    nb.add(" \xC2\xB0" "C");
    row(b, name, num);
}

static void textInput(TextBuffer& b, const char* label, const char* name, const char* value, size_t maxLen,
                      const char* help = nullptr) {
    b.add("<div class=\"mb-3\"><label class=\"form-label\" for=\"f-");
    b.add(name);
    b.add("\">");
    b.add(label);
    b.add("</label><input class=\"form-control\" type=\"text\" id=\"f-");
    b.add(name);
    b.add("\" name=\"");
    b.add(name);
    b.add("\" maxlength=\"");
    b.addUnsigned((uint32_t)maxLen);
    b.add("\" value=\"");
    b.addEscaped(value);
    b.add("\">");
    if (help) {
        b.add("<div class=\"form-text\">");
        b.add(help);
        b.add("</div>");
    }
    b.add("</div>");
}

static void cardStart(TextBuffer& b, const char* title, const char* extraClass = "") {
    b.add("<div class=\"card shadow-sm ");
    b.add(extraClass);
    b.add("\"><div class=\"card-header fw-semibold\">");
    b.add(title);
    b.add("</div>");
}

const char* WebApp::respond(int code, const char* reason, const char* extraHeaders, size_t& len) {
    TextBuffer out(_response, sizeof(_response));
    out.add("HTTP/1.1 ");
    out.addUnsigned((uint32_t)code);
    out.add(" ");
    out.add(reason);
    out.add("\r\n");
    out.add(COMMON_HEADERS);
    if (extraHeaders) out.add(extraHeaders);
    out.add("Content-Length: ");
    size_t bodyLen = strlen(_body);
    out.addUnsigned((uint32_t)bodyLen);
    out.add("\r\n\r\n");
    out.add(_body, bodyLen);
    if (!out.ok()) {
        // Cannot happen with the fixed pages; answer something valid anyway
        static const char FAIL[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        len = sizeof(FAIL) - 1;
        return FAIL;
    }
    len = out.length();
    return _response;
}

const char* WebApp::stylesheet(size_t& len) {
    // Served from flash as it is: headers here, the gzip body via staticBody()
    TextBuffer out(_response, sizeof(_response));
    out.add("HTTP/1.1 200 OK\r\n"
            "Content-Type: text/css; charset=utf-8\r\n"
            "Content-Encoding: gzip\r\n"
            "Cache-Control: public, max-age=31536000, immutable\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Connection: close\r\n"
            "Content-Length: ");
    out.addUnsigned((uint32_t)BOOTSTRAP_CSS_GZ_LEN);
    out.add("\r\n\r\n");
    _staticBody = BOOTSTRAP_CSS_GZ;
    _staticLen = BOOTSTRAP_CSS_GZ_LEN;
    len = out.length();
    return _response;
}

const char* WebApp::loginPage(int code, const char* reason, const char* text, size_t& len) {
    TextBuffer b(_body, sizeof(_body));
    pageStart(b, "RF-Power-Monitor login");
    navEnd(b);
    b.add("<div class=\"row justify-content-center\"><div class=\"col-sm-9 col-md-6 col-lg-4\">"
          "<div class=\"card shadow-sm\"><div class=\"card-body p-4\">"
          "<div class=\"text-center mb-3\"><svg width=\"160\" height=\"101\" viewBox=\"" LOGO_VIEWBOX "\" "
          "role=\"img\" aria-label=\"RF logo\"><path fill=\"" LOGO_COLOR "\" d=\"" LOGO_PATH "\"/></svg></div>"
          "<h1 class=\"h4 mb-3\">Log in</h1>");
    message(b, text, true);
    b.add("<form method=\"post\" action=\"/login\">"
          "<div class=\"mb-3\"><label class=\"form-label\" for=\"f-user\">User</label>"
          "<input class=\"form-control\" type=\"text\" id=\"f-user\" name=\"user\" value=\"admin\" maxlength=\"16\" "
          "autocomplete=\"username\"></div>"
          "<div class=\"mb-3\"><label class=\"form-label\" for=\"f-password\">Password</label>"
          "<input class=\"form-control\" type=\"password\" id=\"f-password\" name=\"password\" maxlength=\"32\" "
          "autofocus autocomplete=\"current-password\"></div>"
          "<button class=\"btn btn-primary w-100\" type=\"submit\">Log in</button></form></div></div>"
          "<p class=\"text-secondary small mt-3\">The default password is shown on the serial status screen "
          "(menu option 1); set your own with menu option 6.</p></div></div>");
    pageEnd(b);
    return respond(code, reason, nullptr, len);
}

const char* WebApp::notice(int code, const char* reason, const char* title, const char* text, size_t& len) {
    TextBuffer b(_body, sizeof(_body));
    pageStart(b, title);
    navEnd(b);
    b.add("<div class=\"row justify-content-center\"><div class=\"col-md-8 col-lg-6\"><div class=\"card shadow-sm\">"
          "<div class=\"card-body p-4\"><h1 class=\"h4\">");
    b.addEscaped(title);
    b.add("</h1><p>");
    b.addEscaped(text);
    b.add("</p><a class=\"btn btn-outline-primary\" href=\"/\">Back to the start page</a></div></div></div></div>");
    pageEnd(b);
    return respond(code, reason, nullptr, len);
}

const char* WebApp::redirect(const char* extraHeaders, size_t& len) {
    _body[0] = '\0';  // a redirect has no body
    char headers[160];
    TextBuffer hb(headers, sizeof(headers));
    hb.add("Location: /\r\n");
    if (extraHeaders) hb.add(extraHeaders);
    return respond(303, "See Other", headers, len);
}

const char* WebApp::mainPage(int code, const char* reason, size_t slot, WebBackend& backend, const char* text,
                             bool error, size_t& len) {
    WebStatus& st = _status;
    memset(&st, 0, sizeof(st));
    backend.status(st);
    const char* csrf = _sessions.csrf(slot);

    TextBuffer b(_body, sizeof(_body));
    pageStart(b, "RF-Power-Monitor");
    b.add("<div class=\"d-flex align-items-center gap-3\"><span class=\"navbar-text\">");
    b.addEscaped(st.hostname);
    b.add(" &middot; v");
    b.addEscaped(st.version ? st.version : "");
    b.add("</span><form method=\"post\" action=\"/logout\" class=\"m-0\">");
    csrfField(b, csrf);
    b.add("<button class=\"btn btn-outline-light btn-sm\" type=\"submit\">Log out</button></form></div>");
    navEnd(b);
    message(b, text, error);

    // Left column: status and restart
    char num[48];
    b.add("<div class=\"row g-4\"><div class=\"col-lg-5\">");
    cardStart(b, "Status");
    b.add("<table class=\"table table-sm mb-0\"><tbody>");
    if (!st.mainsKnown) {
        badgeRow(b, "Mains power", "unknown", "secondary");
    } else {
        badgeRow(b, "Mains power", st.mainsPresent ? "present" : "LOST", st.mainsPresent ? "success" : "danger");
        badgeRow(b, "Mains alarm", st.mainsAlarm ? "lost over 60 s" : "none", st.mainsAlarm ? "danger" : "success");
    }
    TextBuffer nb(num, sizeof(num));
    nb.addUnsigned(st.mainsChanges);
    row(b, "Mains changes since boot", num);
    nb = TextBuffer(num, sizeof(num));
    uint32_t up = st.uptimeSeconds;
    nb.addUnsigned(up / 86400);
    nb.add(" d ");
    nb.addUnsigned(up / 3600 % 24);
    nb.add(" h ");
    nb.addUnsigned(up / 60 % 60);
    nb.add(" min");
    row(b, "Uptime", num);
    tempRow(b, "CPU temperature", st.cpuTempDeciC);
    if (st.ambientTempValid) {
        tempRow(b, "Ambient temperature", st.ambientTempDeciC);
    } else {
        badgeRow(b, "Ambient temperature", "no sensor", "secondary");
    }
    nb = TextBuffer(num, sizeof(num));
    if (st.linkUp) {
        nb.add("up, ");
        nb.addUnsigned(st.linkMbps);
        nb.add(" Mbit/s");
    }
    badgeRow(b, "Ethernet link", st.linkUp ? num : "down", st.linkUp ? "success" : "danger");
    {
        char sw[sizeof(st.lldpSwitch) + sizeof(st.lldpSwitchAddr) + 3];  // text + " (address)"
        TextBuffer sb(sw, sizeof(sw));
        sb.add(st.lldpSwitch);
        if (st.lldpSwitchAddr[0]) {
            sb.add(" (");
            sb.add(st.lldpSwitchAddr);
            sb.add(")");
        }
        row(b, "Switch", sw);  // text from the network: row() escapes it
    }
    row(b, "Hostname", st.hostname);
    nb = TextBuffer(num, sizeof(num));
    nb.add(st.hostname);
    nb.add(".local");
    row(b, "mDNS name", num);
    row(b, "IP address", st.ip);
    if (st.ipv6[0]) row(b, "IPv6 address", st.ipv6);
    if (st.ipv6LinkLocal[0] && strcmp(st.ipv6LinkLocal, st.ipv6) != 0) row(b, "IPv6 link-local", st.ipv6LinkLocal);
    if (st.ipv6Dhcp[0]) {
        char line[IPV6_TEXT_MAX + 24];  // address + " (valid 4294967294 s)"
        TextBuffer lb(line, sizeof(line));
        lb.add(st.ipv6Dhcp);
        if (st.ipv6DhcpValidS == 0xFFFFFFFFu) {
            lb.add(" (valid forever)");
        } else {
            lb.add(" (valid ");
            lb.addUnsigned(st.ipv6DhcpValidS);
            lb.add(" s)");
        }
        row(b, "IPv6 (DHCPv6)", line);
    } else if (st.ipv6DhcpProblem[0]) {
        row(b, "IPv6 (DHCPv6)", st.ipv6DhcpProblem);
    }
    row(b, "MAC address", st.mac);
    row(b, "Network mode", st.dhcp ? "DHCP" : "static");
    if (st.staticIPv6[0]) row(b, "Static IPv6", st.staticIPv6);
    b.add("</tbody></table></div>");

    b.add("<div class=\"card shadow-sm mt-4\"><div class=\"card-body d-flex justify-content-between "
          "align-items-center\"><span>Restart the device</span>"
          "<form method=\"post\" action=\"/reboot\" class=\"m-0\">");
    csrfField(b, csrf);
    b.add("<button class=\"btn btn-outline-danger btn-sm\" type=\"submit\">Restart device</button></form></div></div>");

    b.add("<div class=\"card shadow-sm mt-4\"><div class=\"card-body d-flex justify-content-between "
          "align-items-center gap-3\"><span>Web interface<br><span class=\"form-text\">Off closes this page; "
          "serial menu option 8 turns it on again.</span></span>"
          "<form method=\"post\" action=\"/disable\" class=\"m-0\">");
    csrfField(b, csrf);
    b.add("<button class=\"btn btn-outline-secondary btn-sm text-nowrap\" type=\"submit\">Turn off&hellip;</button>"
          "</form></div></div>");

    // Right column: settings
    b.add("</div><div class=\"col-lg-7\">");
    cardStart(b, "Network");
    b.add("<div class=\"card-body\"><form method=\"post\" action=\"/network\">");
    csrfField(b, csrf);
    b.add("<div class=\"mb-3\"><div class=\"form-check form-check-inline\"><input class=\"form-check-input\" "
          "type=\"radio\" name=\"mode\" id=\"m-dhcp\" value=\"dhcp\"");
    if (st.dhcp) b.add(" checked");
    b.add("><label class=\"form-check-label\" for=\"m-dhcp\">DHCP</label></div>"
          "<div class=\"form-check form-check-inline\"><input class=\"form-check-input\" type=\"radio\" "
          "name=\"mode\" id=\"m-static\" value=\"static\"");
    if (!st.dhcp) b.add(" checked");
    b.add("><label class=\"form-check-label\" for=\"m-static\">Static</label></div></div>"
          "<div class=\"row g-2\"><div class=\"col-sm-4\">");
    // With DHCP the boxes show the address in use. Saving with DHCP keeps the stored static values
    // (handleNetwork); choosing Static saves what the boxes hold, so the DHCP address is a starting point
    bool fromDhcp = st.dhcp && st.liveMask[0];
    textInput(b, "IP address", "ip", fromDhcp ? st.ip : st.staticIP, 15);
    b.add("</div><div class=\"col-sm-4\">");
    textInput(b, "Subnet mask", "mask", fromDhcp ? st.liveMask : st.subnetMask, 15);
    b.add("</div><div class=\"col-sm-4\">");
    textInput(b, "Gateway", "gateway", fromDhcp ? st.liveGateway : st.gateway, 15);
    b.add("</div></div>");
    if (fromDhcp) {
        b.add("<p class=\"form-text mt-0\">From DHCP");
        if (st.dns[0]) {
            b.add(", DNS server ");
            b.addEscaped(st.dns);
        }
        b.add(". Choose Static and save to keep this address.</p>");
    }
    b.add("<button class=\"btn btn-primary\" type=\"submit\">Save and restart</button>"
          "<span class=\"form-text ms-2\">Saving restarts the device.</span></form></div></div>");

    cardStart(b, "IPv6", "mt-4");
    b.add("<div class=\"card-body\"><form method=\"post\" action=\"/ipv6\">");
    csrfField(b, csrf);
    b.add("<div class=\"row g-2\"><div class=\"col-sm-7\">");
    textInput(b, "Static IPv6 address", "ipv6", st.staticIPv6, IPV6_TEXT_MAX - 1,
              "Always /64, e.g. fd00::10. Empty: none. Added next to the automatic (SLAAC) address.");
    b.add("</div><div class=\"col-sm-5\">");
    textInput(b, "IPv6 gateway", "ipv6gw", st.ipv6Gateway, IPV6_TEXT_MAX - 1,
              "Optional; empty: routers that announce themselves.");
    b.add("</div></div><button class=\"btn btn-primary\" type=\"submit\">Save and restart</button>"
          "<span class=\"form-text ms-2\">Saving restarts the device.</span></form></div></div>");

    cardStart(b, "Device name", "mt-4");
    b.add("<div class=\"card-body\"><form method=\"post\" action=\"/hostname\">");
    csrfField(b, csrf);
    textInput(b, "Hostname", "hostname", st.hostname, HOSTNAME_MAX_LENGTH,
              "Also the SNMP sysName and the mDNS name (name.local). 1-32 letters, digits and hyphens. "
              "Active immediately.");
    b.add("<button class=\"btn btn-primary\" type=\"submit\">Save</button></form></div></div>");

    cardStart(b, "SNMP", "mt-4");
    b.add("<div class=\"card-body\"><form method=\"post\" action=\"/community\">");
    csrfField(b, csrf);
    textInput(b, "Community", "community", st.community, COMMUNITY_MAX_LENGTH,
              "1-31 printable characters. Active immediately.");
    b.add("<button class=\"btn btn-primary\" type=\"submit\">Save</button></form></div></div>");

    b.add("</div></div>");
    pageEnd(b);
    return respond(code, reason, nullptr, len);
}

const char* WebApp::login(const HttpRequest& req, const SourceKey& sourceIP, uint32_t nowMs, WebBackend& backend, size_t& len) {
    if (!_throttle.allowed(nowMs)) {
        backend.log(sourceIP, WebEvent::LoginLocked);
        char text[80];
        TextBuffer t(text, sizeof(text));
        t.add("Too many failed logins. Try again in ");
        t.addUnsigned(_throttle.secondsLeft(nowMs));
        t.add(" s.");
        return loginPage(429, "Too Many Requests", text, len);
    }
    char user[17], password[WEB_PASSWORD_MAX + 2];
    bool haveUser = formValue(req.body(), req.bodyLength(), "user", user, sizeof(user));
    bool havePassword = formValue(req.body(), req.bodyLength(), "password", password, sizeof(password));
    // Always check the password, so a wrong user name takes as long as a wrong password
    bool passwordOk = havePassword && backend.checkPassword(password);
    memset(password, 0, sizeof(password));
    if (haveUser && webConstantTimeEquals(user, WEB_USER) && passwordOk) {
        _throttle.succeeded();
        uint8_t random[WEB_SESSION_RANDOM];
        backend.random(random, sizeof(random));
        size_t slot = _sessions.create(random, nowMs);
        memset(random, 0, sizeof(random));
        backend.log(sourceIP, WebEvent::LoggedIn);
        char cookie[128];
        TextBuffer c(cookie, sizeof(cookie));
        c.add("Set-Cookie: sid=");
        c.add(_sessions.token(slot));
        c.add("; Path=/; HttpOnly; SameSite=Strict\r\n");
        return redirect(cookie, len);
    }
    _throttle.failed(nowMs);
    backend.log(sourceIP, WebEvent::LoginFailed);
    return loginPage(401, "Unauthorized", "Wrong user name or password.", len);
}

const char* WebApp::network(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len) {
    char mode[8];
    if (!formValue(req.body(), req.bodyLength(), "mode", mode, sizeof(mode)) ||
        (strcmp(mode, "dhcp") != 0 && strcmp(mode, "static") != 0)) {
        return mainPage(400, "Bad Request", slot, backend, "Choose DHCP or static.", true, len);
    }
    bool ok;
    char ip[16], mask[16], gateway[16];
    if (strcmp(mode, "dhcp") == 0) {
        // Keep the stored static values for a later switch back
        WebStatus& st = _status;
        memset(&st, 0, sizeof(st));
        backend.status(st);
        ok = backend.saveNetwork(true, st.staticIP, st.subnetMask, st.gateway);
        strcpy(ip, "");
    } else {
        if (!formValue(req.body(), req.bodyLength(), "ip", ip, sizeof(ip)) ||
            !formValue(req.body(), req.bodyLength(), "mask", mask, sizeof(mask)) ||
            !formValue(req.body(), req.bodyLength(), "gateway", gateway, sizeof(gateway))) {
            return mainPage(400, "Bad Request", slot, backend, "Enter IPv4 addresses such as 192.168.1.10.", true, len);
        }
        StaticConfigError err = validateStaticConfig(ip, mask, gateway);
        if (err != StaticConfigError::None) {
            return mainPage(400, "Bad Request", slot, backend, staticConfigErrorMessage(err), true, len);
        }
        ok = backend.saveNetwork(false, ip, mask, gateway);
    }
    if (!ok) return mainPage(500, "Internal Server Error", slot, backend, "Saving failed, nothing changed.", true, len);
    backend.requestReboot();
    char text[160];
    TextBuffer t(text, sizeof(text));
    if (ip[0]) {
        t.add("The settings are saved and the device restarts now. It will be reachable at ");
        t.add(ip);
        t.add(" in a few seconds; log in again there.");
    } else {
        t.add("The settings are saved and the device restarts now with DHCP. Find its new address in your "
              "DHCP server or on the serial status screen.");
    }
    return notice(200, "OK", "Restarting", text, len);
}

const char* WebApp::ipv6(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len) {
    char address[IPV6_TEXT_MAX + 1], gateway[IPV6_TEXT_MAX + 1];
    if (!formValue(req.body(), req.bodyLength(), "ipv6", address, sizeof(address)) ||
        !formValue(req.body(), req.bodyLength(), "ipv6gw", gateway, sizeof(gateway))) {
        return mainPage(400, "Bad Request", slot, backend, "Enter IPv6 addresses such as fd00::10.", true, len);
    }
    StaticIPv6Error err = validateStaticIPv6(address, gateway);
    if (err != StaticIPv6Error::None) {
        return mainPage(400, "Bad Request", slot, backend, staticIPv6ErrorMessage(err), true, len);
    }
    // Store the canonical form, so "FD00:0::10" and "fd00::10" are the same setting
    uint8_t bytes[16];
    if (address[0] && parseIPv6(address, bytes)) formatIPv6(bytes, address);
    if (gateway[0] && parseIPv6(gateway, bytes)) formatIPv6(bytes, gateway);
    if (!backend.saveIPv6(address, gateway)) {
        return mainPage(500, "Internal Server Error", slot, backend, "Saving failed, nothing changed.", true, len);
    }
    backend.requestReboot();
    char text[160];
    TextBuffer t(text, sizeof(text));
    if (address[0]) {
        t.add("The settings are saved and the device restarts now. It will also be reachable at [");
        t.add(address);
        t.add("] in a few seconds.");
    } else {
        t.add("The static IPv6 address is removed and the device restarts now.");
    }
    return notice(200, "OK", "Restarting", text, len);
}

const char* WebApp::community(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len) {
    char value[COMMUNITY_MAX_LENGTH + 2];
    if (!formValue(req.body(), req.bodyLength(), "community", value, sizeof(value)) ||
        !settingsTextIsValid(value, COMMUNITY_MAX_LENGTH, false)) {
        return mainPage(400, "Bad Request", slot, backend,
                        "The community must be 1-31 printable characters.", true, len);
    }
    if (!backend.saveCommunity(value)) {
        return mainPage(500, "Internal Server Error", slot, backend, "Saving failed, nothing changed.", true, len);
    }
    return mainPage(200, "OK", slot, backend, "SNMP community saved and active.", false, len);
}

const char* WebApp::hostname(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len) {
    char value[HOSTNAME_MAX_LENGTH + 2];
    if (!formValue(req.body(), req.bodyLength(), "hostname", value, sizeof(value)) || !hostnameIsValid(value)) {
        return mainPage(400, "Bad Request", slot, backend,
                        "The hostname must be 1-32 letters, digits and '-', not starting or ending with '-'.", true, len);
    }
    if (!backend.saveHostname(value)) {
        return mainPage(500, "Internal Server Error", slot, backend, "Saving failed, nothing changed.", true, len);
    }
    return mainPage(200, "OK", slot, backend, "Hostname saved and active.", false, len);
}

const char* WebApp::disable(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len) {
    char confirm[8];
    if (!formValue(req.body(), req.bodyLength(), "confirm", confirm, sizeof(confirm)) || strcmp(confirm, "yes") != 0) {
        // First step: say what happens and ask again
        TextBuffer b(_body, sizeof(_body));
        pageStart(b, "Turn off the web interface");
        navEnd(b);
        b.add("<div class=\"row justify-content-center\"><div class=\"col-md-8 col-lg-6\"><div class=\"card "
              "shadow-sm border-warning\"><div class=\"card-body p-4\"><h1 class=\"h4\">Turn off the web interface?"
              "</h1><p>The device stops serving this page (TCP port 80 closes) and every browser is logged out. "
              "SNMP keeps working. The web interface can only be turned on again from the serial menu "
              "(USB, option 8).</p><form method=\"post\" action=\"/disable\" class=\"d-flex gap-2\">");
        csrfField(b, _sessions.csrf(slot));
        b.add("<input type=\"hidden\" name=\"confirm\" value=\"yes\">"
              "<button class=\"btn btn-danger\" type=\"submit\">Turn off</button>"
              "<a class=\"btn btn-outline-secondary\" href=\"/\">Cancel</a></form></div></div></div></div>");
        pageEnd(b);
        return respond(200, "OK", nullptr, len);
    }
    if (!backend.disableWeb()) {
        return mainPage(500, "Internal Server Error", slot, backend, "Saving failed, nothing changed.", true, len);
    }
    _sessions.clear();
    return notice(200, "OK", "Web interface off",
                  "The web interface is now off. Turn it on again with serial menu option 8.", len);
}

const char* WebApp::favicon(size_t& len) {
    static const char SVG[] = LOGO_SVG_FILE;
    TextBuffer out(_response, sizeof(_response));
    out.add("HTTP/1.1 200 OK\r\n"
            "Content-Type: image/svg+xml\r\n"
            "Cache-Control: public, max-age=86400\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n"
            "Connection: close\r\n"
            "Content-Length: ");
    out.addUnsigned((uint32_t)(sizeof(SVG) - 1));
    out.add("\r\n\r\n");
    out.add(SVG);
    len = out.length();
    return _response;
}

const char* WebApp::handle(const HttpRequest& req, const SourceKey& sourceIP, uint32_t nowMs, WebBackend& backend,
                           size_t& len) {
    _staticBody = nullptr;
    _staticLen = 0;
    if (req.state() == HttpParse::TooLarge) {
        return notice(413, "Payload Too Large", "Request too large", "The request was too large.", len);
    }
    if (req.state() != HttpParse::Complete) {
        return notice(400, "Bad Request", "Bad request", "The request could not be understood.", len);
    }

    int slot = -1;
    char token[WEB_TOKEN_HEX + 2];
    if (req.header("Cookie", _cookies, sizeof(_cookies)) && cookieValue(_cookies, "sid", token, sizeof(token))) {
        slot = _sessions.find(token, nowMs);
    }

    const char* path = req.path();
    bool get = req.method() == HttpMethod::Get, post = req.method() == HttpMethod::Post;

    if (strcmp(path, BOOTSTRAP_CSS_PATH) == 0) {
        if (!get) return notice(405, "Method Not Allowed", "Not allowed", "Use GET for this file.", len);
        return stylesheet(len);  // no login needed: public CSS
    }
    if (strcmp(path, LOGO_FAVICON_PATH) == 0) {
        if (!get) return notice(405, "Method Not Allowed", "Not allowed", "Use GET for this file.", len);
        return favicon(len);  // no login needed: the logo
    }

    if (strcmp(path, "/") == 0) {
        if (!get) return notice(405, "Method Not Allowed", "Not allowed", "Use GET for this page.", len);
        if (slot < 0) return loginPage(200, "OK", nullptr, len);
        return mainPage(200, "OK", (size_t)slot, backend, nullptr, false, len);
    }
    if (strcmp(path, "/login") == 0) {
        if (get) return redirect(nullptr, len);
        if (!post) return notice(405, "Method Not Allowed", "Not allowed", "Use POST for this page.", len);
        return login(req, sourceIP, nowMs, backend, len);
    }

    bool action = strcmp(path, "/logout") == 0 || strcmp(path, "/network") == 0 ||
                  strcmp(path, "/community") == 0 || strcmp(path, "/reboot") == 0 ||
                  strcmp(path, "/hostname") == 0 || strcmp(path, "/disable") == 0 || strcmp(path, "/ipv6") == 0;
    if (!action) return notice(404, "Not Found", "Not found", "There is no such page.", len);
    if (!post) return notice(405, "Method Not Allowed", "Not allowed", "Use the buttons on the start page.", len);
    if (slot < 0) {
        return notice(403, "Forbidden", "Not logged in", "Your session has ended. Log in again.", len);
    }
    char csrf[WEB_TOKEN_HEX + 2];
    if (!formValue(req.body(), req.bodyLength(), "csrf", csrf, sizeof(csrf)) ||
        !webConstantTimeEquals(csrf, _sessions.csrf((size_t)slot))) {
        return notice(403, "Forbidden", "Form expired", "The form was out of date. Reload the page and try again.", len);
    }

    if (strcmp(path, "/logout") == 0) {
        _sessions.remove((size_t)slot);
        return redirect("Set-Cookie: sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n", len);
    }
    if (strcmp(path, "/network") == 0) return network(req, (size_t)slot, backend, len);
    if (strcmp(path, "/ipv6") == 0) return ipv6(req, (size_t)slot, backend, len);
    if (strcmp(path, "/community") == 0) return community(req, (size_t)slot, backend, len);
    if (strcmp(path, "/hostname") == 0) return hostname(req, (size_t)slot, backend, len);
    if (strcmp(path, "/disable") == 0) return disable(req, (size_t)slot, backend, len);
    backend.requestReboot();
    return notice(200, "OK", "Restarting", "The device restarts now. Log in again in a few seconds.", len);
}
