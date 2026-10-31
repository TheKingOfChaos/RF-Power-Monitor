// Host unit tests for lib/dhcp6_client/dhcp6_client: the RFC 8415 client state machine,
// driven with a fake clock and a deterministic random source.
// Run: pio test -e native -f native/test_dhcp6_client

#include <unity.h>
#include <string.h>
#include <dhcp6_client.h>

void setUp() {}
void tearDown() {}

static const uint8_t MAC[6] = {0x1e, 0xb2, 0xcf, 0x4e, 0xa2, 0x2c};
static const uint8_t SRV_A[] = {0, 3, 0, 1, 2, 0, 0, 0, 0xd6, 0x0a};
static const uint8_t SRV_B[] = {0, 3, 0, 1, 2, 0, 0, 0, 0xd6, 0x0b};
static const uint8_t ADDR_A[16] = {0xfd, 0, 0, 0xd6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x0a};
static const uint8_t ADDR_B[16] = {0xfd, 0, 0, 0xd6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x0b};
static const uint32_t INF = 0xFFFFFFFFu;

static uint32_t rng = 1;
static uint32_t fakeRandom() {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static uint8_t buf[256];

static Dhcp6Action next(Dhcp6Client& c, uint32_t now) { return c.poll(now, buf, sizeof(buf)); }

static uint32_t sentXid() { return (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3]; }

/// Top-level option of the message in buf
static const uint8_t* option(size_t len, uint16_t code, uint16_t* olen) {
    size_t i = 4;
    while (i + 4 <= len) {
        uint16_t c = (uint16_t)(buf[i] << 8 | buf[i + 1]), l = (uint16_t)(buf[i + 2] << 8 | buf[i + 3]);
        if (c == code) {
            *olen = l;
            return buf + i + 4;
        }
        i += 4 + l;
    }
    return nullptr;
}

static Dhcp6In serverMsg(uint8_t type, uint32_t xid, const uint8_t* srv, uint8_t pref, const uint8_t* addr,
                         uint32_t preferred, uint32_t valid, uint32_t t1, uint32_t t2) {
    Dhcp6In m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.xid = xid;
    memcpy(m.serverId, srv, sizeof(SRV_A));
    m.serverIdLen = sizeof(SRV_A);
    m.preference = pref;
    if (addr != nullptr) {
        m.hasLease = true;
        memcpy(m.lease.addr, addr, 16);
        m.lease.preferred = preferred;
        m.lease.valid = valid;
        m.lease.t1 = t1;
        m.lease.t2 = t2;
    }
    return m;
}

/// Poll every step ms from *now until a Send (or another action of `kind`) or limit; *now ends there
static Dhcp6Action until(Dhcp6Client& c, uint32_t* now, uint32_t limitMs, Dhcp6Action::Kind kind = Dhcp6Action::Send,
                         uint32_t step = 10) {
    uint32_t end = *now + limitMs;
    for (;; *now += step) {
        Dhcp6Action a = next(c, *now);
        if (a.kind == kind) return a;
        if ((int32_t)(*now - end) >= 0) return a;
    }
}

/// Run a full Solicit/Advertise/Request/Reply with server A; returns with the AddAddress taken
static uint32_t bindWith(Dhcp6Client& c, uint32_t now, uint32_t preferred, uint32_t valid, uint32_t t1, uint32_t t2) {
    c.start(now);
    Dhcp6Action a = until(c, &now, 2000);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, preferred, valid, t1, t2), now);
    a = next(c, now);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REQUEST, buf[0]);
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, ADDR_A, preferred, valid, t1, t2), now);
    a = next(c, now);
    TEST_ASSERT_EQUAL(Dhcp6Action::AddAddress, a.kind);
    TEST_ASSERT_EQUAL(Dhcp6State::Bound, c.state());
    return now;
}

// ---- starting ----

static void test_client_sends_nothing_before_start() {
    Dhcp6Client c(MAC, fakeRandom);
    for (uint32_t t = 0; t < 100000; t += 100) TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, t).kind);
    TEST_ASSERT_EQUAL(Dhcp6State::Idle, c.state());
    TEST_ASSERT_EQUAL_UINT32(0, c.sent());
}

static void test_first_solicit_within_max_delay() {
    Dhcp6Client c(MAC, fakeRandom);
    c.setHostname("rf-ups-monitor");
    uint32_t now = 5000;
    c.start(now);
    Dhcp6Action a = until(c, &now, 1000);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_TRUE(now <= 6000);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_SOLICIT, buf[0]);
    uint16_t l;
    TEST_ASSERT_NOT_NULL(option(a.len, 39, &l));  // hostname sent
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());
}

