/**
 * @file fuzz_text_input.cpp
 * @brief Fuzz the text the serial menu and the web forms accept (lib/netconfig, lib/syslog)
 *
 * Input: up to three lines (split at '\n'), used as (address, mask, gateway),
 * (NTP, syslog) and on their own. Checks the documented promises: an IPv6 address
 * formats and parses back to the same bytes, hostnameFromText() always gives a
 * valid hostname, logSettingsApplyInput() is all or nothing.
 */
#include <string>

#include <log_settings.h>
#include <netconfig.h>

#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string all((const char*)data, size);
    std::string line[3];
    size_t start = 0;
    for (int i = 0; i < 3; i++) {
        size_t nl = all.find('\n', start);
        if (i == 2 || nl == std::string::npos) {
            line[i] = all.substr(start);
            break;
        }
        line[i] = all.substr(start, nl - start);
        start = nl + 1;
    }
    // Exact-size copies so ASan sees reads past the NUL
    std::vector<char> a = fuzzString((const uint8_t*)line[0].data(), line[0].size());
    std::vector<char> b = fuzzString((const uint8_t*)line[1].data(), line[1].size());
    std::vector<char> c = fuzzString((const uint8_t*)line[2].data(), line[2].size());

    uint8_t v4[4];
    if (parseIPv4(a.data(), v4)) isValidSubnetMask(v4);
    validateStaticConfig(a.data(), b.data(), c.data());

    uint8_t v6[16], back[16];
    char text[IPV6_TEXT_MAX];
    if (parseIPv6(a.data(), v6)) {
        formatIPv6(v6, text);
        if (strnlen(text, sizeof(text)) == sizeof(text)) __builtin_trap();
        if (!parseIPv6(text, back) || memcmp(v6, back, 16) != 0) __builtin_trap();
    }
    validateStaticIPv6(a.data(), b.data());

    TrapAddress trap;
    if (parseTrapTarget(a.data(), trap) == TrapTargetError::None) {
        formatTrapAddress(trap, text);
        TrapAddress again;
        if (parseTrapTarget(text, again) != TrapTargetError::None || again.ipv6 != trap.ipv6 ||
            memcmp(again.addr, trap.addr, trap.ipv6 ? 16 : 4) != 0) {
            __builtin_trap();
        }
    }

    char host[HOSTNAME_MAX_LENGTH + 1];
    hostnameFromText(a.data(), host);
    if (!hostnameIsValid(host)) __builtin_trap();
    const uint8_t mac[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};
    hostnameForDevice(a.data(), mac, host);
    if (!hostnameIsValid(host)) __builtin_trap();

    SyslogTarget target;
    parseSyslogTarget(a.data(), target);
    ntpServerTextIsValid(a.data());
    classifyServerInput(a.data());

    LogSettings s, before;
    logSettingsDefaults(s);
    before = s;
    const char* error = nullptr;
    if (!logSettingsApplyInput(s, a.data(), b.data(), error)) {
        if (error == nullptr || !logSettingsEqual(s, before)) __builtin_trap();
    }
    return 0;
}
