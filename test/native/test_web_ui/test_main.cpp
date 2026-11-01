// Host unit tests for lib/web_ui: SHA-256, passwords, sessions, login throttle,
// the HTTP request parser, form/cookie helpers and the web page logic.
// Run: pio test -e native -f native/test_web_ui

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sha256.h>
#include <web_auth.h>
#include <http_request.h>
#include <web_app.h>
#include <bootstrap_css.h>
#include <settings_codec.h>

void setUp() {}
void tearDown() {}

static std::string hex(const uint8_t* d, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "%02x", d[i]);
        s += b;
    }
    return s;
}

static std::string sha(const std::string& in) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(in.data(), in.size(), d);
    return hex(d, sizeof(d));
}

// ---- SHA-256 (FIPS 180-4 / NIST test vectors) ----

static void test_sha256_known_vectors() {
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", sha("").c_str());
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", sha("abc").c_str());
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                             sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

static void test_sha256_million_a() {
    Sha256 s;
    std::string block(1000, 'a');
    for (int i = 0; i < 1000; i++) s.update(block.data(), block.size());
    uint8_t d[SHA256_DIGEST_LEN];
    s.finish(d);
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", hex(d, 32).c_str());
}

static void test_sha256_split_anywhere_equals_one_shot() {
    // Lengths around the 55/56/64-byte padding boundaries, split at every position
    for (size_t len = 50; len <= 130; len++) {
        std::string msg;
        for (size_t i = 0; i < len; i++) msg += (char)('a' + i % 26);
        std::string expected = sha(msg);
        for (size_t cut = 0; cut <= len; cut++) {
            Sha256 s;
            s.update(msg.data(), cut);
            s.update(msg.data() + cut, len - cut);
            uint8_t d[SHA256_DIGEST_LEN];
            s.finish(d);
            TEST_ASSERT_EQUAL_STRING(expected.c_str(), hex(d, 32).c_str());
        }
    }
}

// ---- passwords ----

static void test_password_rules() {
    TEST_ASSERT_FALSE(webPasswordIsValid(nullptr));
    TEST_ASSERT_FALSE(webPasswordIsValid(""));
    TEST_ASSERT_FALSE(webPasswordIsValid("1234567"));                            // 7
    TEST_ASSERT_TRUE(webPasswordIsValid("12345678"));                            // 8
    TEST_ASSERT_TRUE(webPasswordIsValid("12345678901234567890123456789012"));    // 32
    TEST_ASSERT_FALSE(webPasswordIsValid("123456789012345678901234567890123"));  // 33
    TEST_ASSERT_FALSE(webPasswordIsValid("with space"));
    TEST_ASSERT_FALSE(webPasswordIsValid("tab\there1"));
    TEST_ASSERT_FALSE(webPasswordIsValid("del\x7fhere1"));
    TEST_ASSERT_FALSE(webPasswordIsValid("\xc3\xa6\xc3\xb8\xc3\xa5\xc3\xa6\xc3\xb8"));  // non-ASCII
    TEST_ASSERT_TRUE(webPasswordIsValid("!~\"'<>&%#$"));
}

static const uint8_t SALT[WEB_SALT_LEN] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static void test_password_hash_matches_independent_implementation() {
    // Computed with Python hashlib: d = sha256(salt + pw); 1999 x d = sha256(d + salt + pw)
    char out[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, out);
    TEST_ASSERT_EQUAL_STRING(
        "000102030405060708090a0b0c0d0e0f$e26c162ef26c955d928e62311a429a4cf549b87f3f889489c46dd262e14c3356", out);
    TEST_ASSERT_TRUE(webPasswordHashIsWellFormed(out));
}

static void test_password_check() {
    char stored[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, stored);
    TEST_ASSERT_TRUE(webCheckPassword("correct-horse", stored));
    TEST_ASSERT_FALSE(webCheckPassword("correct-hors", stored));
    TEST_ASSERT_FALSE(webCheckPassword("correct-horsee", stored));
    TEST_ASSERT_FALSE(webCheckPassword("Correct-horse", stored));
    TEST_ASSERT_FALSE(webCheckPassword("", stored));
    TEST_ASSERT_FALSE(webCheckPassword(nullptr, stored));
}

static void test_salt_changes_the_hash() {
    uint8_t other[WEB_SALT_LEN];
    memcpy(other, SALT, sizeof(other));
    other[15] ^= 1;
    char a[WEB_PASSWORD_HASH_LEN + 1], b[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, a);
    webHashPassword("correct-horse", other, b);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a + 33, b + 33));
    TEST_ASSERT_TRUE(webCheckPassword("correct-horse", b));
}

static void test_malformed_stored_hash_never_matches() {
    char stored[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, stored);
    std::string good = stored;
    const std::string bad[] = {"", "x", good.substr(0, WEB_PASSWORD_HASH_LEN - 1), good + "0"};
    for (const std::string& b : bad) TEST_ASSERT_FALSE(webCheckPassword("correct-horse", b.c_str()));
    std::string upper = good;
    upper[40] = (char)toupper(upper[40]) == upper[40] ? 'A' : (char)toupper(upper[40]);
    TEST_ASSERT_FALSE(webPasswordHashIsWellFormed(upper.c_str()));
    std::string noDollar = good;
    noDollar[32] = '0';
    TEST_ASSERT_FALSE(webPasswordHashIsWellFormed(noDollar.c_str()));
    TEST_ASSERT_FALSE(webCheckPassword("correct-horse", noDollar.c_str()));
    TEST_ASSERT_FALSE(webPasswordHashIsWellFormed(nullptr));
}

/// Run a stepwise check to the end; returns the number of step() calls
static int runCheck(WebPasswordCheck& c, uint32_t perStep) {
    int calls = 1;
    while (!c.step(perStep)) calls++;
    return calls;
}

static void test_stepwise_check_equals_one_shot_check() {
    char stored[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, stored);
    std::string longPw(WEB_PASSWORD_MAX + 5, 'p');
    const char* passwords[] = {"correct-horse", "correct-hors", "Correct-horse", "", longPw.c_str()};
    const uint32_t steps[] = {1, 40, 1999, 2000, 100000};
    WebPasswordCheck c;
    for (const char* pw : passwords) {
        for (uint32_t perStep : steps) {
            c.begin(pw, stored);
            runCheck(c, perStep);
            TEST_ASSERT_TRUE(c.done());
            TEST_ASSERT_EQUAL_MESSAGE(webCheckPassword(pw, stored), c.matches(), pw);
        }
    }
    c.begin("correct-horse", stored);
    runCheck(c, 7);
    TEST_ASSERT_TRUE(c.matches());
}

static void test_stepwise_check_runs_all_rounds_even_for_bad_input() {
    char stored[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, stored);
    WebPasswordCheck c;
    c.begin("correct-horse", stored);
    TEST_ASSERT_EQUAL((int)(WEB_HASH_ITERATIONS / 40), runCheck(c, 40));
    // A malformed stored hash or an over-long password takes just as long and never matches
    c.begin("correct-horse", "not-a-hash");
    TEST_ASSERT_EQUAL((int)(WEB_HASH_ITERATIONS / 40), runCheck(c, 40));
    TEST_ASSERT_FALSE(c.matches());
    std::string longPw(WEB_PASSWORD_MAX + 1, 'x');
    c.begin(longPw.c_str(), stored);
    TEST_ASSERT_EQUAL((int)(WEB_HASH_ITERATIONS / 40), runCheck(c, 40));
    TEST_ASSERT_FALSE(c.matches());
    c.begin(nullptr, stored);
    runCheck(c, 2000);
    TEST_ASSERT_FALSE(c.matches());
}

