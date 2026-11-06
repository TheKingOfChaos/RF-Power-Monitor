// Host unit tests for lib/access_control: per-source rate limiting and the access log.
// Run: pio test -e native -f native/test_access_control

#include <unity.h>
#include <string.h>
#include <access_control.h>

void setUp() {}
void tearDown() {}

static const uint32_t PC = 0x0A0A0641;     // 10.10.6.65
static const uint32_t OTHER = 0x0A0A0642;  // 10.10.6.66

// ---- RateLimiter ----

static void test_burst_is_allowed_then_refused() {
    RateLimiter r(10, 5);
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE_MESSAGE(r.allow(PC, 1000), "within burst");
    TEST_ASSERT_FALSE(r.allow(PC, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, r.refused());
}

static void test_tokens_refill_at_the_rate() {
    RateLimiter r(10, 5);                       // one token per 100 ms
    for (int i = 0; i < 5; i++) r.allow(PC, 0);
    TEST_ASSERT_FALSE(r.allow(PC, 99));
    TEST_ASSERT_TRUE(r.allow(PC, 100));
    TEST_ASSERT_FALSE(r.allow(PC, 150));
    TEST_ASSERT_TRUE(r.allow(PC, 200));
}

static void test_refill_never_exceeds_burst() {
    RateLimiter r(10, 5);
    r.allow(PC, 0);
    // An hour of silence still only gives `burst` tokens
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(r.allow(PC, 3600000));
    TEST_ASSERT_FALSE(r.allow(PC, 3600000));
}

static void test_sustained_rate_over_ten_seconds() {
    RateLimiter r(50, 100);
    uint32_t allowed = 0;
    for (uint32_t t = 0; t < 10000; t++) {      // 1000 requests per second offered
        if (r.allow(PC, t)) allowed++;
    }
    // burst + 10 s * 50/s
    TEST_ASSERT_UINT32_WITHIN(2, 100 + 500, allowed);
}

static void test_sources_are_limited_independently() {
    RateLimiter r(10, 2);
    TEST_ASSERT_TRUE(r.allow(PC, 0));
    TEST_ASSERT_TRUE(r.allow(PC, 0));
    TEST_ASSERT_FALSE(r.allow(PC, 0));
    TEST_ASSERT_TRUE(r.allow(OTHER, 0));        // a flooding PC does not lock out others
    TEST_ASSERT_TRUE(r.allow(OTHER, 0));
}

static void test_full_table_replaces_least_recently_seen_source() {
    RateLimiter r(1, 1);
    for (uint32_t i = 0; i < RateLimiter::MAX_SOURCES; i++) TEST_ASSERT_TRUE(r.allow(100 + i, i));
    TEST_ASSERT_FALSE(r.allow(100, 20));        // source 100 is out of tokens
    // A new source evicts the least recently seen one (101), not the active 100
    TEST_ASSERT_TRUE(r.allow(999, 30));
    TEST_ASSERT_FALSE(r.allow(100, 31));        // still tracked, still limited
}

static void test_rate_limiter_survives_millis_wraparound() {
    RateLimiter r(10, 1);
    TEST_ASSERT_TRUE(r.allow(PC, 0xFFFFFFFFu - 50));
    TEST_ASSERT_FALSE(r.allow(PC, 0xFFFFFFFFu));
    TEST_ASSERT_TRUE(r.allow(PC, 49));          // 100 ms later, across the wrap
}

static void test_idle_gap_longer_than_24_days_refills_instead_of_freezing() {
    // A gap above 2^31 ms looks negative as a signed value; the bucket must
    // still refill, and keep working afterwards
    RateLimiter r(10, 2, 100, 3);
    TEST_ASSERT_TRUE(r.allow(PC, 1000));
    TEST_ASSERT_TRUE(r.allow(PC, 1000));
    TEST_ASSERT_FALSE(r.allow(PC, 1000));
    uint32_t later = 1000 + 0x80000000u + 5000;   // ~24.9 days later
    TEST_ASSERT_TRUE(r.allow(PC, later));
    TEST_ASSERT_TRUE(r.allow(PC, later));
    TEST_ASSERT_FALSE(r.allow(PC, later));
    TEST_ASSERT_TRUE(r.allow(PC, later + 100));     // normal refill continues
    TEST_ASSERT_TRUE(r.allow(OTHER, later + 100));  // global bucket refilled too
}

// ---- global limit (cannot be reset by rotating or spoofed sources) ----

static void test_rotating_sources_cannot_exceed_global_limit() {
    // Every request from a new address: each one would get a fresh per-source bucket
    RateLimiter r(50, 100, 100, 200);
    uint32_t allowed = 0;
    for (uint32_t t = 0; t < 1000; t++) {
        if (r.allow(0x0A000000u + t, t)) allowed++;
    }
    // Global burst 200 + 1 s * 100/s
    TEST_ASSERT_UINT32_WITHIN(2, 300, allowed);
}

static void test_refused_source_does_not_drain_global_bucket() {
    RateLimiter r(10, 2, 100, 3);
    TEST_ASSERT_TRUE(r.allow(PC, 0));
    TEST_ASSERT_TRUE(r.allow(PC, 0));
    for (int i = 0; i < 50; i++) TEST_ASSERT_FALSE(r.allow(PC, 0));   // PC flooding, refused per source
    TEST_ASSERT_TRUE(r.allow(OTHER, 0));                             // global still has its third token
}

static void test_single_source_is_limited_by_its_own_bucket_under_global() {
    RateLimiter r(50, 100, 100, 200);
    uint32_t allowed = 0;
    for (uint32_t t = 0; t < 10000; t++) {
        if (r.allow(PC, t)) allowed++;
    }
    TEST_ASSERT_UINT32_WITHIN(2, 100 + 500, allowed);
}

static void test_trusted_manager_is_not_starved_by_spoofed_flood() {
    const uint32_t MANAGER = 0x0A0A0610;
    RateLimiter r(50, 100, 100, 200);
    TEST_ASSERT_TRUE(r.allow(MANAGER, 0));
    r.markTrusted(MANAGER);
    // Spoofed sources drain the global bucket completely
    for (uint32_t i = 0; i < 1000; i++) r.allow(0xC0000000u + i, 1);
    TEST_ASSERT_FALSE(r.allow(0xC0001000u, 1));
    // The trusted manager is only limited by its own bucket: 99 tokens left
    uint32_t allowed = 0;
    for (int i = 0; i < 120; i++) {
        if (r.allow(MANAGER, 2)) allowed++;
    }
    TEST_ASSERT_EQUAL_UINT32(99, allowed);
}

static void test_spoofed_sources_cannot_evict_trusted_ones() {
    RateLimiter r(1, 1, 1000, 1000);
    const uint32_t MANAGER = 0x0A0A0610;
    TEST_ASSERT_TRUE(r.allow(MANAGER, 0));
    r.markTrusted(MANAGER);
    // Thousands of new sources rotate through the table
    for (uint32_t i = 0; i < 5000; i++) r.allow(0xC0000000u + i, 10 + i / 100);
    // Still tracked: its bucket (empty, 1/s) was not reset by an eviction
    TEST_ASSERT_FALSE(r.allow(MANAGER, 100));
    TEST_ASSERT_TRUE(r.allow(MANAGER, 1100));
}

static void test_untrusted_entries_are_evicted_before_trusted_ones() {
    RateLimiter r(1, 1);
    // Fill the table: one trusted (oldest), the rest untrusted
    r.allow(1, 0);
    r.markTrusted(1);
    for (uint32_t i = 2; i <= RateLimiter::MAX_SOURCES; i++) r.allow(i, i);
    r.allow(999, 100);                      // evicts the oldest untrusted (2), not trusted 1
    TEST_ASSERT_FALSE(r.allow(1, 100 + 500)); // 1 is still tracked with its empty bucket
}

static void test_mark_trusted_of_unknown_source_is_ignored() {
    RateLimiter r(10, 1, 10, 1);
    r.markTrusted(PC);                      // never seen: nothing happens, nothing breaks
    TEST_ASSERT_TRUE(r.allow(PC, 0));
}

static void test_trusted_sources_share_their_own_bounded_bucket() {
    // Knowing the community (often the default "public") must not give unlimited throughput.
    // Per-source limits set high so the shared trusted bucket is the one that binds.
    RateLimiter r(1000, 1000, 100, 200);
    r.configureTrusted(200, 400, 4);
    for (uint32_t s = 1; s <= 4; s++) {
        r.allow(s, 0);
        r.markTrusted(s);
    }
    uint32_t allowed = 0;
    for (uint32_t t = 1; t <= 1000; t++) {
        for (uint32_t s = 1; s <= 4; s++) {
            if (r.allow(s, t)) allowed++;
        }
    }
    // Trusted burst 400 + 1 s * 200/s, not 4 * 1000
    TEST_ASSERT_UINT32_WITHIN(5, 400 + 200, allowed);
}

// The firmware's limits: 50/s per source, burst 100; untrusted together 100/s, burst 200;
// trusted together 200/s, burst 400, and 400 per trusted source
static RateLimiter firmwareLimiter() {
    RateLimiter r(50, 100, 100, 200);
    r.configureTrusted(200, 400, 4, 600000, 400, 200);
    return r;
}

/// Requests from source answered at one instant, out of n
static uint32_t burstOf(RateLimiter& r, SourceKey source, uint32_t n, uint32_t nowMs) {
    uint32_t allowed = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (r.allow(source, nowMs)) allowed++;
    }
    return allowed;
}

