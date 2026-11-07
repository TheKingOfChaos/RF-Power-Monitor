// Host unit tests for lib/web_ui: SHA-256, passwords, sessions, login throttle,
// the HTTP request parser, form/cookie helpers and the web page logic.
// Run: pio test -e native -f native/test_web_ui

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sha256.h>
#include <web_auth.h>
#include <default_password.h>
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

static void test_reset_wipes_the_received_bytes() {
    // Forms carry passwords and the update server key: they must not stay in RAM after the answer
    static HttpRequest r;
    const char* req = "POST /updateserver HTTP/1.1\r\nContent-Length: 20\r\n\r\nkey=SECRETSECRETKEY!";
    TEST_ASSERT_EQUAL(HttpParse::Complete, feedAll(r, req));
    const char* body = r.body();
    size_t len = r.bodyLength();
    TEST_ASSERT_EQUAL_STRING_LEN("key=SECRET", body, 10);
    const char* raw = body - (strlen(req) - len);  // the start of the buffer
    r.reset();
    for (size_t i = 0; i < strlen(req); i++) TEST_ASSERT_EQUAL_CHAR_MESSAGE(0, raw[i], "byte left after reset");
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
    std::string savedContact, savedLocation;
    int failed = 0, locked = 0, loggedIn = 0, uploadRefused = 0;
    SourceKey lastRefusedSource;
    uint8_t seed = 1;
    WebStatus st;
    WebFirmware fw;
    bool switchOk = true;
    int switches = 0;

    FakeBackend() {
        memset(&st, 0, sizeof(st));
        memset(&fw, 0, sizeof(fw));
        fw.runningSlot = 0;
        strcpy(fw.runningVersion, "3.2.0");
        strcpy(fw.otherVersion, "3.1.0");
        strcpy(fw.lastResult, "ok 3.2.0");
        strcpy(fw.keyFingerprint, "0123456789abcdef");
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
        strcpy(st.hostname, "rf-power-monitor");
        strcpy(st.lldpSwitch, "none seen");
        strcpy(st.time, "2026-11-07 19:42:10 CET (NTP pool.ntp.org, synced 3 min ago)");
        strcpy(st.syslog, "10.132.0.45:514 (from DHCP), 12 sent, 0 waiting");
        strcpy(st.ntpSetting, "pool.ntp.org");
        strcpy(st.syslogSetting, "dhcp");
    }
    void status(WebStatus& out) override { out = st; }
    void firmware(WebFirmware& out) override { out = fw; }
    bool switchSlot() override {
        switches++;
        return switchOk;
    }
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
    bool saveDeviceName(const char* h, const char* contact, const char* location) override {
        hostnameSaves++;
        savedHostname = h;
        savedContact = contact;
        savedLocation = location;
        if (saveOk) {
            strcpy(st.hostname, h);
            strcpy(st.sysContact, contact);
            strcpy(st.sysLocation, location);
        }
        return saveOk;
    }
    bool disableWeb() override {
        disables++;
        return saveOk;
    }
    int timeLogSaves = 0;
    std::string savedNtp, savedSyslog;
    bool saveTimeLog(const char* ntp, const char* syslog, const char*& error) override {
        timeLogSaves++;
        savedNtp = ntp;
        savedSyslog = syslog;
        if (strcmp(ntp, "a..b") == 0) {
            error = "NTP server: an IPv4 or IPv6 address, a name <x>, dhcp or none";
            return false;
        }
        if (!saveOk) {
            error = "Saving failed, nothing changed.";
            return false;
        }
        if (ntp[0]) strcpy(st.ntpSetting, ntp);
        if (syslog[0]) strcpy(st.syslogSetting, syslog);
        return true;
    }
    int updateSaves = 0;
    std::string savedUpdateUrl, savedUpdateKey;
    bool savedUpdateFromDhcp = false, savedUpdateClearKey = false;
    bool saveUpdateServer(const char* url, bool fromDhcp, const char* keyHex, bool clearKey,
                          const char*& error) override {
        // The firmware checks with lib/update_client; the fake knows two bad inputs
        if (strncmp(url, "http://", 7) != 0 && url[0] != '\0') {
            error = "The URL must be http://host[:port][/path] <here>";
            return false;
        }
        if (keyHex[0] != '\0' && strlen(keyHex) != 64) {
            error = "The key must be 64 hex characters.";
            return false;
        }
        updateSaves++;
        savedUpdateUrl = url;
        savedUpdateKey = keyHex;
        savedUpdateFromDhcp = fromDhcp;
        savedUpdateClearKey = clearKey;
        if (!saveOk) {
            error = "Saving failed, nothing changed.";
            return false;
        }
        strcpy(fw.updateUrl, url);
        fw.updateFromDhcp = fromDhcp;
        if (keyHex[0] != '\0') fw.updateHasKey = true;
        if (clearKey) fw.updateHasKey = false;
        return true;
    }
    void requestReboot() override { reboots++; }
    void random(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(seed * 31 + i);
        seed++;
    }
    void log(const SourceKey& s, WebEvent e) override {
        if (e == WebEvent::LoginFailed) failed++;
        if (e == WebEvent::LoginLocked) locked++;
        if (e == WebEvent::LoggedIn) loggedIn++;
        if (e == WebEvent::UploadRefused) {
            uploadRefused++;
            lastRefusedSource = s;
        }
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
        std::string resp = post(p, "community=x&mode=dhcp&hostname=x&contact=&location=&confirm=yes&ipv6=fd00::1&ipv6gw=&csrf=00");
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

// ---- time and syslog ----

static void test_timelog_needs_session_and_csrf() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(403, statusCode(post("/timelog", "ntp=dhcp&syslog=none", token)));
    TEST_ASSERT_NOT_EQUAL(200, statusCode(post("/timelog", "csrf=" + csrf + "&ntp=dhcp&syslog=none", "")));
    TEST_ASSERT_EQUAL(0, fb->timeLogSaves);
}

static void test_timelog_saves_through_backend() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/timelog", "csrf=" + csrf + "&ntp=dk.pool.ntp.org&syslog=10.132.0.45%3A5514", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL_STRING("dk.pool.ntp.org", fb->savedNtp.c_str());
    TEST_ASSERT_EQUAL_STRING("10.132.0.45:5514", fb->savedSyslog.c_str());
    TEST_ASSERT_TRUE(resp.find("Time and syslog settings saved and active.") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("value=\"10.132.0.45:5514\"") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_timelog_bad_value_shows_error_and_saves_nothing() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/timelog", "csrf=" + csrf + "&ntp=a..b&syslog=none", token);
    TEST_ASSERT_EQUAL(400, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("a name &lt;x&gt;, dhcp or none") != std::string::npos);  // escaped
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", fb->st.ntpSetting);
    TEST_ASSERT_EQUAL_STRING("dhcp", fb->st.syslogSetting);
}

static void test_timelog_overlong_or_missing_value_is_refused_before_the_backend() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string longName(WEB_SERVER_TEXT_MAX + 1, 'a');
    TEST_ASSERT_EQUAL(400, statusCode(post("/timelog", "csrf=" + csrf + "&ntp=" + longName + "&syslog=none", token)));
    TEST_ASSERT_EQUAL(400, statusCode(post("/timelog", "csrf=" + csrf + "&ntp=dhcp", token)));
    TEST_ASSERT_EQUAL(0, fb->timeLogSaves);
}

static void test_timelog_empty_field_is_refused_with_a_reason() {
    // The fields show the current value; a cleared field means the user wants something else
    // (the serial menu's "Enter keeps" does not fit a form), so say what to type
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/timelog", "csrf=" + csrf + "&ntp=&syslog=none", token);
    TEST_ASSERT_EQUAL(400, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("Enter a server, dhcp or none in both fields.") != std::string::npos);
    TEST_ASSERT_EQUAL(400, statusCode(post("/timelog", "csrf=" + csrf + "&ntp=dhcp&syslog=", token)));
    TEST_ASSERT_EQUAL(0, fb->timeLogSaves);
}

static void test_main_page_shows_time_rows_and_card() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find(">Time</th><td>2026-11-07 19:42:10 CET (NTP pool.ntp.org, synced 3 min ago)</td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find(">Syslog</th><td>10.132.0.45:514 (from DHCP), 12 sent, 0 waiting</td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find("action=\"/timelog\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"ntp\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"pool.ntp.org\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"syslog\"") != std::string::npos);
}

