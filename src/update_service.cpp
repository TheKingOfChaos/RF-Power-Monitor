#include "update_service.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include <lwip/ip_addr.h>
#include <lwip/netif.h>

#include <update_job.h>
#include <update_protocol.h>
#include <update_schedule.h>
#include <update_settings_store.h>
#include <update_version.h>

#include "fw_update_service.h"
#include "host_stats.h"
#include "hostname.h"
#include "http_fetch.h"
#include "net_stats.h"
#include "version.h"

namespace {

/// Whole check-in (connect, request, reply) must end within this
const uint32_t CHECKIN_TOTAL_MS = 15000;
/// Delay between a verified download and the restart, so the serial message goes out
const uint32_t RESTART_DELAY_MS = 1000;
/// Body reads per loop() pass while downloading (each may write a flash page)
const int DOWNLOAD_READS_PER_POLL = 4;

enum class Phase : uint8_t { Idle, Checkin, Download, Restarting };

Phase phase = Phase::Idle;
CheckinScheduler scheduler;
UpdateJob job;
UpdateUrl url;
char urlText[UPDATE_URL_MAX + 1] = "";
char nonce[UPDATE_NONCE_HEX + 1] = "";
uint32_t failSeqSent = 0;       // the job failure the running check-in carries
char* reply = nullptr;          // heap, while a check-in runs
size_t replyLen = 0;
char lastResult[48] = "off";
bool checkedIn = false;
uint32_t lastCheckinMs = 0;
uint32_t restartAtMs = 0;

// Download
uint8_t head[UPD_HEAD_SIZE];
size_t headLen = 0;
bool uploadStarted = false;

void setResult(const char* text) {
    snprintf(lastResult, sizeof(lastResult), "%s", text);
}

/// The URL check-ins use now: from DHCP when that is set and one arrived, else the stored one
void effectiveUrl(char* out, size_t size) {
    out[0] = '\0';
    if (currentUpdateSettings.fromDhcp) {
        netStatsDhcpUpdateUrl(out, size);
    } else {
        snprintf(out, size, "%s", currentUpdateSettings.url);
    }
}

void randomNonce() {
    for (int i = 0; i < 4; i++) snprintf(nonce + 8 * i, 9, "%08lx", (unsigned long)rp2040.hwrand32());
}

/// Fill the address fields of a check-in from the default interface
void captureAddresses(CheckinInfo& in) {
    DriverLock lock;
    netif* n = netif_default;
    if (n == nullptr) return;
    snprintf(in.mac, sizeof(in.mac), "%02x:%02x:%02x:%02x:%02x:%02x", n->hwaddr[0], n->hwaddr[1], n->hwaddr[2],
             n->hwaddr[3], n->hwaddr[4], n->hwaddr[5]);
    if (!ip4_addr_isany_val(*netif_ip4_addr(n))) ip4addr_ntoa_r(netif_ip4_addr(n), in.ipv4, sizeof(in.ipv4));
#if LWIP_IPV6
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES && in.ipv6Count < UPDATE_IPV6_MAX; i++) {
        if (!ip6_addr_isvalid(netif_ip6_addr_state(n, i))) continue;
        ip6addr_ntoa_r(netif_ip6_addr(n, i), in.ipv6[in.ipv6Count], sizeof(in.ipv6[0]));
        in.ipv6Count++;
    }
#endif
}