static void test_null_hostname_sends_no_fqdn() {
    Dhcp6Client c(MAC, fakeRandom);
    c.setHostname("rf-ups-monitor");
    c.setHostname(nullptr);
    uint32_t now = 0;
    c.start(now);
    Dhcp6Action a = until(c, &now, 2000);
    uint16_t l;
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_NULL(option(a.len, 39, &l));
}

// ---- choosing a server ----

static void test_highest_preference_advertise_wins() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    uint32_t xid = sentXid();
    c.received(serverMsg(DHCP6_ADVERTISE, xid, SRV_A, 10, ADDR_A, 60, 120, 0, 0), now + 100);
    c.received(serverMsg(DHCP6_ADVERTISE, xid, SRV_B, 50, ADDR_B, 60, 120, 0, 0), now + 200);
    TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, now + 300).kind);  // still collecting during the first RT
    now += 300;
    Dhcp6Action a = until(c, &now, 2000);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REQUEST, buf[0]);
    uint16_t l;
    const uint8_t* sid = option(a.len, 2, &l);
    TEST_ASSERT_NOT_NULL(sid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(SRV_B, sid, sizeof(SRV_B));
    const uint8_t* ia = option(a.len, 3, &l);
    TEST_ASSERT_NOT_NULL(ia);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ADDR_B, ia + 16, 16);
}

static void test_preference_255_requests_immediately() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now + 5);
    Dhcp6Action a = next(c, now + 5);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REQUEST, buf[0]);
    TEST_ASSERT_EQUAL(Dhcp6State::Requesting, c.state());
}

static void test_advertise_without_address_is_ignored() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    Dhcp6In none = serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, nullptr, 0, 0, 0, 0);
    none.iaStatus = DHCP6_STATUS_NOADDRSAVAIL;
    c.received(none, now);
    now += 10;
    Dhcp6Action a = until(c, &now, 5000);  // the next transmission is another Solicit
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_SOLICIT, buf[0]);
    TEST_ASSERT_EQUAL_UINT32(1, c.ignored());
}

// ---- getting the address ----

static void test_reply_adds_address_with_lifetimes() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 30, 48), now);
    next(c, now);
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, ADDR_A, 60, 120, 30, 48), now + 50);
    Dhcp6Action a = next(c, now + 50);
    TEST_ASSERT_EQUAL(Dhcp6Action::AddAddress, a.kind);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ADDR_A, a.addr, 16);
    TEST_ASSERT_EQUAL_UINT32(60, a.preferred);
    TEST_ASSERT_EQUAL_UINT32(120, a.valid);
    TEST_ASSERT_EQUAL(Dhcp6State::Bound, c.state());
    TEST_ASSERT_TRUE(c.hasLease());
    TEST_ASSERT_EQUAL_UINT32(120, c.validLeftS(now + 50));
    TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, now + 50).kind);
}

static void test_client_ignores_reply_with_wrong_xid() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now);
    next(c, now);
    c.received(serverMsg(DHCP6_REPLY, (sentXid() + 1) & 0xFFFFFF, SRV_A, 0, ADDR_A, 60, 120, 0, 0), now);
    TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, now).kind);
    TEST_ASSERT_EQUAL(Dhcp6State::Requesting, c.state());
    TEST_ASSERT_EQUAL_UINT32(1, c.ignored());
}

static void test_reply_from_other_server_is_ignored() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now);
    next(c, now);
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_B, 0, ADDR_B, 60, 120, 0, 0), now);
    TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, now).kind);
    TEST_ASSERT_EQUAL(Dhcp6State::Requesting, c.state());
}

static void test_client_returns_to_solicit_on_noaddrsavail() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now);
    next(c, now);
    Dhcp6In none = serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, nullptr, 0, 0, 0, 0);
    none.iaStatus = DHCP6_STATUS_NOADDRSAVAIL;
    c.received(none, now);
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());
    Dhcp6Action a = until(c, &now, 1100);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_SOLICIT, buf[0]);
    TEST_ASSERT_FALSE(c.hasLease());
}

static void test_ia_with_t1_above_t2_is_not_used() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 20, 10), now);
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());  // RFC 8415 21.4: such an IA_NA is discarded
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now);
    next(c, now);
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, ADDR_A, 60, 120, 20, 10), now);
    TEST_ASSERT_NOT_EQUAL(Dhcp6Action::AddAddress, next(c, now).kind);
    TEST_ASSERT_FALSE(c.hasLease());
}

