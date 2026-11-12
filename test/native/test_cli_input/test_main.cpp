// Host unit tests for lib/cli_input: the serial menu's line editor and what each prompt accepts.
// Run: pio test -e native -f native/test_cli_input

#include <unity.h>
#include <string.h>
#include <string>
#include <cli_input.h>
#include <netconfig.h>
#include <settings_codec.h>
#include <update_url.h>

void setUp() {}
void tearDown() {}

// ---- line editor ----

static LineEditor ed;

/// Feed text; returns the events as letters (n None, e Echo, x Erase, L Line, O Overflow)
static std::string type(const char* text, bool secret = false) {
    std::string events;
    for (const char* p = text; *p; p++) {
        switch (ed.feed(*p, secret)) {
        case LineEvent::None: events += 'n'; break;
        case LineEvent::Echo: events += 'e'; break;
        case LineEvent::Erase: events += 'x'; break;
        case LineEvent::Line: events += 'L'; break;
        case LineEvent::Overflow: events += 'O'; break;
        }
    }
    return events;
}

static void test_line_editor_cr_ends_a_line() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("eeL", type("12\r").c_str());
    TEST_ASSERT_EQUAL_STRING("12", ed.line());
}

static void test_line_editor_lf_alone_ends_a_line() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("eL", type("3\n").c_str());
    TEST_ASSERT_EQUAL_STRING("3", ed.line());
}

static void test_line_editor_crlf_is_one_line_end() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("eLneL", type("1\r\n2\r").c_str());
    TEST_ASSERT_EQUAL_STRING("2", ed.line());
}

static void test_line_editor_lf_lf_is_two_empty_lines() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("LL", type("\n\n").c_str());
    TEST_ASSERT_EQUAL_STRING("", ed.line());
}

static void test_line_editor_cr_cr_is_two_empty_lines() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("LL", type("\r\r").c_str());
}

static void test_line_editor_empty_enter_gives_an_empty_line() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("L", type("\r").c_str());
    TEST_ASSERT_EQUAL_STRING("", ed.line());
}

static void test_line_editor_echoes_the_character() {
    ed.reset();
    TEST_ASSERT_EQUAL(LineEvent::Echo, ed.feed('a', false));
    TEST_ASSERT_EQUAL('a', ed.echo());
}

static void test_line_editor_secret_input_echoes_a_star() {
    ed.reset();
    TEST_ASSERT_EQUAL(LineEvent::Echo, ed.feed('p', true));
    TEST_ASSERT_EQUAL('*', ed.echo());
    type("w\r", true);
    TEST_ASSERT_EQUAL_STRING("pw", ed.line());  // the real characters are kept
}

static void test_line_editor_backspace_and_delete_remove_the_last_character() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("eeexeexL", type("abc\bde\x7F\r").c_str());
    TEST_ASSERT_EQUAL_STRING("abd", ed.line());
}

static void test_line_editor_backspace_on_an_empty_line_does_nothing() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("nneL", type("\b\x7F" "a\r").c_str());
    TEST_ASSERT_EQUAL_STRING("a", ed.line());
}

static void test_line_editor_drops_control_characters() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("ennneL", type("a\x1b\t\x01" "b\r").c_str());
    TEST_ASSERT_EQUAL_STRING("ab", ed.line());
}

static void test_line_editor_drops_bytes_above_ascii() {
    ed.reset();
    TEST_ASSERT_EQUAL_STRING("ennL", type("a\xc3\xa6\r").c_str());  // UTF-8 "æ"
    TEST_ASSERT_EQUAL_STRING("a", ed.line());
}

static void test_line_editor_drops_nul() {
    ed.reset();
    TEST_ASSERT_EQUAL(LineEvent::Echo, ed.feed('a', false));
    TEST_ASSERT_EQUAL(LineEvent::None, ed.feed('\0', false));
    TEST_ASSERT_EQUAL(LineEvent::Echo, ed.feed('b', false));
    TEST_ASSERT_EQUAL(LineEvent::Line, ed.feed('\r', false));
    TEST_ASSERT_EQUAL_STRING("ab", ed.line());
}

static void test_line_editor_accepts_space_and_tilde() {
    ed.reset();
    type(" ~\r");
    TEST_ASSERT_EQUAL_STRING(" ~", ed.line());
}