void startCheckin(uint32_t now) {
    struct Work {
        CheckinInfo info;
        char body[1024];
    };
    Work* w = new (std::nothrow) Work;
    reply = new (std::nothrow) char[UPDATE_REPLY_MAX + 1];
    if (w == nullptr || reply == nullptr) {
        delete w;
        delete[] reply;
        reply = nullptr;
        setResult("out of memory");
        scheduler.checkinDone(false, now);
        return;
    }
    CheckinInfo& in = w->info;
    memset(&in, 0, sizeof(in));
    snprintf(in.serial, sizeof(in.serial), "%s", hostStatsSnapshot().serialNumber);
    snprintf(in.hostname, sizeof(in.hostname), "%s", hostnameCurrent());
    captureAddresses(in);
    snprintf(in.version, sizeof(in.version), "%s", FIRMWARE_VERSION);
    FwSnmpState fw;
    fwUpdateSnmpState(fw);
    in.slot = fw.runningSlot == 0 ? 'A' : 'B';
    in.trial = fw.trial;
    in.downloading = false;
    snprintf(in.fail, sizeof(in.fail), "%s", job.failLine());
    failSeqSent = job.failSequence();
    FwResult result;
    char resultVersion[17];
    fwUpdateLastRecord(result, resultVersion);
    checkinLastText(result, resultVersion, in.last, sizeof(in.last));
    randomNonce();
    snprintf(in.nonce, sizeof(in.nonce), "%s", nonce);

    size_t bodyLen = buildCheckinBody(in, w->body, sizeof(w->body));
    char mac[UPDATE_MAC_HEX + 1];
    if (bodyLen > 0) updateMacHex(currentUpdateSettings.key, (const uint8_t*)w->body, bodyLen, mac);
    char headers[128];
    snprintf(headers, sizeof(headers), "Content-Type: text/plain\r\nX-RFPower-Auth: %s\r\n", mac);
    char path[UPDATE_URL_MAX + 16];
    snprintf(path, sizeof(path), "%s/api/checkin", url.path);
    bool started = bodyLen > 0 && httpFetchStart(url, "POST", path, headers, (const uint8_t*)w->body, bodyLen,
                                                 UPDATE_REPLY_MAX, now, CHECKIN_TOTAL_MS);
    delete w;  // the fetch keeps its own copy of the request
    if (!started) {
        setResult(bodyLen == 0 ? "check-in too long" : httpFetchError());
        delete[] reply;
        reply = nullptr;
        scheduler.checkinDone(false, now);
        return;
    }
    replyLen = 0;
    phase = Phase::Checkin;
}

/// Word after "status=" in an error reply ("badauth", "replay", "bad"), else "http <code>"
void errorResult(int status) {
    const char* s = strstr(reply, "status=");
    if (s != nullptr) {
        char word[16];
        size_t n = strcspn(s + 7, "\n");
        if (n >= sizeof(word)) n = sizeof(word) - 1;
        memcpy(word, s + 7, n);
        word[n] = '\0';
        setResult(word);
    } else {
        char text[16];
        snprintf(text, sizeof(text), "http %d", status);
        setResult(text);
    }
}

void startDownload(const CheckinReply& r, uint32_t now) {
    char downloadPath[sizeof(url.path) + UPDATE_PATH_MAX + 1];
    snprintf(downloadPath, sizeof(downloadPath), "%s%s", url.path, r.path);
    headLen = 0;
    uploadStarted = false;
    if (!httpFetchStart(url, "GET", downloadPath, nullptr, nullptr, 0, FW_UPLOAD_MAX, now, 0)) {
        job.failDownload(httpFetchError());
        scheduler.reportSoon(now);
        return;
    }
    phase = Phase::Download;
    Serial.printf("\r\nUpdate server: downloading %s\r\n", r.version);
}

