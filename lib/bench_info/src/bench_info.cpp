#include "bench_info.h"

#include <stdio.h>

/// The value, or "" for nullptr
static const char* text(const char* s) { return s != nullptr ? s : ""; }

size_t formatBenchInfo(const BenchInfo& info, char* out, size_t size) {
    if (size == 0) return 0;
    const uint8_t* m = info.mac;
    int n = snprintf(out, size,
                     "BENCHINFO BEGIN\r\n"
                     "hostname=%s\r\n"
                     "mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n"
                     "serial=%s\r\n"
                     "firmware=%s\r\n"
                     "fwkey=%s\r\n"
                     "password=%s\r\n"
                     "locked=%d\r\n"
                     "lost=%d\r\n"
                     "BENCHINFO END\r\n",
                     text(info.hostname), m[0], m[1], m[2], m[3], m[4], m[5], text(info.serial),
                     text(info.firmware), text(info.fwKey), text(info.password), info.locked ? 1 : 0,
                     info.passwordLost ? 1 : 0);
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
