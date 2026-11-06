// Host unit tests for the update server wire format and MACs (lib/update_client/update_protocol).
// The vectors are the ones in update-server/tests/vectors.json (computed with Python hashlib),
// so the device and the site are checked against the same bytes.
// Run: pio test -e native -f native/test_update_protocol

#include <stdio.h>
#include <string.h>

#include <unity.h>
#include <update_protocol.h>

void setUp() {}
void tearDown() {}

namespace {

const char* NONCE = "00112233445566778899aabbccddeeff";

const char* CHECKIN0 =
    "serial=E6614103E7452D2F\nhostname=rf-power-monitor\nipv4=10.132.0.58\nmac=28:cd:c1:00:00:01\n"
    "version=3.4.0\nslot=A\ntrial=0\nupdate=idle\nfail=\nlast=none\nnonce=00112233445566778899aabbccddeeff\n";
const char* CHECKIN0_MAC = "1fb872a6bd65aebead7d931d1e702af2bd5b6881ed7466704fba5ab56099f134";

const char* CHECKIN4 =
    "serial=E6614103E7452D2F\nhostname=rfpm-lab\nipv4=\nipv6=2001:db8::5\nipv6=2001:db8::6\n"
    "ipv6=fd16:d833:4f0e:3ece::1\nipv6=fe80::2acd:c1ff:fe00:1\nmac=28:cd:c1:00:00:01\nversion=3.4.1-hil\n"
    "slot=B\ntrial=1\nupdate=downloading\nfail=3.4.1 download connection closed\nlast=installed 3.4.0\n"
    "nonce=ffeeddccbbaa99887766554433221100\n";
const char* CHECKIN4_MAC = "2a5998bd357cd8ab55faeaf25f51d680ed07eeed5bee6f55a6da9ec1b75ea6e2";

const char* REPLY_OK = "status=ok\nauth=374a87119fe2a56013337b9e2284bdc58273ea9e92c1fd4c34e527cd03a93abc\n";
const char* REPLY_UPDATE =
    "status=update\nversion=3.4.1\npath=/releases/3.4.1.upd\n"
    "auth=281f3161e74d74b2f3c4d043110abf325d03e766142098c22594f46b17da1e3e\n";

void testKey(uint8_t key[UPDATE_KEY_LEN]) {
    for (size_t i = 0; i < UPDATE_KEY_LEN; i++) key[i] = (uint8_t)i;
}

ReplyKind parse(const char* body, size_t len, const char* nonce, CheckinReply& out) {
    uint8_t key[UPDATE_KEY_LEN];
    testKey(key);
    return parseCheckinReply(body, len, nonce, key, out);
}

ReplyKind parse(const char* body, CheckinReply& out) {
    return parse(body, strlen(body), NONCE, out);
}

/// A reply with a correct auth line for the lines given (each ending in "\n")
void signedReply(const char* lines, char* out, size_t size) {
    uint8_t key[UPDATE_KEY_LEN];
    testKey(key);
    char buf[UPDATE_REPLY_MAX + 64];
    snprintf(buf, sizeof(buf), "%s\n%s", NONCE, lines);
    char mac[UPDATE_MAC_HEX + 1];
    updateMacHex(key, (const uint8_t*)buf, strlen(buf), mac);
    snprintf(out, size, "%sauth=%s\n", lines, mac);
}

CheckinInfo info0() {
    CheckinInfo in;
    memset(&in, 0, sizeof(in));
    strcpy(in.serial, "E6614103E7452D2F");
    strcpy(in.hostname, "rf-power-monitor");
    strcpy(in.ipv4, "10.132.0.58");
    strcpy(in.mac, "28:cd:c1:00:00:01");
    strcpy(in.version, "3.4.0");
    in.slot = 'A';
    strcpy(in.last, "none");
    strcpy(in.nonce, NONCE);
    return in;
}

}  // namespace

static void test_mac_matches_python_vectors() {
    uint8_t key[UPDATE_KEY_LEN];
    testKey(key);
    char mac[UPDATE_MAC_HEX + 1];
    updateMacHex(key, (const uint8_t*)CHECKIN0, strlen(CHECKIN0), mac);
    TEST_ASSERT_EQUAL_STRING(CHECKIN0_MAC, mac);
    updateMacHex(key, (const uint8_t*)CHECKIN4, strlen(CHECKIN4), mac);
    TEST_ASSERT_EQUAL_STRING(CHECKIN4_MAC, mac);
}

static void test_build_checkin_body_matches_vector() {
    char out[1024];
    CheckinInfo in = info0();
    size_t n = buildCheckinBody(in, out, sizeof(out));
    TEST_ASSERT_EQUAL(strlen(CHECKIN0), n);
    TEST_ASSERT_EQUAL_STRING(CHECKIN0, out);
}