/// The check-in's reply is in: act on it
void finishCheckin(uint32_t now) {
    int status = httpFetchStatus();
    reply[replyLen] = '\0';
    httpFetchAbort();
    phase = Phase::Idle;
    lastCheckinMs = now;
    checkedIn = true;
    if (status != 200) {
        errorResult(status);
        scheduler.checkinDone(false, now);
        return;
    }
    CheckinReply r;
    ReplyKind kind = parseCheckinReply(reply, replyLen, nonce, currentUpdateSettings.key, r);
    if (kind == ReplyKind::BadAuth || kind == ReplyKind::Malformed) {
        setResult(kind == ReplyKind::BadAuth ? "reply not authenticated" : "bad reply");
        scheduler.checkinDone(false, now);
        return;
    }
    // Authenticated: the site has the key, so the network works both ways
    fwUpdateNoteValidRequest();
    job.reported(failSeqSent);
    scheduler.checkinDone(true, now);
    FwSnmpState fw;
    fwUpdateSnmpState(fw);
    if (fw.trial) scheduler.reportSoon(now);  // report the kept firmware soon

    if (kind == ReplyKind::Update && job.begin(r.version, FIRMWARE_VERSION)) {
        char text[48];
        snprintf(text, sizeof(text), "update %s", r.version);
        setResult(text);
        startDownload(r, now);
    } else {
        setResult("ok");
    }
}

void pollCheckin(uint32_t now) {
    FetchState st = httpFetchPoll(now);
    if (st == FetchState::Body) {
        replyLen += httpFetchRead((uint8_t*)reply + replyLen, UPDATE_REPLY_MAX - replyLen);
        st = httpFetchPoll(now);
    }
    if (st == FetchState::Done) {
        finishCheckin(now);
    } else if (st == FetchState::Failed) {
        setResult(httpFetchError());
        httpFetchAbort();
        phase = Phase::Idle;
        lastCheckinMs = now;
        checkedIn = true;
        scheduler.checkinDone(false, now);
    }
    if (phase != Phase::Checkin) {
        delete[] reply;
        reply = nullptr;
    }
}

/// End a download that did not install: report it at a check-in soon
void endDownload(uint32_t now) {
    if (uploadStarted) fwUpdateUploadAbort();
    uploadStarted = false;
    httpFetchAbort();
    phase = Phase::Idle;
    setResult(job.failLine());
    Serial.printf("\r\nUpdate server: %s\r\n", job.failLine());
    scheduler.reportSoon(now);
}

/// The receiver refused the file: take its reason
void refusedByReceiver(uint32_t now) {
    char version[17];
    const char* error = "refused";
    FwUploadFault fault;
    fwUpdateUploadFinish(version, error, fault);  // gives the reason; the slot reads empty again
    uploadStarted = false;
    job.refused(error);
    endDownload(now);
}

void pollDownload(uint32_t now) {
    FetchState st = httpFetchPoll(now);
    if (st == FetchState::Failed) {
        job.failDownload(httpFetchError());
        endDownload(now);
        return;
    }
    if (st != FetchState::Body && st != FetchState::Done) return;
    if (httpFetchStatus() != 200) {
        char why[16];
        snprintf(why, sizeof(why), "http %d", httpFetchStatus());
        job.failDownload(why);
        endDownload(now);
        return;
    }

    if (headLen < sizeof(head)) {
        headLen += httpFetchRead(head + headLen, sizeof(head) - headLen);
        if (headLen < sizeof(head)) {
            if (httpFetchPoll(now) == FetchState::Done) {  // the whole file is shorter than a header
                job.refused("not an update file");
                endDownload(now);
            }
            return;
        }
        if (job.head(head, sizeof(head)) == UpdateJob::Step::Refuse) {
            endDownload(now);
            return;
        }
        const char* error = "";
        FwUploadFault fault;
        if (!fwUpdateUploadBegin(httpFetchContentLength(), error, fault)) {
            job.refused(error);
            endDownload(now);
            return;
        }
        uploadStarted = true;
        if (!fwUpdateUploadFeed(head, sizeof(head))) {
            refusedByReceiver(now);
            return;
        }
    }

    static uint8_t chunk[256];  // core 0 only
    for (int i = 0; i < DOWNLOAD_READS_PER_POLL; i++) {
        size_t n = httpFetchRead(chunk, sizeof(chunk));
        if (n == 0) break;
        if (!fwUpdateUploadFeed(chunk, n)) {
            refusedByReceiver(now);
            return;
        }
    }

    if (httpFetchPoll(now) == FetchState::Done) {
        char version[17];
        const char* error = "";
        FwUploadFault fault;
        uploadStarted = false;
        httpFetchAbort();
        if (fwUpdateUploadFinish(version, error, fault)) {
            char text[48];
            snprintf(text, sizeof(text), "installing %s", version);
            setResult(text);
            Serial.printf("\r\nUpdate server: %s verified, restarting into it on trial\r\n", version);
            phase = Phase::Restarting;
            restartAtMs = now + RESTART_DELAY_MS;
        } else {
            job.refused(error);
            endDownload(now);
        }
    }
}

}  // namespace