static void test_trusted_source_may_burst_a_whole_walk() {
    // A walk from the root is ~300 requests at full speed; it must not lose one
    RateLimiter r = firmwareLimiter();
    TEST_ASSERT_TRUE(r.allow(PC, 0));
    r.markTrusted(PC);  // the first request had the right community
    TEST_ASSERT_EQUAL_UINT32(300, burstOf(r, PC, 300, 1));
}

static void test_trusted_source_can_walk_twice_in_a_row() {
    // Two walks of ~280 requests back to back, each in about 1.1 s at full speed
    RateLimiter r = firmwareLimiter();
    r.allow(PC, 0);
    r.markTrusted(PC);
    uint32_t allowed = 0;
    for (uint32_t i = 0; i < 560; i++) {
        if (r.allow(PC, 1 + i * 4)) allowed++;  // one request every 4 ms (250/s)
    }
    TEST_ASSERT_EQUAL_UINT32(560, allowed);
}

static void test_trusted_sources_together_stay_within_their_shared_limit() {
    // A higher rate per trusted source does not raise the cap of all trusted sources together
    RateLimiter r = firmwareLimiter();
    r.allow(PC, 0);
    r.markTrusted(PC);
    burstOf(r, PC, 400, 0);                                  // the shared bucket is empty now
    TEST_ASSERT_UINT32_WITHIN(2, 200, burstOf(r, PC, 1000, 1000));  // 1 s later: 200/s, shared
}