static void test_stepwise_check_state() {
    char stored[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword("correct-horse", SALT, stored);
    WebPasswordCheck c;
    TEST_ASSERT_FALSE(c.active());
    TEST_ASSERT_FALSE(c.step(10));  // nothing started
    TEST_ASSERT_FALSE(c.matches());
    c.begin("correct-horse", stored);
    TEST_ASSERT_TRUE(c.active());
    TEST_ASSERT_FALSE(c.step(10));
    TEST_ASSERT_FALSE(c.done());
    TEST_ASSERT_FALSE(c.matches());  // not before it is finished
    runCheck(c, 5000);
    TEST_ASSERT_TRUE(c.matches());
    TEST_ASSERT_TRUE(c.step(10));    // stays finished
    c.clear();
    TEST_ASSERT_FALSE(c.active());
    TEST_ASSERT_FALSE(c.matches());
}

static void test_default_password_uses_the_unambiguous_alphabet() {
    uint8_t random[WEB_DEFAULT_PASSWORD_LEN];
    for (int round = 0; round < 26; round++) {
        for (size_t i = 0; i < sizeof(random); i++) random[i] = (uint8_t)(round * 10 + i);
        char pw[WEB_DEFAULT_PASSWORD_LEN + 1];
        webMakeDefaultPassword(random, pw);
        TEST_ASSERT_EQUAL(WEB_DEFAULT_PASSWORD_LEN, strlen(pw));
        TEST_ASSERT_TRUE(webPasswordIsValid(pw));
        for (const char* c = pw; *c; c++) TEST_ASSERT_NULL(strchr("01ol", *c));
    }
    uint8_t zeros[WEB_DEFAULT_PASSWORD_LEN] = {0};
    char pw[WEB_DEFAULT_PASSWORD_LEN + 1];
    webMakeDefaultPassword(zeros, pw);
    TEST_ASSERT_EQUAL_STRING("aaaaaaaaaa", pw);
}

static void test_constant_time_equals() {
    TEST_ASSERT_TRUE(webConstantTimeEquals("abc", "abc"));
    TEST_ASSERT_TRUE(webConstantTimeEquals("", ""));
    TEST_ASSERT_FALSE(webConstantTimeEquals("abc", "abd"));
    TEST_ASSERT_FALSE(webConstantTimeEquals("abc", "ab"));
    TEST_ASSERT_FALSE(webConstantTimeEquals("ab", "abc"));
    TEST_ASSERT_FALSE(webConstantTimeEquals("", "a"));
    TEST_ASSERT_FALSE(webConstantTimeEquals(nullptr, "a"));
    TEST_ASSERT_FALSE(webConstantTimeEquals("a", nullptr));
}

// ---- sessions ----

static void fillRandom(uint8_t* r, uint8_t seed) {
    for (size_t i = 0; i < WEB_SESSION_RANDOM; i++) r[i] = (uint8_t)(seed + i * 7);
}

static void test_session_tokens_are_hex_and_different() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 1);
    size_t slot = s.create(r, 1000);
    TEST_ASSERT_EQUAL(WEB_TOKEN_HEX, strlen(s.token(slot)));
    TEST_ASSERT_EQUAL(WEB_TOKEN_HEX, strlen(s.csrf(slot)));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(s.token(slot), s.csrf(slot)));
    TEST_ASSERT_EQUAL_STRING(hex(r, 16).c_str(), s.token(slot));
    TEST_ASSERT_EQUAL_STRING(hex(r + 16, 16).c_str(), s.csrf(slot));
}

static void test_session_found_until_idle_too_long() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 1);
    size_t slot = s.create(r, 1000);
    std::string token = s.token(slot);
    TEST_ASSERT_EQUAL((int)slot, s.find(token.c_str(), 1000 + WEB_SESSION_IDLE_MS));  // refreshes
    TEST_ASSERT_EQUAL((int)slot, s.find(token.c_str(), 1000 + 2 * WEB_SESSION_IDLE_MS));
    TEST_ASSERT_EQUAL(-1, s.find(token.c_str(), 1000 + 3 * WEB_SESSION_IDLE_MS + 1));
    TEST_ASSERT_EQUAL(0, s.count());
}

static void test_session_survives_millis_wrap() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 3);
    size_t slot = s.create(r, 0xFFFFFF00u);
    std::string token = s.token(slot);
    TEST_ASSERT_EQUAL((int)slot, s.find(token.c_str(), 0x00001000u));
}

static void test_unknown_or_malformed_tokens_are_not_found() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 1);
    size_t slot = s.create(r, 0);
    std::string token = s.token(slot);
    std::string wrong = token;
    wrong[31] = wrong[31] == 'a' ? 'b' : 'a';
    TEST_ASSERT_EQUAL(-1, s.find(wrong.c_str(), 1));
    TEST_ASSERT_EQUAL(-1, s.find(token.substr(0, 31).c_str(), 1));
    TEST_ASSERT_EQUAL(-1, s.find((token + "0").c_str(), 1));
    TEST_ASSERT_EQUAL(-1, s.find("", 1));
    TEST_ASSERT_EQUAL(-1, s.find(nullptr, 1));
    TEST_ASSERT_EQUAL(-1, s.find(s.csrf(slot), 1));  // the CSRF token is not a session token
}

static void test_new_login_replaces_least_recently_used_session() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 1);
    std::string a = s.token(s.create(r, 1000));
    fillRandom(r, 2);
    std::string b = s.token(s.create(r, 2000));
    TEST_ASSERT_TRUE(s.find(a.c_str(), 3000) >= 0);  // a is now the most recent
    fillRandom(r, 3);
    std::string c = s.token(s.create(r, 4000));
    TEST_ASSERT_TRUE(s.find(a.c_str(), 5000) >= 0);
    TEST_ASSERT_EQUAL(-1, s.find(b.c_str(), 5000));
    TEST_ASSERT_TRUE(s.find(c.c_str(), 5000) >= 0);
}

static void test_remove_and_clear_end_sessions() {
    WebSessions s;
    uint8_t r[WEB_SESSION_RANDOM];
    fillRandom(r, 1);
    size_t a = s.create(r, 0);
    std::string ta = s.token(a);
    fillRandom(r, 2);
    std::string tb = s.token(s.create(r, 0));
    s.remove(a);
    TEST_ASSERT_EQUAL(-1, s.find(ta.c_str(), 1));
    TEST_ASSERT_TRUE(s.find(tb.c_str(), 1) >= 0);
    s.clear();
    TEST_ASSERT_EQUAL(-1, s.find(tb.c_str(), 1));
    TEST_ASSERT_EQUAL(0, s.count());
}

// ---- login throttle ----

static void test_throttle_locks_after_five_failures_for_30_s() {
    LoginThrottle t;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(t.allowed(1000));
        t.failed(1000);
    }
    TEST_ASSERT_TRUE(t.allowed(1000));
    t.failed(1000);
    TEST_ASSERT_FALSE(t.allowed(1001));
    TEST_ASSERT_EQUAL_UINT32(30, t.secondsLeft(1001));
    TEST_ASSERT_FALSE(t.allowed(1000 + WEB_LOGIN_LOCKOUT_MS - 1));
    TEST_ASSERT_TRUE(t.allowed(1000 + WEB_LOGIN_LOCKOUT_MS));
    TEST_ASSERT_EQUAL_UINT32(0, t.secondsLeft(1000 + WEB_LOGIN_LOCKOUT_MS));
}

static void test_throttle_success_resets_the_count() {
    LoginThrottle t;
    for (int i = 0; i < 4; i++) t.failed(0);
    t.succeeded();
    for (int i = 0; i < 4; i++) t.failed(0);
    TEST_ASSERT_TRUE(t.allowed(0));
}

static void test_throttle_survives_millis_wrap() {
    LoginThrottle t;
    for (int i = 0; i < 5; i++) t.failed(0xFFFFF000u);
    TEST_ASSERT_FALSE(t.allowed(0x00000100u));
    TEST_ASSERT_TRUE(t.allowed(0xFFFFF000u + WEB_LOGIN_LOCKOUT_MS));
}

// ---- HTTP request parser ----

static HttpParse feedAll(HttpRequest& r, const std::string& s) {
    return r.feed((const uint8_t*)s.data(), s.size());
}

static const std::string POST_LOGIN =
    "POST /login HTTP/1.1\r\nHost: 10.10.6.111\r\nContent-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 31\r\n\r\nuser=admin&password=goodpasswd1";

static void test_parse_simple_get() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, "GET /?x=1 HTTP/1.1\r\nHost: a\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpMethod::Get, r.method());
    TEST_ASSERT_EQUAL_STRING("/", r.path());
    TEST_ASSERT_EQUAL(0, r.bodyLength());
}

