#pragma once

/**
 * @file web_app.h
 * @brief The configuration web page: routes, login, CSRF checks and HTML.
 *        Pure C++, host tested with a fake backend.
 *
 * Pages:
 *   GET  /          login form, or the status and settings page when logged in
 *   POST /login     user "admin" + password; sets the session cookie
 *   POST /logout
 *   POST /network   DHCP or static IP; saves and reboots
 *   POST /community SNMP community; saves, active immediately
 *   POST /hostname  hostname (sysName, DHCP, mDNS), sysContact, sysLocation; saves, active immediately
 *   POST /ipv6      static IPv6 address and gateway (empty = none); saves and reboots
 *   POST /reboot
 *   POST /disable   turn the web interface off (asks first; confirm=yes does it)
 *   GET  /firmware  firmware page: slots, upload form, switch form
 *   POST /firmware/switch  boot the other slot (needs confirm=yes); reboots
 *   GET  /firmware/status  the same facts as text/plain key=value lines (fw_update.py)
 *   POST /firmware/upload  not here: the body is streamed, see HttpRequest::streamPath();
 *                  authorizeUpload() checks the head, uploadResult() builds the answer
 *   GET  /favicon.svg  the logo, no login
 *   GET  /static/bootstrap-<version>.min.css  Bootstrap from flash, gzip, cached a year, no login
 * Every POST except /login needs a valid session cookie and the session's
 * CSRF token in the form (the upload: in the X-CSRF-Token header). The cookie
 * is HttpOnly and SameSite=Strict. The pages have no scripts, except the
 * Firmware page: its upload script is allowed by that page's own policy.
 */

#include <stddef.h>
#include <stdint.h>
#include <access_control.h>
#include <netconfig.h>
#include <settings_codec.h>
#include "http_request.h"
#include "web_auth.h"

/**
 * @brief What the status part of the page shows
 */
struct WebStatus {
    const char* version;
    uint32_t uptimeSeconds;
    bool mainsKnown;
    bool mainsPresent;
    uint32_t mainsChanges;
    bool mainsAlarm;          ///< mainsPowerAlarm: lost for 60 s or more
    int32_t cpuTempDeciC;     ///< RP2040 die, tenths of a degree Celsius
    bool ambientTempValid;    ///< TMP1075 answers
    int32_t ambientTempDeciC; ///< TMP1075, tenths of a degree Celsius
    bool linkUp;
    uint32_t linkMbps;
    char ip[16];
    char liveMask[16];    ///< Subnet mask in use (from DHCP or static), empty without an address
    char liveGateway[16]; ///< Gateway in use, empty without an address
    char dns[16];         ///< DNS server in use, empty without an address
    char ipv6[46];        ///< Global (SLAAC) IPv6 address, else link-local, empty without IPv6
    char ipv6LinkLocal[46];  ///< fe80:: address, empty without IPv6
    char ipv6Dhcp[IPV6_TEXT_MAX];  ///< Address leased by DHCPv6, empty without one
    uint32_t ipv6DhcpValidS;       ///< Its remaining valid lifetime, 0xFFFFFFFF = infinite
    char ipv6DhcpProblem[40];      ///< Why a leased address could not be used, e.g. "no free IPv6 address slot"
    char mac[18];
    bool dhcp;            ///< Configured mode
    char staticIP[16];    ///< Stored static configuration (form defaults)
    char subnetMask[16];
    char gateway[16];
    char staticIPv6[IPV6_TEXT_MAX];   ///< Stored static IPv6 address, empty = none
    char ipv6Gateway[IPV6_TEXT_MAX];  ///< Stored static IPv6 gateway, empty = none
    char community[32];
    char hostname[33];    ///< Valid hostname in use (DHCP, mDNS name.local, sysName)
    char sysContact[SYS_TEXT_MAX_LENGTH + 1];   ///< SNMP sysContact, may be empty
    char sysLocation[SYS_TEXT_MAX_LENGTH + 1];  ///< SNMP sysLocation, may be empty
    char lldpSwitch[144];     ///< LLDP neighbour, "<name>, port <port>" or "none seen" (lldpSwitchText)
    char lldpSwitchAddr[46];  ///< Its management address, empty if none
};

/**
 * @brief What the Firmware page shows. Plain text, so this library does not
 *        depend on the update libraries.
 */
struct WebFirmware {
    uint8_t runningSlot;        ///< 0 = slot A, 1 = slot B
    char runningVersion[17];
    bool trial;                 ///< the running firmware is on trial (not yet kept)
    char otherVersion[17];      ///< version of the other slot, empty when it is not OK
    char otherProblem[24];      ///< why the other slot is not OK (slotCheckName), empty when OK
    char lastResult[64];        ///< result of the last update, e.g. "ok 3.3.0" or an error name, may be empty
    char keyFingerprint[17];    ///< fingerprint of the update signing key
    bool uploading;             ///< an upload is in progress
};

/// Events the backend may log
enum class WebEvent : uint8_t { LoginFailed, LoginLocked, LoggedIn, UploadRefused };

/**
 * @brief What the page needs from the device; the firmware implements it
 */