static void test_change_hostname() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string page = get("/", token);
    TEST_ASSERT_TRUE(page.find("<td>rf-power-monitor</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<td>rf-power-monitor.local</td>") != std::string::npos);
    std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=ups-rack4&contact=&location=", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL_STRING("ups-rack4", fb->savedHostname.c_str());
    TEST_ASSERT_TRUE(resp.find("Hostname, contact and location saved and active.") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("<td>ups-rack4.local</td>") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_invalid_hostname_is_not_saved() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string bad[] = {"", "ups+rack", "-ups", "ups-", "ups.local", "ups_4", std::string(33, 'h'), "%3Cb%3E"};
    for (const std::string& h : bad) {
        std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=" + h + "&contact=&location=", token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), h.c_str());
        TEST_ASSERT_TRUE(resp.find("alert alert-danger") != std::string::npos);
    }
    TEST_ASSERT_EQUAL(0, fb->hostnameSaves);
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=" + std::string(32, 'h') +
                                           "&contact=&location=", token)));
    fb->saveOk = false;
    TEST_ASSERT_EQUAL(500, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=other&contact=&location=", token)));
}

static void test_device_name_form_shows_contact_and_location() {
    webSetUp();
    strcpy(fb->st.sysContact, "ops \"A\" <x>");
    strcpy(fb->st.sysLocation, "Rack 4");
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("name=\"contact\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"location\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"ops &quot;A&quot; &lt;x&gt;\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"Rack 4\"") != std::string::npos);
}

static void test_device_name_post_saves_all_three() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=a1&contact=ops%40x&location=Rack+4", token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_EQUAL_STRING("a1", fb->savedHostname.c_str());
    TEST_ASSERT_EQUAL_STRING("ops@x", fb->savedContact.c_str());
    TEST_ASSERT_EQUAL_STRING("Rack 4", fb->savedLocation.c_str());
    TEST_ASSERT_TRUE(resp.find("saved and active.") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_device_name_post_accepts_empty_contact_and_location() {
    webSetUp();
    strcpy(fb->st.sysContact, "old");
    strcpy(fb->st.sysLocation, "old");
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=a1&contact=&location=", token)));
    TEST_ASSERT_EQUAL(1, fb->hostnameSaves);
    TEST_ASSERT_EQUAL_STRING("", fb->savedContact.c_str());
    TEST_ASSERT_EQUAL_STRING("", fb->savedLocation.c_str());
}

static void test_device_name_post_rejects_bad_contact() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string bad[] = {std::string(64, 'c'), "a%01b", "tab%09"};
    for (const std::string& v : bad) {
        std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=a1&contact=" + v + "&location=", token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), v.c_str());
        TEST_ASSERT_TRUE(resp.find("alert alert-danger") != std::string::npos);
    }
    TEST_ASSERT_EQUAL(0, fb->hostnameSaves);
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=a1&contact=" + std::string(63, 'c') +
                                           "&location=", token)));
}

static void test_device_name_post_rejects_bad_location() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string bad[] = {std::string(64, 'l'), "a%01b", "%7F"};
    for (const std::string& v : bad) {
        std::string resp = post("/hostname", "csrf=" + csrf + "&hostname=a1&contact=&location=" + v, token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(resp), v.c_str());
    }
    TEST_ASSERT_EQUAL(0, fb->hostnameSaves);
}

