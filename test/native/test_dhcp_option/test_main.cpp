// Host unit tests for lib/dhcp_option: requesting option 224 in outgoing DHCP messages and
// reading it from DHCPACKs. Frames are real captures (see frames.h).
// Run: pio test -e native -f native/test_dhcp_option

#include <unity.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <dhcp_option.h>
#include "frames.h"

void setUp() {}
void tearDown() {}

static const uint8_t DEVICE_MAC[6] = {0x1e, 0xb2, 0xcf, 0x4e, 0xa2, 0x2c};
static const uint8_t OTHER_MAC[6] = {0x1e, 0xb2, 0xcf, 0x4e, 0xa2, 0x2d};

typedef std::vector<uint8_t> Frame;

template <size_t N>
static Frame frame(const uint8_t (&f)[N]) {
    return Frame(f, f + N);
}

// ---- independent helpers (not the library's code) ----

static size_t udpOffset(const Frame& f) { return 14 + (size_t)(f[14] & 15) * 4; }

static size_t optionsOffset(const Frame& f) { return udpOffset(f) + 8 + 240; }

/// One's-complement checksum check: true if the UDP checksum verifies (or is 0 = unused)
static bool udpChecksumOk(const Frame& f) {
    size_t u = udpOffset(f);
    size_t ulen = (size_t)f[u + 4] << 8 | f[u + 5];
    if (f[u + 6] == 0 && f[u + 7] == 0) return true;
    uint32_t sum = 0;
    for (size_t i = 26; i < 34; i += 2) sum += (uint32_t)f[i] << 8 | f[i + 1];
    sum += 17 + (uint32_t)ulen;
    for (size_t i = 0; i < ulen; i += 2) {
        sum += (uint32_t)f[u + i] << 8 | (i + 1 < ulen ? f[u + i + 1] : 0);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
}

/// The value of an option (first occurrence), empty if absent
static Frame option(const Frame& f, uint8_t code) {
    for (size_t i = optionsOffset(f); i < f.size() && f[i] != 255;) {
        if (f[i] == 0) {
            i++;
            continue;
        }
        if (f[i] == code) return Frame(f.begin() + i + 2, f.begin() + i + 2 + f[i + 1]);
        i += 2 + f[i + 1];
    }
    return Frame();
}

/// All options except 55, in order, as (code, value) bytes, to compare before/after an edit
static Frame optionsExcept55(const Frame& f) {
    Frame out;
    for (size_t i = optionsOffset(f); i < f.size() && f[i] != 255;) {
        if (f[i] == 0) {
            i++;
            continue;
        }
        if (f[i] != 55) out.insert(out.end(), f.begin() + i, f.begin() + i + 2 + f[i + 1]);
        i += 2 + f[i + 1];
    }
    return out;
}

static DhcpEdit edit(Frame& f) { return dhcpRequestOption(f.data(), f.size(), DHCP_OPTION_TRAP_TARGET); }

static DhcpAckInfo lastInfo;

static DhcpAck read(const Frame& f, const uint8_t* mac, uint8_t out[4], uint8_t yi[4]) {
    lastInfo = DhcpAckInfo();
    memcpy(lastInfo.value, out, 4);  // untouched unless the option is found
    DhcpAck r = dhcpReadAck(f.data(), f.size(), mac, DHCP_OPTION_TRAP_TARGET, lastInfo);
    memcpy(out, lastInfo.value, 4);
    memcpy(yi, lastInfo.yiaddr, 4);
    return r;
}

// ---- request editing ----

static void test_captured_requests_are_valid_before_editing() {
    TEST_ASSERT_TRUE(udpChecksumOk(frame(DEVICE_DISCOVER)));
    TEST_ASSERT_TRUE(udpChecksumOk(frame(DEVICE_REQUEST_SELECTING)));
    TEST_ASSERT_TRUE(udpChecksumOk(frame(DEVICE_REQUEST_RENEWING)));
    const uint8_t prl[] = {1, 3, 28, 6};
    Frame p = option(frame(DEVICE_DISCOVER), 55);
    TEST_ASSERT_EQUAL(4, p.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(prl, p.data(), 4);
}

static void test_option_224_is_added_to_every_device_request() {
    Frame frames[] = {frame(DEVICE_DISCOVER), frame(DEVICE_REQUEST_SELECTING), frame(DEVICE_REQUEST_RENEWING)};
    for (Frame& f : frames) {
        Frame before = f;
        TEST_ASSERT_EQUAL(DhcpEdit::Added, edit(f));
        TEST_ASSERT_EQUAL(before.size(), f.size());          // in place, length unchanged
        const uint8_t prl[] = {1, 3, 28, 6, 224};
        Frame p = option(f, 55);
        TEST_ASSERT_EQUAL(5, p.size());
        TEST_ASSERT_EQUAL_HEX8_ARRAY(prl, p.data(), 5);
        TEST_ASSERT_TRUE(optionsExcept55(before) == optionsExcept55(f));  // nothing else changed
        // The checksum still verifies, and is not just dropped (0 would mean "no checksum")
        size_t u = udpOffset(f);
        TEST_ASSERT_TRUE(f[u + 6] != 0 || f[u + 7] != 0);
        TEST_ASSERT_TRUE(udpChecksumOk(f));
        // Headers untouched: Ethernet, IP (incl. its checksum), UDP ports and length, BOOTP fixed part
        size_t opt = optionsOffset(f);
        for (size_t i = 0; i < opt; i++) {
            if (i == udpOffset(f) + 6 || i == udpOffset(f) + 7) continue;  // UDP checksum
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], f[i], "header byte changed");
        }
    }
}