static void test_request_gives_up_after_max_rc() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0;
    c.start(now);
    until(c, &now, 2000);
    c.received(serverMsg(DHCP6_ADVERTISE, sentXid(), SRV_A, 255, ADDR_A, 60, 120, 0, 0), now);
    int requests = 0;
    for (uint32_t end = now + 400000; now < end; now += 100) {
        Dhcp6Action a = next(c, now);
        if (a.kind != Dhcp6Action::Send) continue;
        if (buf[0] == DHCP6_SOLICIT) break;
        TEST_ASSERT_EQUAL_UINT8(DHCP6_REQUEST, buf[0]);
        requests++;
    }
    TEST_ASSERT_EQUAL_INT(10, requests);  // REQ_MAX_RC
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());
}

static void test_solicit_backs_off_to_max_rt_and_survives_millis_wrap() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = 0xFFFF0000u;  // millis() wraps 65 s in
    c.start(now);
    uint32_t last = 0, maxGap = 0, lastGap = 0;
    int sends = 0;
    for (uint32_t i = 0; i < 10 * 3600; i++, now += 1000) {
        if (next(c, now).kind != Dhcp6Action::Send) continue;
        TEST_ASSERT_EQUAL_UINT8(DHCP6_SOLICIT, buf[0]);
        if (sends > 0) {
            lastGap = now - last;
            if (lastGap > maxGap) maxGap = lastGap;
        }
        last = now;
        sends++;
    }
    TEST_ASSERT_TRUE(sends > 10);
    TEST_ASSERT_TRUE(maxGap <= 3600u * 1100u + 1000u);  // SOL_MAX_RT + 10 %
    TEST_ASSERT_TRUE(lastGap >= 3600u * 900u);           // and it got there
}

// ---- keeping the address ----

static void test_renew_at_t1_rebind_at_t2() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t tb = bindWith(c, 0, 20, 40, 6, 12);
    TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, tb + 5999).kind);
    Dhcp6Action a = next(c, tb + 6000);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_RENEW, buf[0]);
    TEST_ASSERT_EQUAL(Dhcp6State::Renewing, c.state());
    uint16_t l;
    TEST_ASSERT_NOT_NULL(option(a.len, 2, &l));  // Renew goes to our server
    uint32_t now = tb + 6010;  // 10 ms steps that land on T2
    a = until(c, &now, 7000);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT32(tb + 12000, now);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_REBIND, buf[0]);
    TEST_ASSERT_NULL(option(a.len, 2, &l));  // Rebind goes to any server
    TEST_ASSERT_EQUAL(Dhcp6State::Rebinding, c.state());
}

static void test_zero_t1_t2_use_half_and_eighty_percent() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t tb = bindWith(c, 0, 100, 200, 0, 0);
    uint32_t now = tb + 10;  // 10 ms steps that land on T1
    until(c, &now, 60000);
    TEST_ASSERT_EQUAL_UINT32(tb + 50000, now);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_RENEW, buf[0]);
    now = tb + 79000;
    for (;; now += 10) {
        if (next(c, now).kind == Dhcp6Action::Send && buf[0] == DHCP6_REBIND) break;
    }
    TEST_ASSERT_EQUAL_UINT32(tb + 80000, now);
}

static void test_renew_reply_extends_lease() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t tb = bindWith(c, 0, 20, 40, 6, 12);
    next(c, tb + 6000);  // Renew
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, ADDR_A, 20, 40, 6, 12), tb + 6100);
    TEST_ASSERT_EQUAL(Dhcp6State::Bound, c.state());
    TEST_ASSERT_EQUAL_UINT32(40, c.validLeftS(tb + 6100));
    // No removal at the old end of the lease
    for (uint32_t t = tb + 6100; t <= tb + 41000; t += 100) {
        Dhcp6Action a = next(c, t);
        TEST_ASSERT_NOT_EQUAL(Dhcp6Action::RemoveAddress, a.kind);
        if (a.kind == Dhcp6Action::Send && buf[0] == DHCP6_RENEW) {
            c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, ADDR_A, 20, 40, 6, 12), t);
        }
    }
    TEST_ASSERT_TRUE(c.hasLease());
}

static void test_address_removed_at_valid_end() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t tb = bindWith(c, 0, 20, 40, 6, 12);
    uint32_t now = tb + 100;
    Dhcp6Action a = until(c, &now, 45000, Dhcp6Action::RemoveAddress, 100);
    TEST_ASSERT_EQUAL(Dhcp6Action::RemoveAddress, a.kind);
    TEST_ASSERT_EQUAL_UINT32(tb + 40000, now);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ADDR_A, a.addr, 16);
    TEST_ASSERT_FALSE(c.hasLease());
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());
}