static void test_device_name_post_without_contact_or_location_keeps_them() {
    // A page loaded from firmware 3.0.0 (still open in a browser) sends only the hostname
    webSetUp();
    strcpy(fb->st.sysContact, "ops@x");
    strcpy(fb->st.sysLocation, "Rack 4");
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=a1", token)));
    TEST_ASSERT_EQUAL_STRING("a1", fb->savedHostname.c_str());
    TEST_ASSERT_EQUAL_STRING("ops@x", fb->savedContact.c_str());
    TEST_ASSERT_EQUAL_STRING("Rack 4", fb->savedLocation.c_str());
    // Only one of them: the other keeps its value
    TEST_ASSERT_EQUAL(200, statusCode(post("/hostname", "csrf=" + csrf + "&hostname=a1&location=Bench", token)));
    TEST_ASSERT_EQUAL_STRING("ops@x", fb->savedContact.c_str());
    TEST_ASSERT_EQUAL_STRING("Bench", fb->savedLocation.c_str());
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
    strcpy(fb->st.sysContact, std::string(63, '"').c_str());
    strcpy(fb->st.sysLocation, std::string(63, '"').c_str());
    strcpy(fb->st.staticIPv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ipv6Gateway, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:ffff");
    strcpy(fb->st.ipv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ip, "255.255.255.255");
    strcpy(fb->st.liveMask, "255.255.255.255");
    strcpy(fb->st.liveGateway, "255.255.255.255");
    strcpy(fb->st.dns, "255.255.255.255");
    // Text from the switch: printable, so it can be all '"' (escaped to 6 bytes each)
    strcpy(fb->st.lldpSwitch, std::string(sizeof(fb->st.lldpSwitch) - 1, '"').c_str());
    strcpy(fb->st.lldpSwitchAddr, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    // Server texts are names and addresses (letters, digits, . - : [ ]): full length, nothing escaped
    strcpy(fb->st.time, std::string(sizeof(fb->st.time) - 1, 'a').c_str());
    strcpy(fb->st.syslog, std::string(sizeof(fb->st.syslog) - 1, 'a').c_str());
    strcpy(fb->st.ntpSetting, std::string(WEB_SERVER_TEXT_MAX, 'a').c_str());
    strcpy(fb->st.syslogSetting, std::string(WEB_SERVER_TEXT_MAX, 'a').c_str());
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_EQUAL(200, statusCode(page));
    TEST_ASSERT_TRUE(page.find("</html>") != std::string::npos);
}

static void test_longest_values_with_an_error_message_still_fit_the_page() {
    // The longest page again, now with the error box of a refused form on top
    webSetUp();
    strcpy(fb->st.community, std::string(31, '"').c_str());
    strcpy(fb->st.hostname, std::string(32, 'h').c_str());
    strcpy(fb->st.sysContact, std::string(63, '"').c_str());
    strcpy(fb->st.sysLocation, std::string(63, '"').c_str());
    strcpy(fb->st.staticIPv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ipv6Gateway, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:ffff");
    strcpy(fb->st.ipv6, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    strcpy(fb->st.ip, "255.255.255.255");
    strcpy(fb->st.liveMask, "255.255.255.255");
    strcpy(fb->st.liveGateway, "255.255.255.255");
    strcpy(fb->st.dns, "255.255.255.255");
    strcpy(fb->st.lldpSwitch, std::string(sizeof(fb->st.lldpSwitch) - 1, '"').c_str());
    strcpy(fb->st.lldpSwitchAddr, "fd16:d833:4f0e:3ece:ffff:ffff:ffff:fffe");
    // Server texts are names and addresses (letters, digits, . - : [ ]): full length, nothing escaped
    strcpy(fb->st.time, std::string(sizeof(fb->st.time) - 1, 'a').c_str());
    strcpy(fb->st.syslog, std::string(sizeof(fb->st.syslog) - 1, 'a').c_str());
    strcpy(fb->st.ntpSetting, std::string(WEB_SERVER_TEXT_MAX, 'a').c_str());
    strcpy(fb->st.syslogSetting, std::string(WEB_SERVER_TEXT_MAX, 'a').c_str());
    std::string csrf;
    std::string token = logIn(csrf);
    const std::string refused[][2] = {
        {"/hostname", "&hostname=-bad-&contact=&location="},
        {"/hostname", "&hostname=a1&contact=" + std::string(64, 'c') + "&location="},
        {"/network", "&mode=static&ip=10.10.6.250&mask=255.255.0.0&gateway=192.168.1.1"},
        {"/ipv6", "&ipv6=fe80::1&ipv6gw="},
        {"/community", "&community=" + std::string(32, 'x')},
    };
    for (const auto& form : refused) {
        std::string page = post(form[0], "csrf=" + csrf + form[1], token);
        TEST_ASSERT_EQUAL_MESSAGE(400, statusCode(page), form[0].c_str());
        TEST_ASSERT_TRUE_MESSAGE(page.find("alert alert-danger") != std::string::npos, form[0].c_str());
        TEST_ASSERT_TRUE_MESSAGE(page.find("</html>") != std::string::npos, form[0].c_str());
    }
}

// ---- streaming upload request and the Firmware page ----

static const size_t UPLOAD_MAX = 0xD7000u * 2 + 64;

static HttpParse streamFeed(HttpRequest& r, const std::string& raw) {
    r.reset();
    r.streamPath("/firmware/upload");
    return r.feed((const uint8_t*)raw.data(), raw.size());
}

static void test_stream_path_returns_streaming_with_prefix() {
    HttpRequest r;
    HttpParse p = streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nHost: d\r\nContent-Length: 900000\r\n\r\nABCDEFGH");
    TEST_ASSERT_EQUAL(HttpParse::Streaming, p);
    TEST_ASSERT_EQUAL(HttpParse::Streaming, r.state());
    TEST_ASSERT_EQUAL(HttpMethod::Post, r.method());
    TEST_ASSERT_EQUAL_STRING("/firmware/upload", r.path());
    TEST_ASSERT_EQUAL(900000, r.contentLength());
    size_t n = 0;
    const uint8_t* prefix = r.bodyPrefix(n);
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_MEMORY("ABCDEFGH", prefix, 8);
    // More bytes after the head are the caller's to stream: the parser keeps its state
    TEST_ASSERT_EQUAL(HttpParse::Streaming, r.feed((const uint8_t*)"X", 1));
    // Headers stay readable
    char v[16];
    TEST_ASSERT_TRUE(r.header("Host", v, sizeof(v)));
}

static void test_stream_prefix_with_nul_bytes_still_streams() {
    // The device reads small chunks: body bytes (binary, with NULs) arrive with the end of the head
    HttpRequest r;
    const std::string head = "POST /firmware/upload HTTP/1.1\r\nHost: d\r\nContent-Length: 900000\r\n\r\n";
    std::string raw = head;
    raw += std::string("\x00\x01\x00\xff", 4);
    TEST_ASSERT_EQUAL(HttpParse::Streaming, streamFeed(r, raw));
    size_t n = 0;
    const uint8_t* p = r.bodyPrefix(n);
    TEST_ASSERT_EQUAL(4, n);
    const uint8_t expect[4] = {0x00, 0x01, 0x00, 0xff};
    TEST_ASSERT_EQUAL_MEMORY(expect, p, 4);
    char v[16];
    TEST_ASSERT_TRUE(r.header("Host", v, sizeof(v)));
    TEST_ASSERT_EQUAL(900000, r.contentLength());
}

static void test_stream_head_split_before_binary_prefix_still_streams() {
    // Every split point of the head, with the NUL prefix in the last chunk
    const std::string head = "POST /firmware/upload HTTP/1.1\r\nHost: d\r\nContent-Length: 900000\r\n\r\n";
    const std::string prefix("\x00\x00\x07", 3);
    for (size_t cut = 1; cut < head.size(); cut++) {
        HttpRequest r;
        r.streamPath("/firmware/upload");
        std::string a = head.substr(0, cut), b = head.substr(cut) + prefix;
        HttpParse p = r.feed((const uint8_t*)a.data(), a.size());
        TEST_ASSERT_EQUAL_MESSAGE(HttpParse::Incomplete, p, std::to_string(cut).c_str());
        p = r.feed((const uint8_t*)b.data(), b.size());
        TEST_ASSERT_EQUAL_MESSAGE(HttpParse::Streaming, p, std::to_string(cut).c_str());
        size_t n = 0;
        const uint8_t* got = r.bodyPrefix(n);
        TEST_ASSERT_EQUAL_MESSAGE(3, n, std::to_string(cut).c_str());
        TEST_ASSERT_EQUAL_MEMORY(prefix.data(), got, 3);
    }
}

static void test_nul_inside_the_head_is_still_bad() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Bad, streamFeed(r, std::string("POST /firmware/upload HTTP/1.1\r\nHo\x00st: d\r\n"
                                                                "Content-Length: 5\r\n\r\n", 63)));
    TEST_ASSERT_EQUAL(HttpParse::Bad, streamFeed(r, std::string("POST /firmware/upl\x00" "ad HTTP/1.1\r\n", 32)));
    // A NUL in a head that is still incomplete is refused at once
    HttpRequest q;
    const std::string part("GET /x HTTP/1.1\r\nHo\x00", 21);
    TEST_ASSERT_EQUAL(HttpParse::Bad, q.feed((const uint8_t*)part.data(), part.size()));
}

static void test_buffered_body_with_nul_is_bad_however_it_arrives() {
    const std::string head = "POST /login HTTP/1.1\r\nContent-Length: 6\r\n\r\n";
    const std::string body("ab\x00" "cde", 6);
    HttpRequest together;
    std::string all = head + body;
    TEST_ASSERT_EQUAL(HttpParse::Bad, together.feed((const uint8_t*)all.data(), all.size()));
    HttpRequest apart;
    TEST_ASSERT_EQUAL(HttpParse::Incomplete, apart.feed((const uint8_t*)head.data(), head.size()));
    TEST_ASSERT_EQUAL(HttpParse::Bad, apart.feed((const uint8_t*)body.data(), body.size()));
}

static void test_stream_path_head_alone_streams_with_empty_prefix() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Streaming,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: 5\r\n\r\n"));
    size_t n = 99;
    r.bodyPrefix(n);
    TEST_ASSERT_EQUAL(0, n);
}

static void test_stream_path_without_content_length_is_bad() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Bad, streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nHost: d\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpParse::Bad, streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: 0\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpParse::Bad, streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: 12x\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpParse::Bad,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"));
}

static void test_stream_path_over_max_is_too_large() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Streaming,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: " +
                                        std::to_string(UPLOAD_MAX) + "\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpParse::TooLarge,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: " +
                                        std::to_string(UPLOAD_MAX + 1) + "\r\n\r\n"));
    TEST_ASSERT_EQUAL(HttpParse::TooLarge,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"));
}

static void test_stream_prefix_longer_than_content_length_is_bad() {
    HttpRequest r;
    TEST_ASSERT_EQUAL(HttpParse::Bad,
                      streamFeed(r, "POST /firmware/upload HTTP/1.1\r\nContent-Length: 3\r\n\r\nABCD"));
}

static void test_other_paths_still_buffered() {
    HttpRequest r;
    r.streamPath("/firmware/upload");
    // A large body on another path is still refused, a small one still buffered and Complete
    const std::string big = "POST /login HTTP/1.1\r\nContent-Length: 300000\r\n\r\n";
    TEST_ASSERT_EQUAL(HttpParse::TooLarge, r.feed((const uint8_t*)big.data(), big.size()));
    r.reset();
    const std::string small = "POST /login HTTP/1.1\r\nContent-Length: 4\r\n\r\nuser";
    TEST_ASSERT_EQUAL(HttpParse::Complete, r.feed((const uint8_t*)small.data(), small.size()));
    TEST_ASSERT_EQUAL(4, r.bodyLength());
    // GET on the stream path is an ordinary request, and the stream path survives reset()
    r.reset();
    const std::string g = "GET /firmware/upload HTTP/1.1\r\nHost: d\r\n\r\n";
    TEST_ASSERT_EQUAL(HttpParse::Complete, r.feed((const uint8_t*)g.data(), g.size()));
    r.reset();
    const std::string again = "POST /firmware/upload HTTP/1.1\r\nContent-Length: 9\r\n\r\n";
    TEST_ASSERT_EQUAL(HttpParse::Streaming, r.feed((const uint8_t*)again.data(), again.size()));
}

/// Parse an upload request head and ask the app whether it may go on
static int uploadStatus(const std::string& cookie, const std::string& csrfHeader, uint32_t now = 1000) {
    static HttpRequest req;
    std::string raw = "POST /firmware/upload HTTP/1.1\r\nHost: d\r\n";
    if (!cookie.empty()) raw += "Cookie: sid=" + cookie + "\r\n";
    if (!csrfHeader.empty()) raw += "X-CSRF-Token: " + csrfHeader + "\r\n";
    raw += "Content-Length: 900000\r\n\r\n";
    TEST_ASSERT_EQUAL(HttpParse::Streaming, streamFeed(req, raw));
    return app->authorizeUpload(req, 0x0A0A0641, now, *fb);
}