static void test_parse_post_with_body() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, POST_LOGIN));
    TEST_ASSERT_EQUAL(HttpMethod::Post, r.method());
    TEST_ASSERT_EQUAL_STRING("/login", r.path());
    TEST_ASSERT_EQUAL(31, r.bodyLength());
    TEST_ASSERT_EQUAL_MEMORY("user=admin&password=goodpasswd1", r.body(), 31);
}

static void test_every_prefix_is_incomplete_and_byte_by_byte_works() {
    for (size_t cut = 0; cut < POST_LOGIN.size(); cut++) {
        HttpRequest r;
        TEST_ASSERT_EQUAL(HttpParse::Incomplete, feedAll(r, POST_LOGIN.substr(0, cut)));
    }
    HttpRequest r;
    HttpParse st = HttpParse::Incomplete;
    for (char c : POST_LOGIN) st = r.feed((const uint8_t*)&c, 1);
    TEST_ASSERT_EQUAL(HttpParse::Complete, st);
    TEST_ASSERT_EQUAL_MEMORY("user=admin&password=goodpasswd1", r.body(), 31);
}

static void test_headers_case_insensitive_and_trimmed() {
    HttpRequest r;
    feedAll(r, "GET / HTTP/1.1\r\ncOOKIE:   sid=abc ; x=1  \r\nX-Long: 0123456789\r\n\r\n");
    char v[32];
    TEST_ASSERT_TRUE(r.header("Cookie", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("sid=abc ; x=1", v);
    TEST_ASSERT_FALSE(r.header("Host", v, sizeof(v)));
    TEST_ASSERT_FALSE(r.header("X-Long", v, 10));  // does not fit
    TEST_ASSERT_FALSE(r.header("Cook", v, sizeof(v)));
}

static void test_too_large_requests_are_refused() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::TooLarge,
                      feedAll(r, "POST /x HTTP/1.1\r\nContent-Length: 4000\r\n\r\n"));
    HttpRequest r2;
    TEST_ASSERT_EQUAL(HttpParse::TooLarge,
                      feedAll(r2, "POST /x HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"));
    HttpRequest r3;
    std::string big = "GET / HTTP/1.1\r\nX: " + std::string(HTTP_REQUEST_MAX, 'a');
    TEST_ASSERT_EQUAL(HttpParse::TooLarge, feedAll(r3, big));
    // Exactly at the limit is still accepted
    HttpRequest r4;
    std::string head = "POST /x HTTP/1.1\r\nContent-Length: ";
    std::string tail = "\r\n\r\n";
    size_t bodyLen = HTTP_REQUEST_MAX - head.size() - 4 - tail.size();
    std::string req = head + std::to_string(bodyLen) + tail + std::string(bodyLen, 'b');
    TEST_ASSERT_EQUAL(HTTP_REQUEST_MAX, req.size());
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r4, req));
}

static void test_malformed_requests_are_bad() {
    const char* bad[] = {
        "GET\r\n\r\n",
        "GET /\r\n\r\n",
        "GET / FTP/1.0\r\n\r\n",
        "GET x HTTP/1.1\r\n\r\n",
        "GET  HTTP/1.1\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\nab",
        "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n",
        "GET / HTTP/1.1\r\n\r\nextra",
    };
    for (const char* b : bad) {
        HttpRequest r;
        TEST_ASSERT_EQUAL_MESSAGE(HttpParse::Bad, feedAll(r, b), b);
    }
    HttpRequest r;
    std::string withNul("GET / HTTP/1.1\r\nX: a\0b\r\n\r\n", 26);
    TEST_ASSERT_EQUAL(HttpParse::Bad, feedAll(r, withNul));
}

static void test_other_methods_and_long_paths() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, "DELETE / HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpMethod::Other, r.method());
    HttpRequest r2;
    std::string path = "/" + std::string(200, 'p');
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r2, "GET " + path + " HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL(HTTP_PATH_MAX, strlen(r2.path()));
}

static void test_reset_allows_a_new_request() {
    HttpRequest r;
    feedAll(r, "GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, "more"));  // stays complete
    r.reset();
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, POST_LOGIN));
    TEST_ASSERT_EQUAL_STRING("/login", r.path());
}

// ---- form, cookie, text helpers ----

