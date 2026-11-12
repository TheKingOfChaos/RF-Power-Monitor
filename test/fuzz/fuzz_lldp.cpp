/**
 * @file fuzz_lldp.cpp
 * @brief Fuzz the LLDP frame parser (lib/lldp): any frame on the wire, then what is made from it
 *
 * Checks the promises of lldp.h: texts terminated inside their fields and printable
 * (0x20..0x7E, others replaced by '?'), raw IDs within their arrays.
 */
#include <lldp.h>

#include "fuzz_common.h"

/// Terminated within size bytes and printable
static void checkText(const char* s, size_t size) {
    size_t n = strnlen(s, size);
    if (n == size) __builtin_trap();
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) __builtin_trap();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> f(data, data + size);  // exact size for ASan
    LldpNeighbour n;
    if (lldpParseFrame(f.data(), f.size(), n) != LldpParse::Ok) return 0;
    checkText(n.chassisId, sizeof(n.chassisId));
    checkText(n.portId, sizeof(n.portId));
    checkText(n.portDescr, sizeof(n.portDescr));
    checkText(n.sysName, sizeof(n.sysName));
    checkText(n.sysDescr, sizeof(n.sysDescr));
    checkText(n.mgmtAddr, sizeof(n.mgmtAddr));
    if (n.chassisIdLen > sizeof(n.chassisIdRaw) || n.portIdLen > sizeof(n.portIdRaw)) __builtin_trap();

    uint8_t subtype;
    const uint8_t* id;
    size_t idLen;
    lldpSnmpPortId(n, subtype, id, idLen);
    const uint8_t* lo = (const uint8_t*)&n;
    if (idLen > 0 && (id < lo || id + idLen > lo + sizeof(n))) __builtin_trap();  // must point into n

    LldpNeighbour same = n;
    if (!lldpSameNeighbour(n, same)) __builtin_trap();
    return 0;
}