static void test_upload_needs_session() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(401, uploadStatus("", csrf));
    TEST_ASSERT_EQUAL(401, uploadStatus("0123456789abcdef0123456789abcdef", csrf));
    // An expired session is no session
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
    TEST_ASSERT_EQUAL(401, uploadStatus(token, csrf, 1000 + WEB_SESSION_IDLE_MS + 1));
}

static void test_request_with_valid_session_is_authenticated() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    get("/", token);
    TEST_ASSERT_TRUE(app->authenticated());
    get("/nope", token);  // any request: the session is what counts
    TEST_ASSERT_TRUE(app->authenticated());
    get("/");
    TEST_ASSERT_FALSE(app->authenticated());
    get("/", "0123456789abcdef0123456789abcdef");
    TEST_ASSERT_FALSE(app->authenticated());
    get("/", token, 1000 + WEB_SESSION_IDLE_MS + 1);  // expired
    TEST_ASSERT_FALSE(app->authenticated());
}

static void test_login_is_authenticated_only_with_right_password() {
    webSetUp();
    post("/login", "user=admin&password=wrongpasswd");
    TEST_ASSERT_FALSE(app->authenticated());
    post("/login", "user=admin&password=goodpasswd1");
    TEST_ASSERT_TRUE(app->authenticated());
    send("BAD\r\n\r\n");
    TEST_ASSERT_FALSE(app->authenticated());
}

static void test_upload_with_valid_session_is_authenticated() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(401, uploadStatus("", csrf));
    TEST_ASSERT_FALSE(app->authenticated());
    TEST_ASSERT_EQUAL(403, uploadStatus(token, ""));  // a session, though the upload is refused
    TEST_ASSERT_TRUE(app->authenticated());
    get("/");
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
    TEST_ASSERT_TRUE(app->authenticated());
}

static void test_upload_needs_csrf_header() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(403, uploadStatus(token, ""));
    std::string wrong = csrf;
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    TEST_ASSERT_EQUAL(403, uploadStatus(token, wrong));
    TEST_ASSERT_EQUAL(403, uploadStatus(token, csrf.substr(0, csrf.size() - 1)));
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
}

static void test_upload_refused_while_on_trial() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    fb->fw.trial = true;
    TEST_ASSERT_EQUAL(409, uploadStatus(token, csrf));
    // Authentication is checked first: a stranger learns nothing about the state
    TEST_ASSERT_EQUAL(401, uploadStatus("", csrf));
    TEST_ASSERT_EQUAL(403, uploadStatus(token, ""));
    fb->fw.trial = false;
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
}

static void test_second_upload_while_busy_is_refused() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    fb->fw.uploading = true;
    TEST_ASSERT_EQUAL(409, uploadStatus(token, csrf));
    fb->fw.uploading = false;
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
}

static std::string responseBody(const std::string& r) { return r.substr(r.find("\r\n\r\n") + 4); }

static std::string uploadResponse(int code, const char* text) {
    size_t len = 0;
    const char* resp = app->uploadResult(code, text, len);
    TEST_ASSERT_EQUAL(strlen(resp), len);
    return std::string(resp, len);
}

static void test_upload_result_names_a_different_key() {
    webSetUp();
    std::string r = uploadResponse(403, "different key");
    TEST_ASSERT_EQUAL(403, statusCode(r));
    TEST_ASSERT_TRUE(r.find("403 Forbidden") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("different key", responseBody(r).c_str());
}

static void test_upload_result_texts() {
    webSetUp();
    std::string r = uploadResponse(200, "3.3.0");
    TEST_ASSERT_EQUAL(200, statusCode(r));
    TEST_ASSERT_TRUE(r.find("Content-Type: text/plain") != std::string::npos);
    TEST_ASSERT_TRUE(r.find("Cache-Control: no-store") != std::string::npos);
    TEST_ASSERT_TRUE(r.find("Connection: close") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("ok 3.3.0", responseBody(r).c_str());
    TEST_ASSERT_EQUAL(responseBody(r).size(), atoi(between(r, "Content-Length: ", "\r\n").c_str()));

    // Only 200 is success; 0 is no HTTP status and is answered as an error
    r = uploadResponse(0, "3.3.0");
    TEST_ASSERT_EQUAL(500, statusCode(r));
    TEST_ASSERT_EQUAL_STRING("3.3.0", responseBody(r).c_str());
    r = uploadResponse(204, "x");
    TEST_ASSERT_EQUAL(500, statusCode(r));
    r = uploadResponse(409, "busy");
    TEST_ASSERT_EQUAL(409, statusCode(r));
    TEST_ASSERT_EQUAL_STRING("busy", responseBody(r).c_str());
    r = uploadResponse(401, "not_logged_in");
    TEST_ASSERT_EQUAL(401, statusCode(r));
    r = uploadResponse(403, "bad_csrf");
    TEST_ASSERT_EQUAL(403, statusCode(r));
    r = uploadResponse(400, "bad_signature");
    TEST_ASSERT_EQUAL(400, statusCode(r));
    TEST_ASSERT_EQUAL_STRING("bad_signature", responseBody(r).c_str());
    r = uploadResponse(413, "too_large");
    TEST_ASSERT_EQUAL(413, statusCode(r));
    // A text that is too long is cut, never overflows
    r = uploadResponse(400, std::string(500, 'x').c_str());
    TEST_ASSERT_EQUAL(400, statusCode(r));
    TEST_ASSERT_TRUE(r.size() < 400);
}

static void test_upload_result_server_errors_have_their_reason_phrase() {
    webSetUp();
    std::string r = uploadResponse(500, "write failed");
    TEST_ASSERT_TRUE(r.find("HTTP/1.1 500 Internal Server Error\r\n") == 0);
    r = uploadResponse(503, "x");
    TEST_ASSERT_TRUE(r.find("HTTP/1.1 503 Service Unavailable\r\n") == 0);
}

static const std::string TEST_KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static void test_update_server_form_needs_login_and_csrf() {
    webSetUp();
    std::string body = "url=http%3A%2F%2F10.0.0.1%3A8071&key=" + TEST_KEY;
    TEST_ASSERT_EQUAL(403, statusCode(post("/updateserver", "csrf=x&" + body)));
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(403, statusCode(post("/updateserver", "csrf=wrong&" + body, token)));
    TEST_ASSERT_EQUAL(403, statusCode(post("/updateserver", body, token)));
    TEST_ASSERT_EQUAL(405, statusCode(get("/updateserver", token)));
    TEST_ASSERT_EQUAL(0, fb->updateSaves);
}

static void test_update_server_post_saves_url_and_key() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/updateserver", "csrf=" + csrf + "&url=http%3A%2F%2F10.0.0.1%3A8071&fromdhcp=on&key=" +
                                                 TEST_KEY, token);
    TEST_ASSERT_EQUAL_MESSAGE(200, statusCode(resp), resp.c_str());
    TEST_ASSERT_EQUAL(1, fb->updateSaves);
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.1:8071", fb->savedUpdateUrl.c_str());
    TEST_ASSERT_EQUAL_STRING(TEST_KEY.c_str(), fb->savedUpdateKey.c_str());
    TEST_ASSERT_TRUE(fb->savedUpdateFromDhcp);
    TEST_ASSERT_FALSE(fb->savedUpdateClearKey);
    TEST_ASSERT_TRUE(resp.find("Update server settings saved.") != std::string::npos);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_update_server_post_empty_key_keeps_key() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(200, statusCode(post("/updateserver", "csrf=" + csrf + "&url=&key=", token)));
    TEST_ASSERT_EQUAL_STRING("", fb->savedUpdateKey.c_str());  // the backend keeps the stored key
    TEST_ASSERT_FALSE(fb->savedUpdateClearKey);
    TEST_ASSERT_FALSE(fb->savedUpdateFromDhcp);  // an unticked checkbox is not sent
    TEST_ASSERT_EQUAL(200, statusCode(post("/updateserver", "csrf=" + csrf + "&url=&key=&clearkey=on", token)));
    TEST_ASSERT_TRUE(fb->savedUpdateClearKey);
}

