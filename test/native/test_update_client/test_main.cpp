// Host unit tests for the update client's URL and HTTP response parsing, .upd
// header version, version order and check-in timing (lib/update_client).
// Run: pio test -e native -f native/test_update_client

#include <stdio.h>
#include <string.h>

#include <unity.h>
#include <http_response.h>
#include <update_job.h>
#include <update_schedule.h>
#include <update_settings.h>
#include <update_url.h>
#include <update_version.h>

#include "../test_fw_receiver/golden_upd.h"

void setUp() {}
void tearDown() {}

namespace {

HttpRespParse feedAll(HttpResponseParser& p, const char* text, size_t& consumed) {
    return p.feed((const uint8_t*)text, strlen(text), consumed);
}

}  // namespace

// --- URL ---

static void test_url_accepts_name_port_path() {
    UpdateUrl u;
    TEST_ASSERT_TRUE(parseUpdateUrl("http://updates.example.lan:8071/rf/", u));
    TEST_ASSERT_EQUAL_STRING("updates.example.lan", u.host);
    TEST_ASSERT_EQUAL(8071, u.port);
    TEST_ASSERT_EQUAL_STRING("/rf", u.path);  // trailing '/' dropped: paths are appended
    TEST_ASSERT_FALSE(u.hostIsIPv6);
}

static void test_url_accepts_ipv4_and_bracketed_ipv6() {
    UpdateUrl u;
    TEST_ASSERT_TRUE(parseUpdateUrl("http://10.132.0.45:8071", u));
    TEST_ASSERT_EQUAL_STRING("10.132.0.45", u.host);
    TEST_ASSERT_EQUAL_STRING("", u.path);
    TEST_ASSERT_TRUE(parseUpdateUrl("http://[fd16:d833:4f0e:3ece::45]:8071/", u));
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::45", u.host);
    TEST_ASSERT_TRUE(u.hostIsIPv6);
    TEST_ASSERT_EQUAL(8071, u.port);
}

static void test_url_default_port_80() {
    UpdateUrl u;
    TEST_ASSERT_TRUE(parseUpdateUrl("http://site/x", u));
    TEST_ASSERT_EQUAL(80, u.port);
    TEST_ASSERT_EQUAL_STRING("/x", u.path);
}