class WebBackend {
public:
    virtual ~WebBackend() {}
    virtual void status(WebStatus& out) = 0;
    /// true if password is the current web password
    virtual bool checkPassword(const char* password) = 0;
    /// Save the network settings (static values only used when dhcp is false); reboot follows
    virtual bool saveNetwork(bool dhcp, const char* ip, const char* mask, const char* gateway) = 0;
    /// Save the static IPv6 address and gateway (validated, RFC 5952 text, empty = none); reboot follows
    virtual bool saveIPv6(const char* address, const char* gateway) = 0;
    /// Save and apply a new SNMP community
    virtual bool saveCommunity(const char* community) = 0;
    /// Save and apply the hostname (sysName, DHCP and mDNS), sysContact and sysLocation in one save
    /// (all already validated); on failure nothing changes
    virtual bool saveDeviceName(const char* hostname, const char* contact, const char* location) = 0;
    /// Save webEnabled = false; the server closes port 80 after this response
    virtual bool disableWeb() = 0;
    /// Reboot once the response has been sent
    virtual void requestReboot() = 0;
    /// Fill out for the Firmware page (out is zeroed first)
    virtual void firmware(WebFirmware& out) = 0;
    /// Make the other slot the one to boot; false when it is not OK or the running firmware is on trial.
    /// On success the caller asks for a reboot
    virtual bool switchSlot() = 0;
    /// Fill out with random bytes from a good source
    virtual void random(uint8_t* out, size_t len) = 0;
    /// source: the client address (lib/access_control SourceKey)
    virtual void log(const SourceKey& source, WebEvent event) = 0;
};

/// Largest page; the status page with the longest values is 10857 bytes (test_longest_values_still_fit_the_page),
/// over 900 of them for a switch name of quotes and 756 for a contact and location of quotes (each escaped to &quot;)
static const size_t WEB_BODY_MAX = 11264;
/// Longest Cookie header read; browsers send every cookie of the host, not only ours
static const size_t WEB_COOKIE_HEADER_MAX = 1024;
static const size_t WEB_RESPONSE_MAX = WEB_BODY_MAX + 768;

class WebApp {
public:
    /**
     * @brief Build the response to a parsed request (state Complete, TooLarge or Bad)
     * @return The full HTTP response; valid until the next call
     */
    const char* handle(const HttpRequest& req, const SourceKey& source, uint32_t nowMs, WebBackend& backend, size_t& len);

    /**
     * @brief May this streamed POST /firmware/upload go on?
     * @param req A request in state Streaming (its headers are read)
     * @return 0 when the session cookie is valid and the X-CSRF-Token header matches the session's
     *         and no upload is running and the firmware is not on trial; else the HTTP status:
     *         401 no session, 403 CSRF token missing or wrong, 409 trial or an upload in progress
     */
    int authorizeUpload(const HttpRequest& req, const SourceKey& source, uint32_t nowMs, WebBackend& backend);

    /**
     * @brief The full text/plain response to an upload
     * @param code 200 for success, else the HTTP status of the failure (400-599; anything else is sent as 500)
     * @param text The new version on success (body "ok <version>"), else the error name
     * @return The response, valid until the next call
     */
    const char* uploadResult(int code, const char* text, size_t& len);

    /**
     * @brief Did the last request (handle() or authorizeUpload()) come from a logged-in browser?
     *
     * true for a valid session cookie (whatever the request then asked for) and
     * for a successful login; a firmware on trial counts that as reachable.
     */
    bool authenticated() const { return _authenticated; }

    /// Log every browser out, e.g. after the password changed
    void logoutAll() { _sessions.clear(); }

    /**
     * @brief Body that follows the response handle() returned, sent straight from flash
     *        (the stylesheet); nullptr for normal pages, whose body is in the response itself
     */
    const uint8_t* staticBody(size_t& len) const {
        len = _staticLen;
        return _staticBody;
    }

    size_t sessionCount() const { return _sessions.count(); }

private:
    const char* login(const HttpRequest& req, const SourceKey& source, uint32_t nowMs, WebBackend& backend, size_t& len);
    const char* network(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* ipv6(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* community(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* hostname(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* disable(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* firmwarePage(int code, const char* reason, size_t slot, WebBackend& backend, const char* message,
                             bool error, size_t& len);
    const char* firmwareSwitch(const HttpRequest& req, size_t slot, WebBackend& backend, size_t& len);
    const char* firmwareStatus(WebBackend& backend, size_t& len);
    const char* plainResponse(int code, const char* body, size_t& len);
    const char* favicon(size_t& len);
    const char* mainPage(int code, const char* reason, size_t slot, WebBackend& backend, const char* message,
                         bool error, size_t& len);
    const char* loginPage(int code, const char* reason, const char* message, size_t& len);
    const char* notice(int code, const char* reason, const char* title, const char* text, size_t& len);
    const char* redirect(const char* extraHeaders, size_t& len);
    const char* respond(int code, const char* reason, const char* extraHeaders, size_t& len);
    const char* stylesheet(size_t& len);

    WebSessions _sessions;
    WebStatus _status;  // ~340 bytes: a member, not on core 0's stack
    WebFirmware _fw;    // ~150 bytes, same
    bool _authenticated = false;  ///< see authenticated()
    bool _scripts = false;  ///< the page being built is the Firmware page: its policy allows its script
    char _scriptHash[45];   ///< base64 SHA-256 of that script, for the policy
    LoginThrottle _throttle;
    char _cookies[WEB_COOKIE_HEADER_MAX];
    char _body[WEB_BODY_MAX];
    char _response[WEB_RESPONSE_MAX];
    const uint8_t* _staticBody = nullptr;
    size_t _staticLen = 0;
};