static void test_update_server_post_rejects_bad_url() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/updateserver", "csrf=" + csrf + "&url=https%3A%2F%2Fsite&key=", token);
    TEST_ASSERT_EQUAL(400, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("alert alert-danger") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("http://host[:port][/path] &lt;here&gt;") != std::string::npos);  // escaped
    TEST_ASSERT_EQUAL(0, fb->updateSaves);
    // A URL longer than 127 characters never reaches the backend
    std::string longUrl = "http%3A%2F%2F" + std::string(121, 'h');
    TEST_ASSERT_EQUAL(400, statusCode(post("/updateserver", "csrf=" + csrf + "&url=" + longUrl + "&key=", token)));
    TEST_ASSERT_EQUAL(0, fb->updateSaves);
    fb->saveOk = false;
    TEST_ASSERT_EQUAL(500, statusCode(post("/updateserver", "csrf=" + csrf + "&url=&key=", token)));
}

static void test_update_server_page_never_shows_key() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/updateserver", "csrf=" + csrf + "&url=http%3A%2F%2F10.0.0.1&key=" + TEST_KEY, token);
    TEST_ASSERT_EQUAL(200, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find(TEST_KEY) == std::string::npos);
    std::string page = get("/firmware", token);
    TEST_ASSERT_TRUE(page.find(TEST_KEY) == std::string::npos);
    TEST_ASSERT_TRUE(page.find("Shared key</th><td>set</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("type=\"password\" id=\"f-key\" name=\"key\" maxlength=\"64\" value=\"\"") !=
                     std::string::npos);
}

static void test_firmware_page_shows_update_server_state() {
    webSetUp();
    strcpy(fb->fw.updateUrl, "http://10.0.0.1:8071/<x>");
    strcpy(fb->fw.updateState, "last check-in 3 min ago: <ok>");
    std::string csrf;
    std::string page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("action=\"/updateserver\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("value=\"http://10.0.0.1:8071/&lt;x&gt;\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("Check-ins</th><td>last check-in 3 min ago: &lt;ok&gt;</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("Shared key</th><td>not set</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"fromdhcp\" checked") == std::string::npos);
    fb->fw.updateFromDhcp = true;
    page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("name=\"fromdhcp\" checked") != std::string::npos);
}

static void test_firmware_page_with_longest_update_values_fits() {
    webSetUp();
    // Every character escaped (" becomes &quot;), plus an error message on top
    memset(fb->fw.updateUrl, '"', WEB_UPDATE_URL_MAX);
    fb->fw.updateUrl[WEB_UPDATE_URL_MAX] = '\0';
    memset(fb->fw.updateState, '"', sizeof(fb->fw.updateState) - 1);
    fb->fw.updateState[sizeof(fb->fw.updateState) - 1] = '\0';
    memset(fb->fw.lastResult, '"', sizeof(fb->fw.lastResult) - 1);
    fb->fw.lastResult[sizeof(fb->fw.lastResult) - 1] = '\0';
    memset(fb->fw.otherProblem, '"', sizeof(fb->fw.otherProblem) - 1);
    fb->fw.otherProblem[sizeof(fb->fw.otherProblem) - 1] = '\0';
    fb->fw.otherVersion[0] = '\0';
    std::string csrf;
    std::string token = logIn(csrf);
    std::string resp = post("/updateserver", "csrf=" + csrf + "&url=https%3A%2F%2Fsite&key=", token);
    TEST_ASSERT_EQUAL(400, statusCode(resp));
    TEST_ASSERT_TRUE(resp.find("</html>") != std::string::npos);
    char msg[64];
    snprintf(msg, sizeof(msg), "firmware page %u bytes", (unsigned)resp.size());
    TEST_MESSAGE(msg);
}

static void test_firmware_page_needs_login() {
    webSetUp();
    std::string r = get("/firmware");
    TEST_ASSERT_EQUAL(200, statusCode(r));
    TEST_ASSERT_TRUE(r.find("name=\"password\"") != std::string::npos);  // the login page
    TEST_ASSERT_TRUE(r.find("/firmware/upload") == std::string::npos);
    TEST_ASSERT_EQUAL(405, statusCode(post("/firmware", "")));
}

static void test_firmware_page_shows_slots_and_fingerprint() {
    webSetUp();
    std::string csrf;
    std::string page = get("/firmware", logIn(csrf));
    TEST_ASSERT_EQUAL(200, statusCode(page));
    TEST_ASSERT_TRUE(page.find("Running</th><td>slot A, version 3.2.0</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("State</th><td><span class=\"badge text-bg-success\">kept</span></td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find("Other slot</th><td>slot B, version 3.1.0</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("Last update</th><td>ok 3.2.0</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("Signing key</th><td>0123456789abcdef</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("type=\"file\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("action=\"/firmware/switch\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"confirm\"") != std::string::npos);
    // The page's csrf token (for the upload script and the switch form) is the session's
    TEST_ASSERT_TRUE(page.find("data-csrf=\"" + csrf + "\"") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("name=\"csrf\" value=\"" + csrf + "\"") != std::string::npos);
    // Slot B running: the labels follow
    fb->fw.runningSlot = 1;
    page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("slot B, version 3.2.0") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("slot A, version 3.1.0") != std::string::npos);
}

static void test_firmware_page_shows_trial_and_unusable_other_slot() {
    webSetUp();
    fb->fw.trial = true;
    fb->fw.otherVersion[0] = '\0';
    strcpy(fb->fw.otherProblem, "bad_signature");
    std::string csrf;
    std::string page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("<span class=\"badge text-bg-warning\">on trial</span>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("Other slot</th><td>slot B, not usable: bad_signature</td>") != std::string::npos);
    fb->fw.lastResult[0] = '\0';
    page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("Last update</th><td>none</td>") != std::string::npos);
}

static void test_firmware_page_escapes_versions() {
    webSetUp();
    strcpy(fb->fw.runningVersion, "<b>&\"'");
    strcpy(fb->fw.otherVersion, "<i>x");
    strcpy(fb->fw.lastResult, "<u>alert(1)</u>");
    strcpy(fb->fw.keyFingerprint, "\"><x");
    std::string csrf;
    std::string page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("version &lt;b&gt;&amp;&quot;&#39;</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("version &lt;i&gt;x</td>") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("&lt;u&gt;alert(1)&lt;/u&gt;") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("&quot;&gt;&lt;x") != std::string::npos);
    TEST_ASSERT_TRUE(page.find("<b>&") == std::string::npos);
    TEST_ASSERT_TRUE(page.find("<u>alert") == std::string::npos);
    TEST_ASSERT_TRUE(page.find("<i>x") == std::string::npos);
}

static void test_firmware_page_script_policy_is_limited_to_that_page() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string fw = get("/firmware", token);
    std::string csp = between(fw, "Content-Security-Policy: ", "\r\n");
    TEST_ASSERT_TRUE(csp.find("script-src 'sha256-") != std::string::npos);
    TEST_ASSERT_TRUE(csp.find("script-src 'unsafe-inline'") == std::string::npos);
    TEST_ASSERT_TRUE(csp.find("connect-src 'self'") != std::string::npos);
    TEST_ASSERT_EQUAL(0, csp.find("default-src 'none'"));
    TEST_ASSERT_TRUE(fw.find("<script>") != std::string::npos);
    TEST_ASSERT_TRUE(fw.find("<script src") == std::string::npos);
    // Every other page keeps the no-scripts policy, also after a Firmware page was built
    const std::string others[] = {get("/", token), get("/"), get("/nothing"), post("/reboot", "csrf=x", token)};
    for (const std::string& r : others) {
        TEST_ASSERT_TRUE(between(r, "Content-Security-Policy: ", "\r\n").find("script-src") == std::string::npos);
        TEST_ASSERT_TRUE(between(r, "Content-Security-Policy: ", "\r\n").find("connect-src") == std::string::npos);
        TEST_ASSERT_TRUE(r.find("<script") == std::string::npos);
    }
}

static std::string base64(const uint8_t* d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += T[v >> 18 & 63];
        s += T[v >> 12 & 63];
        s += i + 1 < n ? T[v >> 6 & 63] : '=';
        s += i + 2 < n ? T[v & 63] : '=';
    }
    return s;
}