static void test_editing_twice_does_not_add_224_again() {
    Frame f = frame(DEVICE_DISCOVER);
    TEST_ASSERT_EQUAL(DhcpEdit::Added, edit(f));
    Frame once = f;
    TEST_ASSERT_EQUAL(DhcpEdit::AlreadyRequested, edit(f));
    TEST_ASSERT_TRUE(once == f);
}

static void test_edited_frame_was_accepted_by_kea() {
    // Kea answered the edited frames with option 224 and the plain ones without it (frames.h)
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::Option, read(frame(KEA_ACK_WITH_224), DEVICE_MAC, out, yi));
    TEST_ASSERT_EQUAL(DhcpAck::NoOption, read(frame(KEA_ACK_WITHOUT_224), DEVICE_MAC, out, yi));
}

static void test_no_padding_after_end_means_no_room() {
    Frame f = frame(DEVICE_DISCOVER);
    // Cut the frame right after END and shorten the IP and UDP lengths to match
    size_t u = udpOffset(f);
    size_t i = optionsOffset(f);
    while (f[i] != 255) i += f[i] == 0 ? 1 : 2 + f[i + 1];
    size_t newLen = i + 1;
    size_t ulen = newLen - u, iplen = newLen - 14;
    f.resize(newLen);
    f[16] = (uint8_t)(iplen >> 8);
    f[17] = (uint8_t)iplen;
    f[u + 4] = (uint8_t)(ulen >> 8);
    f[u + 5] = (uint8_t)ulen;
    f[u + 6] = f[u + 7] = 0;  // no UDP checksum
    f[24] = f[25] = 0;        // recompute the IP header checksum
    uint32_t s = 0;
    for (size_t k = 14; k < 34; k += 2) s += (uint32_t)f[k] << 8 | f[k + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    f[24] = (uint8_t)(~s >> 8);
    f[25] = (uint8_t)~s;
    Frame before = f;
    TEST_ASSERT_EQUAL(DhcpEdit::NoRoom, edit(f));
    TEST_ASSERT_TRUE(before == f);
}

static void test_zero_udp_checksum_stays_zero() {
    Frame f = frame(DEVICE_DISCOVER);
    size_t u = udpOffset(f);
    f[u + 6] = f[u + 7] = 0;
    TEST_ASSERT_EQUAL(DhcpEdit::Added, edit(f));
    TEST_ASSERT_EQUAL_HEX8(0, f[u + 6]);
    TEST_ASSERT_EQUAL_HEX8(0, f[u + 7]);
}

static void test_server_messages_and_other_traffic_are_not_edited() {
    Frame frames[] = {frame(LAN_ACK), frame(LAN_OFFER), frame(KEA_ACK_WITH_224)};
    for (Frame& f : frames) {
        Frame before = f;
        TEST_ASSERT_EQUAL(DhcpEdit::NotDhcpRequest, edit(f));
        TEST_ASSERT_TRUE(before == f);
    }
    Frame arp(60, 0);
    arp[12] = 0x08;
    arp[13] = 0x06;
    TEST_ASSERT_EQUAL(DhcpEdit::NotDhcpRequest, edit(arp));
    TEST_ASSERT_EQUAL(DhcpEdit::NotDhcpRequest, dhcpRequestOption(nullptr, 0, 224));
}

static void test_request_with_bad_checksum_is_left_alone() {
    Frame f = frame(DEVICE_DISCOVER);
    f[optionsOffset(f) + 2] ^= 1;  // corrupt a byte: UDP checksum no longer verifies
    Frame before = f;
    TEST_ASSERT_EQUAL(DhcpEdit::NotDhcpRequest, edit(f));
    TEST_ASSERT_TRUE(before == f);
}

static void test_every_truncation_of_a_request_is_safe() {
    Frame full = frame(DEVICE_DISCOVER);
    for (size_t n = 0; n < full.size(); n++) {
        Frame f(full.begin(), full.begin() + n);
        Frame before = f;
        TEST_ASSERT_EQUAL_MESSAGE(DhcpEdit::NotDhcpRequest, edit(f), "truncated frame edited");
        TEST_ASSERT_TRUE(before == f);
    }
}

static void test_every_single_byte_corruption_of_a_request_is_safe() {
    // Every byte, every value: either unchanged, or a well-formed edit whose checksum verifies
    Frame full = frame(DEVICE_REQUEST_RENEWING);
    for (size_t pos = 0; pos < full.size(); pos++) {
        for (int v = 0; v < 256; v++) {
            Frame f = full;
            f[pos] = (uint8_t)v;
            Frame before = f;
            DhcpEdit r = edit(f);
            if (r == DhcpEdit::Added) {
                TEST_ASSERT_EQUAL(before.size(), f.size());
                TEST_ASSERT_TRUE(udpChecksumOk(f) || (f[udpOffset(f) + 6] == 0 && f[udpOffset(f) + 7] == 0));
            } else {
                TEST_ASSERT_TRUE(before == f);
            }
        }
    }
}

// ---- reading ACKs ----

static void test_kea_ack_with_option_224_gives_the_trap_target() {
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::Option, read(frame(KEA_ACK_WITH_224), DEVICE_MAC, out, yi));
    const uint8_t target[] = {10, 10, 6, 65}, lease[] = {192, 168, 77, 101}, server[] = {192, 168, 77, 2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(target, out, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lease, yi, 4);
    // Transaction id and server: the firmware only trusts an ACK lwIP accepted (same xid and server)
    TEST_ASSERT_EQUAL_HEX32(0x3a559c83, lastInfo.xid);  // the device's request it answers
    TEST_ASSERT_TRUE(lastInfo.hasServerId);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(server, lastInfo.serverId, 4);
}

static void test_lan_ack_xid_matches_the_device_request() {
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::NoOption, read(frame(LAN_ACK), DEVICE_MAC, out, yi));
    Frame req = frame(DEVICE_REQUEST_SELECTING);
    size_t b = udpOffset(req) + 8;
    uint32_t reqXid = (uint32_t)req[b + 4] << 24 | (uint32_t)req[b + 5] << 16 | (uint32_t)req[b + 6] << 8 | req[b + 7];
    TEST_ASSERT_EQUAL_HEX32(reqXid, lastInfo.xid);
    const uint8_t server[] = {10, 10, 0, 2};
    TEST_ASSERT_TRUE(lastInfo.hasServerId);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(server, lastInfo.serverId, 4);
}

