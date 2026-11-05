// Boot record: which firmware slot is active, which is on trial, and the
// outcome of the last update. Stored by the firmware in two flash copies
// (written only by the firmware, never by the boot program), read by the
// boot program. Freestanding safe: no heap, no exceptions.
//
// Encoding, little endian, BOOT_RECORD_SIZE (64) bytes:
//   [0,4)    magic 0x52464252 ("RBFR")
//   [4,8)    sequence number (newer = (int32_t)(a - b) > 0)
//   [8]      active slot (0 or 1)
//   [9]      trial slot (0, 1 or FW_NO_SLOT 0xFF), never equal to active
//   [10]     last result (FwResult)
//   [11]     trap pending (0 or 1)
//   [12,28)  lastVersion, 16 bytes NUL padded printable ASCII
//   [28,60)  reserved: written as zero, ignored on read
//   [60,64)  CRC-32 of bytes [0,60)
#pragma once
#include <stddef.h>
#include <stdint.h>

enum class FwResult : uint8_t {
    None = 0,
    Installed = 1,
    RolledBackCrash = 2,
    RolledBackUnreachable = 3,
    FallbackCrash = 4,
};

static const uint8_t FW_NO_SLOT = 0xFF;
static const size_t BOOT_RECORD_SIZE = 64;
static const uint32_t BOOT_RECORD_MAGIC = 0x52464252u;

struct BootRecord {
    uint32_t sequence;
    uint8_t active;
    uint8_t trial;  // FW_NO_SLOT when no update is on trial
    FwResult lastResult;
    bool trapPending;
    char lastVersion[17];  // version the result is about, NUL terminated
};

// Standard reflected CRC-32 (poly 0xEDB88320, init and final xor 0xFFFFFFFF).
uint32_t fwCrc32(const uint8_t* data, size_t len);

// Writes the 64 byte encoding. Does not validate the record.
void bootRecordEncode(const BootRecord& r, uint8_t out[BOOT_RECORD_SIZE]);

// Parses and validates one copy. Rejects short input, bad magic or CRC, an
// invalid slot, trial == active, an unknown result, a trap flag other than
// 0/1, and a version that is not printable ASCII followed only by NULs.
bool bootRecordDecode(const uint8_t* in, size_t len, BootRecord& out);

// Picks the newer valid copy: 0, 1, or -1 when neither is valid.
int bootRecordNewest(bool ok0, const BootRecord& r0, bool ok1, const BootRecord& r1);