static void test_firmware_page_csp_carries_the_hash_of_its_script() {
    webSetUp();
    std::string csrf;
    std::string page = get("/firmware", logIn(csrf));
    std::string script = between(page, "<script>", "</script>");
    TEST_ASSERT_TRUE(script.size() > 200);
    uint8_t digest[SHA256_DIGEST_LEN];
    sha256(script.data(), script.size(), digest);
    std::string csp = between(page, "Content-Security-Policy: ", "\r\n");
    TEST_ASSERT_TRUE_MESSAGE(csp.find("script-src 'sha256-" + base64(digest, sizeof(digest)) + "'") != std::string::npos,
                             csp.c_str());
    // The same page again carries the same hash (it is computed from the script, not stale state)
    std::string again = get("/firmware", logIn(csrf));
    TEST_ASSERT_EQUAL_STRING(csp.c_str(), between(again, "Content-Security-Policy: ", "\r\n").c_str());
}

static void test_refused_uploads_are_logged_with_the_source() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_EQUAL(401, uploadStatus("", csrf));
    TEST_ASSERT_EQUAL(1, fb->uploadRefused);
    TEST_ASSERT_EQUAL_UINT32(0x0A0A0641, fb->lastRefusedSource.ipv4());
    TEST_ASSERT_EQUAL(403, uploadStatus(token, ""));
    TEST_ASSERT_EQUAL(2, fb->uploadRefused);
    // A busy or on-trial refusal is by a logged-in admin: not an access attempt to log
    fb->fw.uploading = true;
    TEST_ASSERT_EQUAL(409, uploadStatus(token, csrf));
    fb->fw.uploading = false;
    TEST_ASSERT_EQUAL(0, uploadStatus(token, csrf));
    TEST_ASSERT_EQUAL(2, fb->uploadRefused);
}

static void test_main_page_links_to_firmware_page() {
    webSetUp();
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("href=\"/firmware\"") != std::string::npos);
    page = get("/firmware", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("href=\"/\"") != std::string::npos);
}

static void test_switch_needs_confirm_and_csrf() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    // No session
    TEST_ASSERT_EQUAL(403, statusCode(post("/firmware/switch", "csrf=" + csrf + "&confirm=yes")));
    // No or wrong csrf
    TEST_ASSERT_EQUAL(403, statusCode(post("/firmware/switch", "confirm=yes", token)));
    TEST_ASSERT_EQUAL(403, statusCode(post("/firmware/switch", "csrf=nope&confirm=yes", token)));
    // No confirmation: the page again with a message, nothing happens
    std::string r = post("/firmware/switch", "csrf=" + csrf, token);
    TEST_ASSERT_EQUAL(400, statusCode(r));
    TEST_ASSERT_TRUE(r.find("alert alert-danger") != std::string::npos);
    r = post("/firmware/switch", "csrf=" + csrf + "&confirm=no", token);
    TEST_ASSERT_EQUAL(400, statusCode(r));
    TEST_ASSERT_EQUAL(0, fb->switches);
    TEST_ASSERT_EQUAL(0, fb->reboots);
    TEST_ASSERT_EQUAL(405, statusCode(get("/firmware/switch", token)));
}

static void test_switch_switches_and_reboots() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string r = post("/firmware/switch", "csrf=" + csrf + "&confirm=yes", token);
    TEST_ASSERT_EQUAL(200, statusCode(r));
    TEST_ASSERT_EQUAL(1, fb->switches);
    TEST_ASSERT_EQUAL(1, fb->reboots);
    TEST_ASSERT_TRUE(r.find("restarts now") != std::string::npos);
}

static void test_switch_refused_when_other_slot_invalid() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    fb->switchOk = false;
    std::string r = post("/firmware/switch", "csrf=" + csrf + "&confirm=yes", token);
    TEST_ASSERT_EQUAL(409, statusCode(r));
    TEST_ASSERT_TRUE(r.find("alert alert-danger") != std::string::npos);
    TEST_ASSERT_TRUE(r.find("action=\"/firmware/switch\"") != std::string::npos);  // the Firmware page again
    TEST_ASSERT_EQUAL(1, fb->switches);
    TEST_ASSERT_EQUAL(0, fb->reboots);
}

static void test_status_text_lists_all_keys() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    std::string r = get("/firmware/status", token);
    TEST_ASSERT_EQUAL(200, statusCode(r));
    TEST_ASSERT_TRUE(r.find("Content-Type: text/plain") != std::string::npos);
    TEST_ASSERT_TRUE(r.find("Cache-Control: no-store") != std::string::npos);
    std::string body = responseBody(r);
    TEST_ASSERT_EQUAL_STRING("running_slot=A\nrunning_version=3.2.0\nstate=kept\nslot_a=3.2.0\nslot_b=3.1.0\n"
                             "last_result=ok 3.2.0\nkey=0123456789abcdef\n",
                             body.c_str());
    TEST_ASSERT_EQUAL(body.size(), atoi(between(r, "Content-Length: ", "\r\n").c_str()));
    // Slot B on trial, slot A unusable: versions of unusable slots are empty
    fb->fw.runningSlot = 1;
    fb->fw.trial = true;
    fb->fw.otherVersion[0] = '\0';
    fb->fw.lastResult[0] = '\0';
    body = responseBody(get("/firmware/status", token));
    TEST_ASSERT_EQUAL_STRING("running_slot=B\nrunning_version=3.2.0\nstate=trial\nslot_a=\nslot_b=3.2.0\n"
                             "last_result=\nkey=0123456789abcdef\n",
                             body.c_str());
    // Text that could break the one-line format is flattened
    strcpy(fb->fw.lastResult, "a\nb\r=c");
    TEST_ASSERT_TRUE(get("/firmware/status", token).find("last_result=a b =c\n") != std::string::npos);
    TEST_ASSERT_EQUAL(405, statusCode(post("/firmware/status", "csrf=" + csrf, token)));
}

static void test_status_text_needs_login() {
    webSetUp();
    std::string r = get("/firmware/status");
    TEST_ASSERT_EQUAL(401, statusCode(r));
    TEST_ASSERT_TRUE(r.find("running_slot") == std::string::npos);
    TEST_ASSERT_TRUE(r.find("0123456789abcdef") == std::string::npos);
}

static void test_longest_values_still_fit_the_firmware_page() {
    webSetUp();
    strcpy(fb->st.hostname, std::string(32, 'h').c_str());
    strcpy(fb->fw.runningVersion, std::string(16, '"').c_str());  // every character escaped to 6
    strcpy(fb->fw.otherVersion, std::string(16, '"').c_str());
    strcpy(fb->fw.lastResult, std::string(63, '"').c_str());
    strcpy(fb->fw.keyFingerprint, std::string(16, '"').c_str());
    fb->fw.trial = true;
    std::string csrf;
    std::string token = logIn(csrf);
    std::string page = get("/firmware", token);
    TEST_ASSERT_EQUAL(200, statusCode(page));
    TEST_ASSERT_TRUE(page.find("</html>") != std::string::npos);
    // The unusable-slot variant and the refused-switch page too
    fb->fw.otherVersion[0] = '\0';
    strcpy(fb->fw.otherProblem, std::string(23, '"').c_str());
    page = get("/firmware", token);
    TEST_ASSERT_TRUE(page.find("</html>") != std::string::npos);
    fb->switchOk = false;
    fb->fw.trial = false;
    page = post("/firmware/switch", "csrf=" + csrf + "&confirm=yes", token);
    TEST_ASSERT_EQUAL(409, statusCode(page));
    TEST_ASSERT_TRUE(page.find("</html>") != std::string::npos);
    // The status text with the longest values
    TEST_ASSERT_EQUAL(200, statusCode(get("/firmware/status", token)));
}

static void test_pages_carry_the_project_name() {
    webSetUp();
    std::string login = get("/");  // no session: the login page
    TEST_ASSERT_TRUE(login.find("<title>RF-Power-Monitor login</title>") != std::string::npos);
    TEST_ASSERT_TRUE(login.find("</svg>RF-Power-Monitor</span>") != std::string::npos);
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("<title>RF-Power-Monitor</title>") != std::string::npos);
}