static void test_acks_without_option_224() {
    uint8_t out[4] = {1, 2, 3, 4}, yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::NoOption, read(frame(LAN_ACK), DEVICE_MAC, out, yi));
    const uint8_t lease[] = {10, 10, 6, 111};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lease, yi, 4);
    const uint8_t untouched[] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, out, 4);
    TEST_ASSERT_EQUAL(DhcpAck::NoOption, read(frame(KEA_ACK_WITHOUT_224), DEVICE_MAC, out, yi));
}

static void test_offers_and_requests_are_not_acks() {
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::NotAck, read(frame(KEA_OFFER_WITH_224), DEVICE_MAC, out, yi));
    TEST_ASSERT_EQUAL(DhcpAck::NotAck, read(frame(LAN_OFFER), DEVICE_MAC, out, yi));
    TEST_ASSERT_EQUAL(DhcpAck::NotDhcpReply, read(frame(DEVICE_REQUEST_RENEWING), DEVICE_MAC, out, yi));
}

static void test_ack_for_another_device_is_ignored() {
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::NotForUs, read(frame(KEA_ACK_WITH_224), OTHER_MAC, out, yi));
}

/// Kea's ACK with option 224's length byte changed (UDP checksum disabled so it still parses)
static Frame keaAckWithOptionLength(uint8_t length) {
    Frame f = frame(KEA_ACK_WITH_224);
    size_t u = udpOffset(f);
    f[u + 6] = f[u + 7] = 0;
    for (size_t i = optionsOffset(f); f[i] != 255;) {
        if (f[i] == 0) {
            i++;
            continue;
        }
        if (f[i] == 224) {
            f[i + 1] = length;
            break;
        }
        i += 2 + f[i + 1];
    }
    return f;
}

