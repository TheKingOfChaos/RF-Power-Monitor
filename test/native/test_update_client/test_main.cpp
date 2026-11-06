// Host unit tests for the update client's URL and HTTP response parsing, .upd
// header version, version order and check-in timing (lib/update_client).
// Run: pio test -e native -f native/test_update_client

#include <stdio.h>
#include <string.h>

#include <unity.h>
#include <http_response.h>
#include <update_schedule.h>
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
    return UNITY_END();
}
