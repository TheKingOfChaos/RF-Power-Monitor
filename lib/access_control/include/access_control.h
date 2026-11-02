#pragma once

/**
 * @file access_control.h
 * @brief Per-source request rate limiting and a log of rejected access attempts.
 *
 * Pure C++ with no Arduino dependency, unit tested on the host. The SNMP
 * service checks every request with RateLimiter::allow() and records
 * rejections in AccessLog; the serial menu shows the log.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Token bucket per source address, plus shared buckets for untrusted and trusted sources
 *
 * Each source may send `burst` requests at once and then `ratePerSecond` on
 * average. Sources are tracked in a small table; when it is full, the source
 * seen least recently is replaced and the new one starts with a full bucket.
 *
 * Because of that, rotating or spoofed source addresses (trivial with UDP)
 * would each get a fresh bucket. The global bucket caps all untrusted
 * sources together, no matter how many appear.
 *
 * Sources that sent a request with the right community can be marked trusted
 * (markTrusted). They draw from a separate shared trusted bucket instead, so
 * a flood from unauthenticated addresses cannot starve a real manager. Trust
 * is limited, because the community may be the well-known default or be
 * sniffed (SNMPv1 sends it in clear text):
 *   - the trusted bucket caps all trusted sources together
 *   - at most `maxTrusted` sources are trusted at once
 *   - trust expires after `trustTimeoutMs` without requests; every allow()
 *     sweeps the whole table, so idle sources release their slots
 *   - clearTrust() revokes all trust, e.g. when the community changes
 * Trusted sources are evicted from the table only when no untrusted entry is
 * left. A request refused by its source bucket takes no shared token.
 *
 * Time comes from millis() on a single core and only moves forward; a gap
 * long enough to fill a bucket (including more than 2^31 ms) fills it.
 */
/**
 * @brief Who sent a request: the full source address, for rate limiting, trust and the access log
 *
 * Stored as 128 bits; an IPv4 address is held as ::ffff:a.b.c.d. The full
 * address and not the /64: trust is granted per key, and every host on a LAN
 * shares one /64, so a /64 key would let any of them use a manager's trust.
 * A host rotating through addresses gets a fresh bucket for each, the same
 * as spoofed IPv4 addresses; the shared untrusted bucket caps all of them.
 */
struct SourceKey {
    uint64_t hi;  ///< Address bytes 0..7, big-endian
    uint64_t lo;  ///< Address bytes 8..15, big-endian

    SourceKey() : hi(0), lo(0) {}
    /// IPv4 address in host order (10.10.6.65 = 0x0A0A0641)
    SourceKey(uint32_t ipv4) : hi(0), lo(0x0000FFFF00000000ull | ipv4) {}

    bool isIPv4() const { return hi == 0 && (lo >> 32) == 0xFFFF; }
    /// The IPv4 address in host order; only meaningful when isIPv4()
    uint32_t ipv4() const { return (uint32_t)lo; }

    bool operator==(const SourceKey& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const SourceKey& o) const { return !(*this == o); }
};

/// Key of an IPv4 source (network byte order bytes)
SourceKey sourceKeyIPv4(const uint8_t address[4]);

/**
 * @brief Key of an IPv6 source (network byte order bytes)
 *
 * ::ffff:a.b.c.d (IPv4-mapped) gives the IPv4 key. Other addresses in ::/32
 * (loopback, unspecified, IPv4-compatible) cannot come from the LAN: false,
 * drop the request.
 */
bool sourceKeyIPv6(const uint8_t address[16], SourceKey& key);

/// Longest formatSource() text including the NUL ("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
static const size_t SOURCE_TEXT_MAX = 40;

/// "10.10.6.65" or "fd16:d833:4f0e:3ece::a22c" (RFC 5952: lower case, longest zero run as ::)
void formatSource(const SourceKey& key, char* buf);

class RateLimiter {
public:
    /// Sources tracked at the same time
    static constexpr uint8_t MAX_SOURCES = 8;

    /**
     * @param globalRatePerSecond Shared limit for untrusted sources; 0 disables it
     */
    RateLimiter(uint32_t ratePerSecond, uint32_t burst, uint32_t globalRatePerSecond = 0, uint32_t globalBurst = 0)
        : _rate(ratePerSecond), _burst(burst), _global{globalRatePerSecond, globalBurst, globalBurst * 1000u, 0, false} {}

