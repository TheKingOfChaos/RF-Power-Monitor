/**
 * @file fuzz_common.h
 * @brief Helpers shared by the libFuzzer harnesses in test/fuzz (scripts/fuzz.sh builds them)
 *
 * Each harness defines LLVMFuzzerTestOneInput() and calls one parser of a pure
 * library with the fuzzer's bytes. The parsers check checksums and MACs before
 * reading further, which random bytes almost never pass, so the harnesses can
 * make those valid first (fuzzFixIPv4 / fuzzFixIPv6) and still reach the code behind them.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

/// Reads control values and chunks from the front of the fuzz input
class FuzzInput {
public:
    FuzzInput(const uint8_t* data, size_t size) : _p(data), _left(size) {}

    /// Next byte, 0 when the input is used up
    uint8_t byte() {
        if (_left == 0) return 0;
        _left--;
        return *_p++;
    }

    /// Up to n bytes; len receives how many there were
    const uint8_t* take(size_t n, size_t& len) {
        len = n < _left ? n : _left;
        const uint8_t* p = _p;
        _p += len;
        _left -= len;
        return p;
    }

    /// The rest of the input
    const uint8_t* rest(size_t& len) { return take(_left, len); }

    size_t left() const { return _left; }

private:
    const uint8_t* _p;
    size_t _left;
};

/// A copy of the bytes with a NUL after them, for parsers of C strings
inline std::vector<char> fuzzString(const uint8_t* data, size_t size) {
    std::vector<char> s(data, data + size);
    s.push_back('\0');
    return s;
}

/// Ones' complement sum over n bytes (big-endian 16-bit words, odd byte padded)
inline uint32_t fuzzSum16(const uint8_t* p, size_t n, uint32_t acc) {
    for (size_t i = 0; i + 1 < n; i += 2) acc += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) acc += (uint32_t)p[n - 1] << 8;
    return acc;
}

inline uint16_t fuzzFold(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

inline uint16_t fuzzBe16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

inline void fuzzPut16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * @brief Make the IPv4 header checksum and the UDP checksum of an Ethernet frame valid
 *
 * Only where the headers' own lengths fit the frame; anything else is left as it is,
 * so the parser still sees (and must refuse) inconsistent frames.
 */
inline void fuzzFixIPv4(uint8_t* f, size_t len) {
    const size_t eth = 14;
    if (len < eth + 20 || fuzzBe16(f + 12) != 0x0800) return;
    uint8_t* ip = f + eth;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if (ihl < 20 || ihl > len - eth) return;
    fuzzPut16(ip + 10, 0);
    fuzzPut16(ip + 10, fuzzFold(fuzzSum16(ip, ihl, 0)));
    size_t total = fuzzBe16(ip + 2);
    if (ip[9] != 17 || total > len - eth || total < ihl + 8) return;
    uint8_t* udp = ip + ihl;
    size_t ulen = fuzzBe16(udp + 4);
    if (ulen < 8 || ulen > total - ihl) return;
    fuzzPut16(udp + 6, 0);
    uint32_t acc = fuzzSum16(ip + 12, 8, 0) + 17 + (uint32_t)ulen;
    uint16_t sum = fuzzFold(fuzzSum16(udp, ulen, acc));
    fuzzPut16(udp + 6, sum == 0 ? 0xFFFF : sum);
}

/**
 * @brief Make the UDP or ICMPv6 checksum of an Ethernet/IPv6 frame valid (as fuzzFixIPv4)
 */
inline void fuzzFixIPv6(uint8_t* f, size_t len) {
    const size_t eth = 14, ip6 = 40;
    if (len < eth + ip6 || fuzzBe16(f + 12) != 0x86DD) return;
    uint8_t* ip = f + eth;
    size_t plen = fuzzBe16(ip + 4);
    if (plen > len - eth - ip6) return;
    uint8_t next = ip[6];
    size_t field;
    if (next == 17) {
        field = 6;
    } else if (next == 58) {
        field = 2;
    } else {
        return;
    }
    if (plen < field + 2) return;
    uint8_t* data = ip + ip6;
    fuzzPut16(data + field, 0);
    uint32_t acc = fuzzSum16(ip + 8, 32, 0) + (uint32_t)(plen >> 16) + (uint32_t)(plen & 0xFFFF) + next;
    uint16_t sum = fuzzFold(fuzzSum16(data, plen, acc));
    if (sum == 0 && next == 17) sum = 0xFFFF;
    fuzzPut16(data + field, sum);
}