static void test_line_editor_accepts_exactly_line_max_characters() {
    ed.reset();
    std::string full(LineEditor::MAX_CHARS, 'x');
    type(full.c_str());
    TEST_ASSERT_EQUAL(LineEvent::Line, ed.feed('\r', false));
    TEST_ASSERT_EQUAL_STRING(full.c_str(), ed.line());
}

static void test_line_editor_refuses_a_line_one_character_too_long() {
    ed.reset();
    std::string full(LineEditor::MAX_CHARS, 'x');
    type(full.c_str());
    TEST_ASSERT_EQUAL(LineEvent::None, ed.feed('y', false));  // not echoed
    TEST_ASSERT_EQUAL(LineEvent::Overflow, ed.feed('\r', false));
}

static void test_line_editor_overflow_wipes_the_buffer() {
    ed.reset();
    std::string full(LineEditor::MAX_CHARS + 5, 's');
    type(full.c_str(), true);
    TEST_ASSERT_EQUAL(LineEvent::Overflow, ed.feed('\r', true));
    for (size_t i = 0; i <= LineEditor::MAX_CHARS; i++) TEST_ASSERT_EQUAL(0, ed.line()[i]);
}

static void test_line_editor_backspace_after_overflow_does_not_accept_the_line() {
    ed.reset();
    std::string full(LineEditor::MAX_CHARS + 1, 'x');
    type(full.c_str());
    ed.feed('\b', false);  // the dropped character is not the one erased: the line was cut
    TEST_ASSERT_EQUAL(LineEvent::Overflow, ed.feed('\r', false));
}

static void test_line_editor_next_line_after_overflow_works() {
    ed.reset();
    std::string full(LineEditor::MAX_CHARS + 1, 'x');
    type(full.c_str());
    ed.feed('\r', false);
    TEST_ASSERT_EQUAL_STRING("eL", type("1\r").c_str());
    TEST_ASSERT_EQUAL_STRING("1", ed.line());
}

static void test_line_editor_next_line_starts_empty() {
    ed.reset();
    type("abc\r");
    type("d\r");
    TEST_ASSERT_EQUAL_STRING("d", ed.line());
}

static void test_line_editor_wipe_clears_the_typed_password() {
    ed.reset();
    type("secret12\r", true);
    ed.wipe();
    for (size_t i = 0; i <= LineEditor::MAX_CHARS; i++) TEST_ASSERT_EQUAL(0, ed.line()[i]);
}

// ---- prompts ----

static char dest[128];
static char msg[CLI_MSG_MAX];

static void clear() {
    memset(dest, '#', sizeof(dest));
    dest[sizeof(dest) - 1] = '\0';
    msg[0] = '\0';
}

static void test_confirmed_accepts_y_in_either_case() {
    TEST_ASSERT_TRUE(cliConfirmed("y"));
    TEST_ASSERT_TRUE(cliConfirmed("Y"));
    TEST_ASSERT_TRUE(cliConfirmed("yes"));
}

static void test_confirmed_refuses_everything_else() {
    TEST_ASSERT_FALSE(cliConfirmed(""));
    TEST_ASSERT_FALSE(cliConfirmed("n"));
    TEST_ASSERT_FALSE(cliConfirmed(" y"));
}