static void test_untrusted_source_burst_stays_100() {
    RateLimiter r = firmwareLimiter();
    TEST_ASSERT_EQUAL_UINT32(100, burstOf(r, PC, 300, 0));
}

static void test_revoked_trust_returns_to_the_untrusted_burst() {
    RateLimiter r = firmwareLimiter();
    r.allow(PC, 0);
    r.markTrusted(PC);
    r.clearTrust();  // the community changed
    TEST_ASSERT_EQUAL_UINT32(100, burstOf(r, PC, 300, 1));
}

static void test_trusted_source_burst_defaults_to_the_untrusted_one() {
    // Without a trusted per-source burst the old behaviour stays: 100 for everyone
    RateLimiter r(50, 100, 100, 200);
    r.configureTrusted(200, 400, 4, 600000);
    r.allow(PC, 0);
    r.markTrusted(PC);
    TEST_ASSERT_EQUAL_UINT32(99, burstOf(r, PC, 300, 1));
}

static void test_number_of_trusted_sources_is_capped() {
    RateLimiter r(1, 1, 1, 1);
    r.configureTrusted(1000, 1000, 2);
    for (uint32_t s = 1; s <= 3; s++) {
        r.allow(s, 0);
        r.markTrusted(s);
    }
    TEST_ASSERT_EQUAL_UINT8(2, r.trustedCount());
    // Another untrusted source takes the one untrusted shared token...
    TEST_ASSERT_TRUE(r.allow(4, 2000));
    // ...so source 3, which stayed untrusted, is refused, while trusted source 1 is not
    TEST_ASSERT_FALSE(r.allow(3, 2000));
    TEST_ASSERT_TRUE(r.allow(1, 2000));
}