static void test_form_values_are_decoded() {
    const char* body = "a=1&community=pub+lic%21%2B&empty=&flag&ab=2";
    size_t len = strlen(body);
    char v[32];
    TEST_ASSERT_TRUE(formValue(body, len, "community", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("pub lic!+", v);
    TEST_ASSERT_TRUE(formValue(body, len, "a", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("1", v);
    TEST_ASSERT_TRUE(formValue(body, len, "ab", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("2", v);
    TEST_ASSERT_TRUE(formValue(body, len, "empty", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("", v);
    TEST_ASSERT_TRUE(formValue(body, len, "flag", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("", v);
    TEST_ASSERT_FALSE(formValue(body, len, "comm", v, sizeof(v)));
    TEST_ASSERT_FALSE(formValue(body, len, "missing", v, sizeof(v)));
}

static void test_form_value_rejects_bad_escapes_nul_and_overflow() {
    char v[8];
    const char* bad[] = {"x=%", "x=%4", "x=%G1", "x=%00", "x=ab%0"};
    for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(formValue(b, strlen(b), "x", v, sizeof(v)), b);
    TEST_ASSERT_TRUE(formValue("x=1234567", 9, "x", v, sizeof(v)));
    TEST_ASSERT_FALSE(formValue("x=12345678", 10, "x", v, sizeof(v)));
    // The length limits the search: a field past len is not seen
    TEST_ASSERT_FALSE(formValue("a=1&x=2", 3, "x", v, sizeof(v)));
}

static void test_cookie_values() {
    char v[40];
    TEST_ASSERT_TRUE(cookieValue("a=1; sid=abc; b=2", "sid", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("abc", v);
    TEST_ASSERT_TRUE(cookieValue("sid=xyz", "sid", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("xyz", v);
    TEST_ASSERT_FALSE(cookieValue("xsid=1; sidx=2", "sid", v, sizeof(v)));
    TEST_ASSERT_FALSE(cookieValue("", "sid", v, sizeof(v)));
    TEST_ASSERT_FALSE(cookieValue("sid=0123456789", "sid", v, 5));
}

static void test_text_buffer_escapes_and_stops_when_full() {
    char buf[64];
    TextBuffer b(buf, sizeof(buf));
    b.addEscaped("<a href=\"x\">'&'</a>");
    TEST_ASSERT_EQUAL_STRING("&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;", b.c_str());
    TEST_ASSERT_TRUE(b.ok());
    char small[8];
    TextBuffer s(small, sizeof(small));
    s.add("1234567");
    TEST_ASSERT_TRUE(s.ok());
    s.add("8");
    TEST_ASSERT_FALSE(s.ok());
    s.add("");
    TEST_ASSERT_EQUAL_STRING("1234567", s.c_str());
}

// ---- web page logic with a fake backend ----

class FakeBackend : public WebBackend {
public:
    std::string password = "goodpasswd1";
    bool saveOk = true;
    int passwordChecks = 0, networkSaves = 0, communitySaves = 0, hostnameSaves = 0, reboots = 0, disables = 0;
    int ipv6Saves = 0;
    bool savedDhcp = false;
    std::string savedIP, savedMask, savedGateway, savedCommunity, savedHostname, savedIPv6, savedIPv6Gateway;
    int failed = 0, locked = 0, loggedIn = 0;
    uint8_t seed = 1;
    WebStatus st;

    FakeBackend() {
        memset(&st, 0, sizeof(st));
        st.version = "2.0.0";
        st.uptimeSeconds = 90061;
        st.mainsKnown = true;
        st.mainsPresent = true;
        st.mainsChanges = 4;
        st.cpuTempDeciC = 404;
        st.ambientTempValid = true;
        st.ambientTempDeciC = 271;
        st.linkUp = true;
        st.linkMbps = 100;
        strcpy(st.ip, "10.10.6.111");
        strcpy(st.liveMask, "255.255.252.0");
        strcpy(st.liveGateway, "10.132.0.1");
        strcpy(st.dns, "10.4.0.7");
        strcpy(st.ipv6, "fd16:d833:4f0e:3ece:1cb2:cfff:fe4e:a22c");
        strcpy(st.ipv6LinkLocal, "fe80::1cb2:cfff:fe4e:a22c");
        strcpy(st.mac, "1E:B2:CF:4E:A2:2C");
        st.dhcp = true;
        strcpy(st.staticIP, "10.10.6.249");
        strcpy(st.subnetMask, "255.255.0.0");
        strcpy(st.gateway, "10.10.0.1");
        strcpy(st.community, "public");
        strcpy(st.hostname, "rf-ups-monitor");
    }
    void status(WebStatus& out) override { out = st; }
    bool checkPassword(const char* p) override {
        passwordChecks++;
        return password == p;
    }
    bool saveNetwork(bool dhcp, const char* ip, const char* mask, const char* gw) override {
        networkSaves++;
        savedDhcp = dhcp;
        savedIP = ip;
        savedMask = mask;
        savedGateway = gw;
        return saveOk;
    }
    bool saveIPv6(const char* address, const char* gateway) override {
        ipv6Saves++;
        savedIPv6 = address;
        savedIPv6Gateway = gateway;
        return saveOk;
    }
    bool saveCommunity(const char* c) override {
        communitySaves++;
        savedCommunity = c;
        if (saveOk) strcpy(st.community, c);
        return saveOk;
    }
    bool saveHostname(const char* h) override {
        hostnameSaves++;
        savedHostname = h;
        if (saveOk) strcpy(st.hostname, h);
        return saveOk;
    }
    bool disableWeb() override {
        disables++;
        return saveOk;
    }
    void requestReboot() override { reboots++; }
    void random(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(seed * 31 + i);
        seed++;
    }
    void log(const SourceKey&, WebEvent e) override {
        if (e == WebEvent::LoginFailed) failed++;
        if (e == WebEvent::LoginLocked) locked++;
        if (e == WebEvent::LoggedIn) loggedIn++;
    }
};

static WebApp* app;  // large buffers: on the heap, fresh for every test
static FakeBackend* fb;

static std::string send(const std::string& raw, uint32_t now = 1000) {
    static HttpRequest req;
    req.reset();
    req.feed((const uint8_t*)raw.data(), raw.size());
    size_t len = 0;
    const char* resp = app->handle(req, 0x0A0A0641, now, *fb, len);
    TEST_ASSERT_EQUAL(strlen(resp), len);
    return std::string(resp, len);
}

static std::string post(const std::string& path, const std::string& body, const std::string& cookie = "",
                        uint32_t now = 1000) {
    std::string r = "POST " + path + " HTTP/1.1\r\nHost: d\r\n";
    if (!cookie.empty()) r += "Cookie: sid=" + cookie + "\r\n";
    r += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
    return send(r, now);
}

static std::string get(const std::string& path, const std::string& cookie = "", uint32_t now = 1000) {
    std::string r = "GET " + path + " HTTP/1.1\r\nHost: d\r\n";
    if (!cookie.empty()) r += "Cookie: sid=" + cookie + "\r\n";
    return send(r + "\r\n", now);
}

static int statusCode(const std::string& resp) { return atoi(resp.c_str() + 9); }

static std::string between(const std::string& s, const std::string& start, const std::string& end) {
    size_t a = s.find(start);
    if (a == std::string::npos) return "";
    a += start.size();
    size_t b = s.find(end, a);
    return b == std::string::npos ? "" : s.substr(a, b - a);
}

/// Log in and return the session token; csrf receives the page's CSRF token
static std::string logIn(std::string& csrf, uint32_t now = 1000) {
    std::string resp = post("/login", "user=admin&password=goodpasswd1", "", now);
    TEST_ASSERT_EQUAL_MESSAGE(303, statusCode(resp), resp.c_str());
    std::string token = between(resp, "Set-Cookie: sid=", ";");
    TEST_ASSERT_EQUAL(WEB_TOKEN_HEX, token.size());
    std::string page = get("/", token, now);
    csrf = between(page, "name=\"csrf\" value=\"", "\"");
    TEST_ASSERT_EQUAL(WEB_TOKEN_HEX, csrf.size());
    return token;
}

static void webSetUp() {
    static FakeBackend backend;
    backend = FakeBackend();
    fb = &backend;
    delete app;
    app = new WebApp();
}

static void test_start_page_without_session_is_the_login_form() {
    webSetUp();
    std::string resp = get("/");
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("action=\"/login\"") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("type=\"password\"") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("Restart device") == std::string::npos);
    TEST_ASSERT_TRUE(resp.find("10.10.6.111") == std::string::npos);  // no status before login
}

static void test_every_response_has_security_headers_and_correct_length() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string responses[] = {get("/"), get("/", token), get("/nope"), post("/login", "user=x&password=y"),
                               post("/reboot", "csrf=" + csrf, token), send("BAD\r\n\r\n")};
    for (const std::string& r : responses) {
        TEST_ASSERT_TRUE(r.find("Content-Security-Policy: default-src 'none'") != std::string::npos);
        TEST_ASSERT_TRUE(r.find("X-Frame-Options: DENY") != std::string::npos);
        TEST_ASSERT_TRUE(r.find("Cache-Control: no-store") != std::string::npos);
        size_t bodyStart = r.find("\r\n\r\n") + 4;
        TEST_ASSERT_EQUAL(atoi(between(r, "Content-Length: ", "\r\n").c_str()), r.size() - bodyStart);
    }
}

static void test_login_with_right_password_sets_a_strict_session_cookie() {
    webSetUp();
    std::string resp = post("/login", "user=admin&password=goodpasswd1");
    TEST_ASSERT_EQUAL(303, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Location: /\r\n") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("; Path=/; HttpOnly; SameSite=Strict\r\n") != std::string::npos);
    TEST_ASSERT_EQUAL(1, fb->loggedIn);
    std::string page = get("/", between(resp, "Set-Cookie: sid=", ";"));
    TEST_ASSERT_TRUE(page.find("Restart device") != std::string::npos);
}

static void test_wrong_password_or_user_is_refused_and_logged() {
    webSetUp();
    std::string resp = post("/login", "user=admin&password=wrongpasswd");
    TEST_ASSERT_EQUAL(401, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Set-Cookie") == std::string::npos);
    TEST_ASSERT_TRUE(resp.find("Wrong user name or password.") != std::string::npos);
    resp = post("/login", "user=root&password=goodpasswd1");
    TEST_ASSERT_EQUAL(401, statusCode(resp));
    TEST_ASSERT_EQUAL(2, fb->passwordChecks);  // checked even for the wrong user
    resp = post("/login", "user=admin");
    TEST_ASSERT_EQUAL(401, statusCode(resp));
    TEST_ASSERT_EQUAL(3, fb->failed);
    TEST_ASSERT_EQUAL(0, app->sessionCount());
}

static void test_five_failures_lock_logins_for_30_s() {
    webSetUp();
    for (int i = 0; i < 5; i++) post("/login", "user=admin&password=wrong" + std::to_string(i), "", 1000);
    int checks = fb->passwordChecks;
    std::string resp = post("/login", "user=admin&password=goodpasswd1", "", 2000);
    TEST_ASSERT_EQUAL(429, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Try again in 29 s.") != std::string::npos);
    TEST_ASSERT_EQUAL(checks, fb->passwordChecks);  // not even checked while locked
    TEST_ASSERT_EQUAL(1, fb->locked);
    resp = post("/login", "user=admin&password=goodpasswd1", "", 1000 + WEB_LOGIN_LOCKOUT_MS);
    TEST_ASSERT_EQUAL(303, statusCode(resp));
}

static void test_status_page_shows_device_values_escaped() {
    webSetUp();
    strcpy(fb->st.community, "a<b>\"c'&");
    fb->st.cpuTempDeciC = -5;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("&middot; v2.0.0") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<td>10.10.6.111</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<td>fd16:d833:4f0e:3ece:1cb2:cfff:fe4e:a22c</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<td>fe80::1cb2:cfff:fe4e:a22c</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("1E:B2:CF:4E:A2:2C") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("1 d 1 h 1 min") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("-0.5 \xC2\xB0" "C") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("up, 100 Mbit/s") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"a&lt;b&gt;&quot;c&#39;&amp;\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("a<b>") == std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"dhcp\" checked") != std::string::npos);
}

static const char* const ROW_TH = "<tr><th scope=\"row\" class=\"ps-3 fw-normal text-secondary\">";

/// The value of the Network card's text box with this name
static std::string fieldValue(const std::string& page, const char* name) {
    std::string key = std::string("id=\"f-") + name + "\"";
    size_t at = page.find(key);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, name);
    size_t v = page.find("value=\"", at) + 7;
    return page.substr(v, page.find('"', v) - v);
}

// With DHCP the boxes show the addresses in use, not the stored static ones (10.10.6.249, 255.255.0.0, 10.10.0.1)
static void test_network_fields_show_the_addresses_from_dhcp() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL_STRING("10.10.6.111", fieldValue(page, "ip").c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.252.0", fieldValue(page, "mask").c_str());
    TEST_ASSERT_EQUAL_STRING("10.132.0.1", fieldValue(page, "gateway").c_str());
    TEST_ASSERT_TRUE(page.find("From DHCP, DNS server 10.4.0.7.") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("10.10.6.249") == std::string::npos);
}

static void test_network_fields_show_the_stored_values_without_an_address() {
    webSetUp();
    fb->st.liveMask[0] = fb->st.liveGateway[0] = fb->st.dns[0] = '\0';
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL_STRING("10.10.6.249", fieldValue(page, "ip").c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.0.0", fieldValue(page, "mask").c_str());
    TEST_ASSERT_EQUAL_STRING("10.10.0.1", fieldValue(page, "gateway").c_str());
    TEST_ASSERT_TRUE(page.find("From DHCP") == std::string::npos);
}

static void test_network_fields_show_the_stored_values_in_static_mode() {
    webSetUp();
    fb->st.dhcp = false;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL_STRING("10.10.6.249", fieldValue(page, "ip").c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.0.0", fieldValue(page, "mask").c_str());
    TEST_ASSERT_EQUAL_STRING("10.10.0.1", fieldValue(page, "gateway").c_str());
    TEST_ASSERT_TRUE(page.find("From DHCP") == std::string::npos);
}

// Saving with DHCP keeps the stored static values, not the DHCP ones shown in the boxes
static void test_saving_dhcp_keeps_the_stored_static_values_not_the_shown_ones() {
    webSetUp();
    std::string csrf;
    std::string cookie = logIn(csrf);
    post("/network", "mode=dhcp&ip=10.10.6.111&mask=255.255.252.0&gateway=10.132.0.1&csrf=" + csrf, cookie);
    TEST_ASSERT_EQUAL(1, fb->networkSaves);
    TEST_ASSERT_TRUE(fb->savedDhcp);
    TEST_ASSERT_EQUAL_STRING("10.10.6.249", fb->savedIP.c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.0.0", fb->savedMask.c_str());
    TEST_ASSERT_EQUAL_STRING("10.10.0.1", fb->savedGateway.c_str());
}

static void test_status_page_shows_cpu_and_ambient_temperature() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find(std::string(ROW_TH) + "CPU temperature</th><td>40.4 \xC2\xB0" "C</td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find(std::string(ROW_TH) + "Ambient temperature</th><td>27.1 \xC2\xB0" "C</td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find("Board temperature") == std::string::npos);
}

static void test_status_page_ambient_temperature_without_sensor() {
    webSetUp();
    fb->st.ambientTempValid = false;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find(std::string(ROW_TH) + "Ambient temperature</th><td><span class=\"badge text-bg-secondary\">"
                               "no sensor</span></td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("27.1") == std::string::npos);
}

static void test_status_page_ambient_temperature_below_zero() {
    webSetUp();
    fb->st.ambientTempDeciC = -123;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("Ambient temperature</th><td>-12.3 \xC2\xB0" "C</td>") != std::string::npos);
}

static void test_ipv6_rows_only_when_the_device_has_ipv6() {
    webSetUp();
    fb->st.ipv6[0] = fb->st.ipv6LinkLocal[0] = '\0';
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("IPv6 address</th>") == std::string::npos);
    TEST_ASSERT_TRUE(page.find("IPv6 link-local</th>") == std::string::npos);
    TEST_ASSERT_TRUE(page.find("Static IPv6</th>") == std::string::npos);
}

static void test_status_page_shows_dhcpv6_address_when_present() {
    webSetUp();
    strcpy(fb->st.ipv6Dhcp, "fd54:b56a:26e1:2183::d600:1");
    fb->st.ipv6DhcpValidS = 90;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("IPv6 (DHCPv6)</th><td>fd54:b56a:26e1:2183::d600:1 (valid 90 s)</td>") !=
                     std::string::npos);
}

static void test_status_page_dhcpv6_infinite_lifetime() {
    webSetUp();
    strcpy(fb->st.ipv6Dhcp, "fd54:b56a:26e1:2183::d600:1");
    fb->st.ipv6DhcpValidS = 0xFFFFFFFFu;
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("fd54:b56a:26e1:2183::d600:1 (valid forever)</td>") != std::string::npos);
}

static void test_status_page_shows_why_dhcpv6_has_no_address() {
    webSetUp();
    strcpy(fb->st.ipv6DhcpProblem, "no free IPv6 address slot");
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("IPv6 (DHCPv6)</th><td>no free IPv6 address slot</td>") != std::string::npos);
}

static void test_status_page_has_no_dhcpv6_row_without_address() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("DHCPv6") == std::string::npos);
}

static void test_actions_need_a_session() {
    webSetUp();
    const char* paths[] = {"/community", "/network", "/reboot", "/logout", "/hostname", "/disable", "/ipv6"};
    for (const char* p : paths) {
        std::string resp = post(p, "community=x&mode=dhcp&hostname=x&confirm=yes&ipv6=fd00::1&ipv6gw=&csrf=00");
        TEST_ASSERT_EQUAL_MESSAGE(403, statusCode(resp), p);
        resp = post(p, "community=x&mode=dhcp", std::string(32, 'a'));
        TEST_ASSERT_EQUAL_MESSAGE(403, statusCode(resp), p);
    }
    TEST_ASSERT_EQUAL(0, fb->communitySaves + fb->networkSaves + fb->hostnameSaves + fb->reboots + fb->disables +
                             fb->ipv6Saves);
}

static void test_actions_need_the_session_csrf_token() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string wrong = csrf;
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    TEST_ASSERT_EQUAL(403, statusCode(post("/community", "community=newcomm", token)));
    TEST_ASSERT_EQUAL(403, statusCode(post("/community", "community=newcomm&csrf=" + wrong, token)));
    TEST_ASSERT_EQUAL(403, statusCode(post("/reboot", "csrf=" + token, token)));  // session token is not the CSRF token
    TEST_ASSERT_EQUAL(0, fb->communitySaves + fb->reboots);
}

static void test_change_community() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/community", "csrf=" + csrf + "&community=s3cret%21", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL_STRING("s3cret!", fb->savedCommunity.c_str());
    TEST_ASSERT_TRUE(resp.find("SNMP community saved and active.") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("value=\"s3cret!\"") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_change_hostname() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string page = get("/", token);
    TEST_ASSERT_TRUE(page.find("<td>rf-ups-monitor</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<td>rf-ups-monitor.local</td>") != std::string::npos);
    std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=ups-rack4", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL_STRING("ups-rack4", fb->savedHostname.c_str());
    TEST_ASSERT_TRUE(resp.find("Hostname saved and active.") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("<td>ups-rack4.local</td>") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_invalid_hostname_is_not_saved() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string bad[] = {"", "ups+rack", "-ups", "ups-", "ups.local", "ups_4", std::string(33, 'h'), "%3Cb%3E"};
    for (const std::string& h : bad) {
        std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=" + h, token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), h.c_str());
        TEST_ASSERT_TRUE(resp.find("alert alert-danger") != std::string::npos);
    }
    TEST_ASSERT_EQUAL(0, fb->hostnameSaves);
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=" + std::string(32, 'h'), token)));
    fb->saveOk = false;
    TEST_ASSERT_EQUAL(500, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=other", token)));
}

static void test_invalid_community_is_not_saved() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string bad[] = {"", "%01ctrl", std::string(32, 'c'), "tab%09x"};
    for (const std::string& c : bad) {
        std::string resp = post("/community", "csrf=" + csrf + "&community=" + c, token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), c.c_str());
    }
    TEST_ASSERT_EQUAL(400, statusCode(post("/community", "csrf=" + csrf, token)));
    TEST_ASSERT_EQUAL(0, fb->communitySaves);
    // 31 characters is the maximum and is accepted
    TEST_ASSERT_EQUAL(200, statusCode(post("/community", "csrf=" + csrf + "&community=" + std::string(31, 'c'), token)));
}

static void test_switch_to_dhcp_keeps_static_values_and_reboots() {
    webSetUp();
    fb->st.dhcp = false;
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/network", "csrf=" + csrf + "&mode=dhcp&ip=1.2.3.4&mask=x&gateway=y", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_TRUE(fb->savedDhcp);
    TEST_ASSERT_EQUAL_STRING("10.10.6.249", fb->savedIP.c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.0.0", fb->savedMask.c_str());
    TEST_ASSERT_EQUAL_STRING("10.10.0.1", fb->savedGateway.c_str());
    TEST_ASSERT_EQUAL(1, fb->reboots);
    TEST_ASSERT_TRUE(resp.find("restarts now with DHCP") != std::string::npos);
}

static void test_static_ip_is_validated_saved_and_reboots() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/network",
                            "csrf=" + csrf + "&mode=static&ip=192.168.1.20&mask=255.255.255.0&gateway=192.168.1.1", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_FALSE(fb->savedDhcp);
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", fb->savedIP.c_str());
    TEST_ASSERT_EQUAL(1, fb->reboots);
    TEST_ASSERT_TRUE(resp.find("reachable at 192.168.1.20") != std::string::npos);
}

static void test_invalid_static_ip_is_not_saved() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    const char* bad[] = {
        "mode=static&ip=192.168.1.20&mask=255.255.255.0&gateway=10.0.0.1",   // gateway outside
        "mode=static&ip=192.168.1.0&mask=255.255.255.0&gateway=192.168.1.1", // network address
        "mode=static&ip=999.1.1.1&mask=255.255.255.0&gateway=192.168.1.1",
        "mode=static&ip=192.168.1.20&mask=255.0.255.0&gateway=192.168.1.1",
        "mode=static&ip=192.168.1.20&mask=255.255.255.0",
        "mode=static&ip=192.168.100.200.1&mask=255.255.255.0&gateway=192.168.1.1",  // too long for the field
        "mode=other",
        "",
    };
    for (const char* b : bad) {
        std::string resp = post("/network", "csrf=" + csrf + "&" + b, token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), b);
        TEST_ASSERT_TRUE_MESSAGE(resp.find("alert alert-danger") != std::string::npos, b);
    }
    TEST_ASSERT_EQUAL(0, fb->networkSaves);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_failed_save_does_not_reboot() {
    webSetUp();
    fb->saveOk = false;
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(500, statusCode(post("/network", "csrf=" + csrf + "&mode=dhcp", token)));
    TEST_ASSERT_EQUAL(500, statusCode(post("/community", "csrf=" + csrf + "&community=abc", token)));
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_disable_asks_first_then_turns_the_web_interface_off() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_TRUE(get("/", token).find("action=\"/disable\"") != std::string::npos);
    // Without confirm=yes: a confirmation page with its own form, nothing changed yet
    std::string ask = post("/disable", "csrf=" + csrf, token);
    TEST_ASSERT_EQUAL(200, statusCode(ask));
    TEST_ASSERT_TRUE(ask.find("Turn off the web interface?") != std::string::npos);
    TEST_ASSERT_TRUE(ask.find("name=\"confirm\" value=\"yes\"") != std::string::npos);
    TEST_ASSERT_TRUE(ask.find("name=\"csrf\" value=\"" + csrf + "\"") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->disables);
    TEST_ASSERT_EQUAL(200, statusCode(post("/disable", "csrf=" + csrf + "&confirm=no", token)));
    TEST_ASSERT_EQUAL(0, fb->disables);
    // Confirmed: saved, explained, and every session ends
    std::string done = post("/disable", "csrf=" + csrf + "&confirm=yes", token);
    TEST_ASSERT_EQUAL(200, statusCode(done));
    TEST_ASSERT_EQUAL(1, fb->disables);
    TEST_ASSERT_TRUE(done.find("serial menu option 8") != std::string::npos);
    TEST_ASSERT_EQUAL(0, app->sessionCount());
    TEST_ASSERT_TRUE(get("/", token).find("action=\"/login\"") != std::string::npos);
}

static void test_disable_needs_the_csrf_token_and_reports_save_failure() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(403, statusCode(post("/disable", "confirm=yes", token)));
    TEST_ASSERT_EQUAL(403, statusCode(post("/disable", "confirm=yes&csrf=" + token, token)));
    TEST_ASSERT_EQUAL(405, statusCode(get("/disable", token)));
    fb->saveOk = false;
    TEST_ASSERT_EQUAL(500, statusCode(post("/disable", "csrf=" + csrf + "&confirm=yes", token)));
    TEST_ASSERT_EQUAL(1, fb->disables);
    TEST_ASSERT_EQUAL(1, app->sessionCount());  // still logged in: nothing changed
}

static void test_logo_in_navbar_and_login_card_and_favicon_link() {
    webSetUp();
    std::string login = get("/");
    TEST_ASSERT_TRUE(login.find("<link rel=\"icon\" type=\"image/svg+xml\" href=\"/favicon.svg\">") != std::string::npos);
    // Navbar logo and the large one on the login card
    size_t first = login.find("fill=\"#FF7300\" d=\"M339.4847107,58.6125908");
    TEST_ASSERT_TRUE(first != std::string::npos);
    TEST_ASSERT_TRUE(login.find("fill=\"#FF7300\" d=\"M339.4847107,58.6125908", first + 1) != std::string::npos);
    TEST_ASSERT_TRUE(login.find("aria-label=\"RF logo\"") != std::string::npos);
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("fill=\"#FF7300\"") != std::string::npos);  // navbar on every page
}

static void test_favicon_is_the_logo_without_login() {
    webSetUp();
    std::string r = get("/favicon.svg");
    TEST_ASSERT_EQUAL(200, statusCode(r));
    TEST_ASSERT_TRUE(r.find("Content-Type: image/svg+xml\r\n") != std::string::npos);
    TEST_ASSERT_TRUE(r.find("Cache-Control: public, max-age=86400") != std::string::npos);
    size_t body = r.find("\r\n\r\n") + 4;
    TEST_ASSERT_EQUAL(atoi(between(r, "Content-Length: ", "\r\n").c_str()), r.size() - body);
    std::string svg = r.substr(body);
    TEST_ASSERT_EQUAL(0, svg.find("<svg xmlns=\"http://www.w3.org/2000/svg\""));
    TEST_ASSERT_TRUE(svg.find("H339.4847107z\"/></svg>") != std::string::npos);
    TEST_ASSERT_TRUE(svg.find("<script") == std::string::npos);
    TEST_ASSERT_EQUAL(405, statusCode(post("/favicon.svg", "")));
}

static void test_reboot_button() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/reboot", "csrf=" + csrf, token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL(1, fb->reboots);
}

static void test_logout_ends_the_session() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/logout", "csrf=" + csrf, token);
    TEST_ASSERT_EQUAL(303, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Set-Cookie: sid=; Path=/; Max-Age=0") != std::string::npos);
    TEST_ASSERT_TRUE(get("/", token).find("action=\"/login\"") != std::string::npos);
    TEST_ASSERT_EQUAL(403, statusCode(post("/reboot", "csrf=" + csrf, token)));
}

static void test_session_expires_after_idle_time_and_logout_all() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf, 1000);
    TEST_ASSERT_TRUE(get("/", token, 1000 + WEB_SESSION_IDLE_MS).find("Restart device") != std::string::npos);
    TEST_ASSERT_TRUE(get("/", token, 2000 + 2 * WEB_SESSION_IDLE_MS).find("action=\"/login\"") != std::string::npos);
    token = logIn(csrf, 5000);
    app->logoutAll();
    TEST_ASSERT_TRUE(get("/", token, 5000).find("action=\"/login\"") != std::string::npos);
}

static void test_unknown_paths_methods_and_bad_requests() {
    webSetUp();
    TEST_ASSERT_EQUAL(404, statusCode(get("/admin")));
    TEST_ASSERT_EQUAL(404, statusCode(get("/favicon.ico")));
    TEST_ASSERT_EQUAL(405, statusCode(get("/network")));
    TEST_ASSERT_EQUAL(405, statusCode(post("/", "")));
    TEST_ASSERT_EQUAL(405, statusCode(send("PUT /login HTTP/1.1\r\n\r\n")));
    TEST_ASSERT_EQUAL(303, statusCode(get("/login")));
    TEST_ASSERT_EQUAL(400, statusCode(send("GARBAGE\r\n\r\n")));
    TEST_ASSERT_EQUAL(413, statusCode(send("POST /login HTTP/1.1\r\nContent-Length: 5000\r\n\r\n")));
}

// ---- the embedded Bootstrap stylesheet ----

static void test_stylesheet_is_served_gzipped_from_flash_without_login() {
    webSetUp();
    std::string resp = get(BOOTSTRAP_CSS_PATH);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Content-Type: text/css; charset=utf-8\r\n") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("Cache-Control: public, max-age=31536000, immutable\r\n") != std::string::npos);
    TEST_ASSERT_EQUAL((int)BOOTSTRAP_CSS_GZ_LEN, atoi(between(resp, "Content-Length: ", "\r\n").c_str()));
    // The response itself is only the headers; the body comes straight from flash
    TEST_ASSERT_EQUAL(resp.size(), resp.find("\r\n\r\n") + 4);
    size_t len = 0;
    TEST_ASSERT_EQUAL_PTR(BOOTSTRAP_CSS_GZ, app->staticBody(len));
    TEST_ASSERT_EQUAL(BOOTSTRAP_CSS_GZ_LEN, len);
}

static void test_embedded_stylesheet_is_a_gzip_file_of_bootstrap() {
    // gzip magic, deflate, no name (deterministic build), and the trailer's size field
    TEST_ASSERT_GREATER_THAN(1000, BOOTSTRAP_CSS_GZ_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x1f, BOOTSTRAP_CSS_GZ[0]);
    TEST_ASSERT_EQUAL_HEX8(0x8b, BOOTSTRAP_CSS_GZ[1]);
    TEST_ASSERT_EQUAL_HEX8(0x08, BOOTSTRAP_CSS_GZ[2]);
    const uint8_t* t = BOOTSTRAP_CSS_GZ + BOOTSTRAP_CSS_GZ_LEN - 4;
    uint32_t isize = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
    TEST_ASSERT_EQUAL_UINT32(BOOTSTRAP_CSS_LEN, isize);
}

static void test_other_responses_have_no_static_body_and_pages_link_the_stylesheet() {
    webSetUp();
    get(BOOTSTRAP_CSS_PATH);
    std::string page = get("/");
    size_t len = 99;
    TEST_ASSERT_NULL(app->staticBody(len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_TRUE(page.find("<link rel=\"stylesheet\" href=\"" BOOTSTRAP_CSS_PATH "\">") != std::string::npos);
    // The policy allows the own stylesheet and Bootstrap's data: images, still no scripts
    TEST_ASSERT_TRUE(page.find("default-src 'none'; style-src 'self' 'unsafe-inline'; img-src 'self' data:;") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find("<script") == std::string::npos);
    TEST_ASSERT_EQUAL(405, statusCode(post(BOOTSTRAP_CSS_PATH, "")));
    TEST_ASSERT_EQUAL(404, statusCode(get("/static/bootstrap-0.0.0.min.css")));
}

static void test_mains_and_link_badges_show_the_state() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string page = get("/", token);
    TEST_ASSERT_TRUE(page.find("text-bg-success\">present</span>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("text-bg-success\">up, 100 Mbit/s</span>") != std::string::npos);
    fb->st.mainsPresent = false;
    fb->st.linkUp = false;
    page = get("/", token);
    TEST_ASSERT_TRUE(page.find("text-bg-danger\">LOST</span>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("text-bg-danger\">down</span>") != std::string::npos);
    fb->st.mainsKnown = false;
    TEST_ASSERT_TRUE(get("/", token).find("text-bg-secondary\">unknown</span>") != std::string::npos);
}

static void test_status_page_shows_mains_alarm() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string page = get("/", token);
    TEST_ASSERT_TRUE(page.find("Mains alarm</th><td><span class=\"badge text-bg-success\">none</span>") !=
                     std::string::npos);
    fb->st.mainsPresent = false;
    fb->st.mainsAlarm = true;
    page = get("/", token);
    TEST_ASSERT_TRUE(page.find("Mains alarm</th><td><span class=\"badge text-bg-danger\">lost over 60 s</span>") !=
                     std::string::npos);
    fb->st.mainsKnown = false;
    TEST_ASSERT_TRUE(get("/", token).find("Mains alarm") == std::string::npos);
}

static void test_session_found_among_many_other_cookies() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    // Other web apps on the same host add their cookies to every request
    std::string others;
    for (int i = 0; i < 20; i++) others += "other" + std::to_string(i) + "=" + std::string(24, 'v') + "; ";
    TEST_ASSERT_GREATER_THAN(600, others.size());
    std::string page = send("GET / HTTP/1.1\r\nCookie: " + others + "sid=" + token + "\r\n\r\n");
    TEST_ASSERT_TRUE(page.find("Restart device") != std::string::npos);
}