    /**
     * @brief Limits for trusted sources (default: no shared cap, up to MAX_SOURCES, no expiry)
     * @param globalRatePerSecond Shared limit for all trusted sources; 0 disables it
     * @param maxTrusted          Most sources trusted at once
     * @param trustTimeoutMs      Trust is dropped after this long without requests; 0 = never
     * @param sourceBurst         Burst of each trusted source (its average stays ratePerSecond), so a
     *                            manager's walk runs at full speed; 0 = the untrusted burst. A source
     *                            gets it in full when it becomes trusted, and loses it with its trust
     * @param sourceRatePerSecond Average of each trusted source; 0 = the untrusted rate. All trusted
     *                            sources together still stay within globalRatePerSecond
     */
    void configureTrusted(uint32_t globalRatePerSecond, uint32_t globalBurst, uint8_t maxTrusted,
                          uint32_t trustTimeoutMs = 0, uint32_t sourceBurst = 0, uint32_t sourceRatePerSecond = 0);

    /**
     * @brief Take one token for this source
     * @param source sourceKeyIPv4() / sourceKeyIPv6()
     * @return true if the request may be handled
     */
    bool allow(SourceKey source, uint32_t nowMs);

    /**
     * @brief Mark a tracked source as trusted (it authenticated successfully)
     *
     * Call after allow() for a request that had the right community. Unknown
     * sources are ignored, and nothing happens when maxTrusted is reached.
     */
    void markTrusted(SourceKey source);

    /// Revoke trust of every source (call when the community changes)
    void clearTrust();

    /// Sources currently trusted
    uint8_t trustedCount() const;

    /// Requests refused since start
    uint32_t refused() const { return _refused; }

private:
    struct Bucket {
        SourceKey source;
        uint32_t milliTokens;  // tokens * 1000, avoids floating point
        uint32_t lastMs;
        bool used;
        bool trusted;
    };

    struct SharedBucket {
        uint32_t rate;
        uint32_t burst;
        uint32_t milliTokens;
        uint32_t lastMs;
        bool started;
    };

    Bucket* find(SourceKey source, uint32_t nowMs);
    /// The source's own burst and rate: the trusted ones while it is trusted
    uint32_t sourceBurst(const Bucket& b) const;
    uint32_t sourceRate(const Bucket& b) const;
    void expireTrust(uint32_t nowMs);
    static void refill(uint32_t& milliTokens, uint32_t& lastMs, uint32_t rate, uint32_t burst, uint32_t nowMs);
    static void refillShared(SharedBucket& b, uint32_t nowMs);

    uint32_t _rate;
    uint32_t _burst;
    SharedBucket _global;
    SharedBucket _trustedGlobal = {0, 0, 0, 0, false};
    uint8_t _maxTrusted = MAX_SOURCES;
    uint32_t _trustTimeoutMs = 0;
    uint32_t _trustedSourceBurst = 0;  // 0: trusted sources keep _burst
    uint32_t _trustedSourceRate = 0;   // 0: trusted sources keep _rate
    uint32_t _refused = 0;
    Bucket _buckets[MAX_SOURCES] = {};
};

/// Kinds of rejected access
enum class AccessEvent : uint8_t {
    BadCommunity,  ///< SNMP request with a wrong community
    BadVersion,    ///< SNMPv2c/v3 request
    Malformed,     ///< Not a valid SNMP message
    RateLimited,   ///< Dropped by the rate limiter
    SetRejected,   ///< SetRequest with the right community (all objects are read-only)
    WebLoginFailed,  ///< Wrong user name or password on the web page
    WebLoginLocked   ///< Web login refused during the lockout after repeated failures
};

/**
 * @brief Short name of an event for the log display
 */
const char* accessEventName(AccessEvent event);

/**
 * @brief Ring buffer of the most recent rejected access attempts
 *
 * Repeated events from the same source and of the same kind are folded into
 * one entry with a count, so a flood does not push everything else out.
 */
class AccessLog {
public:
    static constexpr uint8_t CAPACITY = 16;

    struct Entry {
        uint32_t firstMs;   ///< millis() of the first event
        uint32_t lastMs;    ///< millis() of the latest event
        SourceKey source;   ///< See SourceKey
        uint32_t count;     ///< Events folded into this entry
        AccessEvent event;
    };

    /**
     * @brief Record an event
     * @return true if it started a new entry (worth printing), false if it was folded
     */
    bool record(SourceKey source, AccessEvent event, uint32_t nowMs);

    /// Number of entries held (at most CAPACITY)
    uint8_t size() const { return _size; }

    /// Entry by age, 0 = newest
    const Entry& newest(uint8_t index) const;

    /// Events recorded since start, including folded and overwritten ones
    uint32_t total() const { return _total; }

private:
    Entry _entries[CAPACITY] = {};
    uint8_t _head = 0;   // Next slot to write
    uint8_t _size = 0;
    uint32_t _total = 0;
};

/**
 * @brief Format an IPv4 address (host-order 32-bit) as dotted quad
 * @param buf At least 16 bytes
 */
void formatIPv4(uint32_t address, char* buf);