static void test_option_224_with_wrong_length_is_ignored() {
    uint8_t out[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::Option, read(keaAckWithOptionLength(4), DEVICE_MAC, out, yi));  // control
    // Shorter: the remaining options no longer line up, so the ACK is malformed; never a value
    TEST_ASSERT_NOT_EQUAL(DhcpAck::Option, read(keaAckWithOptionLength(3), DEVICE_MAC, out, yi));
    TEST_ASSERT_NOT_EQUAL(DhcpAck::Option, read(keaAckWithOptionLength(0), DEVICE_MAC, out, yi));
    TEST_ASSERT_NOT_EQUAL(DhcpAck::Option, read(keaAckWithOptionLength(8), DEVICE_MAC, out, yi));
}

static void test_corrupted_ack_is_rejected_by_its_checksums() {
    uint8_t out[4], yi[4];
    Frame f = frame(KEA_ACK_WITH_224);
    f[optionsOffset(f) + 3] ^= 0x40;  // body byte: UDP checksum fails
    TEST_ASSERT_EQUAL(DhcpAck::NotDhcpReply, read(f, DEVICE_MAC, out, yi));
    f = frame(KEA_ACK_WITH_224);
    f[22] ^= 1;  // TTL: IP header checksum fails
    TEST_ASSERT_EQUAL(DhcpAck::NotDhcpReply, read(f, DEVICE_MAC, out, yi));
}

static void test_fragments_are_ignored() {
    uint8_t out[4], yi[4];
    Frame f = frame(KEA_ACK_WITH_224);
    f[20] |= 0x20;  // More Fragments
    TEST_ASSERT_EQUAL(DhcpAck::NotDhcpReply, read(f, DEVICE_MAC, out, yi));
}

static void test_every_truncation_and_corruption_of_an_ack_is_safe() {
    uint8_t out[4], yi[4];
    Frame full = frame(KEA_ACK_WITH_224);
    for (size_t n = 0; n < full.size(); n++) {
        Frame f(full.begin(), full.begin() + n);
        TEST_ASSERT_NOT_EQUAL(DhcpAck::Option, read(f, DEVICE_MAC, out, yi));
    }
    for (size_t pos = 0; pos < full.size(); pos++) {
        for (int v = 0; v < 256; v++) {
            Frame f = full;
            f[pos] = (uint8_t)v;
            DhcpAck r = read(f, DEVICE_MAC, out, yi);  // must not crash or read out of bounds (sanitizers)
            if (r == DhcpAck::Option && f != full) {
                // Only changes the checksums cannot see can still give a value, e.g. the
                // checksum bytes themselves become 0 ("no checksum") or a compensating value
                TEST_ASSERT_TRUE(pos >= udpOffset(f) + 6 || pos < 34);
            }
        }
    }
}

// ---- option 225 (update server URL, text) ----