static void test_every_single_byte_corruption_gets_a_well_formed_response() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string body = "csrf=" + csrf + "&community=abc";
    const std::string requests[] = {
        POST_LOGIN,
        "POST /community HTTP/1.1\r\nCookie: sid=" + token + "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n" + body,
    };
    for (const std::string& original : requests) {
        for (size_t pos = 0; pos < original.size(); pos++) {
            for (int v = 0; v < 256; v++) {
                std::string raw = original;
                raw[pos] = (char)v;
                std::string resp = send(raw);
                TEST_ASSERT_EQUAL_STRING("HTTP/1.1 ", resp.substr(0, 9).c_str());
                size_t bodyStart = resp.find("\r\n\r\n");
                TEST_ASSERT_TRUE(bodyStart != std::string::npos);
                TEST_ASSERT_EQUAL(atoi(between(resp, "Content-Length: ", "\r\n").c_str()), resp.size() - bodyStart - 4);
            }
        }
    }
    // Only valid community values were ever saved
    TEST_ASSERT_TRUE(settingsTextIsValid(fb->savedCommunity.c_str(), COMMUNITY_MAX_LENGTH, true));
}

static void test_longest_values_still_fit_the_page() {
    webSetUp();
    strcpy(fb->st.community, std::string(31, '"').c_str());  // every character escaped to 6
    strcpy(fb->st.hostname, std::string(32, 'h').c_str());
    strcpy(fb->st.staticIPv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ipv6Gateway, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:ffff");
    strcpy(fb->st.ipv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ip, "255.255.255.255");
    strcpy(fb->st.liveMask, "255.255.255.255");
    strcpy(fb->st.liveGateway, "255.255.255.255");
    strcpy(fb->st.dns, "255.255.255.255");
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL(200, statusCode(page));
    TEST_ASSERT_TRUE(page.find("</html>") != std::string::npos);
}

static void test_ipv6_form_shows_the_stored_static_values() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("action=\"/ipv6\"") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("", between(page, "name=\"ipv6\" maxlength=\"39\" value=\"", "\"").c_str());
    TEST_ASSERT_TRUE(page.find("Static IPv6</th>") == std::string::npos);  // no row without a static address
    webSetUp();
    strcpy(fb->st.staticIPv6, "fd16:d833:4f0e:3ece::249");
    strcpy(fb->st.ipv6Gateway, "fe80::1");
    page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::249",
                             between(page, "name=\"ipv6\" maxlength=\"39\" value=\"", "\"").c_str());
    TEST_ASSERT_EQUAL_STRING("fe80::1", between(page, "name=\"ipv6gw\" maxlength=\"39\" value=\"", "\"").c_str());
    TEST_ASSERT_TRUE(page.find("fd16:d833:4f0e:3ece::249</td>") != std::string::npos);  // status row
}