static void test_url_rejects_https() {
    UpdateUrl u;
    TEST_ASSERT_FALSE(parseUpdateUrl("https://site/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("site:8071", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("ftp://site/", u));
}

static void test_url_rejects_128_chars() {
    UpdateUrl u;
    // 127 characters: a 63-character host, the longest port and a 51-character path
    char ok[UPDATE_URL_MAX + 1];
    strcpy(ok, "http://");
    memset(ok + 7, 'h', 63);
    strcpy(ok + 70, ":65535/");
    memset(ok + 77, 'p', UPDATE_URL_MAX - 77);
    ok[UPDATE_URL_MAX] = '\0';
    TEST_ASSERT_EQUAL(127, strlen(ok));
    TEST_ASSERT_TRUE(parseUpdateUrl(ok, u));
    char tooLong[UPDATE_URL_MAX + 2];
    strcpy(tooLong, ok);
    strcat(tooLong, "p");
    TEST_ASSERT_FALSE(parseUpdateUrl(tooLong, u));
}

static void test_url_rejects_host_or_path_over_63() {
    char url[UPDATE_URL_MAX + 1];
    UpdateUrl u;
    strcpy(url, "http://");
    memset(url + 7, 'h', 64);
    url[71] = '\0';
    TEST_ASSERT_FALSE(parseUpdateUrl(url, u));
    strcpy(url, "http://h/");
    memset(url + 9, 'p', 63);  // path "/" + 63 = 64 characters
    url[72] = '\0';
    TEST_ASSERT_FALSE(parseUpdateUrl(url, u));
}

static void test_url_rejects_port_0_and_65536() {
    UpdateUrl u;
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site:0/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site:65536/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site:/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site:80a/", u));
    TEST_ASSERT_TRUE(parseUpdateUrl("http://site:65535/", u));
}

static void test_url_rejects_empty_host() {
    UpdateUrl u;
    TEST_ASSERT_FALSE(parseUpdateUrl("http://", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http:///x", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://:8071/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://[]/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://[fd16::1/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://[not-ipv6]/", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("", u));
    TEST_ASSERT_FALSE(parseUpdateUrl(nullptr, u));
}

static void test_url_rejects_userinfo() {
    UpdateUrl u;
    TEST_ASSERT_FALSE(parseUpdateUrl("http://admin:pw@site/", u));
}

static void test_url_rejects_space_and_control() {
    UpdateUrl u;
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site/a b", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://site/a\nb", u));
    TEST_ASSERT_FALSE(parseUpdateUrl("http://si_te/", u));
}

// --- HTTP response ---

static void test_http_response_reads_status_and_length() {
    HttpResponseParser p;
    p.reset(512);
    const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\ncontent-length: 12\r\n\r\nstatus=ok\n";
    size_t consumed = 0;
    TEST_ASSERT_EQUAL(HttpRespParse::HeadDone, feedAll(p, resp, consumed));
    TEST_ASSERT_EQUAL(200, p.status());
    TEST_ASSERT_EQUAL(12, p.contentLength());
    TEST_ASSERT_EQUAL_STRING("status=ok\n", resp + consumed);
}

static void test_http_response_head_in_pieces() {
    HttpResponseParser p;
    p.reset(512);
    const char* resp = "HTTP/1.0 401 Unauthorized\nContent-Length: 15\n\n";
    size_t total = 0;
    for (size_t i = 0; resp[i] != '\0'; i++) {
        size_t consumed = 0;
        HttpRespParse r = p.feed((const uint8_t*)resp + i, 1, consumed);
        total += consumed;
        if (resp[i + 1] != '\0') TEST_ASSERT_EQUAL(HttpRespParse::Incomplete, r);
        else TEST_ASSERT_EQUAL(HttpRespParse::HeadDone, r);
    }
    TEST_ASSERT_EQUAL(strlen(resp), total);
    TEST_ASSERT_EQUAL(401, p.status());
}

static void test_http_response_rejects_chunked() {
    HttpResponseParser p;
    p.reset(512);
    size_t c;
    TEST_ASSERT_EQUAL(HttpRespParse::Malformed,
                      feedAll(p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n", c));
}

static void test_http_response_rejects_missing_length() {
    HttpResponseParser p;
    p.reset(512);
    size_t c;
    TEST_ASSERT_EQUAL(HttpRespParse::Malformed, feedAll(p, "HTTP/1.1 200 OK\r\n\r\n", c));
}

static void test_http_response_rejects_bad_status_line() {
    const char* bad[] = {"HTP/1.1 200 OK\r\n", "HTTP/1.1 20 OK\r\n", "HTTP/1.1 2000 OK\r\n", "HTTP/1.1 abc OK\r\n",
                         "\r\n"};
    for (const char* line : bad) {
        HttpResponseParser p;
        p.reset(512);
        size_t c;
        TEST_ASSERT_EQUAL_MESSAGE(HttpRespParse::Malformed, feedAll(p, line, c), line);
    }
}

static void test_http_response_rejects_bad_length() {
    const char* bad[] = {"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
                         "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
                         "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n",
                         "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n"};
    for (const char* resp : bad) {
        HttpResponseParser p;
        p.reset(0xFFFFFFFFu);
        size_t c;
        TEST_ASSERT_EQUAL_MESSAGE(HttpRespParse::Malformed, feedAll(p, resp, c), resp);
    }
}

static void test_http_response_rejects_head_over_1024_bytes() {
    static char resp[1200];
    strcpy(resp, "HTTP/1.1 200 OK\r\nX-Pad: ");
    size_t n = strlen(resp);
    memset(resp + n, 'a', 1010);
    strcpy(resp + n + 1010, "\r\nContent-Length: 1\r\n\r\n");
    HttpResponseParser p;
    p.reset(512);
    size_t c;
    TEST_ASSERT_EQUAL(HttpRespParse::Malformed, feedAll(p, resp, c));
}

static void test_http_response_rejects_body_over_max() {
    HttpResponseParser p;
    p.reset(512);
    size_t c;
    TEST_ASSERT_EQUAL(HttpRespParse::Malformed, feedAll(p, "HTTP/1.1 200 OK\r\nContent-Length: 513\r\n\r\n", c));
    p.reset(512);
    TEST_ASSERT_EQUAL(HttpRespParse::HeadDone, feedAll(p, "HTTP/1.1 200 OK\r\nContent-Length: 512\r\n\r\n", c));
}

static void test_http_response_every_truncation_is_incomplete_or_malformed() {
    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\n";
    size_t len = strlen(resp);
    for (size_t cut = 0; cut < len; cut++) {
        HttpResponseParser p;
        p.reset(512);
        size_t c;
        HttpRespParse r = p.feed((const uint8_t*)resp, cut, c);
        TEST_ASSERT_TRUE(r == HttpRespParse::Incomplete || r == HttpRespParse::Malformed);
    }
}

static void test_http_response_after_head_consumes_nothing() {
    HttpResponseParser p;
    p.reset(512);
    size_t c;
    TEST_ASSERT_EQUAL(HttpRespParse::HeadDone, feedAll(p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n", c));
    TEST_ASSERT_EQUAL(HttpRespParse::HeadDone, feedAll(p, "ok", c));
    TEST_ASSERT_EQUAL(0, c);
}

// --- .upd header version and version order ---

static void test_upd_header_version_from_golden_upd() {
    char v[UPDATE_VERSION_MAX + 1];
    TEST_ASSERT_TRUE(updHeaderVersion(GOLDEN_UPD, 64, v));
    TEST_ASSERT_EQUAL_STRING("3.4.0", v);
}

static void test_upd_header_rejects_bad_magic_and_short() {
    char v[UPDATE_VERSION_MAX + 1];
    uint8_t head[64];
    memcpy(head, GOLDEN_UPD, sizeof(head));
    TEST_ASSERT_FALSE(updHeaderVersion(head, 63, v));
    head[0] ^= 1;
    TEST_ASSERT_FALSE(updHeaderVersion(head, 64, v));
    memcpy(head, GOLDEN_UPD, sizeof(head));
    head[4] = 2;  // format 2
    TEST_ASSERT_FALSE(updHeaderVersion(head, 64, v));
    memcpy(head, GOLDEN_UPD, sizeof(head));
    memset(head + 8, 'x', 16);  // no NUL in the version field
    TEST_ASSERT_FALSE(updHeaderVersion(head, 64, v));
    memcpy(head, GOLDEN_UPD, sizeof(head));
    head[8] = '\0';  // empty version
    TEST_ASSERT_FALSE(updHeaderVersion(head, 64, v));
}

static void test_compare_versions_orders_3_10_0_after_3_9_0() {
    TEST_ASSERT_TRUE(compareVersions("3.10.0", "3.9.0") > 0);
    TEST_ASSERT_TRUE(compareVersions("3.9.0", "3.10.0") < 0);
    TEST_ASSERT_TRUE(compareVersions("4.0.0", "3.99.99") > 0);
    TEST_ASSERT_EQUAL(0, compareVersions("3.4.0", "3.4.0"));
}

static void test_compare_versions_prerelease_below_release() {
    TEST_ASSERT_TRUE(compareVersions("3.4.0-hil", "3.4.0") < 0);
    TEST_ASSERT_TRUE(compareVersions("3.4.1-hil", "3.4.0") > 0);
    TEST_ASSERT_TRUE(compareVersions("3.4.0", "3.4.0-rc1") > 0);
}

// --- check-in timing ---

static void test_scheduler_first_checkin_60s_after_net_ready() {
    CheckinScheduler s;
    s.netReady(true, 1000);
    TEST_ASSERT_FALSE(s.due(1000 + 59999));
    TEST_ASSERT_TRUE(s.due(1000 + 60000));
}

static void test_scheduler_next_after_30_min() {
    CheckinScheduler s;
    s.netReady(true, 0);
    s.checkinDone(true, 60000);
    TEST_ASSERT_FALSE(s.due(60000 + 1800000 - 1));
    TEST_ASSERT_TRUE(s.due(60000 + 1800000));
}

static void test_scheduler_retry_after_5_min_on_failure() {
    CheckinScheduler s;
    s.netReady(true, 0);
    s.checkinDone(false, 60000);
    TEST_ASSERT_FALSE(s.due(60000 + 299999));
    TEST_ASSERT_TRUE(s.due(60000 + 300000));
}

static void test_scheduler_report_soon_is_60s() {
    CheckinScheduler s;
    s.netReady(true, 0);
    s.checkinDone(true, 60000);
    s.reportSoon(100000);
    TEST_ASSERT_FALSE(s.due(159999));
    TEST_ASSERT_TRUE(s.due(160000));
}

static void test_scheduler_report_soon_never_delays() {
    CheckinScheduler s;
    s.netReady(true, 0);  // due at 60000
    s.reportSoon(30000);  // would be 90000: keeps 60000
    TEST_ASSERT_TRUE(s.due(60000));
}

static void test_scheduler_report_soon_keeps_overdue_checkin_due() {
    CheckinScheduler s;
    s.netReady(true, 0);  // due at 60000
    s.reportSoon(70000);  // overdue already: stays due
    TEST_ASSERT_TRUE(s.due(70000));
}

static void test_scheduler_not_due_while_net_down() {
    CheckinScheduler s;
    TEST_ASSERT_FALSE(s.due(0));
    TEST_ASSERT_FALSE(s.due(10000000));
    s.netReady(true, 0);
    s.netReady(false, 30000);
    TEST_ASSERT_FALSE(s.due(70000));
    s.netReady(true, 100000);  // back up: first check-in 60 s later again
    TEST_ASSERT_FALSE(s.due(159999));
    TEST_ASSERT_TRUE(s.due(160000));
}

static void test_scheduler_net_ready_repeated_does_not_postpone() {
    CheckinScheduler s;
    s.netReady(true, 0);
    s.netReady(true, 50000);
    TEST_ASSERT_TRUE(s.due(60000));
}

static void test_scheduler_force_now() {
    CheckinScheduler s;
    s.netReady(true, 0);
    s.checkinDone(true, 60000);
    s.forceNow(70000);
    TEST_ASSERT_TRUE(s.due(70000));
}

static void test_scheduler_survives_millis_wrap() {
    CheckinScheduler s;
    uint32_t start = 0xFFFFFFFFu - 30000;
    s.netReady(true, start);
    TEST_ASSERT_FALSE(s.due(start + 59999));  // wrapped
    TEST_ASSERT_TRUE(s.due(start + 60000));
    s.checkinDone(true, start + 60000);
    TEST_ASSERT_FALSE(s.due(start + 60000 + 1799999));
    TEST_ASSERT_TRUE(s.due(start + 60000 + 1800000));
}

// --- update settings codec ---

static UpdateSettings fullSettings() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    strcpy(s.url, "http://10.132.0.45:8071");
    s.fromDhcp = true;
    s.hasKey = true;
    for (size_t i = 0; i < UPDATE_KEY_LEN; i++) s.key[i] = (uint8_t)(0xA0 + i);
    s.sequence = 7;
    return s;
}

static void test_update_settings_defaults_are_off() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    TEST_ASSERT_EQUAL_STRING("", s.url);
    TEST_ASSERT_FALSE(s.fromDhcp);
    TEST_ASSERT_FALSE(s.hasKey);
    TEST_ASSERT_EQUAL(UPDATE_SETTINGS_VERSION, s.version);
}

static void test_update_settings_round_trip() {
    UpdateSettings in = fullSettings(), out;
    char json[UPDATE_SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, updateSettingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_TRUE(updateSettingsFromJson(json, out));
    TEST_ASSERT_TRUE(updateSettingsEqual(in, out));
    TEST_ASSERT_EQUAL_MEMORY(in.key, out.key, UPDATE_KEY_LEN);
    TEST_ASSERT_EQUAL(7, out.sequence);
}

static void test_update_settings_without_key_round_trip() {
    UpdateSettings in = fullSettings(), out;
    in.hasKey = false;
    char json[UPDATE_SETTINGS_JSON_MAX];
    TEST_ASSERT_GREATER_THAN(0, updateSettingsToJson(in, json, sizeof(json)));
    TEST_ASSERT_TRUE(updateSettingsFromJson(json, out));
    TEST_ASSERT_FALSE(out.hasKey);
}

static void test_update_settings_worst_case_fits_512() {
    UpdateSettings in = fullSettings();
    memset(in.url, 0, sizeof(in.url));
    strcpy(in.url, "http://");
    memset(in.url + 7, 'h', 63);
    strcpy(in.url + 70, ":65535/");
    memset(in.url + 77, 'p', UPDATE_URL_MAX - 77);
    in.sequence = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL(UPDATE_URL_MAX, strlen(in.url));
    char json[UPDATE_SETTINGS_JSON_MAX];
    TEST_ASSERT_EQUAL(512, UPDATE_SETTINGS_JSON_MAX);
    TEST_ASSERT_GREATER_THAN(0, updateSettingsToJson(in, json, sizeof(json)));
    UpdateSettings out;
    TEST_ASSERT_TRUE(updateSettingsFromJson(json, out));
    TEST_ASSERT_TRUE(updateSettingsEqual(in, out));
}

static void test_update_settings_empty_url_is_valid() {
    UpdateSettings in = fullSettings(), out;
    in.url[0] = '\0';
    char json[UPDATE_SETTINGS_JSON_MAX];
    updateSettingsToJson(in, json, sizeof(json));
    TEST_ASSERT_TRUE(updateSettingsFromJson(json, out));
    TEST_ASSERT_EQUAL_STRING("", out.url);
}

static void test_update_settings_rejects_bad_url() {
    UpdateSettings out;
    TEST_ASSERT_FALSE(updateSettingsFromJson(
        "{\"version\":1,\"sequence\":1,\"url\":\"https://x\",\"fromDhcp\":false,\"key\":\"\"}", out));
}

static void test_update_settings_rejects_short_key() {
    UpdateSettings out;
    TEST_ASSERT_FALSE(updateSettingsFromJson(
        "{\"version\":1,\"sequence\":1,\"url\":\"\",\"fromDhcp\":false,\"key\":\"0011\"}", out));
}

static void test_update_settings_rejects_wrong_version() {
    UpdateSettings out;
    TEST_ASSERT_FALSE(updateSettingsFromJson(
        "{\"version\":2,\"sequence\":1,\"url\":\"\",\"fromDhcp\":false,\"key\":\"\"}", out));
    TEST_ASSERT_FALSE(updateSettingsFromJson("{\"sequence\":1,\"url\":\"\",\"fromDhcp\":false,\"key\":\"\"}", out));
}

static void test_update_settings_rejects_mistyped_fields() {
    UpdateSettings out;
    TEST_ASSERT_FALSE(updateSettingsFromJson(
        "{\"version\":1,\"sequence\":1,\"url\":5,\"fromDhcp\":false,\"key\":\"\"}", out));
    TEST_ASSERT_FALSE(updateSettingsFromJson(
        "{\"version\":1,\"sequence\":1,\"url\":\"\",\"fromDhcp\":\"yes\",\"key\":\"\"}", out));
    TEST_ASSERT_FALSE(updateSettingsFromJson("[]", out));
    TEST_ASSERT_FALSE(updateSettingsFromJson("", out));
    TEST_ASSERT_FALSE(updateSettingsFromJson(nullptr, out));
}

static void test_update_settings_every_truncation_rejected() {
    UpdateSettings in = fullSettings(), out;
    char json[UPDATE_SETTINGS_JSON_MAX];
    size_t len = updateSettingsToJson(in, json, sizeof(json));
    for (size_t cut = 0; cut < len; cut++) {
        char part[UPDATE_SETTINGS_JSON_MAX];
        memcpy(part, json, cut);
        part[cut] = '\0';
        TEST_ASSERT_FALSE(updateSettingsFromJson(part, out));
    }
}

static void test_update_settings_to_json_too_small_returns_zero() {
    UpdateSettings in = fullSettings();
    char json[40];
    TEST_ASSERT_EQUAL(0, updateSettingsToJson(in, json, sizeof(json)));
}

// --- update job (a download the site asked for) ---

static void test_job_same_version_does_nothing() {
    UpdateJob job;
    TEST_ASSERT_FALSE(job.begin("3.4.0", "3.4.0"));
    TEST_ASSERT_FALSE(job.active());
    TEST_ASSERT_TRUE(job.begin("3.4.1", "3.4.0"));
    TEST_ASSERT_TRUE(job.active());
    TEST_ASSERT_EQUAL_STRING("3.4.1", job.version());
}

static void test_job_downgrade_is_allowed() {
    UpdateJob job;
    TEST_ASSERT_TRUE(job.begin("3.4.0", "3.4.1-hil"));
}

static void test_download_version_mismatch_is_refused() {
    UpdateJob job;
    job.begin("3.4.1", "3.4.0");
    // GOLDEN_UPD's header says 3.4.0: not the version the site named
    TEST_ASSERT_EQUAL(UpdateJob::Step::Refuse, job.head(GOLDEN_UPD, UPD_HEAD_SIZE));
    TEST_ASSERT_FALSE(job.active());
    TEST_ASSERT_EQUAL_STRING("3.4.1 refused version mismatch", job.failLine());
}

static void test_download_matching_version_continues() {
    UpdateJob job;
    job.begin("3.4.0", "3.3.0");
    TEST_ASSERT_EQUAL(UpdateJob::Step::Continue, job.head(GOLDEN_UPD, UPD_HEAD_SIZE));
    TEST_ASSERT_TRUE(job.active());
    TEST_ASSERT_EQUAL_STRING("", job.failLine());
}

static void test_download_bad_header_is_refused() {
    UpdateJob job;
    job.begin("3.4.0", "3.3.0");
    uint8_t head[UPD_HEAD_SIZE];
    memset(head, 0, sizeof(head));
    TEST_ASSERT_EQUAL(UpdateJob::Step::Refuse, job.head(head, sizeof(head)));
    TEST_ASSERT_EQUAL_STRING("3.4.0 refused not an update file", job.failLine());
}

static void test_job_fail_line_formats_download_and_refused() {
    UpdateJob job;
    job.begin("3.4.1", "3.4.0");
    job.failDownload("connection closed");
    TEST_ASSERT_EQUAL_STRING("3.4.1 download connection closed", job.failLine());
    TEST_ASSERT_FALSE(job.active());
    job.begin("3.4.2", "3.4.0");
    job.refused("bad signature");
    TEST_ASSERT_EQUAL_STRING("3.4.2 refused bad signature", job.failLine());
}

static void test_job_fail_line_cleared_after_report() {
    UpdateJob job;
    job.begin("3.4.1", "3.4.0");
    job.failDownload("no answer");
    uint32_t seq = job.failSequence();
    job.reported(seq);
    TEST_ASSERT_EQUAL_STRING("", job.failLine());
}

static void test_job_newer_failure_not_cleared_by_older_report() {
    UpdateJob job;
    job.begin("3.4.1", "3.4.0");
    job.failDownload("no answer");
    uint32_t seq = job.failSequence();  // a check-in carries this one...
    job.begin("3.4.1", "3.4.0");
    job.failDownload("connection closed");  // ...while a newer failure happens
    job.reported(seq);
    TEST_ASSERT_EQUAL_STRING("3.4.1 download connection closed", job.failLine());
}

static void test_job_begin_keeps_fail_line_until_reported() {
    UpdateJob job;
    job.begin("3.4.1", "3.4.0");
    job.failDownload("no answer");
    job.begin("3.4.1", "3.4.0");  // retry: the earlier failure is still to be reported
    TEST_ASSERT_EQUAL_STRING("3.4.1 download no answer", job.failLine());
}

static void test_job_long_reason_is_cut_to_fit() {
    UpdateJob job;
    job.begin("3.4.1-1234567890", "3.4.0");
    char why[200];
    memset(why, 'x', sizeof(why) - 1);
    why[sizeof(why) - 1] = '\0';
    job.failDownload(why);
    TEST_ASSERT_TRUE(strlen(job.failLine()) < sizeof(CheckinInfo::fail));
}

static void test_checkin_last_text_for_each_result() {
    char out[48];
    checkinLastText(FwResult::None, "3.4.0", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("none", out);
    checkinLastText(FwResult::Installed, "3.4.0", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("installed 3.4.0", out);
    checkinLastText(FwResult::RolledBackCrash, "3.4.1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("rolledback-crash 3.4.1", out);
    checkinLastText(FwResult::RolledBackUnreachable, "3.4.1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("rolledback-unreachable 3.4.1", out);
    checkinLastText(FwResult::FallbackCrash, "3.4.1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("fallback-crash 3.4.1", out);
    checkinLastText(FwResult::Installed, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("installed", out);
    checkinLastText((FwResult)99, "3.4.1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("none", out);
}

// --- applying a change from the menu or the web page ---

static const char* KEY64 = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static void test_apply_sets_url_and_key() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    const char* error = nullptr;
    TEST_ASSERT_TRUE(updateSettingsApply(s, "http://10.0.0.1:8071", false, KEY64, false, error));
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.1:8071", s.url);
    TEST_ASSERT_TRUE(s.hasKey);
    TEST_ASSERT_EQUAL_UINT8(0x1f, s.key[31]);
}

static void test_apply_empty_key_keeps_key() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    const char* error = nullptr;
    updateSettingsApply(s, "http://a", false, KEY64, false, error);
    TEST_ASSERT_TRUE(updateSettingsApply(s, "http://b", true, "", false, error));
    TEST_ASSERT_TRUE(s.hasKey);
    TEST_ASSERT_EQUAL_UINT8(0x1f, s.key[31]);
    TEST_ASSERT_TRUE(s.fromDhcp);
    TEST_ASSERT_EQUAL_STRING("http://b", s.url);
}

static void test_apply_clear_key_wins() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    const char* error = nullptr;
    updateSettingsApply(s, "", false, KEY64, false, error);
    TEST_ASSERT_TRUE(updateSettingsApply(s, "", false, KEY64, true, error));
    TEST_ASSERT_FALSE(s.hasKey);
    for (size_t i = 0; i < UPDATE_KEY_LEN; i++) TEST_ASSERT_EQUAL_UINT8(0, s.key[i]);  // wiped, not only flagged
}

static void test_apply_refuses_bad_url_and_changes_nothing() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    const char* error = nullptr;
    updateSettingsApply(s, "http://a", false, KEY64, false, error);
    UpdateSettings before = s;
    TEST_ASSERT_FALSE(updateSettingsApply(s, "https://b", true, "", true, error));
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_TRUE(updateSettingsEqual(before, s));
    TEST_ASSERT_EQUAL(before.fromDhcp, s.fromDhcp);
}

static void test_apply_refuses_bad_key_and_changes_nothing() {
    UpdateSettings s;
    updateSettingsDefaults(s);
    const char* error = nullptr;
    UpdateSettings before = s;
    TEST_ASSERT_FALSE(updateSettingsApply(s, "http://a", false, "0011", false, error));
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_TRUE(updateSettingsEqual(before, s));
    TEST_ASSERT_FALSE(updateSettingsApply(s, "http://a", false, nullptr, false, error));
}

static void test_signed_slot_version_mismatch_is_refused() {
    UpdateJob job;
    job.begin("3.4.2", "3.4.0");
    // The outer header said 3.4.2, the signed slot header says 3.4.1: a mislabeled file
    TEST_ASSERT_EQUAL(UpdateJob::Step::Refuse, job.signedVersion("3.4.1"));
    TEST_ASSERT_FALSE(job.active());
    TEST_ASSERT_EQUAL_STRING("3.4.2 refused version mismatch", job.failLine());
}

static void test_signed_slot_version_match_continues() {
    UpdateJob job;
    job.begin("3.4.2", "3.4.0");
    TEST_ASSERT_EQUAL(UpdateJob::Step::Continue, job.signedVersion("3.4.2"));
    TEST_ASSERT_TRUE(job.active());
}

static void test_job_refuses_version_older_than_3_4_0() {
    // Older firmware never checks in again: refused on the device too, not only on the site
    UpdateJob job;
    TEST_ASSERT_FALSE(job.begin("3.3.0", "3.4.0"));
    TEST_ASSERT_FALSE(job.active());
    TEST_ASSERT_EQUAL_STRING("3.3.0 refused older than 3.4.0", job.failLine());
    TEST_ASSERT_FALSE(job.begin("3.4.0-rc1", "3.4.1"));
    TEST_ASSERT_TRUE(job.begin("3.4.0", "3.4.1"));  // the floor itself is allowed
}

static void test_download_deadline_scales_with_size() {
    TEST_ASSERT_EQUAL_UINT32(120000, downloadDeadlineMs(0));
    TEST_ASSERT_EQUAL_UINT32(120000 + 844064 / 2, downloadDeadlineMs(844064));  // 2 KB/s at least
    TEST_ASSERT_EQUAL_UINT32(120000 + 1761344 / 2, downloadDeadlineMs(1761344));
    TEST_ASSERT_EQUAL_UINT32(120000 + 0xFFFFFFFFu / 2, downloadDeadlineMs(0xFFFFFFFFu));  // no overflow
}

static void test_scheduler_force_survives_net_ready_edge() {
    // "checkin" typed in the same loop pass as a settings save (net ready drops and returns),
    // or before the network is up after boot: the forced check-in must still happen at once
    CheckinScheduler s;
    s.netReady(true, 0);
    s.netReady(false, 1000);  // a settings save
    s.forceNow(1000);
    s.netReady(true, 1001);   // the next poll
    TEST_ASSERT_TRUE(s.due(1001));
    s.checkinDone(true, 1002);
    TEST_ASSERT_FALSE(s.due(1003));  // done: back to the normal plan
}

static void test_scheduler_force_waits_for_network() {
    CheckinScheduler s;
    s.forceNow(0);
    TEST_ASSERT_FALSE(s.due(0));
    s.netReady(true, 5000);
    TEST_ASSERT_TRUE(s.due(5000));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_url_accepts_name_port_path);
    RUN_TEST(test_url_accepts_ipv4_and_bracketed_ipv6);
    RUN_TEST(test_url_default_port_80);
    RUN_TEST(test_url_rejects_https);
    RUN_TEST(test_url_rejects_128_chars);
    RUN_TEST(test_url_rejects_host_or_path_over_63);
    RUN_TEST(test_url_rejects_port_0_and_65536);
    RUN_TEST(test_url_rejects_empty_host);
    RUN_TEST(test_url_rejects_userinfo);
    RUN_TEST(test_url_rejects_space_and_control);
    RUN_TEST(test_http_response_reads_status_and_length);
    RUN_TEST(test_http_response_head_in_pieces);
    RUN_TEST(test_http_response_rejects_chunked);
    RUN_TEST(test_http_response_rejects_missing_length);
    RUN_TEST(test_http_response_rejects_bad_status_line);
    RUN_TEST(test_http_response_rejects_bad_length);
    RUN_TEST(test_http_response_rejects_head_over_1024_bytes);
    RUN_TEST(test_http_response_rejects_body_over_max);
    RUN_TEST(test_http_response_every_truncation_is_incomplete_or_malformed);
    RUN_TEST(test_http_response_after_head_consumes_nothing);
    RUN_TEST(test_upd_header_version_from_golden_upd);
    RUN_TEST(test_upd_header_rejects_bad_magic_and_short);
    RUN_TEST(test_compare_versions_orders_3_10_0_after_3_9_0);
    RUN_TEST(test_compare_versions_prerelease_below_release);
    RUN_TEST(test_scheduler_first_checkin_60s_after_net_ready);
    RUN_TEST(test_scheduler_next_after_30_min);
    RUN_TEST(test_scheduler_retry_after_5_min_on_failure);
    RUN_TEST(test_scheduler_report_soon_is_60s);
    RUN_TEST(test_scheduler_report_soon_never_delays);
    RUN_TEST(test_scheduler_report_soon_keeps_overdue_checkin_due);
    RUN_TEST(test_scheduler_not_due_while_net_down);
    RUN_TEST(test_scheduler_net_ready_repeated_does_not_postpone);
    RUN_TEST(test_scheduler_force_now);
    RUN_TEST(test_scheduler_survives_millis_wrap);
    RUN_TEST(test_update_settings_defaults_are_off);
    RUN_TEST(test_update_settings_round_trip);
    RUN_TEST(test_update_settings_without_key_round_trip);
    RUN_TEST(test_update_settings_worst_case_fits_512);
    RUN_TEST(test_update_settings_empty_url_is_valid);
    RUN_TEST(test_update_settings_rejects_bad_url);
    RUN_TEST(test_update_settings_rejects_short_key);
    RUN_TEST(test_update_settings_rejects_wrong_version);
    RUN_TEST(test_update_settings_rejects_mistyped_fields);
    RUN_TEST(test_update_settings_every_truncation_rejected);
    RUN_TEST(test_update_settings_to_json_too_small_returns_zero);
    RUN_TEST(test_job_same_version_does_nothing);
    RUN_TEST(test_job_downgrade_is_allowed);
    RUN_TEST(test_download_version_mismatch_is_refused);
    RUN_TEST(test_download_matching_version_continues);
    RUN_TEST(test_download_bad_header_is_refused);
    RUN_TEST(test_job_fail_line_formats_download_and_refused);
    RUN_TEST(test_job_fail_line_cleared_after_report);
    RUN_TEST(test_job_newer_failure_not_cleared_by_older_report);
    RUN_TEST(test_job_begin_keeps_fail_line_until_reported);
    RUN_TEST(test_job_long_reason_is_cut_to_fit);
    RUN_TEST(test_checkin_last_text_for_each_result);
    RUN_TEST(test_apply_sets_url_and_key);
    RUN_TEST(test_apply_empty_key_keeps_key);
    RUN_TEST(test_apply_clear_key_wins);
    RUN_TEST(test_apply_refuses_bad_url_and_changes_nothing);
    RUN_TEST(test_apply_refuses_bad_key_and_changes_nothing);
    RUN_TEST(test_signed_slot_version_mismatch_is_refused);
    RUN_TEST(test_signed_slot_version_match_continues);
    RUN_TEST(test_job_refuses_version_older_than_3_4_0);
    RUN_TEST(test_download_deadline_scales_with_size);
    RUN_TEST(test_scheduler_force_survives_net_ready_edge);
    RUN_TEST(test_scheduler_force_waits_for_network);
    return UNITY_END();
}