static void test_status_page_shows_switch() {
    webSetUp();
    std::string csrf;
    std::string token = logIn(csrf);
    TEST_ASSERT_TRUE(get("/", token).find("Switch</th><td>none seen</td>") != std::string::npos);
    strcpy(fb->st.lldpSwitch, "ex2200, port ge-0/0/5.0");
    TEST_ASSERT_TRUE(get("/", token).find("Switch</th><td>ex2200, port ge-0/0/5.0</td>") != std::string::npos);
    strcpy(fb->st.lldpSwitchAddr, "10.132.0.2");
    TEST_ASSERT_TRUE(get("/", token).find("Switch</th><td>ex2200, port ge-0/0/5.0 (10.132.0.2)</td>") !=
                     std::string::npos);
}

static void test_status_page_escapes_switch_name() {
    webSetUp();
    strcpy(fb->st.lldpSwitch, "<b>sw&1</b>, port \"x\"");
    std::string csrf;
    std::string page = get("/", logIn(csrf));
    TEST_ASSERT_TRUE(page.find("Switch</th><td>&lt;b&gt;sw&amp;1&lt;/b&gt;, port &quot;x&quot;</td>") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(page.find("<b>sw") == std::string::npos);
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

// ---- StreamBody: reading a streamed body to its end ----

static void test_stream_body_counts_down_to_done() {
    StreamBody b;
    b.begin(1000, 5000);
    TEST_ASSERT_FALSE(b.done());
    TEST_ASSERT_EQUAL_UINT32(1000, b.left());
    b.consumed(600, 5100);
    TEST_ASSERT_EQUAL_UINT32(400, b.left());
    b.consumed(400, 5200);
    TEST_ASSERT_TRUE(b.done());
    TEST_ASSERT_EQUAL_UINT32(0, b.left());
}

static void test_stream_body_never_goes_below_zero() {
    StreamBody b;
    b.begin(10, 0);
    b.consumed(25, 1);  // a caller bug must not wrap around to 4 GB left
    TEST_ASSERT_TRUE(b.done());
    TEST_ASSERT_EQUAL_UINT32(0, b.left());
}

static void test_stream_body_empty_is_done_at_once() {
    StreamBody b;
    b.begin(0, 0);
    TEST_ASSERT_TRUE(b.done());
    TEST_ASSERT_FALSE(b.expired(0));
}

static void test_stream_body_expires_after_idle_timeout() {
    StreamBody b;
    b.begin(1000, 1000);
    TEST_ASSERT_FALSE(b.expired(1000 + STREAM_IDLE_TIMEOUT_MS));
    TEST_ASSERT_TRUE(b.expired(1000 + STREAM_IDLE_TIMEOUT_MS + 1));
}

static void test_stream_body_data_restarts_idle_timeout() {
    StreamBody b;
    b.begin(1000, 0);
    b.consumed(1, STREAM_IDLE_TIMEOUT_MS - 10);
    TEST_ASSERT_FALSE(b.expired(2 * STREAM_IDLE_TIMEOUT_MS - 20));
    TEST_ASSERT_TRUE(b.expired(2 * STREAM_IDLE_TIMEOUT_MS));
}

static void test_stream_body_expires_after_total_deadline_despite_trickle() {
    // One byte every 20 s never trips the idle timeout, but the whole body has a deadline
    StreamBody b;
    b.begin(100000, 0);
    uint32_t t = 0;
    while (t + 20000 <= STREAM_TOTAL_TIMEOUT_MS) {
        t += 20000;
        b.consumed(1, t);
        TEST_ASSERT_FALSE(b.expired(t));
    }
    TEST_ASSERT_TRUE(b.expired(STREAM_TOTAL_TIMEOUT_MS + 1));
}

static void test_stream_body_timeouts_survive_millis_wrap() {
    StreamBody b;
    b.begin(1000, 0xFFFFFF00u);
    TEST_ASSERT_FALSE(b.expired(0x00000100u));  // 512 ms later, across the wrap
    TEST_ASSERT_TRUE(b.expired(0xFFFFFF00u + STREAM_IDLE_TIMEOUT_MS + 1));
}

static void test_stream_body_done_never_expires() {
    StreamBody b;
    b.begin(10, 0);
    b.consumed(10, 1);
    TEST_ASSERT_FALSE(b.expired(STREAM_TOTAL_TIMEOUT_MS * 2));
}

static void test_stream_body_drain_expires_after_short_deadline_despite_trickle() {
    // A refused upload without a login: a client sending now and then holds the connection
    // for STREAM_DRAIN_TOTAL_MS, not the 5 minutes an accepted upload gets
    StreamBody b;
    b.begin(100000, 1000);
    b.drainOnly();
    uint32_t t = 1000;
    while (t + 1000 <= 1000 + STREAM_DRAIN_TOTAL_MS) {
        t += 1000;
        b.consumed(1, t);
        TEST_ASSERT_FALSE(b.expired(t));
    }
    TEST_ASSERT_TRUE(b.expired(1000 + STREAM_DRAIN_TOTAL_MS + 1));
}

static void test_stream_body_drain_expires_after_short_idle() {
    StreamBody b;
    b.begin(1000, 0);
    b.drainOnly();
    TEST_ASSERT_FALSE(b.expired(STREAM_DRAIN_IDLE_MS));
    TEST_ASSERT_TRUE(b.expired(STREAM_DRAIN_IDLE_MS + 1));
}

static void test_stream_body_begin_restores_full_deadlines_after_drain() {
    // The next upload on the static StreamBody gets the normal deadlines again
    StreamBody b;
    b.begin(1000, 0);
    b.drainOnly();
    b.begin(1000, 0);
    TEST_ASSERT_FALSE(b.expired(STREAM_DRAIN_TOTAL_MS + 1));
    TEST_ASSERT_FALSE(b.expired(STREAM_IDLE_TIMEOUT_MS));
}

static void test_stream_body_drain_deadline_fits_a_full_size_body() {
    // A full-size refused body must still drain at a modest LAN rate (100 KB/s), so the
    // client sees the answer; the deadline is not so short that real clients get a reset
    TEST_ASSERT_TRUE(STREAM_DRAIN_TOTAL_MS >= HTTP_STREAM_MAX / 100);
    TEST_ASSERT_TRUE(STREAM_DRAIN_TOTAL_MS < STREAM_TOTAL_TIMEOUT_MS / 10);
    TEST_ASSERT_TRUE(STREAM_DRAIN_IDLE_MS < STREAM_DRAIN_TOTAL_MS);
}

// ---- Default password: two copies, lock (default_password.h) ----

static void test_default_password_valid_needs_ten_alphabet_chars() {
    TEST_ASSERT_TRUE(webDefaultPasswordIsValid("k7mq2xr9tp"));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid(nullptr));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid(""));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9t"));    // 9
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9tpa"));  // 11
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9t0"));   // 0 is not in the alphabet
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("K7MQ2XR9TP"));   // upper case
}

static void test_default_password_both_copies_equal_writes_nothing() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "k7mq2xr9tp", false, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_FALSE(c.writeA);
    TEST_ASSERT_FALSE(c.writeB);
    TEST_ASSERT_FALSE(c.lostWhileLocked);
}

static void test_default_password_missing_b_is_repaired_from_a() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", nullptr, true, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_FALSE(c.writeA);
    TEST_ASSERT_TRUE(c.writeB);
    TEST_ASSERT_FALSE(c.lostWhileLocked);
}

static void test_default_password_corrupt_a_uses_b_and_repairs_a() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2x\xff", "k7mq2xr9tp", true, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyB);
    TEST_ASSERT_TRUE(c.writeA);
    TEST_ASSERT_FALSE(c.writeB);
    TEST_ASSERT_FALSE(c.lostWhileLocked);
}

static void test_default_password_copies_differ_a_wins() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "abcdefghij", false, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_FALSE(c.writeA);
    TEST_ASSERT_TRUE(c.writeB);
}

static void test_default_password_old_firmware_single_file_is_copy_a() {
    // A device updated from 3.4.0 has only data/web_default.txt, which is copy A
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", nullptr, false, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_TRUE(c.writeB);
}

static void test_default_password_none_valid_unlocked_is_not_lost() {
    DefaultPasswordChoice c = chooseDefaultPassword(nullptr, "", false, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::None);
    TEST_ASSERT_TRUE(c.writeA);
    TEST_ASSERT_TRUE(c.writeB);
    TEST_ASSERT_FALSE(c.lostWhileLocked);
}