static void test_static_ipv6_is_validated_normalized_saved_and_reboots() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    // Browsers send ':' as %3A; upper case and leading zeros are stored canonically
    std::string resp = post("/ipv6", "csrf=" + csrf + "&ipv6=FD16%3AD833%3A4F0E%3A3ECE%3A0%3A0%3A0%3A0249&ipv6gw=FE80%3A%3A1",
                            token);
    TEST_ASSERT_EQUAL_MESSAGE(200, statusCode(resp), resp.c_str());
    TEST_ASSERT_EQUAL(1, fb->ipv6Saves);
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::249", fb->savedIPv6.c_str());
    TEST_ASSERT_EQUAL_STRING("fe80::1", fb->savedIPv6Gateway.c_str());
    TEST_ASSERT_EQUAL(1, fb->reboots);
    TEST_ASSERT_TRUE(resp.find("reachable at [fd16:d833:4f0e:3ece::249]") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->networkSaves);  // the IPv4 settings are not touched
}

static void test_empty_ipv6_form_removes_the_static_address() {
    webSetUp();
    strcpy(fb->st.staticIPv6, "fd00::10");
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/ipv6", "csrf=" + csrf + "&ipv6=&ipv6gw=", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL(1, fb->ipv6Saves);
    TEST_ASSERT_EQUAL_STRING("", fb->savedIPv6.c_str());
    TEST_ASSERT_EQUAL_STRING("", fb->savedIPv6Gateway.c_str());
    TEST_ASSERT_EQUAL(1, fb->reboots);
}