static void test_build_checkin_body_with_four_ipv6_matches_vector() {
    CheckinInfo in = info0();
    strcpy(in.hostname, "rfpm-lab");
    in.ipv4[0] = '\0';
    strcpy(in.ipv6[0], "2001:db8::5");
    strcpy(in.ipv6[1], "2001:db8::6");
    strcpy(in.ipv6[2], "fd16:d833:4f0e:3ece::1");
    strcpy(in.ipv6[3], "fe80::2acd:c1ff:fe00:1");
    in.ipv6Count = 4;
    strcpy(in.version, "3.4.1-hil");
    in.slot = 'B';
    in.trial = true;
    in.downloading = true;
    strcpy(in.fail, "3.4.1 download connection closed");
    strcpy(in.last, "installed 3.4.0");
    strcpy(in.nonce, "ffeeddccbbaa99887766554433221100");
    char out[1024];
    TEST_ASSERT_EQUAL(strlen(CHECKIN4), buildCheckinBody(in, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(CHECKIN4, out);
}

static void test_build_checkin_body_returns_zero_when_too_small() {
    CheckinInfo in = info0();
    char out[1024];
    TEST_ASSERT_EQUAL(0, buildCheckinBody(in, out, strlen(CHECKIN0)));  // no room for the NUL
    TEST_ASSERT_EQUAL(strlen(CHECKIN0), buildCheckinBody(in, out, strlen(CHECKIN0) + 1));
}

static void test_build_checkin_body_rejects_newline_in_field() {
    CheckinInfo in = info0();
    strcpy(in.hostname, "evil\nupdate=x");
    char out[1024];
    TEST_ASSERT_EQUAL(0, buildCheckinBody(in, out, sizeof(out)));
}

static void test_parse_reply_ok_with_valid_auth() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::Ok, parse(REPLY_OK, r));
}

static void test_parse_reply_update_fills_version_and_path() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::Update, parse(REPLY_UPDATE, r));
    TEST_ASSERT_EQUAL_STRING("3.4.1", r.version);
    TEST_ASSERT_EQUAL_STRING("/releases/3.4.1.upd", r.path);
}

static void test_parse_reply_accepts_missing_final_newline() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::Ok, parse(REPLY_OK, strlen(REPLY_OK) - 1, NONCE, r));
}

static void test_parse_reply_rejects_wrong_auth() {
    char body[256];
    strcpy(body, REPLY_OK);
    char* a = strstr(body, "auth=") + 5;
    a[10] = a[10] == '0' ? '1' : '0';
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse(body, r));
}

static void test_parse_reply_rejects_changed_content() {
    char body[256];
    strcpy(body, REPLY_UPDATE);
    body[strlen("status=update\nversion=3.4.")] = '2';  // 3.4.2 under the 3.4.1 MAC
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse(body, r));
}

static void test_parse_reply_rejects_missing_auth() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse("status=ok\n", r));
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse("status=update\nversion=3.4.1\npath=/x.upd\n", r));
}

static void test_parse_reply_rejects_other_nonce() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse(REPLY_OK, strlen(REPLY_OK), "ffeeddccbbaa99887766554433221100", r));
}

static void test_parse_reply_rejects_malformed_nonce() {
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse(REPLY_OK, strlen(REPLY_OK), "0011", r));
    TEST_ASSERT_EQUAL(ReplyKind::BadAuth, parse(REPLY_OK, strlen(REPLY_OK), nullptr, r));
}

static void test_parse_reply_rejects_truncated_at_every_length() {
    const char* replies[] = {REPLY_OK, REPLY_UPDATE};
    for (const char* full : replies) {
        size_t len = strlen(full);
        for (size_t cut = 0; cut + 1 < len; cut++) {  // len - 1 drops only the final "\n": still valid
            CheckinReply r;
            ReplyKind k = parse(full, cut, NONCE, r);
            TEST_ASSERT_TRUE_MESSAGE(k == ReplyKind::Malformed || k == ReplyKind::BadAuth, full);
        }
    }
}

static void test_parse_reply_rejects_over_max_length() {
    static char body[UPDATE_REPLY_MAX + 2];
    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    CheckinReply r;
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
}

