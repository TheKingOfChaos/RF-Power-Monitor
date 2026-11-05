#include "boot_record.h"

#include <string.h>

namespace {

const size_t OFF_SEQ = 4, OFF_ACTIVE = 8, OFF_TRIAL = 9, OFF_RESULT = 10, OFF_TRAP = 11;
const size_t OFF_VERSION = 12, VERSION_BYTES = 16, OFF_CRC = 60;

void putLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t getLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

uint32_t fwCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void bootRecordEncode(const BootRecord& r, uint8_t out[BOOT_RECORD_SIZE]) {
    memset(out, 0, BOOT_RECORD_SIZE);
    putLe32(out, BOOT_RECORD_MAGIC);
    putLe32(out + OFF_SEQ, r.sequence);
    out[OFF_ACTIVE] = r.active;
    out[OFF_TRIAL] = r.trial;
    out[OFF_RESULT] = (uint8_t)r.lastResult;
    out[OFF_TRAP] = r.trapPending ? 1 : 0;
    for (size_t i = 0; i < VERSION_BYTES && r.lastVersion[i] != '\0'; i++) out[OFF_VERSION + i] = (uint8_t)r.lastVersion[i];
    putLe32(out + OFF_CRC, fwCrc32(out, OFF_CRC));
}

bool bootRecordDecode(const uint8_t* in, size_t len, BootRecord& out) {
    if (in == nullptr || len < BOOT_RECORD_SIZE) return false;
    if (getLe32(in) != BOOT_RECORD_MAGIC) return false;
    if (getLe32(in + OFF_CRC) != fwCrc32(in, OFF_CRC)) return false;
    uint8_t active = in[OFF_ACTIVE], trial = in[OFF_TRIAL];
    if (active > 1) return false;
    if (trial > 1 && trial != FW_NO_SLOT) return false;
    if (trial == active) return false;
    if (in[OFF_RESULT] > (uint8_t)FwResult::FallbackCrash) return false;
    if (in[OFF_TRAP] > 1) return false;
    bool ended = false;
    for (size_t i = 0; i < VERSION_BYTES; i++) {
        uint8_t c = in[OFF_VERSION + i];
        if (c == 0) {
            ended = true;
        } else if (ended || c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    out.sequence = getLe32(in + OFF_SEQ);
    out.active = active;
    out.trial = trial;
    out.lastResult = (FwResult)in[OFF_RESULT];
    out.trapPending = in[OFF_TRAP] != 0;
    memcpy(out.lastVersion, in + OFF_VERSION, VERSION_BYTES);
    out.lastVersion[VERSION_BYTES] = '\0';
    return true;
}

int bootRecordNewest(bool ok0, const BootRecord& r0, bool ok1, const BootRecord& r1) {
    if (!ok0 && !ok1) return -1;
    if (!ok1) return 0;
    if (!ok0) return 1;
    return (int32_t)(r1.sequence - r0.sequence) > 0 ? 1 : 0;
}
