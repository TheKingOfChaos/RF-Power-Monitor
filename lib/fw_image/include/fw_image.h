// Firmware slot header: parsing, key id and Ed25519 verification.
//
// Slot header, little endian, FW_SLOT_HEADER_SIZE (256) bytes:
//   [0,64)    Ed25519 signature
//   [64,68)   magic 0x57464652 ("RFFW")
//   [68,72)   header version (1)
//   [72,76)   slot (0 or 1)
//   [76,80)   image length in bytes
//   [80,96)   firmware version, NUL padded ASCII
//   [96,104)  key id (first 8 bytes of SHA-512 of the public key)
//   [104,256) 0xFF
// The image follows at byte 256. The signed message is bytes
// [64, 256 + imageLength) of the slot, contiguous in flash, so it can be
// verified straight from XIP. Freestanding safe: no heap, no exceptions.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fw_layout.h"

static const uint32_t FW_MAGIC = 0x57464652u;  // "RFFW" read as little endian
static const uint32_t FW_HEADER_VERSION = 1u;

struct SlotHeader {
    uint32_t slot;
    uint32_t imageLength;
    char version[17];  // NUL terminated
    uint8_t keyId[8];
};

enum class SlotCheck : uint8_t {
    Ok,
    Empty,         // erased flash (all 0xFF)
    BadMagic,      // not a slot header, or truncated
    WrongSlot,     // image was linked/signed for the other slot
    TooLong,       // image length does not fit the slot
    BadVersion,    // version not NUL terminated printable ASCII
    WrongKey,      // key id differs from the trusted key
    BadSignature,  // Ed25519 check failed
};

/// Parse and check the header at `bytes` (everything except the signature).
/// `len < FW_SLOT_HEADER_SIZE` gives Empty if all bytes are 0xFF, else BadMagic.
/// `keyId` is the trusted key's id (see fwKeyId). `out` is filled only on Ok.
SlotCheck slotHeaderParse(const uint8_t* bytes, size_t len, uint32_t expectedSlot,
                          const uint8_t keyId[8], SlotHeader& out);

/// Parse the header and verify the signature. `slot` points at FW_SLOT_SIZE
/// readable bytes. `out` is filled only on Ok.
SlotCheck slotVerify(const uint8_t* slot, uint32_t expectedSlot,
                     const uint8_t publicKey[32], SlotHeader& out);

/// Key id: first 8 bytes of the SHA-512 of the public key.
void fwKeyId(const uint8_t publicKey[32], uint8_t out[8]);

/// Key id as 16 lowercase hex characters plus NUL.
void fwKeyFingerprint(const uint8_t publicKey[32], char out[17]);

/// Short lowercase description of a result, e.g. "bad signature".
const char* slotCheckName(SlotCheck c);
