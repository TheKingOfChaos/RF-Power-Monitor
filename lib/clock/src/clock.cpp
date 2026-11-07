#include "clock.h"

#include <stdio.h>

namespace {

const uint64_t SECONDS_PER_DAY = 86400;

/// Days since 1970-01-01 of a calendar date (Howard Hinnant's days_from_civil)
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/// Seconds since 1970 of the last Sunday of `month` in `year`, 01:00 UTC
uint64_t lastSunday0100(uint16_t year, unsigned month) {
    const int64_t days = daysFromCivil(year, month, 31);  // March and October have 31 days
    const int64_t weekday = (days + 4) % 7;               // 1970-01-01 was a Thursday (4); 0 = Sunday
    return (uint64_t)(days - weekday) * SECONDS_PER_DAY + 3600;
}

}  // namespace

CivilTime civilFromEpoch(uint64_t epochSeconds) {
    // civil_from_days (Howard Hinnant), for days >= 0
    const int64_t z = (int64_t)(epochSeconds / SECONDS_PER_DAY) + 719468;
    const uint32_t secs = (uint32_t)(epochSeconds % SECONDS_PER_DAY);
    const int64_t era = z / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    CivilTime c;
    c.year = (uint16_t)(yoe + era * 400 + (m <= 2));
    c.month = (uint8_t)m;
    c.day = (uint8_t)d;
    c.hour = (uint8_t)(secs / 3600);
    c.minute = (uint8_t)(secs / 60 % 60);
    c.second = (uint8_t)(secs % 60);
    return c;
}

bool euSummerTime(uint64_t epochSeconds) {
    const uint16_t year = civilFromEpoch(epochSeconds).year;
    return epochSeconds >= lastSunday0100(year, 3) && epochSeconds < lastSunday0100(year, 10);
}

CivilTime danishLocal(uint64_t epochSeconds, bool& summer) {
    summer = euSummerTime(epochSeconds);
    return civilFromEpoch(epochSeconds + (summer ? 7200 : 3600));
}

size_t formatLocalTime(uint64_t epochSeconds, char* out, size_t size) {
    bool summer;
    const CivilTime c = danishLocal(epochSeconds, summer);
    char buf[40];
    const int n = snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u %s", c.year, c.month, c.day, c.hour,
                           c.minute, c.second, summer ? "CEST" : "CET");
    if (n < 0 || out == nullptr || (size_t)n >= size) return 0;
    snprintf(out, size, "%s", buf);
    return (size_t)n;
}

size_t formatUtcRfc5424(uint64_t epochMs, char* out, size_t size) {
    const CivilTime c = civilFromEpoch(epochMs / 1000);
    char buf[40];
    const int n = snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", c.year, c.month, c.day, c.hour,
                           c.minute, c.second, (unsigned)(epochMs % 1000));
    if (n < 0 || out == nullptr || (size_t)n >= size) return 0;
    snprintf(out, size, "%s", buf);
    return (size_t)n;
}

void dateAndTime(uint64_t epochSeconds, uint8_t out[11]) {
    bool summer;
    const CivilTime c = danishLocal(epochSeconds, summer);
    out[0] = (uint8_t)(c.year >> 8);
    out[1] = (uint8_t)c.year;
    out[2] = c.month;
    out[3] = c.day;
    out[4] = c.hour;
    out[5] = c.minute;
    out[6] = c.second;
    out[7] = 0;
    out[8] = '+';
    out[9] = summer ? 2 : 1;
    out[10] = 0;
}