static void test_invalid_static_ipv6_is_not_saved() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    const char* bad[] = {
        "ipv6=fe80%3A%3A10&ipv6gw=",                         // link-local
        "ipv6=fd00%3A%3A1%2F64&ipv6gw=",                     // with /64
        "ipv6=fd00%3A%3A1&ipv6gw=fd01%3A%3A1",               // gateway in another /64
        "ipv6=fd00%3A%3A1&ipv6gw=fd00%3A%3A1",               // gateway = address
        "ipv6=&ipv6gw=fe80%3A%3A1",                          // gateway without address
        "ipv6=fd00%3A%3A1",                                  // gateway field missing
        "ipv6=0000%3A0000%3A0000%3A0000%3A0000%3A0000%3A0000%3A00001&ipv6gw=",  // 40 characters
        "ipv6=" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&ipv6gw=",  // longer than the field
        "",
    };
    for (const char* b : bad) {
        std::string resp = post("/ipv6", "csrf=" + csrf + "&" + b, token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), b);
        TEST_ASSERT_TRUE_MESSAGE(resp.find("alert alert-danger") != std::string::npos, b);
    }
    TEST_ASSERT_EQUAL(0, fb->ipv6Saves);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_failed_ipv6_save_does_not_reboot() {
    webSetUp();
    fb->saveOk = false;
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(500, statusCode(post("/ipv6", "csrf=" + csrf + "&ipv6=fd00%3A%3A1&ipv6gw=", token)));
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_known_vectors);
    RUN_TEST(test_sha256_million_a);
    RUN_TEST(test_sha256_split_anywhere_equals_one_shot);
    RUN_TEST(test_password_rules);
    RUN_TEST(test_password_hash_matches_independent_implementation);
    RUN_TEST(test_password_check);
    RUN_TEST(test_salt_changes_the_hash);
    RUN_TEST(test_malformed_stored_hash_never_matches);
    RUN_TEST(test_stepwise_check_equals_one_shot_check);
    RUN_TEST(test_stepwise_check_runs_all_rounds_even_for_bad_input);
    RUN_TEST(test_stepwise_check_state);
    RUN_TEST(test_default_password_uses_the_unambiguous_alphabet);
    RUN_TEST(test_constant_time_equals);
    RUN_TEST(test_session_tokens_are_hex_and_different);
    RUN_TEST(test_session_found_until_idle_too_long);
    RUN_TEST(test_session_survives_millis_wrap);
    RUN_TEST(test_unknown_or_malformed_tokens_are_not_found);
    RUN_TEST(test_new_login_replaces_least_recently_used_session);
    RUN_TEST(test_remove_and_clear_end_sessions);
    RUN_TEST(test_throttle_locks_after_five_failures_for_30_s);
    RUN_TEST(test_throttle_success_resets_the_count);
    RUN_TEST(test_throttle_survives_millis_wrap);
    RUN_TEST(test_parse_simple_get);
    RUN_TEST(test_parse_post_with_body);
    RUN_TEST(test_every_prefix_is_incomplete_and_byte_by_byte_works);
    RUN_TEST(test_headers_case_insensitive_and_trimmed);
    RUN_TEST(test_too_large_requests_are_refused);
    RUN_TEST(test_malformed_requests_are_bad);
    RUN_TEST(test_other_methods_and_long_paths);
    RUN_TEST(test_reset_allows_a_new_request);
    RUN_TEST(test_form_values_are_decoded);
    RUN_TEST(test_form_value_rejects_bad_escapes_nul_and_overflow);
    RUN_TEST(test_cookie_values);
    RUN_TEST(test_text_buffer_escapes_and_stops_when_full);
    RUN_TEST(test_start_page_without_session_is_the_login_form);
    RUN_TEST(test_every_response_has_security_headers_and_correct_length);
    RUN_TEST(test_login_with_right_password_sets_a_strict_session_cookie);
    RUN_TEST(test_wrong_password_or_user_is_refused_and_logged);
    RUN_TEST(test_five_failures_lock_logins_for_30_s);
    RUN_TEST(test_status_page_shows_device_values_escaped);
    RUN_TEST(test_network_fields_show_the_addresses_from_dhcp);
    RUN_TEST(test_network_fields_show_the_stored_values_without_an_address);
    RUN_TEST(test_network_fields_show_the_stored_values_in_static_mode);
    RUN_TEST(test_saving_dhcp_keeps_the_stored_static_values_not_the_shown_ones);
    RUN_TEST(test_status_page_shows_cpu_and_ambient_temperature);
    RUN_TEST(test_status_page_ambient_temperature_without_sensor);
    RUN_TEST(test_status_page_ambient_temperature_below_zero);
    RUN_TEST(test_ipv6_rows_only_when_the_device_has_ipv6);
    RUN_TEST(test_status_page_shows_dhcpv6_address_when_present);
    RUN_TEST(test_status_page_dhcpv6_infinite_lifetime);
    RUN_TEST(test_status_page_shows_why_dhcpv6_has_no_address);
    RUN_TEST(test_status_page_has_no_dhcpv6_row_without_address);
    RUN_TEST(test_actions_need_a_session);
    RUN_TEST(test_actions_need_the_session_csrf_token);
    RUN_TEST(test_change_community);
    RUN_TEST(test_change_hostname);
    RUN_TEST(test_invalid_hostname_is_not_saved);
    RUN_TEST(test_invalid_community_is_not_saved);
    RUN_TEST(test_switch_to_dhcp_keeps_static_values_and_reboots);
    RUN_TEST(test_static_ip_is_validated_saved_and_reboots);
    RUN_TEST(test_invalid_static_ip_is_not_saved);
    RUN_TEST(test_failed_save_does_not_reboot);
    RUN_TEST(test_disable_asks_first_then_turns_the_web_interface_off);
    RUN_TEST(test_disable_needs_the_csrf_token_and_reports_save_failure);
    RUN_TEST(test_logo_in_navbar_and_login_card_and_favicon_link);
    RUN_TEST(test_favicon_is_the_logo_without_login);
    RUN_TEST(test_reboot_button);
    RUN_TEST(test_logout_ends_the_session);
    RUN_TEST(test_session_expires_after_idle_time_and_logout_all);
    RUN_TEST(test_unknown_paths_methods_and_bad_requests);
    RUN_TEST(test_stylesheet_is_served_gzipped_from_flash_without_login);
    RUN_TEST(test_embedded_stylesheet_is_a_gzip_file_of_bootstrap);
    RUN_TEST(test_other_responses_have_no_static_body_and_pages_link_the_stylesheet);
    RUN_TEST(test_mains_and_link_badges_show_the_state);
    RUN_TEST(test_status_page_shows_mains_alarm);
    RUN_TEST(test_session_found_among_many_other_cookies);
    RUN_TEST(test_every_single_byte_corruption_gets_a_well_formed_response);
    RUN_TEST(test_longest_values_still_fit_the_page);
    RUN_TEST(test_ipv6_form_shows_the_stored_static_values);
    RUN_TEST(test_static_ipv6_is_validated_normalized_saved_and_reboots);
    RUN_TEST(test_empty_ipv6_form_removes_the_static_address);
    RUN_TEST(test_invalid_static_ipv6_is_not_saved);
    RUN_TEST(test_failed_ipv6_save_does_not_reboot);
    return UNITY_END();
}