static void test_evicted_trusted_source_frees_its_slot() {
    RateLimiter r(1, 1);
    r.configureTrusted(1000, 1000, RateLimiter::MAX_SOURCES);
    for (uint32_t i = 1; i <= RateLimiter::MAX_SOURCES; i++) {
        r.allow(i, i);
        r.markTrusted(i);
    }
    TEST_ASSERT_EQUAL_UINT8(RateLimiter::MAX_SOURCES, r.trustedCount());
    // Table full of trusted sources: a new one evicts the least recently seen (1)
    r.allow(999, 100);
    TEST_ASSERT_EQUAL_UINT8(RateLimiter::MAX_SOURCES - 1, r.trustedCount());
    r.markTrusted(999);
    TEST_ASSERT_EQUAL_UINT8(RateLimiter::MAX_SOURCES, r.trustedCount());
}

static void test_clear_trust_revokes_every_trusted_source() {
    // Used when the community changes: trust earned with the old one must not carry over
    RateLimiter r(1000, 1000, 1, 1);
    r.configureTrusted(1000, 1000, 4);
    r.allow(PC, 0);
    r.markTrusted(PC);
    TEST_ASSERT_EQUAL_UINT8(1, r.trustedCount());
    r.clearTrust();
    TEST_ASSERT_EQUAL_UINT8(0, r.trustedCount());
    // Back on the untrusted global bucket (1 token, already used by the first request)
    TEST_ASSERT_FALSE(r.allow(PC, 0));
}

static void test_trust_expires_after_inactivity() {
    RateLimiter r(1000, 1000, 1, 1);
    r.configureTrusted(1000, 1000, 4, 60000);
    r.allow(PC, 0);
    r.markTrusted(PC);
    TEST_ASSERT_TRUE(r.allow(PC, 59000));       // still active, still trusted
    TEST_ASSERT_EQUAL_UINT8(1, r.trustedCount());
    TEST_ASSERT_TRUE(r.allow(PC, 59000 + 60001)); // idle for longer than the timeout: trust dropped,
    TEST_ASSERT_EQUAL_UINT8(0, r.trustedCount()); // this request used the refilled untrusted bucket
    TEST_ASSERT_FALSE(r.allow(PC, 59000 + 60001));
}

static void test_idle_trusted_sources_release_their_slots() {
    // Managers that went quiet must not hold the trust slots forever
    RateLimiter r(1000, 1000, 1000, 1000);
    r.configureTrusted(1000, 1000, 4, 60000);
    for (uint32_t s = 1; s <= 4; s++) {
        r.allow(s, 0);
        r.markTrusted(s);
    }
    TEST_ASSERT_EQUAL_UINT8(4, r.trustedCount());
    // None of them sends again; a new manager arrives after the timeout
    const uint32_t NEW_MANAGER = 99;
    r.allow(NEW_MANAGER, 70000);
    r.markTrusted(NEW_MANAGER);
    TEST_ASSERT_EQUAL_UINT8(1, r.trustedCount());
}

static void test_expired_trust_does_not_protect_against_eviction() {
    RateLimiter r(1, 1, 1000, 1000);
    r.configureTrusted(1000, 1000, RateLimiter::MAX_SOURCES, 60000);
    // Fill the table with trusted sources, then let their trust expire
    for (uint32_t s = 1; s <= RateLimiter::MAX_SOURCES; s++) {
        r.allow(s, s);
        r.markTrusted(s);
    }
    // A new source at 70 s evicts source 1 (oldest); all entries are untrusted by then
    r.allow(100, 70000);
    TEST_ASSERT_EQUAL_UINT8(0, r.trustedCount());
    // Source 2 is still tracked (only one eviction happened): its bucket refilled normally
    TEST_ASSERT_TRUE(r.allow(2, 70001));
}

static void test_default_keeps_trusted_sources_out_of_global_limit() {
    // Without configureTrusted(): trusted sources have no shared cap (backward compatible)
    RateLimiter r(1000, 1000, 1, 1);
    r.allow(PC, 0);
    r.markTrusted(PC);
    for (int i = 0; i < 500; i++) TEST_ASSERT_TRUE(r.allow(PC, 1));
}