/// Kea's ACK without 224, with option code/value inserted before END (lengths and IP checksum fixed,
/// UDP checksum set to 0 = unused, which is valid for UDP over IPv4)
static Frame ackWithOption(uint8_t code, const uint8_t* value, size_t valueLen) {
    Frame f = frame(KEA_ACK_WITHOUT_224);
    size_t u = udpOffset(f);
    size_t i = optionsOffset(f);
    while (f[i] != 255) i += f[i] == 0 ? 1 : 2 + f[i + 1];
    Frame opt;
    opt.push_back(code);
    opt.push_back((uint8_t)valueLen);
    opt.insert(opt.end(), value, value + valueLen);
    f.insert(f.begin() + (long)i, opt.begin(), opt.end());
    size_t iplen = ((size_t)f[16] << 8 | f[17]) + opt.size();
    size_t ulen = ((size_t)f[u + 4] << 8 | f[u + 5]) + opt.size();
    f[16] = (uint8_t)(iplen >> 8);
    f[17] = (uint8_t)iplen;
    f[u + 4] = (uint8_t)(ulen >> 8);
    f[u + 5] = (uint8_t)ulen;
    f[u + 6] = f[u + 7] = 0;
    f[24] = f[25] = 0;
    uint32_t s = 0;
    for (size_t k = 14; k < 34; k += 2) s += (uint32_t)f[k] << 8 | f[k + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    f[24] = (uint8_t)(~s >> 8);
    f[25] = (uint8_t)~s;
    return f;
}

static Frame ackWithText(const char* text) {
    return ackWithOption(DHCP_OPTION_UPDATE_URL, (const uint8_t*)text, strlen(text));
}

static void test_ack_text_reads_url_option() {
    Frame f = ackWithText("http://10.132.0.45:8071");
    uint8_t out4[4], yi[4];
    TEST_ASSERT_EQUAL(DhcpAck::NoOption, read(f, DEVICE_MAC, out4, yi));  // still a valid ACK for us
    char url[128];
    TEST_ASSERT_TRUE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("http://10.132.0.45:8071", url);
}

static void test_ack_text_accepts_127_and_rejects_too_long() {
    char text[129];
    memset(text, 'a', 127);
    text[127] = '\0';
    char url[128];
    Frame f = ackWithText(text);
    TEST_ASSERT_TRUE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    TEST_ASSERT_EQUAL(127, strlen(url));
    strcat(text, "a");
    f = ackWithText(text);
    strcpy(url, "unchanged");
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("", url);  // rejected, never truncated
}

static void test_ack_text_rejects_non_printable() {
    char url[128];
    const uint8_t withNul[] = {'h', 't', 0, 'p'};
    Frame f = ackWithOption(DHCP_OPTION_UPDATE_URL, withNul, sizeof(withNul));
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    const uint8_t withHigh[] = {'h', 0xC3, 0xA6};
    f = ackWithOption(DHCP_OPTION_UPDATE_URL, withHigh, sizeof(withHigh));
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    f = ackWithText("http://a\nb");
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
}

static void test_ack_text_rejects_zero_length() {
    char url[128];
    Frame f = ackWithOption(DHCP_OPTION_UPDATE_URL, nullptr, 0);
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
}

static void test_ack_text_absent() {
    char url[128];
    Frame f = frame(KEA_ACK_WITH_224);
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    TEST_ASSERT_FALSE(dhcpAckText(nullptr, 0, DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
}

static void test_ack_text_not_read_from_requests_or_bad_checksum() {
    char url[128];
    Frame f = ackWithText("http://site");
    size_t u = udpOffset(f);
    f[u + 6] = 0x12;  // a checksum that does not verify
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    f = frame(DEVICE_DISCOVER);
    TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
}

static void test_ack_text_truncated_frame() {
    char url[128];
    Frame full = ackWithText("http://10.132.0.45:8071/rf");
    for (size_t n = 0; n < full.size(); n++) {
        Frame f(full.begin(), full.begin() + (long)n);
        TEST_ASSERT_FALSE(dhcpAckText(f.data(), f.size(), DHCP_OPTION_UPDATE_URL, url, sizeof(url)));
    }
}

static void test_request_two_options_in_captured_discover() {
    Frame frames[] = {frame(DEVICE_DISCOVER), frame(DEVICE_REQUEST_SELECTING), frame(DEVICE_REQUEST_RENEWING)};
    for (Frame& f : frames) {
        TEST_ASSERT_EQUAL(DhcpEdit::Added, dhcpRequestOption(f.data(), f.size(), DHCP_OPTION_TRAP_TARGET));
        DhcpEdit r = dhcpRequestOption(f.data(), f.size(), DHCP_OPTION_UPDATE_URL);
        TEST_ASSERT_EQUAL(DhcpEdit::Added, r);  // lwIP pads enough for both 224 and 225
        Frame list = option(f, 55);
        TEST_ASSERT_TRUE(std::find(list.begin(), list.end(), DHCP_OPTION_TRAP_TARGET) != list.end());
        TEST_ASSERT_TRUE(std::find(list.begin(), list.end(), DHCP_OPTION_UPDATE_URL) != list.end());
        TEST_ASSERT_TRUE(udpChecksumOk(f));
    }
}

static void test_one_spare_byte_takes_224_and_refuses_225() {
    // Cut the DISCOVER to END plus one padding byte (lengths, IP checksum fixed; UDP checksum kept valid)
    Frame f = frame(DEVICE_DISCOVER);
    size_t u = udpOffset(f);
    size_t i = optionsOffset(f);
    while (f[i] != 255) i += f[i] == 0 ? 1 : 2 + f[i + 1];
    size_t newLen = i + 2;
    size_t ulen = newLen - u, iplen = newLen - 14;
    f.resize(newLen);
    f[newLen - 1] = 0;
    f[16] = (uint8_t)(iplen >> 8);
    f[17] = (uint8_t)iplen;
    f[u + 4] = (uint8_t)(ulen >> 8);
    f[u + 5] = (uint8_t)ulen;
    f[24] = f[25] = 0;
    uint32_t s = 0;
    for (size_t k = 14; k < 34; k += 2) s += (uint32_t)f[k] << 8 | f[k + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    f[24] = (uint8_t)(~s >> 8);
    f[25] = (uint8_t)~s;
    f[u + 6] = f[u + 7] = 0;  // recompute the UDP checksum over the shorter datagram
    uint32_t c = 0;
    for (size_t k = 26; k < 34; k += 2) c += (uint32_t)f[k] << 8 | f[k + 1];
    c += 17 + (uint32_t)ulen;
    for (size_t k = 0; k < ulen; k += 2) c += (uint32_t)f[u + k] << 8 | (k + 1 < ulen ? f[u + k + 1] : 0);
    while (c >> 16) c = (c & 0xFFFF) + (c >> 16);
    f[u + 6] = (uint8_t)(~c >> 8);
    f[u + 7] = (uint8_t)~c;
    TEST_ASSERT_TRUE(udpChecksumOk(f));

    TEST_ASSERT_EQUAL(DhcpEdit::Added, dhcpRequestOption(f.data(), f.size(), DHCP_OPTION_TRAP_TARGET));
    Frame after224 = f;
    TEST_ASSERT_EQUAL(DhcpEdit::NoRoom, dhcpRequestOption(f.data(), f.size(), DHCP_OPTION_UPDATE_URL));
    TEST_ASSERT_TRUE(after224 == f);  // unchanged
    TEST_ASSERT_TRUE(udpChecksumOk(f));
    Frame list = option(f, 55);
    TEST_ASSERT_TRUE(std::find(list.begin(), list.end(), DHCP_OPTION_TRAP_TARGET) != list.end());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_one_spare_byte_takes_224_and_refuses_225);
    RUN_TEST(test_ack_text_reads_url_option);
    RUN_TEST(test_ack_text_accepts_127_and_rejects_too_long);
    RUN_TEST(test_ack_text_rejects_non_printable);
    RUN_TEST(test_ack_text_rejects_zero_length);
    RUN_TEST(test_ack_text_absent);
    RUN_TEST(test_ack_text_not_read_from_requests_or_bad_checksum);
    RUN_TEST(test_ack_text_truncated_frame);
    RUN_TEST(test_request_two_options_in_captured_discover);
    RUN_TEST(test_captured_requests_are_valid_before_editing);
    RUN_TEST(test_option_224_is_added_to_every_device_request);
    RUN_TEST(test_editing_twice_does_not_add_224_again);
    RUN_TEST(test_edited_frame_was_accepted_by_kea);
    RUN_TEST(test_no_padding_after_end_means_no_room);
    RUN_TEST(test_zero_udp_checksum_stays_zero);
    RUN_TEST(test_server_messages_and_other_traffic_are_not_edited);
    RUN_TEST(test_request_with_bad_checksum_is_left_alone);
    RUN_TEST(test_every_truncation_of_a_request_is_safe);
    RUN_TEST(test_every_single_byte_corruption_of_a_request_is_safe);
    RUN_TEST(test_kea_ack_with_option_224_gives_the_trap_target);
    RUN_TEST(test_acks_without_option_224);
    RUN_TEST(test_lan_ack_xid_matches_the_device_request);
    RUN_TEST(test_offers_and_requests_are_not_acks);
    RUN_TEST(test_ack_for_another_device_is_ignored);
    RUN_TEST(test_option_224_with_wrong_length_is_ignored);
    RUN_TEST(test_corrupted_ack_is_rejected_by_its_checksums);
    RUN_TEST(test_fragments_are_ignored);
    RUN_TEST(test_every_truncation_and_corruption_of_an_ack_is_safe);
    return UNITY_END();
}