static void test_parse_reply_rejects_update_without_version_or_path() {
    char body[512];
    CheckinReply r;
    signedReply("status=update\npath=/releases/3.4.1.upd\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
    signedReply("status=update\nversion=3.4.1\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
}

static void test_parse_reply_rejects_relative_path() {
    char body[512];
    CheckinReply r;
    signedReply("status=update\nversion=3.4.1\npath=releases/3.4.1.upd\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
    signedReply("status=update\nversion=3.4.1\npath=/a b.upd\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
}

static void test_parse_reply_rejects_overlong_version() {
    char body[512];
    CheckinReply r;
    signedReply("status=update\nversion=3.4.1-12345678901\npath=/x.upd\n", body, sizeof(body));  // 17 chars
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
    signedReply("status=update\nversion=3.4.1-1234567890\npath=/x.upd\n", body, sizeof(body));  // 16 chars
    TEST_ASSERT_EQUAL(ReplyKind::Update, parse(body, r));
}

static void test_parse_reply_rejects_unknown_status() {
    char body[512];
    CheckinReply r;
    signedReply("status=reboot\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
    signedReply("status=ok\nextra=1\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
}

static void test_parse_reply_rejects_nul_and_cr() {
    char body[512];
    CheckinReply r;
    signedReply("status=ok\r\n", body, sizeof(body));
    TEST_ASSERT_EQUAL(ReplyKind::Malformed, parse(body, r));
    char withNul[] = "status=ok\nauth=374a87119fe2a56013337b9e2284bdc58273ea9e92c1fd4c34e527cd03a93abc\n";
    withNul[3] = '\0';
    TEST_ASSERT_NOT_EQUAL(ReplyKind::Ok, parse(withNul, sizeof(withNul) - 1, NONCE, r));
}

static void test_parse_key_hex_accepts_64_hex_either_case() {
    uint8_t key[UPDATE_KEY_LEN];
    TEST_ASSERT_TRUE(parseKeyHex("000102030405060708090A0B0C0D0E0F101112131415161718191a1b1c1d1e1f", key));
    for (size_t i = 0; i < UPDATE_KEY_LEN; i++) TEST_ASSERT_EQUAL_UINT8(i, key[i]);
}

static void test_parse_key_hex_rejects_63_and_65_chars_and_non_hex() {
    uint8_t key[UPDATE_KEY_LEN];
    TEST_ASSERT_FALSE(parseKeyHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1", key));
    TEST_ASSERT_FALSE(parseKeyHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0", key));
    TEST_ASSERT_FALSE(parseKeyHex("g00102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key));
    TEST_ASSERT_FALSE(parseKeyHex("", key));
    TEST_ASSERT_FALSE(parseKeyHex(nullptr, key));
}

static void test_status_word_reads_error_replies() {
    char w[16];
    replyStatusWord("status=badauth\n", 15, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("badauth", w);
    replyStatusWord("status=replay", 13, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("replay", w);
    replyStatusWord("<html>502 Bad Gateway</html>", 28, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("", w);
    replyStatusWord("", 0, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("", w);
}

static void test_status_word_drops_control_and_odd_characters() {
    // Unauthenticated text must not put terminal escape sequences on the serial console
    char w[16];
    const char evil[] = "status=\x1b[2J\x1b]0;pwned\x07x\n";
    replyStatusWord(evil, sizeof(evil) - 1, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("", w);  // stops at the first character outside [a-z0-9-]
    replyStatusWord("status=bad-auth2\r\n", 18, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("bad-auth2", w);
}

static void test_status_word_is_cut_to_the_buffer() {
    char w[8];
    replyStatusWord("status=abcdefghijklmnop", 23, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("abcdefg", w);
    char nul[] = "status=ok\0more";
    replyStatusWord(nul, sizeof(nul) - 1, w, sizeof(w));
    TEST_ASSERT_EQUAL_STRING("ok", w);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_mac_matches_python_vectors);
    RUN_TEST(test_build_checkin_body_matches_vector);
    RUN_TEST(test_build_checkin_body_with_four_ipv6_matches_vector);
    RUN_TEST(test_build_checkin_body_returns_zero_when_too_small);
    RUN_TEST(test_build_checkin_body_rejects_newline_in_field);
    RUN_TEST(test_parse_reply_ok_with_valid_auth);
    RUN_TEST(test_parse_reply_update_fills_version_and_path);
    RUN_TEST(test_parse_reply_accepts_missing_final_newline);
    RUN_TEST(test_parse_reply_rejects_wrong_auth);
    RUN_TEST(test_parse_reply_rejects_changed_content);
    RUN_TEST(test_parse_reply_rejects_missing_auth);
    RUN_TEST(test_parse_reply_rejects_other_nonce);
    RUN_TEST(test_parse_reply_rejects_malformed_nonce);
    RUN_TEST(test_parse_reply_rejects_truncated_at_every_length);
    RUN_TEST(test_parse_reply_rejects_over_max_length);
    RUN_TEST(test_parse_reply_rejects_update_without_version_or_path);
    RUN_TEST(test_parse_reply_rejects_relative_path);
    RUN_TEST(test_parse_reply_rejects_overlong_version);
    RUN_TEST(test_parse_reply_rejects_unknown_status);
    RUN_TEST(test_parse_reply_rejects_nul_and_cr);
    RUN_TEST(test_parse_key_hex_accepts_64_hex_either_case);
    RUN_TEST(test_parse_key_hex_rejects_63_and_65_chars_and_non_hex);
    RUN_TEST(test_status_word_reads_error_replies);
    RUN_TEST(test_status_word_drops_control_and_odd_characters);
    RUN_TEST(test_status_word_is_cut_to_the_buffer);
    return UNITY_END();
}
