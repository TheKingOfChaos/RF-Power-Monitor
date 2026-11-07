#pragma once

/**
 * @file bench_info.h
 * @brief The device data block printed by the hidden menu command `benchinfo`
 *        for the bench tool (tools/bench_device.py). Pure C++, host tested.
 *
 * One key=value per line between fixed markers, lines ending in CRLF:
 *
 *     BENCHINFO BEGIN
 *     hostname=RF-Power-A1B2C3
 *     mac=2C:CF:67:A1:B2:C3
 *     serial=E6614103E7A1B2C3
 *     firmware=3.5.0
 *     fwkey=0123456789abcdef
 *     password=k7mq2xr9tp
 *     locked=1
 *     lost=0
 *     updatedefault=http://10.132.0.45:8074
 *     updatedefaultkey=1
 *     BENCHINFO END
 */

#include <stddef.h>
#include <stdint.h>

/// Room for the whole block with the longest values (32-character hostname, 16-character
/// version, 127-character update URL)
static const size_t BENCH_INFO_MAX = 400;

/// What the bench tool needs from the device; nullptr strings print as empty values
struct BenchInfo {
    const char* hostname;  // name in use (DHCP, mDNS, sysName)
    uint8_t mac[6];
    const char* serial;    // board serial number (hex)
    const char* firmware;  // running firmware version
    const char* fwKey;     // fingerprint of the key the firmware trusts (16 hex)
    const char* password;  // device default web password
    bool locked;           // default password locked (on the label)
    bool passwordLost;     // no valid copy was found at boot while locked
    const char* updateDefault;  // update server kept through a factory reset: URL or "dhcp"; nullptr prints "none"
    bool updateDefaultKey;      // that default has the shared key (the key itself is never printed)
};

/**
 * @brief Write the block into out
 * @return Its length, or 0 if it does not fit in size bytes (out[0] is then
 *         '\0' when size > 0)
 */
size_t formatBenchInfo(const BenchInfo& info, char* out, size_t size);