static void test_infinite_valid_lifetime_never_expires() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = bindWith(c, 0, INF, INF, 0, 0);
    for (int h = 0; h < 100 * 24; h++, now += 3600000u) {  // 100 days, millis() wraps twice
        TEST_ASSERT_EQUAL(Dhcp6Action::None, next(c, now).kind);
    }
    TEST_ASSERT_EQUAL(Dhcp6State::Bound, c.state());
    TEST_ASSERT_EQUAL_UINT32(INF, c.validLeftS(now));
}

// ---- duplicate address ----

static void test_duplicate_address_is_declined_and_solicit_restarts() {
    Dhcp6Client c(MAC, fakeRandom);
    uint32_t now = bindWith(c, 0, 60, 120, 0, 0) + 1500;
    c.addressDuplicate(now);
    Dhcp6Action a = next(c, now);
    TEST_ASSERT_EQUAL(Dhcp6Action::RemoveAddress, a.kind);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ADDR_A, a.addr, 16);
    a = next(c, now);
    TEST_ASSERT_EQUAL(Dhcp6Action::Send, a.kind);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_DECLINE, buf[0]);
    TEST_ASSERT_EQUAL(Dhcp6State::Declining, c.state());
    uint16_t l;
    const uint8_t* ia = option(a.len, 3, &l);
    TEST_ASSERT_NOT_NULL(ia);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ADDR_A, ia + 16, 16);
    c.received(serverMsg(DHCP6_REPLY, sentXid(), SRV_A, 0, nullptr, 0, 0, 0, 0), now + 20);
    TEST_ASSERT_EQUAL(Dhcp6State::Soliciting, c.state());
    TEST_ASSERT_FALSE(c.hasLease());
    now += 20;
    a = until(c, &now, 1100);
    TEST_ASSERT_EQUAL_UINT8(DHCP6_SOLICIT, buf[0]);
}

// ---- address slots ----

static void test_pick_slot_skips_link_local_and_used_slots() {
    const uint8_t states[4] = {0x30, 0x30, 0, 0};  // link-local, SLAAC, free, free
    TEST_ASSERT_EQUAL_INT(2, dhcp6PickSlot(states, 4));
    const uint8_t allFree[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(1, dhcp6PickSlot(allFree, 4));  // never slot 0
}

static void test_pick_slot_returns_none_when_all_used() {
    const uint8_t states[4] = {0x30, 0x30, 0x30, 0x08};  // the last one tentative
    TEST_ASSERT_EQUAL_INT(-1, dhcp6PickSlot(states, 4));
}

static void test_every_state_has_a_name() {
    for (uint8_t s = 0; s <= (uint8_t)Dhcp6State::Declining; s++) {
        TEST_ASSERT_TRUE(strcmp("?", dhcp6StateName((Dhcp6State)s)) != 0);
    }
    TEST_ASSERT_EQUAL_STRING("bound", dhcp6StateName(Dhcp6State::Bound));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_client_sends_nothing_before_start);
    RUN_TEST(test_first_solicit_within_max_delay);
    RUN_TEST(test_null_hostname_sends_no_fqdn);
    RUN_TEST(test_highest_preference_advertise_wins);
    RUN_TEST(test_preference_255_requests_immediately);
    RUN_TEST(test_advertise_without_address_is_ignored);
    RUN_TEST(test_reply_adds_address_with_lifetimes);
    RUN_TEST(test_client_ignores_reply_with_wrong_xid);
    RUN_TEST(test_reply_from_other_server_is_ignored);
    RUN_TEST(test_client_returns_to_solicit_on_noaddrsavail);
    RUN_TEST(test_ia_with_t1_above_t2_is_not_used);
    RUN_TEST(test_request_gives_up_after_max_rc);
    RUN_TEST(test_solicit_backs_off_to_max_rt_and_survives_millis_wrap);
    RUN_TEST(test_renew_at_t1_rebind_at_t2);
    RUN_TEST(test_zero_t1_t2_use_half_and_eighty_percent);
    RUN_TEST(test_renew_reply_extends_lease);
    RUN_TEST(test_address_removed_at_valid_end);
    RUN_TEST(test_infinite_valid_lifetime_never_expires);
    RUN_TEST(test_duplicate_address_is_declined_and_solicit_restarts);
    RUN_TEST(test_pick_slot_skips_link_local_and_used_slots);
    RUN_TEST(test_pick_slot_returns_none_when_all_used);
    RUN_TEST(test_every_state_has_a_name);
    return UNITY_END();
}