void updateServiceBegin() {
    netStatsRequestDhcpUpdateUrl(currentUpdateSettings.fromDhcp);
}

void updateServicePoll(bool netReady) {
    uint32_t now = millis();
    netStatsRequestDhcpUpdateUrl(currentUpdateSettings.fromDhcp);  // acts only on a change

    effectiveUrl(urlText, sizeof(urlText));
    bool urlOk = urlText[0] != '\0' && parseUpdateUrl(urlText, url);
    bool configured = urlOk && currentUpdateSettings.hasKey;
    if (phase == Phase::Idle && !checkedIn) {
        setResult(urlText[0] == '\0' ? "off" : !urlOk ? "bad url" : !currentUpdateSettings.hasKey ? "no key" : "waiting");
    }
    scheduler.netReady(netReady && configured, now);

    switch (phase) {
    case Phase::Idle:
        if (scheduler.due(now)) startCheckin(now);
        break;
    case Phase::Checkin:
        pollCheckin(now);
        break;
    case Phase::Download:
        pollDownload(now);
        break;
    case Phase::Restarting:
        if ((int32_t)(now - restartAtMs) >= 0) {
            Serial.flush();
            rp2040.reboot();  // a planned restart into the new slot, on trial
        }
        break;
    }
}

void updateServiceForceCheckin() {
    scheduler.forceNow(millis());
}

bool updateServiceSave(const char* url, bool fromDhcp, const char* keyHex, bool clearKey, const char*& error) {
    UpdateSettings before = currentUpdateSettings;
    if (!updateSettingsApply(currentUpdateSettings, url, fromDhcp, keyHex, clearKey, error)) return false;
    if (!saveUpdateSettings()) {
        currentUpdateSettings = before;
        memset(&before, 0, sizeof(before));
        error = "Saving failed, nothing changed.";
        return false;
    }
    memset(&before, 0, sizeof(before));
    // Start over with the new settings: first check-in 60 s from the next poll
    scheduler.netReady(false, millis());
    checkedIn = false;
    return true;
}

void updateServiceStateText(char* out, size_t size) {
    if (phase == Phase::Download) {
        snprintf(out, size, "downloading %s", job.version());
    } else if (!checkedIn) {
        snprintf(out, size, "%s", strcmp(lastResult, "waiting") == 0 ? "waiting for the first check-in" : lastResult);
    } else {
        uint32_t ageS = (millis() - lastCheckinMs) / 1000;
        if (ageS < 60) {
            snprintf(out, size, "last check-in %lu s ago: %s", (unsigned long)ageS, lastResult);
        } else {
            snprintf(out, size, "last check-in %lu min ago: %s", (unsigned long)(ageS / 60), lastResult);
        }
    }
}

void updateServiceStatus(UpdateServiceStatus& out) {
    memset(&out, 0, sizeof(out));
    snprintf(out.urlInUse, sizeof(out.urlInUse), "%s", urlText);
    out.fromDhcp = currentUpdateSettings.fromDhcp;
    out.hasKey = currentUpdateSettings.hasKey;
    out.downloading = phase == Phase::Download;
    out.checkedIn = checkedIn;
    out.lastCheckinAgeS = checkedIn ? (millis() - lastCheckinMs) / 1000 : 0;
    snprintf(out.lastResult, sizeof(out.lastResult), "%s", lastResult);
}