static void test_global_limit_is_off_by_default() {
    RateLimiter r(10, 1);
    for (uint32_t i = 0; i < 1000; i++) TEST_ASSERT_TRUE(r.allow(i, 0));   // all new sources
}

// ---- AccessLog ----

static void test_log_records_newest_first() {
    AccessLog log;
    TEST_ASSERT_TRUE(log.record(PC, AccessEvent::BadCommunity, 10));
    TEST_ASSERT_TRUE(log.record(OTHER, AccessEvent::BadVersion, 20));
    TEST_ASSERT_EQUAL_UINT8(2, log.size());
    TEST_ASSERT_EQUAL_UINT32(OTHER, log.newest(0).source.ipv4());
    TEST_ASSERT_EQUAL(AccessEvent::BadVersion, log.newest(0).event);
    TEST_ASSERT_EQUAL_UINT32(PC, log.newest(1).source.ipv4());
}

static void test_repeats_are_folded_into_one_entry() {
    AccessLog log;
    TEST_ASSERT_TRUE(log.record(PC, AccessEvent::BadCommunity, 10));
    TEST_ASSERT_FALSE(log.record(PC, AccessEvent::BadCommunity, 20));
    TEST_ASSERT_FALSE(log.record(PC, AccessEvent::BadCommunity, 30));
    TEST_ASSERT_EQUAL_UINT8(1, log.size());
    TEST_ASSERT_EQUAL_UINT32(3, log.newest(0).count);
    TEST_ASSERT_EQUAL_UINT32(10, log.newest(0).firstMs);
    TEST_ASSERT_EQUAL_UINT32(30, log.newest(0).lastMs);
    TEST_ASSERT_EQUAL_UINT32(3, log.total());
}

static void test_different_event_or_source_starts_new_entry() {
    AccessLog log;
    log.record(PC, AccessEvent::BadCommunity, 1);
    TEST_ASSERT_TRUE(log.record(PC, AccessEvent::RateLimited, 2));
    TEST_ASSERT_TRUE(log.record(OTHER, AccessEvent::RateLimited, 3));
    TEST_ASSERT_TRUE(log.record(PC, AccessEvent::BadCommunity, 4));  // not adjacent to the first
    TEST_ASSERT_EQUAL_UINT8(4, log.size());
}

static void test_log_keeps_only_the_most_recent_entries() {
    AccessLog log;
    for (uint32_t i = 0; i < AccessLog::CAPACITY + 5; i++) log.record(i, AccessEvent::Malformed, i);
    TEST_ASSERT_EQUAL_UINT8(AccessLog::CAPACITY, log.size());
    TEST_ASSERT_EQUAL_UINT32(AccessLog::CAPACITY + 4, log.newest(0).source.ipv4());
    TEST_ASSERT_EQUAL_UINT32(5, log.newest(AccessLog::CAPACITY - 1).source.ipv4());
    TEST_ASSERT_EQUAL_UINT32(AccessLog::CAPACITY + 5, log.total());
}

static void test_newest_out_of_range_is_clamped() {
    AccessLog log;
    log.record(PC, AccessEvent::Malformed, 1);
    TEST_ASSERT_EQUAL_UINT32(PC, log.newest(200).source.ipv4());
}

static void test_event_names_and_ip_format() {
    TEST_ASSERT_EQUAL_STRING("bad community", accessEventName(AccessEvent::BadCommunity));
    TEST_ASSERT_EQUAL_STRING("bad version", accessEventName(AccessEvent::BadVersion));
    TEST_ASSERT_EQUAL_STRING("malformed", accessEventName(AccessEvent::Malformed));
    TEST_ASSERT_EQUAL_STRING("rate limited", accessEventName(AccessEvent::RateLimited));
    TEST_ASSERT_EQUAL_STRING("set rejected", accessEventName(AccessEvent::SetRejected));
    TEST_ASSERT_EQUAL_STRING("web login failed", accessEventName(AccessEvent::WebLoginFailed));
    TEST_ASSERT_EQUAL_STRING("web locked out", accessEventName(AccessEvent::WebLoginLocked));
    TEST_ASSERT_EQUAL_STRING("web upload refused", accessEventName(AccessEvent::WebUploadRefused));
    char buf[16];
    formatIPv4(PC, buf);
    TEST_ASSERT_EQUAL_STRING("10.10.6.65", buf);
    formatIPv4(0xFFFFFFFFu, buf);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", buf);
    formatIPv4(0, buf);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);
}