static void test_address_empty_keeps_current() {
    clear();
    TEST_ASSERT_TRUE(cliTakeAddress("", "192.168.1.100", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", dest);
}

static void test_address_takes_a_valid_address() {
    clear();
    TEST_ASSERT_TRUE(cliTakeAddress("10.0.0.5", "192.168.1.100", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", dest);
}

static void test_address_refuses_bad_text_with_its_message() {
    clear();
    TEST_ASSERT_FALSE(cliTakeAddress("10.0.0.256", "192.168.1.100", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("'10.0.0.256' is not a valid IPv4 address", msg);
    TEST_ASSERT_EQUAL('#', dest[0]);  // unchanged
}

static void test_address_refuses_a_bad_current_value_on_enter() {
    clear();
    TEST_ASSERT_FALSE(cliTakeAddress("", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("'' is not a valid IPv4 address", msg);
}

static void test_text_empty_keeps_current() {
    clear();
    TEST_ASSERT_TRUE(cliTakeText("", "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("public", dest);
}

static void test_text_dash_clears_when_empty_is_allowed() {
    clear();
    TEST_ASSERT_TRUE(cliTakeText("-", "Rack 4", dest, SYS_TEXT_MAX_LENGTH, true, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
}

static void test_text_dash_refused_when_empty_is_not_allowed() {
    clear();
    TEST_ASSERT_FALSE(cliTakeText("-", "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Must not be empty", msg);
}

static void test_text_takes_exactly_max_length() {
    clear();
    std::string s(COMMUNITY_MAX_LENGTH, 'c');
    TEST_ASSERT_TRUE(cliTakeText(s.c_str(), "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING(s.c_str(), dest);
}

static void test_text_refuses_one_over_max_length() {
    clear();
    std::string s(COMMUNITY_MAX_LENGTH + 1, 'c');
    TEST_ASSERT_FALSE(cliTakeText(s.c_str(), "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Too long, at most 31 characters", msg);
    TEST_ASSERT_EQUAL('#', dest[0]);
}

static void test_text_refuses_non_printable_characters() {
    clear();
    TEST_ASSERT_FALSE(cliTakeText("ab\x01", "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Only printable ASCII characters are allowed", msg);
}

static void test_hostname_empty_keeps_current_even_if_not_a_hostname() {
    clear();
    TEST_ASSERT_TRUE(cliTakeHostname("", "Rack 4 / UPS", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Rack 4 / UPS", dest);  // free text from older firmware stays
}

static void test_hostname_dash_goes_back_to_the_default() {
    clear();
    TEST_ASSERT_TRUE(cliTakeHostname("-", "rack-4", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
}

static void test_hostname_takes_a_valid_name() {
    clear();
    TEST_ASSERT_TRUE(cliTakeHostname("rack-4", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("rack-4", dest);
}

static void test_hostname_refuses_an_invalid_name() {
    clear();
    TEST_ASSERT_FALSE(cliTakeHostname("-rack", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Not a valid hostname: 1-32 letters, digits and '-', not starting or ending with '-'",
                             msg);
}

static void test_trap_empty_keeps_target_and_dhcp_choice() {
    clear();
    bool fromDhcp = false;
    TEST_ASSERT_TRUE(cliTakeTrapTarget("", "10.132.0.45", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("10.132.0.45", dest);
    TEST_ASSERT_TRUE(fromDhcp);
}

static void test_trap_dhcp_keeps_the_fixed_address_for_later() {
    clear();
    bool fromDhcp = false;
    TEST_ASSERT_TRUE(cliTakeTrapTarget("DHCP", "10.132.0.45", false, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("10.132.0.45", dest);
    TEST_ASSERT_TRUE(fromDhcp);
}

static void test_trap_dash_turns_traps_off() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_TRUE(cliTakeTrapTarget("-", "10.132.0.45", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
    TEST_ASSERT_FALSE(fromDhcp);
}

static void test_trap_ipv6_is_stored_in_rfc5952_form() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_TRUE(cliTakeTrapTarget("FD00:0:0:0:0:0:0:45", "", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("fd00::45", dest);
    TEST_ASSERT_FALSE(fromDhcp);
}

static void test_trap_refuses_link_local_with_the_netconfig_message() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_FALSE(cliTakeTrapTarget("fe80::1", "10.132.0.45", true, dest, fromDhcp, msg, sizeof(msg)));
    TrapAddress a;
    std::string want = std::string("'fe80::1': ") + trapTargetErrorMessage(parseTrapTarget("fe80::1", a));
    TEST_ASSERT_EQUAL_STRING(want.c_str(), msg);
    TEST_ASSERT_EQUAL('#', dest[0]);
    TEST_ASSERT_TRUE(fromDhcp);  // unchanged
}

static void test_trap_version_empty_keeps_current() {
    uint8_t v = 0;
    TEST_ASSERT_TRUE(cliTakeTrapVersion("", TRAP_VERSION_V1, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(TRAP_VERSION_V1, v);
}

static void test_trap_version_takes_1_and_2c_in_either_case() {
    uint8_t v = 0;
    TEST_ASSERT_TRUE(cliTakeTrapVersion("1", TRAP_VERSION_V2C, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(TRAP_VERSION_V1, v);
    TEST_ASSERT_TRUE(cliTakeTrapVersion("2c", TRAP_VERSION_V1, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(TRAP_VERSION_V2C, v);
    v = 0;
    TEST_ASSERT_TRUE(cliTakeTrapVersion("2C", TRAP_VERSION_V1, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(TRAP_VERSION_V2C, v);
}

static void test_trap_version_refuses_2_and_v1() {
    uint8_t v = 0;
    TEST_ASSERT_FALSE(cliTakeTrapVersion("2", TRAP_VERSION_V1, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("Enter 1 or 2c", msg);
    TEST_ASSERT_FALSE(cliTakeTrapVersion("v1", TRAP_VERSION_V1, v, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, v);
}

static void test_ipv6_empty_keeps_current() {
    clear();
    TEST_ASSERT_TRUE(cliTakeIPv6("", "fd00::5", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("fd00::5", dest);
}

static void test_ipv6_empty_with_empty_current_stays_empty() {
    clear();
    TEST_ASSERT_TRUE(cliTakeIPv6("", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
}

static void test_ipv6_dash_clears() {
    clear();
    TEST_ASSERT_TRUE(cliTakeIPv6("-", "fd00::5", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
}

static void test_ipv6_is_stored_in_rfc5952_form() {
    clear();
    TEST_ASSERT_TRUE(cliTakeIPv6("FD00:0000::0010", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("fd00::10", dest);
}

static void test_ipv6_refuses_a_prefix_length_with_a_hint() {
    clear();
    TEST_ASSERT_FALSE(cliTakeIPv6("fd00::10/64", "", dest, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("'fd00::10/64' is not a valid IPv6 address (write it without /64, e.g. fd00::10)", msg);
    TEST_ASSERT_EQUAL('#', dest[0]);
}

static void test_update_url_empty_keeps_url_and_dhcp_choice() {
    clear();
    bool fromDhcp = false;
    TEST_ASSERT_TRUE(cliTakeUpdateUrl("", "http://10.132.0.45:8074", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("http://10.132.0.45:8074", dest);
    TEST_ASSERT_TRUE(fromDhcp);
}

static void test_update_url_none_turns_check_ins_off() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_TRUE(cliTakeUpdateUrl("none", "http://10.132.0.45:8074", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", dest);
    TEST_ASSERT_FALSE(fromDhcp);
}

static void test_update_url_dhcp_keeps_the_stored_url() {
    clear();
    bool fromDhcp = false;
    TEST_ASSERT_TRUE(cliTakeUpdateUrl("dhcp", "http://10.132.0.45:8074", false, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("http://10.132.0.45:8074", dest);
    TEST_ASSERT_TRUE(fromDhcp);
}

static void test_update_url_takes_a_url() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_TRUE(cliTakeUpdateUrl("http://[fd00::45]:8074/site", "", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("http://[fd00::45]:8074/site", dest);
    TEST_ASSERT_FALSE(fromDhcp);
}

static void test_update_url_refuses_https() {
    clear();
    bool fromDhcp = true;
    TEST_ASSERT_FALSE(cliTakeUpdateUrl("https://x", "", true, dest, fromDhcp, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("'https://x': not an http://host[:port][/path] URL of at most 127 characters", msg);
    TEST_ASSERT_EQUAL('#', dest[0]);
    TEST_ASSERT_TRUE(fromDhcp);
}

static void test_update_url_refuses_one_over_the_maximum_length() {
    clear();
    std::string url = "http://a/" + std::string(UPDATE_URL_MAX + 1 - 9, 'p');
    TEST_ASSERT_EQUAL(UPDATE_URL_MAX + 1, url.size());
    bool fromDhcp = false;
    TEST_ASSERT_FALSE(cliTakeUpdateUrl(url.c_str(), "", false, dest, fromDhcp, msg, sizeof(msg)));
}

static void test_messages_are_cut_to_the_buffer() {
    char small[8];
    memset(small, '#', sizeof(small));
    TEST_ASSERT_FALSE(cliTakeAddress("not-an-address", "", dest, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("'not-an", small);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_line_editor_cr_ends_a_line);
    RUN_TEST(test_line_editor_lf_alone_ends_a_line);
    RUN_TEST(test_line_editor_crlf_is_one_line_end);
    RUN_TEST(test_line_editor_lf_lf_is_two_empty_lines);
    RUN_TEST(test_line_editor_cr_cr_is_two_empty_lines);
    RUN_TEST(test_line_editor_empty_enter_gives_an_empty_line);
    RUN_TEST(test_line_editor_echoes_the_character);
    RUN_TEST(test_line_editor_secret_input_echoes_a_star);
    RUN_TEST(test_line_editor_backspace_and_delete_remove_the_last_character);
    RUN_TEST(test_line_editor_backspace_on_an_empty_line_does_nothing);
    RUN_TEST(test_line_editor_drops_control_characters);
    RUN_TEST(test_line_editor_drops_bytes_above_ascii);
    RUN_TEST(test_line_editor_drops_nul);
    RUN_TEST(test_line_editor_accepts_space_and_tilde);
    RUN_TEST(test_line_editor_accepts_exactly_line_max_characters);
    RUN_TEST(test_line_editor_refuses_a_line_one_character_too_long);
    RUN_TEST(test_line_editor_overflow_wipes_the_buffer);
    RUN_TEST(test_line_editor_backspace_after_overflow_does_not_accept_the_line);
    RUN_TEST(test_line_editor_next_line_after_overflow_works);
    RUN_TEST(test_line_editor_next_line_starts_empty);
    RUN_TEST(test_line_editor_wipe_clears_the_typed_password);
    RUN_TEST(test_confirmed_accepts_y_in_either_case);
    RUN_TEST(test_confirmed_refuses_everything_else);
    RUN_TEST(test_address_empty_keeps_current);
    RUN_TEST(test_address_takes_a_valid_address);
    RUN_TEST(test_address_refuses_bad_text_with_its_message);
    RUN_TEST(test_address_refuses_a_bad_current_value_on_enter);
    RUN_TEST(test_text_empty_keeps_current);
    RUN_TEST(test_text_dash_clears_when_empty_is_allowed);
    RUN_TEST(test_text_dash_refused_when_empty_is_not_allowed);
    RUN_TEST(test_text_takes_exactly_max_length);
    RUN_TEST(test_text_refuses_one_over_max_length);
    RUN_TEST(test_text_refuses_non_printable_characters);
    RUN_TEST(test_hostname_empty_keeps_current_even_if_not_a_hostname);
    RUN_TEST(test_hostname_dash_goes_back_to_the_default);
    RUN_TEST(test_hostname_takes_a_valid_name);
    RUN_TEST(test_hostname_refuses_an_invalid_name);
    RUN_TEST(test_trap_empty_keeps_target_and_dhcp_choice);
    RUN_TEST(test_trap_dhcp_keeps_the_fixed_address_for_later);
    RUN_TEST(test_trap_dash_turns_traps_off);
    RUN_TEST(test_trap_ipv6_is_stored_in_rfc5952_form);
    RUN_TEST(test_trap_refuses_link_local_with_the_netconfig_message);
    RUN_TEST(test_trap_version_empty_keeps_current);
    RUN_TEST(test_trap_version_takes_1_and_2c_in_either_case);
    RUN_TEST(test_trap_version_refuses_2_and_v1);
    RUN_TEST(test_ipv6_empty_keeps_current);
    RUN_TEST(test_ipv6_empty_with_empty_current_stays_empty);
    RUN_TEST(test_ipv6_dash_clears);
    RUN_TEST(test_ipv6_is_stored_in_rfc5952_form);
    RUN_TEST(test_ipv6_refuses_a_prefix_length_with_a_hint);
    RUN_TEST(test_update_url_empty_keeps_url_and_dhcp_choice);
    RUN_TEST(test_update_url_none_turns_check_ins_off);
    RUN_TEST(test_update_url_dhcp_keeps_the_stored_url);
    RUN_TEST(test_update_url_takes_a_url);
    RUN_TEST(test_update_url_refuses_https);
    RUN_TEST(test_update_url_refuses_one_over_the_maximum_length);
    RUN_TEST(test_messages_are_cut_to_the_buffer);
    return UNITY_END();
}