static void test_default_password_none_valid_locked_is_lost() {
    DefaultPasswordChoice c = chooseDefaultPassword("garbage", "k7mq2", true, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::None);
    TEST_ASSERT_TRUE(c.writeA);
    TEST_ASSERT_TRUE(c.writeB);
    TEST_ASSERT_TRUE(c.lostWhileLocked);
}

static void test_default_password_lost_marker_keeps_reporting_after_reboot() {
    // The boot that found no valid copy wrote new ones; the next boot finds them valid,
    // but the label still has the old password until the bench locks again
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "k7mq2xr9tp", true, true);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_TRUE(c.lostWhileLocked);
}

static void test_default_password_copies_differ_while_locked_is_lost() {
    // Firmware before 3.5.0 (after a rollback) replaces only copy A in option 7
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "abcdefghij", true, false);
    TEST_ASSERT_TRUE(c.source == DefaultPasswordSource::CopyA);
    TEST_ASSERT_TRUE(c.writeB);
    TEST_ASSERT_TRUE(c.lostWhileLocked);
}

static void test_default_password_lost_marker_ignored_when_unlocked() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "k7mq2xr9tp", false, true);
    TEST_ASSERT_FALSE(c.lostWhileLocked);
}

static void test_made_default_password_is_valid() {
    uint8_t random[WEB_DEFAULT_PASSWORD_LEN];
    for (size_t i = 0; i < sizeof(random); i++) random[i] = (uint8_t)(i * 37 + 200);
    char out[WEB_DEFAULT_PASSWORD_LEN + 1];
    webMakeDefaultPassword(random, out);
    TEST_ASSERT_TRUE(webDefaultPasswordIsValid(out));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_known_vectors);
    RUN_TEST(test_stream_body_counts_down_to_done);
    RUN_TEST(test_stream_body_never_goes_below_zero);
    RUN_TEST(test_stream_body_empty_is_done_at_once);
    RUN_TEST(test_stream_body_expires_after_idle_timeout);
    RUN_TEST(test_stream_body_data_restarts_idle_timeout);
    RUN_TEST(test_stream_body_expires_after_total_deadline_despite_trickle);
    RUN_TEST(test_stream_body_timeouts_survive_millis_wrap);
    RUN_TEST(test_stream_body_done_never_expires);
    RUN_TEST(test_stream_body_drain_expires_after_short_deadline_despite_trickle);
    RUN_TEST(test_stream_body_drain_expires_after_short_idle);
    RUN_TEST(test_stream_body_begin_restores_full_deadlines_after_drain);
    RUN_TEST(test_stream_body_drain_deadline_fits_a_full_size_body);
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
    RUN_TEST(test_reset_wipes_the_received_bytes);
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
    RUN_TEST(test_timelog_needs_session_and_csrf);
    RUN_TEST(test_timelog_saves_through_backend);
    RUN_TEST(test_timelog_bad_value_shows_error_and_saves_nothing);
    RUN_TEST(test_timelog_overlong_or_missing_value_is_refused_before_the_backend);
    RUN_TEST(test_timelog_empty_field_is_refused_with_a_reason);
    RUN_TEST(test_main_page_shows_time_rows_and_card);
    RUN_TEST(test_change_hostname);
    RUN_TEST(test_invalid_hostname_is_not_saved);
    RUN_TEST(test_device_name_form_shows_contact_and_location);
    RUN_TEST(test_device_name_post_saves_all_three);
    RUN_TEST(test_device_name_post_accepts_empty_contact_and_location);
    RUN_TEST(test_device_name_post_rejects_bad_contact);
    RUN_TEST(test_device_name_post_rejects_bad_location);
    RUN_TEST(test_device_name_post_without_contact_or_location_keeps_them);
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
    RUN_TEST(test_longest_values_with_an_error_message_still_fit_the_page);
    RUN_TEST(test_pages_carry_the_project_name);
    RUN_TEST(test_status_page_shows_switch);
    RUN_TEST(test_status_page_escapes_switch_name);
    RUN_TEST(test_ipv6_form_shows_the_stored_static_values);
    RUN_TEST(test_static_ipv6_is_validated_normalized_saved_and_reboots);
    RUN_TEST(test_empty_ipv6_form_removes_the_static_address);
    RUN_TEST(test_invalid_static_ipv6_is_not_saved);
    RUN_TEST(test_failed_ipv6_save_does_not_reboot);
    RUN_TEST(test_stream_path_returns_streaming_with_prefix);
    RUN_TEST(test_stream_path_head_alone_streams_with_empty_prefix);
    RUN_TEST(test_stream_path_without_content_length_is_bad);
    RUN_TEST(test_stream_path_over_max_is_too_large);
    RUN_TEST(test_stream_prefix_longer_than_content_length_is_bad);
    RUN_TEST(test_other_paths_still_buffered);
    RUN_TEST(test_upload_needs_session);
    RUN_TEST(test_upload_needs_csrf_header);
    RUN_TEST(test_request_with_valid_session_is_authenticated);
    RUN_TEST(test_login_is_authenticated_only_with_right_password);
    RUN_TEST(test_upload_with_valid_session_is_authenticated);
    RUN_TEST(test_upload_refused_while_on_trial);
    RUN_TEST(test_second_upload_while_busy_is_refused);
    RUN_TEST(test_upload_result_names_a_different_key);
    RUN_TEST(test_upload_result_texts);
    RUN_TEST(test_upload_result_server_errors_have_their_reason_phrase);
    RUN_TEST(test_update_server_form_needs_login_and_csrf);
    RUN_TEST(test_update_server_post_saves_url_and_key);
    RUN_TEST(test_update_server_post_empty_key_keeps_key);
    RUN_TEST(test_update_server_post_rejects_bad_url);
    RUN_TEST(test_update_server_page_never_shows_key);
    RUN_TEST(test_firmware_page_shows_update_server_state);
    RUN_TEST(test_firmware_page_with_longest_update_values_fits);
    RUN_TEST(test_firmware_page_needs_login);
    RUN_TEST(test_firmware_page_shows_slots_and_fingerprint);
    RUN_TEST(test_firmware_page_shows_trial_and_unusable_other_slot);
    RUN_TEST(test_firmware_page_escapes_versions);
    RUN_TEST(test_firmware_page_script_policy_is_limited_to_that_page);
    RUN_TEST(test_firmware_page_csp_carries_the_hash_of_its_script);
    RUN_TEST(test_refused_uploads_are_logged_with_the_source);
    RUN_TEST(test_stream_prefix_with_nul_bytes_still_streams);
    RUN_TEST(test_stream_head_split_before_binary_prefix_still_streams);
    RUN_TEST(test_nul_inside_the_head_is_still_bad);
    RUN_TEST(test_buffered_body_with_nul_is_bad_however_it_arrives);
    RUN_TEST(test_main_page_links_to_firmware_page);
    RUN_TEST(test_switch_needs_confirm_and_csrf);
    RUN_TEST(test_switch_switches_and_reboots);
    RUN_TEST(test_switch_refused_when_other_slot_invalid);
    RUN_TEST(test_status_text_lists_all_keys);
    RUN_TEST(test_status_text_needs_login);
    RUN_TEST(test_longest_values_still_fit_the_firmware_page);
    RUN_TEST(test_default_password_valid_needs_ten_alphabet_chars);
    RUN_TEST(test_default_password_both_copies_equal_writes_nothing);
    RUN_TEST(test_default_password_missing_b_is_repaired_from_a);
    RUN_TEST(test_default_password_corrupt_a_uses_b_and_repairs_a);
    RUN_TEST(test_default_password_copies_differ_a_wins);
    RUN_TEST(test_default_password_old_firmware_single_file_is_copy_a);
    RUN_TEST(test_default_password_none_valid_unlocked_is_not_lost);
    RUN_TEST(test_default_password_none_valid_locked_is_lost);
    RUN_TEST(test_made_default_password_is_valid);
    RUN_TEST(test_default_password_lost_marker_keeps_reporting_after_reboot);
    RUN_TEST(test_default_password_lost_marker_ignored_when_unlocked);
    RUN_TEST(test_default_password_copies_differ_while_locked_is_lost);
    return UNITY_END();
}
