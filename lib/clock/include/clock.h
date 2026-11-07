// Calendar time from seconds since 1970 (UTC), Danish local time and the
// SNMPv2-TC DateAndTime encoding. Pure C++: no time.h, so the host and the
// Pico give the same results. Denmark: CET (UTC+1), CEST (UTC+2) from the last
// Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC (EU rule).
#pragma once
#include <stddef.h>
#include <stdint.h>

/// Times before this (2026-01-01 UTC) mean "no clock": the core's clock
/// counts from 1970 at boot until NTP sets it
static const uint64_t CLOCK_VALID_AFTER = 1767225600ull;

struct CivilTime {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
};

/// UTC calendar time of `epochSeconds`
CivilTime civilFromEpoch(uint64_t epochSeconds);
/// EU summer time (CEST) is in force at `epochSeconds`
bool euSummerTime(uint64_t epochSeconds);
/// Danish local time of `epochSeconds`; `summer` tells CEST from CET
CivilTime danishLocal(uint64_t epochSeconds, bool& summer);
/// "2026-11-07 19:42:10 CET" (Danish local time); the length, or 0 if it does not fit
size_t formatLocalTime(uint64_t epochSeconds, char* out, size_t size);
/// "2026-11-07T18:42:10.123Z" (UTC, RFC 5424); the length, or 0 if it does not fit
size_t formatUtcRfc5424(uint64_t epochMs, char* out, size_t size);
/// SNMPv2-TC DateAndTime of the Danish local time: year (2 bytes, big endian),
/// month, day, hour, minute, second, deci-seconds (0), '+', hours from UTC, 0
void dateAndTime(uint64_t epochSeconds, uint8_t out[11]);
