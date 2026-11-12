/**
 * @file fuzz_snmp_ber.cpp
 * @brief Fuzz the BER decoder (lib/snmp_ber): every reader on every element, nested
 */
#include <snmp_ber.h>

#include "fuzz_common.h"

/// Read each element of r with the reader its tag suggests, descending into constructed ones
static void walk(BerReader r, int depth) {
    uint8_t tag;
    while (depth < 8 && r.peekTag(tag)) {
        BerReader copy = r;
        int32_t i32;
        uint32_t u32;
        uint64_t u64;
        const uint8_t* data;
        size_t len;
        Oid oid;
        switch (tag) {
        case BerTag::Integer: copy.readInteger(i32); break;
        case BerTag::OctetString: copy.readOctetString(data, len); break;
        case BerTag::Null: copy.readNull(); break;
        case BerTag::ObjectId:
            if (copy.readOid(oid)) {
                char text[OID_MAX_ARCS * 11 + 1];
                oidToString(oid, text, sizeof(text));
            }
            break;
        case BerTag::Counter64: copy.readUnsigned64(tag, u64); break;
        default:
            if (tag & 0x20) {  // constructed: SEQUENCE and the PDUs
                BerReader inner;
                if (copy.readSequence(tag, inner)) walk(inner, depth + 1);
            } else {
                copy.readUnsigned(tag, u32);
            }
        }
        // Then step over it generically, as the message layer does for unknown elements
        BerReader contents;
        if (!r.readTlv(tag, contents)) return;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    walk(BerReader(data, size), 0);
    BerReader raw(data, size);
    const uint8_t* start;
    size_t len;
    while (raw.readRaw(start, len)) {
    }
    // Dotted OID text as the CLI and tests give it
    std::vector<char> s = fuzzString(data, size);
    Oid oid;
    oidFromString(s.data(), oid);
    return 0;
}