// ---- IPv6 sources ----

static const uint8_t V6_A[16] = {0xfd, 0x16, 0xd8, 0x33, 0x4f, 0x0e, 0x3e, 0xce,   // fd16:d833:4f0e:3ece:9000:9aaa:490a:3189
                                 0x90, 0x00, 0x9a, 0xaa, 0x49, 0x0a, 0x31, 0x89};
static const uint8_t V6_A2[16] = {0xfd, 0x16, 0xd8, 0x33, 0x4f, 0x0e, 0x3e, 0xce,  // same /64, other host
                                  0x19, 0x6a, 0xb2, 0x18, 0x7f, 0xdb, 0x1c, 0xce};
static const uint8_t V6_B[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x01, 0x00, 0x02,   // 2001:db8:1:2::1
                                 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t V4_MAPPED[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 10, 6, 65};
static const uint8_t LOOPBACK6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static SourceKey v6(const uint8_t a[16]) {
    SourceKey k;
    TEST_ASSERT_TRUE(sourceKeyIPv6(a, k));
    return k;
}

static void test_ipv4_source_key_is_the_address() {
    const uint8_t a[4] = {10, 10, 6, 65};
    SourceKey k = sourceKeyIPv4(a);
    TEST_ASSERT_TRUE(k == SourceKey(PC));
    TEST_ASSERT_TRUE(k.isIPv4());
    TEST_ASSERT_EQUAL_UINT32(PC, k.ipv4());
    TEST_ASSERT_TRUE(k != SourceKey(OTHER));
}

static void test_ipv6_source_key_is_the_full_address() {
    SourceKey a = v6(V6_A), a2 = v6(V6_A2), b = v6(V6_B);
    TEST_ASSERT_TRUE(a.hi == 0xfd16d8334f0e3eceull && a.lo == 0x90009aaa490a3189ull);
    TEST_ASSERT_TRUE(a != a2);  // same /64, different hosts: different keys
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_FALSE(a.isIPv4());
    TEST_ASSERT_FALSE(b.isIPv4());
    // Same low 64 bits as an IPv4 key but another prefix: not IPv4
    uint8_t lookalike[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 10, 6, 65};
    SourceKey l = v6(lookalike);
    TEST_ASSERT_FALSE(l.isIPv4());
    TEST_ASSERT_TRUE(l != SourceKey(PC));
}

static void test_ipv4_mapped_ipv6_counts_as_ipv4_and_reserved_space_is_refused() {
    SourceKey k(1);
    TEST_ASSERT_TRUE(sourceKeyIPv6(V4_MAPPED, k));
    TEST_ASSERT_TRUE(k == SourceKey(PC));
    // ::/32 (loopback, unspecified, IPv4-compatible ...) cannot come from the LAN
    TEST_ASSERT_FALSE(sourceKeyIPv6(LOOPBACK6, k));
    uint8_t zero32[16] = {0, 0, 0, 0, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9};
    TEST_ASSERT_FALSE(sourceKeyIPv6(zero32, k));
    uint8_t compat[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 10, 6, 65};  // ::10.10.6.65
    TEST_ASSERT_FALSE(sourceKeyIPv6(compat, k));
    uint8_t almostMapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0xff, 10, 10, 6, 65};
    TEST_ASSERT_FALSE(sourceKeyIPv6(almostMapped, k));
}

static void test_ipv6_hosts_in_one_64_have_their_own_buckets() {
    RateLimiter rl(1, 2);
    SourceKey a = v6(V6_A), a2 = v6(V6_A2), b = v6(V6_B);
    TEST_ASSERT_TRUE(rl.allow(a, 0));
    TEST_ASSERT_TRUE(rl.allow(a, 0));
    TEST_ASSERT_FALSE(rl.allow(a, 0));
    TEST_ASSERT_TRUE(rl.allow(a2, 0));   // same /64, another host: its own bucket
    TEST_ASSERT_TRUE(rl.allow(b, 0));
    TEST_ASSERT_TRUE(rl.allow(PC, 0));   // and IPv4 sources are separate
}

static void test_trust_does_not_extend_to_other_hosts_in_the_managers_64() {
    // The manager authenticated over IPv6; a neighbour in the same /64 did not.
    // Untrusted traffic shares a bucket of 10, trusted traffic one of 1000.
    RateLimiter rl(1000, 1000, 1, 10);
    rl.configureTrusted(1, 1000, 4);
    SourceKey manager = v6(V6_A), neighbour = v6(V6_A2);
    TEST_ASSERT_TRUE(rl.allow(manager, 0));
    rl.markTrusted(manager);
    TEST_ASSERT_EQUAL_UINT8(1, rl.trustedCount());
    uint32_t allowed = 0;
    for (int i = 0; i < 100; i++) {
        if (rl.allow(neighbour, 0)) allowed++;
    }
    TEST_ASSERT_EQUAL_UINT32(9, allowed);  // what is left of the untrusted bucket, not the trusted one
    TEST_ASSERT_EQUAL_UINT8(1, rl.trustedCount());
    TEST_ASSERT_TRUE(rl.allow(manager, 0));  // the manager still draws from the trusted bucket
}

static void test_neighbour_cannot_drain_the_managers_bucket() {
    RateLimiter rl(1, 3);
    SourceKey manager = v6(V6_A), neighbour = v6(V6_A2);
    rl.allow(manager, 0);
    rl.markTrusted(manager);
    for (int i = 0; i < 50; i++) rl.allow(neighbour, 0);
    TEST_ASSERT_TRUE(rl.allow(manager, 0));
    TEST_ASSERT_TRUE(rl.allow(manager, 0));
    TEST_ASSERT_FALSE(rl.allow(manager, 0));  // only its own 3 tokens were used
}

static void test_access_log_keeps_ipv6_addresses() {
    AccessLog log;
    SourceKey a = v6(V6_A), a2 = v6(V6_A2);
    TEST_ASSERT_TRUE(log.record(a, AccessEvent::BadCommunity, 1));
    TEST_ASSERT_FALSE(log.record(a, AccessEvent::BadCommunity, 2));  // folded
    TEST_ASSERT_TRUE(log.record(a2, AccessEvent::BadCommunity, 3));  // same /64, another host
    TEST_ASSERT_TRUE(log.record(PC, AccessEvent::BadCommunity, 4));  // IPv4 is another source
    TEST_ASSERT_TRUE(log.newest(2).source == a);
    TEST_ASSERT_TRUE(log.newest(1).source == a2);
}

static void test_format_source_ipv4() {
    char buf[SOURCE_TEXT_MAX];
    formatSource(PC, buf);
    TEST_ASSERT_EQUAL_STRING("10.10.6.65", buf);
    formatSource(0xFFFFFFFFu, buf);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", buf);
}

static void test_format_source_ipv6_full_address() {
    char buf[SOURCE_TEXT_MAX];
    formatSource(v6(V6_A), buf);
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece:9000:9aaa:490a:3189", buf);
    SourceKey max;
    max.hi = max.lo = 0xFFFFFFFFFFFFFFFFull;
    formatSource(max, buf);
    TEST_ASSERT_EQUAL_STRING("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", buf);
    TEST_ASSERT_EQUAL_size_t(SOURCE_TEXT_MAX - 1, strlen(buf));  // the longest text fits exactly
}

static void test_format_source_ipv6_compresses_longest_zero_run() {
    char buf[SOURCE_TEXT_MAX];
    formatSource(v6(V6_B), buf);
    TEST_ASSERT_EQUAL_STRING("2001:db8:1:2::1", buf);
    SourceKey k;
    k.hi = 0xfd16d8334f0e3eceull;  // run at the end
    k.lo = 0;
    formatSource(k, buf);
    TEST_ASSERT_EQUAL_STRING("fd16:d833:4f0e:3ece::", buf);
    k.hi = 0x2001000000000001ull;  // 2001:0:0:1:0:0:0:1, the longer run wins
    k.lo = 0x0000000000000001ull;
    formatSource(k, buf);
    TEST_ASSERT_EQUAL_STRING("2001:0:0:1::1", buf);
    k.hi = 0x2001000000000001ull;  // 2001:0:0:1:0:0:2:3, tie: the first run
    k.lo = 0x0000000000020003ull;
    formatSource(k, buf);
    TEST_ASSERT_EQUAL_STRING("2001::1:0:0:2:3", buf);
    k.hi = 0x2001000100000002ull;  // a single zero group is not compressed
    k.lo = 0x0003000400050006ull;
    formatSource(k, buf);
    TEST_ASSERT_EQUAL_STRING("2001:1:0:2:3:4:5:6", buf);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ipv4_source_key_is_the_address);
    RUN_TEST(test_ipv6_source_key_is_the_full_address);
    RUN_TEST(test_ipv4_mapped_ipv6_counts_as_ipv4_and_reserved_space_is_refused);
    RUN_TEST(test_ipv6_hosts_in_one_64_have_their_own_buckets);
    RUN_TEST(test_trust_does_not_extend_to_other_hosts_in_the_managers_64);
    RUN_TEST(test_neighbour_cannot_drain_the_managers_bucket);
    RUN_TEST(test_access_log_keeps_ipv6_addresses);
    RUN_TEST(test_format_source_ipv4);
    RUN_TEST(test_format_source_ipv6_full_address);
    RUN_TEST(test_format_source_ipv6_compresses_longest_zero_run);
    RUN_TEST(test_burst_is_allowed_then_refused);
    RUN_TEST(test_tokens_refill_at_the_rate);
    RUN_TEST(test_refill_never_exceeds_burst);
    RUN_TEST(test_sustained_rate_over_ten_seconds);
    RUN_TEST(test_sources_are_limited_independently);
    RUN_TEST(test_full_table_replaces_least_recently_seen_source);
    RUN_TEST(test_rate_limiter_survives_millis_wraparound);
    RUN_TEST(test_idle_gap_longer_than_24_days_refills_instead_of_freezing);
    RUN_TEST(test_rotating_sources_cannot_exceed_global_limit);
    RUN_TEST(test_refused_source_does_not_drain_global_bucket);
    RUN_TEST(test_single_source_is_limited_by_its_own_bucket_under_global);
    RUN_TEST(test_trusted_manager_is_not_starved_by_spoofed_flood);
    RUN_TEST(test_spoofed_sources_cannot_evict_trusted_ones);
    RUN_TEST(test_untrusted_entries_are_evicted_before_trusted_ones);
    RUN_TEST(test_mark_trusted_of_unknown_source_is_ignored);
    RUN_TEST(test_trusted_sources_share_their_own_bounded_bucket);
    RUN_TEST(test_trusted_source_may_burst_a_whole_walk);
    RUN_TEST(test_trusted_source_can_walk_twice_in_a_row);
    RUN_TEST(test_trusted_sources_together_stay_within_their_shared_limit);
    RUN_TEST(test_untrusted_source_burst_stays_100);
    RUN_TEST(test_revoked_trust_returns_to_the_untrusted_burst);
    RUN_TEST(test_trusted_source_burst_defaults_to_the_untrusted_one);
    RUN_TEST(test_number_of_trusted_sources_is_capped);
    RUN_TEST(test_evicted_trusted_source_frees_its_slot);
    RUN_TEST(test_clear_trust_revokes_every_trusted_source);
    RUN_TEST(test_trust_expires_after_inactivity);
    RUN_TEST(test_idle_trusted_sources_release_their_slots);
    RUN_TEST(test_expired_trust_does_not_protect_against_eviction);
    RUN_TEST(test_default_keeps_trusted_sources_out_of_global_limit);
    RUN_TEST(test_global_limit_is_off_by_default);
    RUN_TEST(test_log_records_newest_first);
    RUN_TEST(test_repeats_are_folded_into_one_entry);
    RUN_TEST(test_different_event_or_source_starts_new_entry);
    RUN_TEST(test_log_keeps_only_the_most_recent_entries);
    RUN_TEST(test_newest_out_of_range_is_clamped);
    RUN_TEST(test_event_names_and_ip_format);
    return UNITY_END();
}
